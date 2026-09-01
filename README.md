# Prado Speedo

A minimal digital gauge cluster (speedometer) for a 1996 Toyota Land Cruiser
Prado (J90), running on a Waveshare **ESP32-S3-Touch-LCD-1.85** round display.

It reads real road speed from a **serial GPS module**, shows a clean "STEALTH"
speedo, and includes a swipe-up diagnostics page with a live signal readout, a
demo toggle, and a GPS status button.

> **Note:** this build used to read the car's SPD ("pink") VSS wire through a
> PC817 optocoupler. That path is gone — the signal was too unreliable to get a
> clean reading from. GPS ground speed needs no opto and no calibration. The old
> driver is still in git history as `Code/speed_input.h` if you ever want it.

## Hardware

- **Board:** [Waveshare ESP32-S3-Touch-LCD-1.85](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85)
  (ST77916 360×360 round QSPI display, CST816 touch, TCA9554 IO expander, QMI8658 IMU)
- **Speed input:** an **ATGM336H-5N** GPS module (AT6558 chipset —
  GPS + BeiDou + GLONASS + Galileo) on the UART header, with an external
  **28 dB active puck antenna** on u.FL. The firmware parses raw NMEA itself,
  so there is no library to install, and it also speaks u-blox (UBX) and
  MediaTek (PMTK) config dialects — see `applyConfig()` in `gps_speed.h`.
- **Power:** the ESP32 is powered through its **USB-C port from a 12 V → USB
  cigarette-lighter adapter**. The GPS module runs off the board's 3V3 rail
  (ME6217C33M5G, 800 mA — the module draws ~25 mA plus ~10 mA for the antenna).

### Parts

| Part | Notes |
|---|---|
| ATGM336H-5N module | four constellations, backup cell for warm starts, u.FL |
| 28 dB active GPS antenna, SMA **male**, 3–5 V | magnetic puck, 3 m cable |
| u.FL → SMA **female** pigtail | skip if your module has SMA on board |

Multi-constellation matters more than update rate here: it is what holds a fix
under tree cover and overpasses, which is the only real weakness of a GPS
speedo. A backup cell matters too — without one, every ignition cycle is a cold
start and the gauge is blank for ~30 s.

> Full board documentation, pinouts, and driver references:
> **https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85**

### Wiring

Four wires, all on the ESP32 side — nothing taps into the car's loom any more.

| GPS module | ESP32 pin | Notes |
|---|---|---|
| `VCC` | **3V3** | most modules take 3.3–5 V; use 3V3 so its TX is 3.3 V logic |
| `GND` | **GND** | |
| `TX`  | **GPIO44** (UART-header RXD pad) | the data we read |
| `RX`  | **GPIO43** (UART-header TXD pad) | config commands we send — connect it |

> ⚠️ The ESP32-S3 is **not** 5 V tolerant. If you power the module from 5 V,
> check its datasheet — many have a 3.3 V regulator but still drive TX at 3.3 V,
> which is fine; anything driving a 5 V TX needs a divider or level shifter.

The module's `RX` line is technically optional — speed still reads without it —
but leaving it off costs you both of the things worth having: the fix rate stays
at the module's **1 Hz default** (sluggish on a speedo), and the receiver stays
on whatever constellations it booted with instead of the GPS + BeiDou + GLONASS
set the firmware asks for. Connect it.

**Antenna placement matters more than the module does.** A patch antenna is
directional — it receives out of its flat face, so it wants to lie flat with sky
above it, not stand vertically behind the screen. That is why this build uses an
external puck on the dash top rather than a module with the antenna on board.

> ⚠️ **An active antenna is powered by DC bias the module puts on its own
> antenna pin.** If a board does not supply it, the antenna is dead and you will
> see zero satellites forever. Verify before blaming the firmware: **~3 V
> between the u.FL centre pin and GND** with the board powered. If it reads 0 V,
> an inline bias-T injector fixes it.

Keep the puck ~10 cm away from any other GPS antenna on the dash (a head unit's,
for example) so the two LNAs do not desense each other, and keep the coax away
from the 12 V → USB adapter — cheap ones radiate broadband noise right across
the 1575 MHz GPS band.

