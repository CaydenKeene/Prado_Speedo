#pragma once
// vehicle_data.h
// Mock vehicle data source for developing the Prado gauge UI on the bench
// (no car wiring required). Produces a believable, repeating ~60s drive cycle.
//
// USAGE:
//   #include "vehicle_data.h"
//   MockVehicle vehicle;
//   ... in setup():            vehicle.begin();
//   ... in an lv_timer (50ms): vehicle.update();
//   ... read fields:           vehicle.data.speed_mph, etc.
//
// When the car is wired up, keep this struct and replace MockVehicle::update()
// with a real source (PCNT for speed/RPM, QMI8658 for pitch/roll, ADS1115 for
// volts/temp/fuel). The UI only ever touches VehicleData, so nothing else changes.

#include <Arduino.h>
#include <math.h>

struct VehicleData {
  float    speed_mph   = 0.0f;   // road speed
  float    rpm         = 0.0f;   // engine rpm
  float    coolant_f   = 90.0f;  // coolant temp, deg F
  float    battery_v   = 12.5f;  // system voltage
  float    fuel_pct    = 78.0f;  // tank level, 0-100
  float    pitch_deg   = 0.0f;   // nose up(+)/down(-)  -> inclinometer
  float    roll_deg    = 0.0f;   // lean right(+)/left(-)
  float    accel_g     = 0.0f;   // longitudinal g (+accel / -brake)
  float    lateral_g   = 0.0f;   // cornering g
  float    trip_miles  = 0.0f;   // accumulated trip distance
  float    max_mph     = 0.0f;   // session max speed
  bool     engine_on   = true;
};

class MockVehicle {
public:
  VehicleData data;

  void begin() {
    _t0   = millis();
    _last = _t0;
    randomSeed(_t0 ^ analogRead(0));
  }

  // Call ~20x/sec from an lv_timer.
  void update() {
    const uint32_t now = millis();
    float dt = (now - _last) / 1000.0f;
    if (dt <= 0.0f) return;
    if (dt > 0.25f) dt = 0.25f;          // guard against long stalls
    _last = now;

    const float t  = (now - _t0) / 1000.0f;   // seconds since boot
    const float tt = fmodf(t, 60.0f);         // 60s drive-cycle phase

    // ---- continuously varying target so the readout is ALWAYS moving ----
    // Several out-of-phase sines never settle on a constant and never park at 0:
    // sweeps roughly 8..96 mph with both fast wiggles and slow swells.
    (void)tt;
    float target = 52.0f
                 + 30.0f * sinf(t * 0.21f)
                 + 12.0f * sinf(t * 0.67f)
                 +  6.0f * sinf(t * 1.70f);
    if (target < 3.0f) target = 3.0f;

    // low-pass toward target -> smooth, natural accel/decel
    const float prev = data.speed_mph;
    data.speed_mph += (target - data.speed_mph) * fminf(dt * 0.9f, 1.0f);
    if (data.speed_mph < 0.05f) data.speed_mph = 0.0f;
    if (data.speed_mph > data.max_mph) data.max_mph = data.speed_mph;

    // ---- longitudinal g from speed change (mph/s -> g) ----
    const float dv_mph = (data.speed_mph - prev) / dt;
    data.accel_g = (dv_mph * 0.447f) / 9.81f + noise(0.01f);

    // ---- lateral g: gentle cornering while moving ----
    data.lateral_g = (data.speed_mph > 10.0f)
                       ? 0.30f * sinf(t * 0.8f) + noise(0.01f)
                       : noise(0.01f);

    // ---- rpm: piecewise gears, idle when stopped (gives a tach that sweeps
    //      and drops on shifts, like the real thing) ----
    if (data.speed_mph < 2.0f) {
      data.rpm = 750.0f + noise(25.0f);
    } else {
      static const float topMph[5] = { 12, 25, 40, 55, 85 };
      static const float botMph[5] = {  0, 12, 25, 40, 55 };
      int g = 4;
      for (int i = 0; i < 5; ++i) {
        if (data.speed_mph < topMph[i]) { g = i; break; }
      }
      const float frac = (data.speed_mph - botMph[g]) /
                         (topMph[g] - botMph[g] + 0.001f);
      data.rpm = 1200.0f + frac * 2200.0f + noise(30.0f);   // 1200..3400
    }

    // ---- coolant warm-up curve, then hold ~195F ----
    data.coolant_f = 195.0f - 105.0f * expf(-t / 15.0f) + noise(0.6f);

    // ---- charging voltage (engine running) ----
    data.battery_v = 14.1f + 0.10f * sinf(t * 0.3f) + noise(0.03f);
    data.engine_on = true;

    // ---- fuel slowly drains with distance ----
    data.fuel_pct -= data.speed_mph * dt * 0.0008f;
    if (data.fuel_pct < 0.0f) data.fuel_pct = 78.0f;   // "refuel" on loop

    // ---- terrain pitch/roll for the inclinometer page ----
    // Off-road amplitudes (was 6/4 deg, too subtle to read on screen).
    data.pitch_deg = 18.0f * sinf(t * 0.30f) + noise(0.3f);
    data.roll_deg  = 14.0f * sinf(t * 0.22f) + noise(0.3f);

    // ---- trip distance integration ----
    data.trip_miles += data.speed_mph * (dt / 3600.0f);
  }

private:
  uint32_t _t0   = 0;
  uint32_t _last = 0;
  static float noise(float amp) {
    return (random(-1000, 1001) / 1000.0f) * amp;
  }
};
