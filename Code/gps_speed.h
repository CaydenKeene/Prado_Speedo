#pragma once
// gps_speed.h
// Road-speed source for the Prado cluster, taken from a serial (NMEA) GPS
// module instead of the car's VSS "pink" wire.
//
// WHY GPS: the VSS wire needs an opto, a clean edge, and a per-vehicle
// pulses-per-mile calibration. A GPS module reports ground speed directly and
// is already calibrated (it comes from satellite Doppler, not from wheel size
// or diff ratio), so it survives tyre and gearing changes. The trade-offs are
// ~0.2-1 s of latency, a dead readout until the module gets a fix, and nothing
// at all in a tunnel or a shed.
//
// HARDWARE: any 3.3 V UART NMEA module. This build uses an ATGM336H-5N (AT6558,
// GPS+BDS+GLONASS+Galileo) with an external 28 dB active puck antenna on u.FL,
// but u-blox NEO-6M/7M/M8N and MediaTek PA1010D work too — see applyConfig().
// Only the module's TX line is strictly required; its RX is used to raise the
// update rate at boot (optional, but 1 Hz feels sluggish on a speedo).
//
// ACTIVE ANTENNA: powered by DC bias the module puts on its own antenna pin. If
// a board does not do that, an active antenna reads zero satellites forever.
// Check with a multimeter: ~3 V between the u.FL centre pin and GND.
//
// WIRING (replaces the PC817 opto entirely):
//   GPS VCC -> ESP32 3V3 (check your module: most breakouts take 3.3-5 V, but
//              the ESP32-S3 is NOT 5 V tolerant, so its TX must be 3.3 V logic)
//   GPS GND -> ESP32 GND
//   GPS TX  -> GPIO44  (UART-header RXD pad)   <- the data we read
//   GPS RX  -> GPIO43  (UART-header TXD pad)   <- config commands we send
//
// Arduino IDE: keep "USB CDC On Boot: Enabled" so the serial console stays on
// native USB and leaves GPIO43/44 free for the GPS.
//
// METHOD: parse only $..RMC (ground speed in knots + fix validity) and $..GGA
// (satellites, HDOP) with a checksum-verified minimal parser — no external
// library. Speed is clamped to 0 below GPS_ZERO_MPH (a stationary receiver
// still reports a fraction of a knot of noise) and lightly EMA-smoothed.

#include <Arduino.h>

// ---- pins & tuning ---------------------------------------------------------
#define GPS_RX_PIN        44        // ESP32 RX  <- GPS TX  (UART-header RXD pad)
#define GPS_TX_PIN        43        // ESP32 TX  -> GPS RX  (UART-header TXD pad)

// Baud the module talks out of the box (9600 for nearly everything), and the
// baud we want it to run at. Leave them equal to skip the switch; 10 Hz needs
// at least 38400 to fit the sentences into each epoch.
#define GPS_BAUD_BOOT     9600
#define GPS_BAUD_RUN      38400     // 10 Hz does not fit in 9600 baud; 38400 does
#define GPS_RATE_HZ       10        // fix rate to request

#define GPS_ZERO_MPH      1.2f      // below this, report a hard 0 (kills parked drift)
#define GPS_EMA_ALPHA     0.50f     // display smoothing (higher = snappier, less lag)
#define GPS_STALE_MS      1500      // no valid fix for this long -> stop trusting it
#define GPS_HOLD_MS       5000      // ...then coast on the last speed for this long
#define GPS_PROBE_MS      4000      // no valid sentence for this long -> try next baud
#define GPS_LINE_MAX      100       // longest NMEA sentence we buffer

#define GPS_KNOTS_TO_MPH  1.150779f

class GpsSpeed {
public:
  void begin() {
    _baudIdx = 0;
    openPort(kBaudTable[_baudIdx]);
    _lastSentenceMs = millis();
  }