## Repository layout

```
Prado_Speedo/
├── Code/            # Arduino sketch — open Code.ino here
│   ├── Code.ino
│   └── *.cpp / *.h / *.c   (display, touch, LVGL, GPS speed input, gauge UI)
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
     native USB and frees GPIO43/44 for the GPS.
   - PSRAM: **OPI PSRAM**

5. **Upload.**

## Using it

- **Gauge page:** big speed number + ring (full at 100 mph). The number is
  **greyed out until the GPS has a fix**, so a parked "0" is never mistaken for
  a real reading. (Set `GAUGE_DIM_ON_NO_FIX 0` in `gauge_ui.h` to turn that off.)
- **Diagnostics page:** **swipe up** to reveal it, **swipe down** to return.
  Shows live speed, fix rate, satellite count, and HDOP, plus a **DEMO** toggle
  and a **GPS INFO** button that reports link/fix state.

### First fix

A cold module can take **30–90 seconds** to find its first fix (and up to a few
minutes if it has been off for weeks or has moved a long way). After that,
warm starts are typically a few seconds. Watch the diagnostics page:

| What you see | Meaning |
|---|---|
| `sats 0`, `no fix`, `fix 0 Hz` | no NMEA arriving — check TX/RX wiring and power |
| `fix 1 Hz`, `sats 0–3`, `no fix` | module is talking and searching; give it time / better sky |
| `fix 1 Hz` forever, good sats | rate config was rejected — wrong command dialect for this module |
| `sats 0` forever, but `fix 1 Hz` | almost certainly the antenna bias problem above |
| `fix 10 Hz`, `sats 8+`, `hdop 1.x` | working normally |
| `holding` | fix lost; coasting on the last speed (see below) |

Tapping **GPS INFO** gives the same thing as a toast: `NO GPS`, `SEARCHING`,
`HOLDING`, or `FIX n SAT`.

### Calibration

**There isn't any.** GPS ground speed comes from satellite Doppler, so it is
inherently correct and unaffected by tyre size, gearing, or diff ratio — which
is the main reason for the switch away from the VSS wire.

### Tuning

All knobs are named constants at the top of `gps_speed.h`:

| Constant | Default | What it does |
|---|---|---|
| `GPS_RATE_HZ` | `10` | fix rate requested from the module |
| `GPS_BAUD_RUN` | `38400` | 10 Hz does not fit in 9600 baud |
| `GPS_ZERO_MPH` | `1.2` | below this, show a hard 0 (kills parked drift) |
| `GPS_EMA_ALPHA` | `0.50` | display smoothing — higher is snappier, lower is steadier |
| `GPS_STALE_MS` | `1500` | no fix for this long → stop trusting it, start coasting |
| `GPS_HOLD_MS` | `5000` | how long to coast on the last speed before showing 0 |

Display deadband and repaint rate still live in `gauge_ui.h`.

## Notes

- **DEMO mode** (swipe-up page) drives the gauge from a simulated drive cycle so
  it animates on the bench with no GPS fix. `DEMO_DEFAULT` in `Code.ino` sets the
  power-on state; it ships **off**.
- The firmware auto-detects the module's baud rate: it cycles through 9600 /
  38400 / 115200 until valid NMEA arrives, then configures the fix rate. So a
  module that has been reconfigured before will still be found.
- **Dropouts are handled in two stages.** Losing a fix does *not* immediately
  show 0 — that is the worst thing a speedo can do, since you notice it exactly
  when you pass under an overpass at speed. Past `GPS_STALE_MS` the number greys
  out but **holds the last speed** (the car is almost certainly still doing what
  it was doing); only after a further `GPS_HOLD_MS` does it fall to 0. The diag
  page reads `holding` during the coast.
- **GPS speed lags by roughly 0.2–1 s** and dies completely in long tunnels and
  parking garages. It is the right trade for this car, but it is a trade — do
  not treat the readout as instantaneous during hard braking.
- The board has a **QMI8658 IMU** that is currently unused. Integrating its
  longitudinal accelerometer during a dropout would extend the useful coast well
  past `GPS_HOLD_MS` — poor man's dead reckoning, for free.
