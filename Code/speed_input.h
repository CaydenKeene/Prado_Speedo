#pragma once
// speed_input.h
// Real road-speed source for the Prado cluster (Toyota VSS, 3-wire Hall-effect,
// ~0-12V square wave, read through a PC817 opto-isolator).
//
// METHOD: reciprocal (period) counting, NOT fixed-window pulse counting. We
// timestamp each edge and compute speed from the time span across the last N
// edges:
//        speed = (N-1) * dist_per_pulse / (t_newest - t_oldest)
// This gives good resolution at low speed (where a fixed window would see only a
// pulse or two) and a fast, smooth reading at speed.
//
// WIRING (through the PC817 opto, isolating the 12V car side from the ESP32):
//   car speed wire ->  opto INPUT (use the module variant that matches the
//                      signal voltage: 5V or 12V; LED wants ~15-20 mA)
//   opto OUTPUT     ->  SPEED_PIN (GPIO44, UART-header RX pad) and GND.
//                       Internal pull-up only -> line swings 0..3.3V (the S3 is
//                       NOT 5V tolerant; never pull the output up to 5V/12V).
//   The opto INVERTS the signal, so we trigger on FALLING at the MCU pin, which
//   corresponds to the sensor's RISING edge. One edge type only -> an asymmetric
//   duty cycle cannot bias the reading.
//
// Arduino IDE: set "USB CDC On Boot: Enabled" so the serial console stays on
// native USB and frees GPIO43/44 for this input.

#include <Arduino.h>
#include <Preferences.h>
#include "esp_timer.h"     // esp_timer_get_time(): monotonic int64 microseconds

// ---- pin & tuning ----------------------------------------------------------
// Anti-jitter knobs live here so they are easy to tune:
//   * SPEED_AVG_EDGES  -> raw averaging window (higher = steadier, more lag)
//   * SPEED_EMA_ALPHA  -> display smoothing      (lower  = steadier, more lag)
//   * (display deadband + refresh rate live in gauge_ui.h)
#define SPEED_PIN              44        // UART-header RXD pad
#define SPEED_AVG_EDGES        12        // N: edges in the sliding speed window
#define SPEED_EMA_ALPHA        0.35f     // EMA factor applied to computed speed (higher = snappier, less lag)
#define SPEED_EDGE_BUF         16        // edge ring (MUST be power of 2 AND >= SPEED_AVG_EDGES)
#define SPEED_GLITCH_US        150       // ignore edges closer than this (noise)
#define SPEED_ZERO_TIMEOUT_US  800000    // no edge for 0.8 s -> exactly 0 mph
#define SPEED_CAL_KNOWN_MPH    24.85f    // 40 km/h in mph; drive this (phone GPS), then tap CALIBRATE
#define SPEED_CAL_SAMPLE_MS    2500      // calibrate averages pulses over ~this long
#define SPEED_CAL_MIN_PULSES   8         // need at least this many pulses to trust a cal

// Pre-calibration placeholder ONLY (miles per pulse). The real value is learned
// by calibrate() against GPS and stored in flash; this just makes the readout
// move on first power-up. (~8.33e-5 mi/pulse == the old "0.30 mph per Hz" guess.)
#define SPEED_DEFAULT_DIST_PER_PULSE  8.3333e-5f

#define SPEED_BUF_MASK  (SPEED_EDGE_BUF - 1)

// ---- ISR-shared state (one speed input; this header is included once) -------
static portMUX_TYPE      s_speed_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t  s_edge_ts[SPEED_EDGE_BUF] = {0};  // ring of edge times
static volatile uint32_t s_edge_count   = 0;   // total edges ever (monotonic)
static volatile int64_t  s_last_edge_us = 0;   // time of most recent edge

static void IRAM_ATTR speed_isr() {
  int64_t now = esp_timer_get_time();
  // Glitch reject: only this ISR writes s_last_edge_us and it is non-reentrant,
  // so this read is consistent without a lock.
  if (now - s_last_edge_us < SPEED_GLITCH_US) return;

  portENTER_CRITICAL_ISR(&s_speed_mux);
  s_last_edge_us = now;
  s_edge_ts[s_edge_count & SPEED_BUF_MASK] = now;
  s_edge_count++;
  portEXIT_CRITICAL_ISR(&s_speed_mux);
}

class SpeedSensor {
public:
  void begin() {
    pinMode(SPEED_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SPEED_PIN), speed_isr, FALLING);