  // Call every UI tick (~50 ms). Non-blocking apart from the one-shot config.
  void update() {
    while (_serial->available()) {
      const char c = (char)_serial->read();
      if (c == '\r') continue;
      if (c == '\n') { _line[_len] = '\0'; handleLine(); _len = 0; continue; }
      if (_len < GPS_LINE_MAX - 1) _line[_len++] = c;
      else                         _len = 0;        // overlong garbage: resync
    }

    const uint32_t now = millis();

    // ---- link watchdog: hunt for the module's baud until sentences arrive ---
    if (now - _lastSentenceMs > GPS_PROBE_MS) {
      _linked = false;
      _configured = false;
      _baudIdx = (_baudIdx + 1) % kBaudCount;
      openPort(kBaudTable[_baudIdx]);
      _lastSentenceMs = now;                        // give the new baud a full window
    }

    // ---- once it is talking to us, raise the fix rate (once) ---------------
    if (_linked && !_configured) { applyConfig(); _configured = true; }

    // ---- fix watchdog --------------------------------------------------------
    // Dropping straight to 0 the moment a fix goes stale is the worst failure
    // mode a speedo has: pass under an overpass at 45 and the gauge says 0. So
    // there are two stages. Past GPS_STALE_MS we stop claiming a fix (the gauge
    // greys the number), but COAST on the last known speed — under a bridge the
    // car is almost certainly still doing what it was doing. Only once the
    // outage outlasts GPS_HOLD_MS do we admit we have no idea and show 0.
    const uint32_t age = now - _lastFixMs;
    if (age > GPS_STALE_MS) {
      _hasFix  = false;
      _holding = (_mph > 0.0f) && (age <= GPS_STALE_MS + GPS_HOLD_MS);
      if (!_holding) _mph = 0.0f;
    }
  }

  float    mph()      const { return _mph; }        // smoothed, ready for the gauge
  float    rawMph()   const { return _rawMph; }     // last reported ground speed
  bool     hasFix()   const { return _hasFix; }
  bool     holding()  const { return _holding; }    // coasting through a dropout
  bool     linked()   const { return _linked; }     // seeing valid NMEA at all
  uint8_t  sats()     const { return _sats; }
  float    hdop()     const { return _hdop; }
  float    fixHz()    const { return _fixHz; }      // measured fix rate
  uint32_t baud()     const { return kBaudTable[_baudIdx]; }

private:
  // Baud hunt order: the two we configure first, then the other common ones, so
  // an unknown module (or a half-applied baud switch) still finds its way home.
  static const uint32_t kBaudTable[5];
  static const uint8_t  kBaudCount = 5;

