# HW2 – Light & Temperature Station

An event-driven Arduino Uno project that measures **temperature** (LM35) and **ambient light** (photoresistor), shows them on a 16x2 I2C LCD, and **controls a night light** that fades on when it gets dark. User settings (dark threshold and temperature calibration) are stored in **EEPROM** with validation and survive power cycles.

The firmware uses **no `delay()` and no blocking calls**. Interrupts only set flags; all logic runs in a main-loop state machine.

---

## Features

- Temperature measurement at 10 Hz with an 8-sample moving average
- Light level measurement (raw ADC and %)
- Night LED with **hysteresis** and smooth PWM fade
- Two-button menu driven by external interrupts (INT0, INT1)
- Hardware **Timer1** interrupt drives periodic sampling
- EEPROM settings with magic byte, version and checksum, **write-on-change**
- Feedback: LCD, night LED, heartbeat LED, piezo beeps, Serial debug output

---

## Hardware

| Part | Qty | Notes |
|---|---|---|
| Arduino Uno | 1 | ATmega328P, 16 MHz |
| LCD 1602 with I2C backpack (HW-61 / PCF8574) | 1 | |
| LM35 temperature sensor | 1 | 10 mV/°C, 0 °C = 0 V |
| Photoresistor (LDR) | 1 | |
| Resistor 10 kΩ | 1 | LDR voltage divider |
| Resistor 220 Ω | 2 | LED current limiting |
| LED | 2 | Night light + status |
| Push button (4-pin) | 2 | Wired diagonally, internal pull-up |
| Piezo buzzer | 1 | |

---

## Wiring / Pinout

| Arduino pin | Connected to | Function |
|---|---|---|
| A0 | LM35 Vout (middle pin) | Temperature input |
| A1 | LDR / 10 kΩ junction | Light input |
| A4 (SDA) | LCD backpack SDA | I2C data |
| A5 (SCL) | LCD backpack SCL | I2C clock |
| D2 (INT0) | Button 1 → GND | Mode / Save |
| D3 (INT1) | Button 2 → GND | Settings / Increment |
| D4 | Piezo (+), (−) → GND | Beeps |
| D5 (PWM) | 220 Ω → LED → GND | Night light |
| D13 | 220 Ω → LED → GND | Heartbeat (also on-board LED) |
| 5V / GND | LM35, LDR divider, LCD | Power |

### Schematic

```
                 +5V
                  │
        ┌─────────┼──────────────┬──────────────┐
        │         │              │              │
     LM35 +Vs    LDR          LCD VCC        (LM35 GND, LCD GND,
     LM35 Vout ─── A0          │              buttons, LEDs → GND)
     LM35 GND ── GND           ├──── A1
                              10 kΩ
                               │
                              GND

  D2 ──[BTN1]── GND        (INPUT_PULLUP, no external resistor)
  D3 ──[BTN2]── GND        (INPUT_PULLUP, no external resistor)
  D4 ──[PIEZO]── GND
  D5 ──[220 Ω]──[LED >|]── GND
  D13 ─[220 Ω]──[LED >|]── GND
  A4 ── SDA (LCD)   A5 ── SCL (LCD)
```

LM35 pinout: holding the flat side with the label facing you, legs down: **left = +Vs, middle = Vout, right = GND**.

### Safety notes

- **LED current:** (5 V − ~2 V) / 220 Ω ≈ 14 mA, below the 20 mA recommended per pin.
- **Buttons** use the internal pull-up (~20–50 kΩ), so the interrupt pins never float. Pressed = LOW, interrupts trigger on `FALLING`.
- **LM35 orientation:** a reversed LM35 gets hot quickly. If the reading is near 500 °C, check the wiring.
- **Total current** stays well under the Uno's limits (two LEDs, LCD backlight, sensors).

---

## Build & Upload

The project uses **PlatformIO** (tested in CLion).

```
.
├── platformio.ini
├── src/
│   └── main.cpp
└── README.md
```

