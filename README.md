# Prado Speedo

A minimal digital gauge cluster (speedometer) for a 1996 Toyota Land Cruiser
Prado (J90), running on a Waveshare **ESP32-S3-Touch-LCD-1.85** round display.

It reads real road speed from the car's SPD ("pink") wire, shows a clean
"STEALTH" speedo, and includes a swipe-up diagnostics page with a live signal
readout, a demo toggle, and a calibration button.

## Hardware

- **Board:** [Waveshare ESP32-S3-Touch-LCD-1.85](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85)
  (ST77916 360×360 round QSPI display, CST816 touch, TCA9554 IO expander, QMI8658 IMU)
- **Speed input:** the car's SPD ("pink") wire — a square wave whose *frequency*
  is proportional to road speed — read on **GPIO44** (UART-header RX pad).
- **Isolation:** a **PC817 optocoupler module** between the 12 V car side and the
  ESP32. This build uses a [1-channel PC817 isolation module](https://www.amazon.com/dp/B0DD3KBQV5)
  (screw terminals: `+ / −` on the input, `VCC / OUT / GND` on the output).

> Full board documentation, pinouts, and driver references:
> **https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85**

### Wiring

Two wires come from behind the radio: the **SPD ("pink") speed wire** and a
**red switched 12 V** wire.

**Input side (car, behind the radio):**

| Optocoupler input | Wire |
|---|---|
| `+` (positive) | red **12 V** wire |
| `−` (negative) | **SPD (pink)** speed wire |

**Output side (ESP32):**

| Optocoupler output | ESP32 pin |
|---|---|
| `VCC` | **3V3** |
| `GND` | **GND** |
| `OUT` | **GPIO44** (UART-header RX pad) |

> ⚠️ Power the output side from the ESP32's **3.3 V only** — the module does not
> step the signal down on its own; whatever you feed `VCC` is the logic-high
> level, and the ESP32-S3 is **not** 5 V tolerant. The opto also **inverts** the
> signal, which the firmware accounts for (it triggers on the falling edge).

## Repository layout

```
Prado_Speedo/
├── Code/            # Arduino sketch — open Code.ino here
│   ├── Code.ino
│   └── *.cpp / *.h / *.c   (display, touch, LVGL, speed input, gauge UI)
├── Libraries/       # third-party libraries this sketch depends on
└── Prints/          # 3D-printable mounts (STL)
```

## Building & flashing

1. **Install the libraries.** Copy the contents of this repo's **`Libraries/`**
   folder into your Arduino libraries folder:

   ```
   Documents/Arduino/libraries/
   ```

   (On Windows that's `C:\Users\<you>\Documents\Arduino\libraries\`.) Each
   library should end up as its own subfolder there. Restart the Arduino IDE
   afterward so it picks them up.

2. **Install the ESP32 core.** In the Arduino IDE Boards Manager, install
   **esp32 by Espressif** (developed against core **3.3.10**).

3. **Open the sketch:** `Code/Code.ino`.

4. **Board settings** (Tools menu):
   - Board: **ESP32S3 Dev Module**
   - **USB CDC On Boot: Enabled** — required, so the serial console stays on
     native USB and frees GPIO43/44 for the speed input.
   - PSRAM: **OPI PSRAM**

5. **Upload.**

## Using it

- **Gauge page:** big speed number + ring (full at 100 mph).
- **Diagnostics page:** **swipe up** to reveal it, **swipe down** to return.
  Shows live speed, pulse rate, edge count, and the current calibration
  constant, plus a **DEMO** toggle and a **CALIBRATE** button.

### Calibration

Speed is derived from pulse timing, so the pulses-per-mile factor must be
learned once against a known speed:

1. On the diagnostics page, turn **DEMO off**.
2. Drive a steady **40 mph** (use your phone's GPS to confirm).
3. Tap **CALIBRATE**. It averages the signal over a couple of seconds and
   saves the factor to flash, so it persists across reboots.

> The known calibration speed (`SPEED_CAL_KNOWN_MPH`) and all anti-jitter
> filter settings are named constants at the top of `speed_input.h` /
> `gauge_ui.h` if you want to tune them.

## Notes

- The board starts in **demo mode** so the gauge animates on the bench without a
  live signal; flip DEMO off once wired to the car.