  void openPort(uint32_t baud) {
    _serial->end();
    _serial->begin(baud, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    _len = 0;
  }

  // ------------------------------------------------------------- parsing -----
  void handleLine() {
    if (_len < 9 || _line[0] != '$') return;

    // "*CS" tail: XOR of everything between '$' and '*'.
    char *star = strchr(_line, '*');
    if (!star || (_line + _len) - star != 3) return;
    const uint8_t want = (uint8_t)strtoul(star + 1, NULL, 16);
    uint8_t got = 0;
    for (char *p = _line + 1; p < star; ++p) got ^= (uint8_t)*p;
    if (got != want) return;
    *star = '\0';

    _lastSentenceMs = millis();
    _linked = true;

    // Split on commas in place. The talker ID varies (GP/GN/GL/GA/BD), so match
    // on the 3-char sentence type only.
    char *f[16] = {0};
    int   nf    = 0;
    f[nf++] = _line;
    for (char *p = _line; *p && nf < 16; ++p)
      if (*p == ',') { *p = '\0'; f[nf++] = p + 1; }

    const char *type = _line + 3;
    if      (!strcmp(type, "RMC") && nf > 7) parseRmc(f);
    else if (!strcmp(type, "GGA") && nf > 8) parseGga(f);
  }

  void parseRmc(char **f) {
    const uint32_t now   = millis();
    const bool     valid = (f[2][0] == 'A');

    // Measure the real fix rate — this is how you confirm the rate config took.
    if (_lastRmcMs) {
      const float dt = (now - _lastRmcMs) / 1000.0f;
      if (dt > 0.02f && dt < 5.0f) _fixHz += 0.25f * ((1.0f / dt) - _fixHz);
    }
    _lastRmcMs = now;

    if (!valid) return;                   // no fix: let the stale watchdog zero us

    float mph = atof(f[7]) * GPS_KNOTS_TO_MPH;
    if (mph < GPS_ZERO_MPH) mph = 0.0f;   // parked-receiver noise floor

    _rawMph    = mph;
    _hasFix    = true;
    _holding   = false;
    _lastFixMs = now;

    // Two zero fixes in a row -> snap to 0 instead of letting the EMA crawl down.
    if (mph == 0.0f && _prevZero) _mph = 0.0f;
    else                          _mph += GPS_EMA_ALPHA * (mph - _mph);
    if (_mph < 0.05f) _mph = 0.0f;
    _prevZero = (mph == 0.0f);
  }

  void parseGga(char **f) {
    _sats = (uint8_t)atoi(f[7]);
    _hdop = atof(f[8]);
  }

  // -------------------------------------------------- module configuration ---
  // Sent once, after we know the module is talking. THREE dialects go out and
  // every module ignores the two that are not its own:
  //   UBX   (binary)  - u-blox NEO-6M/7M/M8x. Note gen 9/10 (M9/M10) dropped
  //                     these legacy CFG messages for CFG-VALSET; add that if
  //                     you ever fit one.
  //   PMTK  ($PMTK..) - MediaTek MT3339 / Adafruit Ultimate GPS.
  //   PCAS  ($PCAS..) - AT6558 / ATGM336H. NOT the same as PMTK.
  // Unknown NMEA commands are simply discarded by the receiver, and the binary
  // UBX frames fail an NMEA parser's framing, so cross-talk is harmless.
  void applyConfig() {
    if (GPS_BAUD_RUN != kBaudTable[_baudIdx]) {
      setBaud(GPS_BAUD_RUN);
      openPort(GPS_BAUD_RUN);
      _baudIdx = 1;                     // kBaudTable[1] == GPS_BAUD_RUN
      delay(60);
    }
    trimSentences();                    // free the bandwidth before raising the rate
    setRate(GPS_RATE_HZ);
    _lastSentenceMs = millis();         // don't let the reconfigure trip the watchdog
  }

  // AT6558 takes an index, not a rate: 0=4800 1=9600 2=19200 3=38400 4=57600
  // 5=115200. Returns -1 for a rate it cannot express.
  static int pcasBaudIndex(uint32_t b) {
    switch (b) {
      case 4800:   return 0;
      case 9600:   return 1;
      case 19200:  return 2;
      case 38400:  return 3;
      case 57600:  return 4;
      case 115200: return 5;
      default:     return -1;
    }
  }

  void setBaud(uint32_t baud) {
    char body[32];
    snprintf(body, sizeof(body), "PMTK251,%lu", (unsigned long)baud);
    sendNmea(body);

    const int pcas = pcasBaudIndex(baud);
    if (pcas >= 0) {
      snprintf(body, sizeof(body), "PCAS01,%d", pcas);
      sendNmea(body);
    }

    uint8_t p[20] = {0};                          // UBX-CFG-PRT
    p[0]  = 1;                                    // portID: UART1
    p[4]  = 0xD0; p[5] = 0x08;                    // mode: 8N1
    p[8]  = (uint8_t)(baud);       p[9]  = (uint8_t)(baud >> 8);
    p[10] = (uint8_t)(baud >> 16); p[11] = (uint8_t)(baud >> 24);
    p[12] = 0x07;                                 // inProtoMask:  UBX+NMEA+RTCM
    p[14] = 0x07;                                 // outProtoMask: UBX+NMEA
    sendUbx(0x06, 0x00, p, sizeof(p));
    _serial->flush();
    delay(120);
  }

  // Keep only RMC (speed) and GGA (sats/HDOP); GSV in particular is a firehose.
  void trimSentences() {
    //        fields: GLL RMC VTG GGA GSA GSV ... (19 total)
    sendNmea("PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");

    static const uint8_t off[4] = { 0x01,   // GLL
                                    0x02,   // GSA
                                    0x03,   // GSV
                                    0x05 }; // VTG
    for (uint8_t i = 0; i < 4; ++i) {
      uint8_t p[3] = { 0xF0, off[i], 0 };
      sendUbx(0x06, 0x01, p, 3);            // UBX-CFG-MSG, rate 0 = disable
    }
    uint8_t rmc[3] = { 0xF0, 0x04, 1 };  sendUbx(0x06, 0x01, rmc, 3);
    uint8_t gga[3] = { 0xF0, 0x00, 1 };  sendUbx(0x06, 0x01, gga, 3);

    // PCAS03 field order (AT6558): GGA GLL GSA GSV RMC VTG ZDA ANT DHV LPS
    // res res UTC GST res res res TIM. Empty fields are left unchanged.
    sendNmea("PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0,,,,0");

    // PCAS04 constellations: 1=GPS 2=BDS 3=GPS+BDS 4=GLONASS 5=GPS+GLONASS
    // 6=BDS+GLONASS 7=GPS+BDS+GLONASS. 7 is the point of buying this chip —
    // three constellations is what keeps a fix under tree cover.
    sendNmea("PCAS04,7");
  }

  void setRate(uint8_t hz) {
    const uint16_t ms = (hz > 0) ? (uint16_t)(1000 / hz) : 1000;
    char body[24];
    snprintf(body, sizeof(body), "PMTK220,%u", (unsigned)ms);
    sendNmea(body);

    snprintf(body, sizeof(body), "PCAS02,%u", (unsigned)ms);   // AT6558
    sendNmea(body);

    uint8_t p[6] = { (uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8),   // measRate, ms
                     1, 0,                                       // navRate, cycles
                     1, 0 };                                     // timeRef, GPS
    sendUbx(0x06, 0x08, p, sizeof(p));                           // UBX-CFG-RATE
  }

  // $<body>*CS\r\n — checksum computed here so the command strings stay readable.
  void sendNmea(const char *body) {
    uint8_t cs = 0;
    for (const char *p = body; *p; ++p) cs ^= (uint8_t)*p;
    _serial->printf("$%s*%02X\r\n", body, cs);
  }

  // UBX frame: B5 62 | class id | len(LE) | payload | 8-bit Fletcher checksum.
  void sendUbx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
    uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; ++i)        { a += hdr[i];     b += a; }
    for (uint16_t i = 0; i < len; ++i) { a += payload[i]; b += a; }
    _serial->write(hdr, 6);
    if (len) _serial->write(payload, len);
    _serial->write(a);
    _serial->write(b);
  }

  HardwareSerial *_serial = &Serial1;   // UART1 remapped to GPIO43/44 (UART0 = USB CDC)
  char     _line[GPS_LINE_MAX] = {0};
  uint8_t  _len = 0;

  float    _mph    = 0.0f;
  float    _rawMph = 0.0f;
  float    _fixHz  = 0.0f;
  float    _hdop   = 0.0f;
  uint8_t  _sats   = 0;
  bool     _hasFix = false;
  bool     _holding = false;
  bool     _linked = false;
  bool     _configured = false;
  bool     _prevZero   = true;

  uint8_t  _baudIdx        = 0;
  uint32_t _lastSentenceMs = 0;
  uint32_t _lastFixMs      = 0;
  uint32_t _lastRmcMs      = 0;
};

// This header is included exactly once (single speed source), so the table can
// live here next to the class.
const uint32_t GpsSpeed::kBaudTable[5] = { GPS_BAUD_BOOT, GPS_BAUD_RUN, 9600, 38400, 115200 };
