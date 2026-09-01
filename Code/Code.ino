/* Prado_Speedo.ino
 * Minimal digital speedometer for a 1996 Land Cruiser Prado (J90).
 * Board: Waveshare ESP32-S3-Touch-LCD-1.85 (ST77916 360x360 round QSPI).
 *
 * GAUGE page (gauge_ui.h): big "STEALTH" speedo.
 * DIAG page  (below it):   swipe UP to reveal, swipe DOWN to return. Shows live
 *                          GPS output, a DEMO toggle, and a GPS status button.
 *
 * Speed comes from a serial GPS module on the UART header (GPIO44 = RX from the
 * module's TX). Ground speed is read straight out of the NMEA $..RMC sentence,
 * so there is nothing to calibrate. See gps_speed.h for wiring & method.
 *
 * Arduino IDE: set "USB CDC On Boot: Enabled" so the serial console stays on
 * native USB and frees GPIO43/44 for the GPS.
 */

#define DEMO_DEFAULT  false    // start on the real GPS input (flip DEMO on for the bench)

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include "Touch_CST816.h"
#include "LVGL_Driver.h"
#include "vehicle_data.h"
#include "gps_speed.h"
#include "gauge_ui.h"

MockVehicle  vehicle;          // simulated drive cycle (demo mode)
GpsSpeed     gps;              // real road speed from the GPS module
VehicleData  vehicle_data;     // what the gauge reads (only speed_mph matters)

static bool g_demo = DEMO_DEFAULT;

// ---- UI callbacks ----------------------------------------------------------
static void on_demo_toggle(bool on) { g_demo = on; }

// The diag button: GPS needs no calibration, so it reports link/fix state
// instead. Useful when the unit is on the dash and there is no serial console.
static void on_gps_status() {
  if (g_demo)            { gauge_ui_toast("DEMO MODE"); return; }
  if (!gps.linked())     { gauge_ui_toast("NO GPS");    return; }
  if (gps.holding())     { gauge_ui_toast("HOLDING");   return; }
  if (!gps.hasFix())     { gauge_ui_toast("SEARCHING"); return; }
  gauge_ui_toast_fmt("FIX %d SAT", (int)gps.sats());
}

// ---- 50 ms tick: source -> gauge, diag values, page gestures ---------------
void gauge_tick(lv_timer_t *t) {
  float    spd;
  bool     fix     = false;
  bool     holding = false;
  int      sats = 0;
  int      hdop_x10 = 0;
  int      fix_hz   = 0;

  if (g_demo) {
    vehicle.update();
    spd = vehicle.data.speed_mph;
    fix = true;                       // demo never shows the no-fix dim
  } else {
    gps.update();
    spd      = gps.mph();
    fix      = gps.hasFix();
    holding  = gps.holding();
    sats     = gps.sats();
    hdop_x10 = (int)(gps.hdop() * 10.0f + 0.5f);
    fix_hz   = (int)(gps.fixHz() + 0.5f);
  }

  vehicle_data.speed_mph = spd;
  gauge_ui_update(vehicle_data, fix);
  gauge_ui_set_diag(spd, fix_hz, sats, hdop_x10, g_demo, fix, holding);

  // Page navigation via the latched touch gesture (swipe up = page below).
  if (Lvgl_Last_Gesture != NONE) {
    GESTURE g = Lvgl_Last_Gesture;
    Lvgl_Last_Gesture = NONE;
    if (g == SWIPE_UP)   gauge_ui_show_diag();
    if (g == SWIPE_DOWN) gauge_ui_show_gauge();
  }
}

void setup() {
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();
  Set_Backlight(100);
  Touch_Init();
  LCD_Init();
  Lvgl_Init();

  gauge_ui_init();
  gauge_ui_set_gps_cb(on_gps_status);
  gauge_ui_set_demo_cb(on_demo_toggle);

  vehicle.begin();
  gps.begin();

  lv_timer_create(gauge_tick, 50, NULL);
}

void loop() {
  Lvgl_Loop();
  delay(5);
}
