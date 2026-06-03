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

## Current Bring-Up State

The project is currently being brought up in small sketches instead of debugging the full LOGOS/MELOS firmware at once.

Confirmed hardware fact:

```text
Record button on pin 32:
  pressed = LOW
  released = HIGH

Confirmed by:
  button_led_test
  pin_scan_test
```

Current bring-up order:

```text
button_led_test   -> pin 32 button
tone_button_test  -> Audio Shield output / amp / speaker
sd_smoke_test     -> built-in Teensy SD slot
psram_memtest     -> soldered PSRAM
record_raw_test   -> PSRAM-first WAV recording, playback, GRAINS.CSV
logos_one_stream_test -> manual LOGOS grain playback, left channel
melos_short_test  -> short MELOS render/playback, right channel
ooho_combined_test -> current stable integrated Stage 8 baseline
the_ooho_rev1     -> full experimental LOGOS/MELOS firmware
```

`record_raw_test` is the safest next integrated test. It records into PSRAM/RAM only while the button is held, saves `Wxxxx.WAV` after release, appends LOGOS candidates to `GRAINS.CSV`, and supports Serial playback:

```text
r = start/stop recording
p = play latest full WAV
s = stop playback or stop recording
```

The full `the_ooho_rev1/the_ooho_rev1.ino` is the experimental artistic firmware. It currently keeps `MELOS_SECONDS = 30` for debugging; the intended field value is 480 seconds only after the rest of the pipeline is reliable.

## Current Stable Firmware Baseline

The current on-device baseline is:

```text
ooho_combined_test/ooho_combined_test.ino
```

This is the stable Stage 8 lab build, not the final field build.

Behavior:

- public button on pin `32`;
- hold = record to PSRAM;
- release = process;
- SD writes happen only after release;
- playback continues during recording;
- after release, raw WAV is saved, LOGOS grains are appended, and `MODEL.BIN` is updated;
- MELOS render is deferred while playback is active, to avoid crunch/glitch;
- one LOGOS stream plays real grains on left;
- MELOS v2 soft plays on right;
- `MELOS_SECONDS = 30` for testing.

Important current constants:

```text
MELOS_GAIN = 0.48
LOGOS_STREAM_GAIN = 0.28
LOGOS_WINDOW_PEAK_FACTOR = 0.18
LOGOS_MAX_RMS_THRESHOLD = 0.090
AUDIO_FEED_MIN_US = 2300
```

The `AUDIO_FEED_MIN_US` value matters. A test value of `3200 us` caused rhythmic modulation/starvation across the audio output. `2300 us` restored normal playback.

Recent confirmed recordings:

```text
W0023.WAV peak=0.92508, LOGOS grains added: 3
W0024.WAV LOGOS grains added: 6
W0025.WAV peak=0.79196, LOGOS grains added: 3
W0027.WAV LOGOS grains added: 1
```

`GRAINS.CSV` reached 70+ rows during bring-up, and fresh recordings appeared in later LOGOS playback.

## MELOS Current Behavior

MELOS is not random playback. It is synthesized from `MODEL.BIN`.

After a recording, the firmware analyzes the PSRAM buffer:

- estimates median F0 with autocorrelation;
- updates mean log F0;
- updates voiced/noise ratio;
- updates 8 broad spectral bands;
- updates a 64-bin envelope;
- increments `voiceCount`.

The audible MELOS file is a rendered mono WAV:

```text
MELOS.WAV
44.1 kHz, 16-bit mono
manual FsFile read -> AudioPlayQueue -> right channel
```

The current approved MELOS tone is **MELOS v2 soft**:

- recognizable pitch center;
- soft sine/harmonic body rather than sharp saw;
- slight breath/noise;
- slow random-walk pitch drift;
- smoothed envelope;
- conservative gain.

Because continuous playback is currently prioritized, the model updates immediately after each recording, but `MELOS.WAV` is not automatically regenerated while the existing MELOS is playing. Serial reports:

```text
MELOS render deferred to keep playback stable.
```

Manual Serial command `e` can render MELOS during bench testing, but this may interrupt or destabilize playback. Use it only intentionally.

## Failed / Deferred Experiments

### Same-card MELOS double-buffer

Tried:

```text
render MNEXT.WAV while MELOS.WAV keeps playing
swap or crossfade to MNEXT.WAV later
```

Observed:

- audible crunch/glitch for roughly 10-15 seconds after release;
- rhythmic/strange modulation across both LOGOS and MELOS;
- one version caused the loop to stop responding to Serial.

Likely cause:

- same SD card is being used for playback reads, raw/model/grain writes, and MELOS render writes;
- AudioPlayQueue can starve or stall if feed timing is wrong;
- rendering while also reading LOGOS/MELOS from the same card creates too much contention.

Decision:

- same-card double-buffer/crossfade is disabled for now;
- delete/ignore `MNEXT.WAV` and `MNEXT.TMP` on boot in the stable build;
- do not re-enable without a more deliberate storage/audio plan.

Possible future solution:

- use two SD cards: built-in Teensy SD for archive/model writes, Audio Shield SPI SD for MELOS playback;
- or implement a simpler offline MELOS update during scheduled quiet windows;
- or accept a short intentional MELOS update pause after processing.

## Old Ear Button Wiring

The illuminated buttons have a separate switch and LED.

Button pinout from the datasheet:

```text
1/2 = one side of switch
3/4 = other side of switch
5/6 = LED
```

Use one pin from the `1/2` side and one pin from the `3/4` side for the switch.

Current one-button wiring:

```text
Record button switch:
  one switch side -> Teensy pin 32
  other switch side -> GND
```

Old two-button/bench wiring:

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

- `the_ooho_rev1/the_ooho_rev1.ino`: full experimental LOGOS/MELOS firmware.
- `record_raw_test/record_raw_test.ino`: integrated recording bring-up without MELOS/model/idle playback.
- `sd_smoke_test/sd_smoke_test.ino`: built-in SD slot write/read/delete test.
- `psram_memtest/psram_memtest.ino`: verifies soldered PSRAM.
- `button_led_test/button_led_test.ino`: one-button LED test on pin 32.
- `pin_scan_test/pin_scan_test.ino`: scans likely button pins and confirmed pin 32.
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

The full artistic firmware branch is **LOGOS/MELOS Field v1**:

```text
after each recording:
  save raw Wxxxx.WAV
  analyze PSRAM buffer
  update MODEL.BIN
  append GRAINS.CSV
  render MELOS.WAV

idle:
  left/internal speaker = LOGOS real word grains
  right/surface exciter = MELOS synthesized collective voice
```

See `docs/bringup_pipeline.md` before using the full firmware, and `docs/collective_voice_implementation_plan.md` for the artistic implementation plan.
