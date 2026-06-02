# The Ooho II - Project Knowledge

This document preserves the current practical knowledge about the build so it does not only live in chat.

## Current Direction

The Ooho II is a sound-art device built around a listening/recording ear and a sealed metal body.

Current practical branches:

1. **Old ear retrofit**: replace the unstable AI Thinker ESP32 AudioKit stack with Teensy 4.1 + Teensy Audio Shield.
2. **Sealed cube version**: one public record gesture, internal archive voices, and a surface-exciter layer that becomes a collective anonymous voice.

The working technical baseline is:

```text
Teensy 4.1 + Audio Shield Rev D
MAX4466 electret preamp -> LINE IN L
PSRAM-first recording
built-in Teensy 4.1 SD slot
WAV files on microSD
line out -> external amplifier -> speaker / surface exciter
```

Do not return to live SD recording during capture. It produced regular ticking clicks in the recorded WAV. RAM/PSRAM-first recording fixed the issue.

## Hardware State

Core electronics:

- Teensy 4.1
- Teensy Audio Shield Rev D
- 8 MB PSRAM soldered to the small PSRAM pads on Teensy 4.1
- Adafruit MAX4466 microphone amplifier
- microSD card in the built-in Teensy 4.1 SD slot
- external amplifier
- speaker or surface exciter
- momentary buttons
- optional LED indicators

Known-good PSRAM test output:

```text
zeros: PASS
ones: PASS
walking: PASS
PSRAM TEST PASS
```

## Current Uploaded Firmware

The currently uploaded firmware is `the_ooho_rev1/the_ooho_rev1.ino` in classic two-button mode.

Behavior:

```text
Record button on pin 32:
  hold = record into PSRAM
  release = stop and save Wxxxx.WAV

Playback button on pin 30:
  press = play latest Wxxxx.WAV

Status LED on pin 31:
  on only while recording
```

On boot, the firmware scans the SD card for the latest `Wxxxx.WAV`, so playback still works after power cycling if files are present.

Serial debug commands:

```text
r = start/stop recording
p = play last saved WAV
s = stop playback or stop recording
t = diagnostic tone
```

## Old Ear Button Wiring

The illuminated buttons have a separate switch and LED.

Button pinout from the datasheet:

```text
1/2 = one side of switch
3/4 = other side of switch
5/6 = LED
```

Use one pin from the `1/2` side and one pin from the `3/4` side for the switch.

Classic wiring:

```text
Record button switch:
  one switch side -> Teensy pin 32
  other switch side -> GND

Playback button switch:
  one switch side -> Teensy pin 30
  other switch side -> GND
```

The firmware uses `INPUT_PULLUP`, so no external 3.3 V pullup is needed for the switch. Pressed reads `LOW`.

Optional button LEDs:

```text
Record LED:
  Teensy pin 31 -> 330R-1k resistor -> LED pin 5
  LED pin 6 -> GND

Second button LED in the two-button test:
  Teensy pin 29 -> 330R-1k resistor -> LED pin 5
  LED pin 6 -> GND
```

If an LED does not light, swap LED pins `5` and `6`.

## Audio Wiring

Microphone:

```text
MAX4466 VCC -> 3.3V
MAX4466 GND -> Audio Shield LINE IN G / common GND
MAX4466 OUT -> Audio Shield LINE IN L
```

Do not connect MAX4466 output to the Audio Shield `MIC` input. MAX4466 is already a preamp, so use `LINE IN L`.

Mono amplifier example:

```text
Audio Shield LINE OUT L -> amp audio input +
Audio Shield LINE OUT G -> amp audio input -
amp supply +            -> 5V/VIN power rail
amp supply GND          -> common GND
amp speaker + / -       -> speaker or exciter terminals
```

For class-D BTL amplifier outputs, do not connect speaker `-` to GND.

Stereo amplifier / split output:

```text
LINE OUT L -> stereo input tip
LINE OUT R -> stereo input ring
LINE OUT G -> stereo input sleeve
```

Use `LINE OUT L/G/R`, not `VGND` from the headphone output, for external amplifiers.

## Sketches

- `the_ooho_rev1/the_ooho_rev1.ino`: current PSRAM-first firmware, currently in classic two-button mode.
- `psram_memtest/psram_memtest.ino`: verifies soldered PSRAM.
- `button_led_test/button_led_test.ino`: one-button LED test on pin 32.
- `two_button_test/two_button_test.ino`: two-button test on pins 32 and 30.
- `tone_button_test/tone_button_test.ino`: hold button to send 440 Hz tone to audio outputs.

## Known Risks

- SD writes during recording can contaminate the mic recording with ticking artifacts.
- Long unshielded MAX4466 output wiring can pick up digital or amplifier noise.
- Cheap powerbanks may auto-shut off at low current draw.
- The surface exciter can sound harsh on metal without mechanical damping and conservative filtering.
- Button LEDs need resistors; do not connect an LED directly to a Teensy output.
- Teensy inputs are 3.3 V logic; do not feed 5 V into input pins.

## Next Firmware Direction

The next artistic firmware branch is **Collective Breath + Playback v1**:

```text
after each recording:
  save raw Wxxxx.WAV
  analyze PSRAM buffer
  update MODEL.BIN
  append LOG.CSV
  render COLLECT.WAV

idle:
  left/internal speaker = real archived voices
  right/surface exciter = slowly changing collective voice
```

See `docs/collective_voice_implementation_plan.md` for the implementation plan.
