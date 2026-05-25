# The Ooho II - Teensy 4.1 Firmware

Firmware and bench-test sketches for **The Ooho II**, a portable sound-art device shaped as an ear. The device records whispered voice in public space and stores each recording as a local WAV file on microSD. The current firmware is optimized for reliable field recording: audio is captured into PSRAM first, then written to SD only after recording stops.

## Current Architecture

```text
MAX4466 mic preamp
  -> Teensy Audio Shield Rev D LINE IN L
  -> SGTL5000 / Teensy Audio Library
  -> PSRAM buffer on Teensy 4.1
  -> built-in Teensy 4.1 microSD slot after stop
  -> WAV files W0001.WAV, W0002.WAV, ...
```

Playback/output tests use:

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
- momentary button
- optional status LED
- amplifier and speaker / surface transducer

Button and LED:

```text
Button contact A -> Teensy pin 32
Button contact B -> GND

Teensy pin 31 -> 220-1000 ohm resistor -> LED anode (+)
LED cathode (-) -> GND
```

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

Main firmware for recording.

Behavior:

- Button press starts recording.
- LED is on while recording.
- Second button press stops recording.
- Audio is saved from PSRAM to SD as `T0001.WAV`, then renamed to `W0001.WAV`.
- LED is off when idle/saving/playback.
- No automatic playback after recording in the current build.

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
├── the_ooho_rev1/
│   └── the_ooho_rev1.ino
├── psram_memtest/
│   └── psram_memtest.ino
├── button_led_test/
│   └── button_led_test.ino
└── tone_button_test/
    └── tone_button_test.ino
```