`platformio.ini`:

```ini
[env:uno]
platform = atmelavr
board = uno
framework = arduino
monitor_speed = 9600
lib_deps =
    marcoschwartz/LiquidCrystal_I2C@^1.1.4
```

Steps:

1. Clone the repository and open it in CLion / VS Code with PlatformIO.
2. Set the LCD address in `src/main.cpp` (`0x27` or `0x3F`). If unsure, run an I2C scanner sketch.
3. Build: `pio run`
4. Upload: `pio run -t upload`
5. Serial monitor (optional): `pio device monitor`

`Wire` and `EEPROM` are part of the Arduino framework and need no installation.

---

## Usage

| Screen | Button 1 (D2) | Button 2 (D3) |
|---|---|---|
| Temperature | → Light screen | → Threshold setting |
| Light | → Temperature screen | → Threshold setting |
| Threshold | **Save** → Calibration | Threshold +25 (wraps 50–1000) |
| Calibration | **Save** → Temperature | Offset +0.5 °C (wraps −5.0…+5.0) |

- `*` on the LCD means the value was changed but not yet saved.
- High beep = saved to EEPROM. Short low beep = nothing changed, no EEPROM write.
- On boot the LCD shows `EEPROM OK` (valid settings loaded) or `EEPROM default` (invalid or empty, defaults written).

---

## Software Design

### Architecture

```
 Timer1 ISR (10 Hz) ──► sampleFlag ─┐
 INT0 ISR (button 1) ─► btn1Flag  ──┼──► loop(): state machine + millis() tasks
 INT1 ISR (button 2) ─► btn2Flag  ──┘
```

Main loop:

```cpp
void loop() {
    if (btn1Flag)   { btn1Flag = false;   handleBtn1(); }
    if (btn2Flag)   { btn2Flag = false;   handleBtn2(); }
    if (sampleFlag) { sampleFlag = false; readSensors(); updateNightTarget(); }
    updateBoot();       // BOOT -> SHOW_TEMP after 1.5 s
    updateFade();       // night LED fade, every 10 ms
    updateStatusLed();  // heartbeat, every 500 ms
    updateLcd();        // LCD refresh, every 300 ms
    updateSerial();     // debug line, every 1 s
}
```

### State machine

```
 BOOT ──(1.5 s)──► SHOW_TEMP ◄──BTN1──► SHOW_LIGHT
                      │                     │
                      └──BTN2──┐  ┌──BTN2───┘
                               ▼  ▼
                          SET_THRESHOLD ──BTN1 (save)──► CALIBRATE
                                                            │
                         SHOW_TEMP ◄────BTN1 (save)─────────┘
```

### Interrupt (ISR) roles

All ISRs are in the clearly marked `ISR SECTION` of `main.cpp`. They are kept minimal: no Serial, LCD, ADC or EEPROM access.

| ISR | Source | Trigger | Role |
|---|---|---|---|
| `TIMER1_COMPA_vect` | Timer1 compare match A | Every 100 ms | Sets `sampleFlag` |
| `isrBtn1` | INT0 (D2) | `FALLING` | Sets `btn1Flag`, 50 ms debounce via `millis()` |
| `isrBtn2` | INT1 (D3) | `FALLING` | Sets `btn2Flag`, 50 ms debounce via `millis()` |

Shared variables are declared `volatile`.

### Timer configuration

Timer1 in **CTC mode** generates a 10 Hz interrupt:

```
f_tick = 16 MHz / 1024 = 15 625 Hz
OCR1A  = 15 625 / 10 − 1 = 1561   →  ~10.0 Hz (100.03 ms)
```

| Register | Value | Meaning |
|---|---|---|
| `TCCR1A` | `0` | Normal port operation |
| `TCCR1B` | `WGM12` | CTC mode (TOP = OCR1A) |
| `TCCR1B` | `CS12 \| CS10` | Prescaler 1024 |
| `OCR1A` | `1561` | Compare value |
| `TIMSK1` | `OCIE1A` | Enable compare A interrupt |