    Preferences p;
    p.begin("speedo", true);                       // read-only
    _distPerPulse = p.getFloat("dpp", SPEED_DEFAULT_DIST_PER_PULSE);
    p.end();
  }

  // Call every UI tick (~50 ms).
  void update() {
    int64_t  ts[SPEED_EDGE_BUF];
    uint32_t count;
    int64_t  lastEdge;

    // Atomic snapshot: 64-bit values are NOT atomic on the 32-bit Xtensa core,
    // so copy everything under one critical section to avoid torn reads.
    portENTER_CRITICAL(&s_speed_mux);
    count    = s_edge_count;
    lastEdge = s_last_edge_us;
    for (int i = 0; i < SPEED_EDGE_BUF; ++i) ts[i] = s_edge_ts[i];
    portEXIT_CRITICAL(&s_speed_mux);

    const int64_t now = esp_timer_get_time();

    // ---- finish a running calibration if its window has elapsed -------------
    // (runs every update, even when stopped, so a stalled cal can time out)
    serviceCalibration(count, lastEdge, now);

    // ---- zero-speed: no edge within the timeout -> hard 0 (no stale value) ---
    if (count == 0 || (now - lastEdge) > SPEED_ZERO_TIMEOUT_US) {
      _mph = 0.0f;
      _hz  = 0.0f;
      return;
    }

    // ---- need at least 2 edges to form a span -------------------------------
    // Sliding window of the last N edges (Layer 1: multi-pulse averaging).
    const uint32_t n = (count < SPEED_AVG_EDGES) ? count : SPEED_AVG_EDGES;
    if (n < 2) return;          // just started moving: hold (still 0) until edge #2

    const int64_t t_new = ts[(count - 1) & SPEED_BUF_MASK];
    const int64_t t_old = ts[(count - n) & SPEED_BUF_MASK];
    const int64_t span  = t_new - t_old;
    if (span <= 0) return;      // guard div-by-zero / impossible ordering

    // ---- reciprocal speed over the window -----------------------------------
    const float span_s = span / 1e6f;                          // us -> s
    _hz = (float)(n - 1) / span_s;                             // raw pulse rate
    const float dist   = (n - 1) * _distPerPulse;              // miles
    float mph = (dist / span_s) * 3600.0f;                     // mi/s -> mph

    // ---- deceleration / stop-transition correction --------------------------
    // The buffer holds OLD (fast) pulses; if "now" is already past when the next
    // edge was due, we are slowing. Scale the estimate down by how overdue it is
    // so the readout falls smoothly instead of clinging to a stale high value
    // until the hard timeout fires.
    const float avg_period_us = (float)span / (n - 1);
    const float since_last    = (float)(now - t_new);
    if (since_last > avg_period_us)
      mph *= avg_period_us / since_last;

    // ---- EMA smoothing before display (Layer 2) -----------------------------
    // smoothed += alpha * (raw - smoothed). Lower alpha = steadier, more lag.
    _mph += SPEED_EMA_ALPHA * (mph - _mph);
  }

  // Start a calibration: the driver holds knownMph steady, then taps CALIBRATE.
  // Instead of a single instantaneous reading (which bakes in jitter), we mark a
  // start point and let serviceCalibration() average pulses over SPEED_CAL_SAMPLE_MS.
  // Returns false only if there is no signal at all to start from.
  bool beginCalibration(float knownMph) {
    if (knownMph < 1.0f) return false;

    uint32_t count;
    int64_t  lastEdge;
    portENTER_CRITICAL(&s_speed_mux);
    count    = s_edge_count;
    lastEdge = s_last_edge_us;
    portEXIT_CRITICAL(&s_speed_mux);

    const int64_t now = esp_timer_get_time();
    if (count == 0 || (now - lastEdge) > SPEED_ZERO_TIMEOUT_US) return false;

    _calKnownMph   = knownMph;
    _calStartCount = count;        // edges seen so far
    _calStartEdge  = lastEdge;     // timestamp of the last edge before we started
    _calStartUs    = now;
    _calActive     = true;
    _calResult     = 0;            // pending
    return true;
  }

  // Poll for the outcome of a calibration started with beginCalibration():
  //   0 = still running / nothing pending, +1 = success, -1 = failed.
  // The result is latched and cleared on read.
  int takeCalResult() { int r = _calResult; _calResult = 0; return r; }
  bool calibrating() const { return _calActive; }

  float    mph()          const { return _mph; }
  float    hz()           const { return _hz; }            // raw pulse rate
  uint32_t pulses()       const { return s_edge_count; }   // total edges (atomic 32-bit)
  float    distPerPulse() const { return _distPerPulse; }

private:
  // Called from update() with the current snapshot. When the averaging window has
  // elapsed, learn dist_per_pulse from ALL pulses counted across the window:
  //   dist_per_pulse = knownMph * span_s / (3600 * pulses)
  void serviceCalibration(uint32_t count, int64_t lastEdge, int64_t now) {
    if (!_calActive) return;
    if ((now - _calStartUs) < (int64_t)SPEED_CAL_SAMPLE_MS * 1000) return;  // not done yet

    _calActive = false;
    const uint32_t pulses = count - _calStartCount;       // edges during the window
    const int64_t  span   = lastEdge - _calStartEdge;     // time those edges spanned
    if (pulses < SPEED_CAL_MIN_PULSES || span <= 0) { _calResult = -1; return; }

    const float span_s = span / 1e6f;
    _distPerPulse = _calKnownMph * span_s / (3600.0f * pulses);

    Preferences p;
    p.begin("speedo", false);
    p.putFloat("dpp", _distPerPulse);
    p.end();
    _calResult = 1;
  }

  float _distPerPulse = SPEED_DEFAULT_DIST_PER_PULSE;
  float _mph          = 0.0f;
  float _hz           = 0.0f;

  // non-blocking calibration window
  bool     _calActive     = false;
  int      _calResult     = 0;       // 0 pending/idle, +1 ok, -1 fail (latched)
  float    _calKnownMph   = 0.0f;
  uint32_t _calStartCount = 0;
  int64_t  _calStartEdge  = 0;
  int64_t  _calStartUs    = 0;
};
