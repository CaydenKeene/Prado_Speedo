/* Prado_Speedo.ino
 * Minimal digital speedometer for a 1996 Land Cruiser Prado (J90).
 * Board: Waveshare ESP32-S3-Touch-LCD-1.85 (ST77916 360x360 round QSPI).
 *
 * GAUGE page (gauge_ui.h): big "STEALTH" speedo.
 * DIAG page  (below it):   swipe UP to reveal, swipe DOWN to return. Shows live
 *                          car output, a DEMO toggle, and a CALIBRATE button.
 *
 * Speed comes from the car's SPD ("pink") wire: a square wave whose FREQUENCY is
 * proportional to road speed, read as pulses on GPIO44 (UART-header RX pad)
 * through a PC817 opto-isolator. See speed_input.h for wiring & method.
 *
 * CALIBRATION: flip DEMO off, drive a steady SPEED_CAL_KNOWN_MPH (phone GPS),
 * then tap CALIBRATE. The factor is learned and saved to flash.
 *
 * Arduino IDE: set "USB CDC On Boot: Enabled" so the serial console stays on
 * native USB and frees GPIO43/44 for the speed input.
 */

#define DEMO_DEFAULT  false    // start on the real VSS input (flip DEMO on for the bench)

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include "Touch_CST816.h"
#include "LVGL_Driver.h"
#include "vehicle_data.h"
#include "speed_input.h"
#include "gauge_ui.h"

MockVehicle  vehicle;          // simulated drive cycle (demo mode)
SpeedSensor  speedo;           // real VSS pulse input
VehicleData  vehicle_data;     // what the gauge reads (only speed_mph matters)

static bool g_demo = DEMO_DEFAULT;

// ---- UI callbacks ----------------------------------------------------------
static void on_demo_toggle(bool on) { g_demo = on; }

static void on_calibrate() {
  if (g_demo) { gauge_ui_toast("DEMO MODE"); return; }
  // Non-blocking: averages pulses over ~SPEED_CAL_SAMPLE_MS, result polled below.
  if (speedo.beginCalibration(SPEED_CAL_KNOWN_MPH)) gauge_ui_toast("CALIBRATING");
  else                                              gauge_ui_toast("NO SIGNAL");
}

// ---- 50 ms tick: source -> gauge, diag values, page gestures ---------------
void gauge_tick(lv_timer_t *t) {
  float spd, hz = 0.0f, dpp = speedo.distPerPulse();
  uint32_t pulses = speedo.pulses();

  if (g_demo) {
    vehicle.update();
    spd = vehicle.data.speed_mph;
  } else {
    speedo.update();
    spd = speedo.mph();
    hz  = speedo.hz();
    // Report the outcome of a running calibration once it finishes.
    int cal = speedo.takeCalResult();
    if      (cal > 0) gauge_ui_toast("CAL OK");
    else if (cal < 0) gauge_ui_toast("NO SIGNAL");
  }

  vehicle_data.speed_mph = spd;
  gauge_ui_update(vehicle_data);
  gauge_ui_set_diag(spd, hz, pulses, g_demo, dpp);

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
  gauge_ui_set_calibrate_cb(on_calibrate);
  gauge_ui_set_demo_cb(on_demo_toggle);

  vehicle.begin();
  speedo.begin();

  lv_timer_create(gauge_tick, 50, NULL);
}

void loop() {
  Lvgl_Loop();
  delay(5);
}
