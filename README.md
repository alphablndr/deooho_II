# The Ooho II - Teensy 4.1 Firmware

Firmware and bench-test sketches for **The Ooho II**, a portable sound-art device shaped as an ear. The device records whispered voice in public space and stores each recording as a local WAV file on microSD. The current firmware is optimized for reliable field recording: audio is captured into PSRAM first, then written to SD only after recording stops.

Current uploaded build: **classic two-button mode** for the old ear retrofit.

```text
Record button on pin 32:
  hold = record
  release = stop + save WAV

Playback button on pin 30:
  press = play latest Wxxxx.WAV
```

## Current Architecture

```text
MAX4466 mic preamp
  -> Teensy Audio Shield Rev D LINE IN L
  -> SGTL5000 / Teensy Audio Library
  -> PSRAM buffer on Teensy 4.1
  -> built-in Teensy 4.1 microSD slot after stop
  -> WAV files W0001.WAV, W0002.WAV, ...
```

Playback/output uses:

```text
Audio Shield LINE OUT L/R/G
  -> mono amp or stereo amp
  -> speaker / surface transducer
```

The important design decision is **RAM-first recording**. Earlier live-SD recording produced regular ticking clicks in the recorded WAV. A RAM-only test was clean, which confirmed that SD writes during recording were contaminating the analog input path. With PSRAM installed, SD writes happen only after recording stops.

## Hardware

Core parts:

- `Teensy 4.1`
- `Teensy Audio Shield Rev D`
- `APS6404L-3SQR 8 MB PSRAM` soldered to the small PSRAM pads on the underside of Teensy 4.1
- `Adafruit MAX4466` electret microphone amplifier
- microSD card in the **built-in Teensy 4.1 SD slot**
- record momentary button on pin `32`
- playback momentary button on pin `30`
- optional status LED
- amplifier and speaker / surface transducer

Classic two-button wiring:

```text
Record button switch:
  one side -> Teensy pin 32
  other side -> GND

Playback button switch:
  one side -> Teensy pin 30
  other side -> GND

Teensy pin 31 -> 220-1000 ohm resistor -> LED anode (+)
LED cathode (-) -> GND
```

Use `INPUT_PULLUP` for both buttons. Pressed = `LOW`, released = `HIGH`.

Illuminated button pinout used in the old ear:

```text
1/2 = one side of the switch
3/4 = other side of the switch
5/6 = internal LED
```

For the switch, use one pin from the `1/2` side and one pin from the `3/4` side. For the LED, use a Teensy output pin through a resistor; if it does not light, swap `5` and `6`.

Microphone:

```text
MAX4466 VCC -> Teensy / Audio Shield 3.3V
MAX4466 GND -> Audio Shield LINE IN G / common GND
MAX4466 OUT -> Audio Shield LINE IN L
```

For lower noise, keep `MAX4466 OUT + GND` as a short twisted pair. Useful analog cleanup:

```text
0.1uF ceramic + 10uF-100uF bulk capacitor across MAX4466 VCC/GND
optional 1k series resistor from MAX4466 OUT to LINE IN L
optional 470pF-1nF capacitor from LINE IN L to GND
```

Mono amp example:

```text
Audio Shield LINE OUT L -> PAM8302A Audio In +
Audio Shield LINE OUT G -> PAM8302A Audio In -
PAM8302A 2-5VDC        -> Teensy VIN / 5V supply
PAM8302A GND           -> common GND
PAM8302A Shutdown      -> PAM8302A 2-5VDC
PAM8302A OUT + / -     -> 4-8 ohm speaker or exciter
```

Stereo amp / split-output idea:

```text
Audio Shield LINE OUT L -> stereo amp jack tip
Audio Shield LINE OUT R -> stereo amp jack ring
Audio Shield LINE OUT G -> stereo amp jack sleeve
```

Do not use `VGND` from the Audio Shield headphone jack as normal ground. For an external amplifier, use the `LINE OUT L/G/R` pads, not the headphone mini-jack output.

## Firmware Sketches

### `the_ooho_rev1/the_ooho_rev1.ino`

Main firmware for recording and latest-file playback.

Behavior:

- Record button on pin `32` starts recording while held.
- Releasing record button stops recording and saves the WAV.
- Playback button on pin `30` plays the latest saved `Wxxxx.WAV`.
- LED is on while recording.
- Audio is saved from PSRAM to SD as `T0001.WAV`, then renamed to `W0001.WAV`.
- LED is off when idle/saving/playback.
- On boot, firmware scans the SD card and remembers the latest existing `Wxxxx.WAV` for playback.