Timer usage overview:

| Timer | Used by |
|---|---|
| Timer0 | `millis()`, `micros()`, PWM on D5 (night LED) |
| Timer1 | **10 Hz sampling interrupt** (PWM on D9/D10 unavailable) |
| Timer2 | `tone()` for the piezo |

### EEPROM layout

Settings are stored as one packed struct (7 bytes) starting at address 0.

| Address | Size | Field | Type | Default | Description |
|---|---|---|---|---|---|
| 0 | 1 B | `magic` | `uint8_t` | `0xA5` | Identifies this project's data |
| 1 | 1 B | `version` | `uint8_t` | `1` | Data format version |
| 2–3 | 2 B | `tempOffsetX10` | `int16_t` | `0` | Temperature calibration, 0.1 °C units |
| 4–5 | 2 B | `darkThreshold` | `uint16_t` | `400` | Night light threshold, ADC 0–1023 |
| 6 | 1 B | `checksum` | `uint8_t` | — | XOR of bytes 0–5 |

**Validation on boot:** settings are accepted only if `magic`, `version` and `checksum` match and the threshold is within range. Otherwise defaults are loaded and written once.

**Write-on-change:** values are written only when the user presses *Save* **and** a value actually changed (`dirty` flag). `EEPROM.put()` additionally rewrites only bytes that differ, which preserves the ~100 000 write-cycle endurance.

### Signal processing

- **Temperature (LM35):** `mV = ADC × 5000 / 1023`, and since 10 mV/°C, `temp × 10 = mV + offset`. Integer math only.
- **Light:** raw ADC average and `map()` to 0–100 %.
- **Averaging:** 8-sample circular buffer per channel (0.8 s window).
- **ADC channel switching:** the first reading after switching channels is discarded.
- **Night light hysteresis:** ON below `threshold − 30`, OFF above `threshold + 30`; PWM fades 5 steps every 10 ms (~0.5 s full fade).

---

## Timing Budget

Measured with `micros()` and printed on Serial (`sens_us`, `lcd_us`):

| Task | Period | Duration | CPU load |
|---|---|---|---|
| Sensor read + averaging | 100 ms | ~0.55 ms | ~0.6 % |
| LCD refresh (2 lines, I2C) | 300 ms | ~20.8 ms | ~6.9 % |
| LED fade, heartbeat, Serial | 10–1000 ms | < 0.1 ms | negligible |
| **Total** | | | **~8 %** |

---

## Testing

| Test | Expected result | Result |
|---|---|---|
| Boot screen | `Stotele HW2` + EEPROM status | YES |
| Temperature vs. reference thermometer | Within ±1 °C after calibration | NOT TESTED |
| Warming LM35 with fingers | Temperature rises within seconds | YES |
| Covering the LDR | Night LED fades on | YES |
| Uncovering the LDR | Night LED fades off, no flicker at threshold | YES |
| Button presses | One action per press, no double triggers | YES |
| Power cycle after saving | Threshold and offset retained, `EEPROM OK` | YES |
| Corrupted EEPROM (magic byte changed) | `EEPROM default`, defaults restored | NOT TESTED |

---

## Known Issues / Limitations

- **LCD refresh takes ~20 ms** because the I2C library sends each character separately with internal delays. Button events are not lost (captured by interrupts) but may be handled up to ~20 ms late.
- **ADC resolution:** with the 5 V reference one ADC step ≈ 0.49 °C. Averaging reduces noise but not this quantization. The 1.1 V internal reference would improve it, but would saturate the light channel on A1.
- **USB supply voltage varies** (≈4.8–5.1 V), which shifts temperature readings slightly; the calibration offset compensates.
- **LM35 range:** about 2 °C to 100+ °C with a single supply; it cannot measure below 0 °C in this circuit.
- **Light reading is not linear** in lux; it is a relative level only.
- Settings can only be increased (with wrap-around) because there are only two buttons.

---