Serial commands for bench debugging:

```text
r  start/stop recording
p  play last saved WAV
s  stop playback
t  diagnostic tone
```

Memory behavior:

- With PSRAM detected: up to 60 seconds at 44.1 kHz mono 16-bit.
- Without PSRAM: 3 second internal RAM fallback.
- `AudioMemory(200)` provides extra queue slack.
- Global Teensy `AUDIO_BLOCK_SAMPLES` is not modified.

### `psram_memtest/psram_memtest.ino`

Minimal PSRAM verification sketch.

Expected output with one 8 MB chip:

```text
external_psram_size = 8 MB
zeros: PASS
ones: PASS
walking: PASS
PSRAM TEST PASS
```

Run this after soldering PSRAM and before using the audio firmware.

### `button_led_test/button_led_test.ino`

Hardware test for the trigger button and status LED.

```text
button pressed  -> built-in LED and external LED on
button released -> LEDs off
```

### `two_button_test/two_button_test.ino`

Hardware test for two illuminated buttons.

```text
Button A switch -> pin 32 + GND
Button B switch -> pin 30 + GND
Optional LED A  -> pin 31 through resistor
Optional LED B  -> pin 29 through resistor
```

The built-in Teensy LED turns on when either button is pressed. The optional external LEDs turn on independently for each button.

### `tone_button_test/tone_button_test.ino`

Transducer / amplifier test.

```text
button pressed  -> 440 Hz sine tone on Audio Shield L/R outputs
button released -> silence
```

This is useful for testing speakers, exciters, amplifier gain, resonance, and mechanical mounting without SD or microphone recording.

## Arduino / Teensyduino Settings

Use Arduino IDE with Teensyduino installed.

```text
Board: Teensy 4.1
USB Type: Serial
CPU Speed: default is fine
Port: the detected Teensy serial/USB port
```

Libraries used from Teensyduino:

```text
Audio
SD
SdFat
SPI
Wire
SerialFlash
```

Compile from CLI, if Arduino CLI is installed:

```sh
arduino-cli compile --fqbn teensy:avr:teensy41 the_ooho_rev1
```

Upload example:

```sh
arduino-cli upload -p usb:2132000 --fqbn teensy:avr:teensy41 the_ooho_rev1
```

If upload fails with `Unable find Teensy Loader`, open the Teensy Loader app from the local Teensy package and repeat upload.

## Field Notes

Use the built-in Teensy 4.1 SD slot, not the Audio Shield SD slot. The built-in slot is native SDIO and is faster than the SPI slot on the shield.

Use short filenames:

```text
W0001.WAV
W0002.WAV
T0001.WAV temporary while saving
```

The SD card should be formatted FAT32 for first field tests. Endurance cards are preferred for repeated writes.

Noise lessons learned:

- The earlier ticking artifact was present inside the WAV file, not just in playback.
- Internal sine tone playback was clean, so the output amp/speaker chain was not the cause.
- Powerbank recording did not remove the ticking.
- RAM-first recording removed the ticking.
- Conclusion: do not write to SD during recording.

## Research Notes

The next artistic/technical direction is documented in:

- [`docs/collective_voice_research.md`](docs/collective_voice_research.md) - research map for the sealed metal cube, internal archive speaker, external surface exciter, and the "collective voice" model.
- [`docs/collective_voice_implementation_plan.md`](docs/collective_voice_implementation_plan.md) - implementation plan for Collective Breath + Playback v1.
- [`docs/project_knowledge.md`](docs/project_knowledge.md) - consolidated current-state notes: hardware, wiring, firmware behavior, risks, and next steps.

## Safety / Wiring Warnings

- Do not connect class-D speaker output `-` to GND.
- Do not power the speaker amplifier from Teensy `3.3V`.
- Do not use Teensy `USB HOST 5V` as the amplifier supply.
- Do not connect MAX4466 output to the Audio Shield `MIC` input; use `LINE IN L`.
- Do not use `VGND` as normal ground.
- Start all amplifier tests at low volume.

## Repository Layout

```text
.
├── README.md
├── docs/
│   ├── collective_voice_implementation_plan.md
│   ├── collective_voice_research.md
│   └── project_knowledge.md
├── the_ooho_rev1/
│   └── the_ooho_rev1.ino
├── psram_memtest/
│   └── psram_memtest.ino
├── button_led_test/
│   └── button_led_test.ino
├── two_button_test/
│   └── two_button_test.ino
└── tone_button_test/
    └── tone_button_test.ino
```
