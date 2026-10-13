# The Ooho II - Teensy 4.1 Firmware

Firmware and bench-test sketches for **The Ooho II**, a portable sound-art device shaped as an ear. The device records whispered voice in public space and stores each recording as a local WAV file on microSD. The firmware direction is optimized for reliable field recording: audio is captured into PSRAM first, then written to SD only after recording stops.

Current practical workflow: **incremental bring-up builds**. Do not debug the full LOGOS/MELOS firmware until the simple hardware layers pass.

**Pre-exhibition work list (voicebody, Oct 2026):** [`docs/exhibition_todo.md`](docs/exhibition_todo.md).

Current stable integrated lab build:

```text
ooho_combined_test
```

This is the Stage 8 baseline. It records, saves WAV, updates `GRAINS.CSV`/`MODEL.BIN`, plays one LOGOS stream on the left, and plays MELOS v2 soft on the right. It keeps playback running during recording. MELOS re-render is currently deferred while playback is active, because same-card render/playback caused crunch and rhythmic modulation.

Latest confirmed hardware fact:

```text
Record button on pin 32:
  pressed = LOW
  released = HIGH
  verified by pin_scan_test/button_led_test
```

## Current Architecture Direction

```text
MAX4466 mic preamp
  -> Teensy Audio Shield Rev D LINE IN L
  -> SGTL5000 / Teensy Audio Library
  -> PSRAM buffer on Teensy 4.1
  -> built-in Teensy 4.1 microSD slot after stop
  -> WAV files W0001.WAV, W0002.WAV, ...
```

Final playback/output direction:

```text
Audio Shield LINE OUT L -> internal speaker       -> LOGOS real word grains
Audio Shield LINE OUT R -> surface exciter / cube -> MELOS synthesized collective voice
Audio Shield LINE OUT G -> amplifier ground / sleeve
```

The important design decision is **RAM-first recording**. Earlier live-SD recording produced regular ticking clicks in the recorded WAV. A RAM-only test was clean, which confirmed that SD writes during recording were contaminating the analog input path. With PSRAM installed, SD writes happen only after recording stops.

## Hardware

Core parts:

- `Teensy 4.1`
- `Teensy Audio Shield Rev D`
- `APS6404L-3SQR 8 MB PSRAM` soldered to the small PSRAM pads on the underside of Teensy 4.1
- `Adafruit MAX4466` electret microphone amplifier
- microSD card in the **built-in Teensy 4.1 SD slot**
- public record momentary button on pin `32`
- optional bench/debug playback button on pin `30` in old test sketches only
- optional status LED
- amplifier and speaker / surface transducer

Current one-button wiring:

```text
Record button switch:
  one side -> Teensy pin 32
  other side -> GND
```

Old two-button/bench wiring:

Playback button switch:
  one side -> Teensy pin 30
  other side -> GND

Teensy pin 31 -> 220-1000 ohm resistor -> LED anode (+)
LED cathode (-) -> GND
```

Use `INPUT_PULLUP` for buttons. Pressed = `LOW`, released = `HIGH`.

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

### Recommended Bring-Up Order

See [`docs/bringup_pipeline.md`](docs/bringup_pipeline.md) for the full checklist.

Use this order:

```text
button_led_test   -> verify pin 32 button
tone_button_test  -> verify Audio Shield output / amp / speaker
sd_smoke_test     -> verify built-in Teensy SD slot
psram_memtest     -> verify soldered PSRAM
record_raw_test   -> verify PSRAM-first recording, WAV save, serial playback, GRAINS.CSV
ooho_simple_random -> current simple two-button version: record on pin 32, random archive playback on pin 30
logos_one_stream_test -> verify manual LOGOS grain playback
melos_short_test  -> verify short MELOS render/playback
ooho_combined_test -> current stable integrated Stage 8 baseline
ooho_combined_emergent -> Claude Code branch: live MELOS synth + 4-stream LOGOS crowd
ooho_combined_voicebody -> MELOS rebuilt as a collective vocal body (source-formant-filter)
the_ooho_rev1     -> full experimental field firmware
```

The safest recording-only debug build is `record_raw_test`, because it records and saves real WAV files without MELOS/model/idle complexity. The safest integrated artistic debug build is currently `ooho_combined_test`.

### `ooho_simple_random/ooho_simple_random.ino`

Simple two-button OOHO version, separate from the cube/collective-voice firmware.

Behavior:

- Record button on pin `32`: hold to record, release to save `Wxxxx.WAV`.
- Playback button on pin `30`: press to choose one existing `Wxxxx.WAV` at random from the full archive and play it.
- Random playback does not repeat the same recording twice in a row when the archive has at least two files.
- WAV archive is mono. Playback sends the mono signal to both line-out channels so either output path can be used.
- Recording is still PSRAM-first; SD writes happen only after release.
- Post-record voice cleanup runs in RAM before saving: SGTL5000 ADC high-pass, firmware high-pass/low-pass, soft noise-floor reduction, light compression, soft limiting, and RMS/peak normalization.
- Bad takes are ignored instead of saved: shorter than `500 ms`, or too quiet (`rms < 0.006` and `peak < 0.025`).
- Playback has a separate gain stage (`PLAYBACK_GAIN = 1.35`) so archived voices come back a little louder without changing the recording path.
- No `GRAINS.CSV`, no `MODEL.BIN`, no `MELOS.WAV`, no LOGOS/MELOS processing.
- Optional LEDs: record/status on pin `31`, playback on pin `29`.

### `the_ooho_rev1/the_ooho_rev1.ino`

Full experimental LOGOS/MELOS firmware. This is the final artistic direction, not the first sketch to use during hardware bring-up.

Behavior:

- Record button on pin `32`: hold to record, release to process.
- Max record time: 30 seconds.
- Recording is PSRAM-first; SD writes happen only after release.
- Saves raw archive files as `Wxxxx.WAV`.
- Appends LOGOS word candidates to `GRAINS.CSV`.
- Updates `MODEL.BIN`.
- Renders `MELOS.WAV`.
- In idle, plays LOGOS grains on the left channel and MELOS on the right channel.

Current debug setting:

```text
MELOS_SECONDS = 30
```

Keep this short value until the whole pipeline is reliable. The intended field value is 480 seconds, but that creates a large WAV and slows every processing pass.

### `ooho_combined_voicebody/ooho_combined_voicebody.ino`

Forked from the waveset build. MELOS (right channel) is rebuilt as a **collective
vocal body** (design: [`docs/superpowers/specs/2026-06-03-melos-collective-voice-body-design.md`](docs/superpowers/specs/2026-06-03-melos-collective-voice-body-design.md)).
The LOGOS left channel is unchanged.

- **Source–formant-filter engine**, not additive partials: 6 glottal sources →
  one shared 3-formant "mouth" (TPT state-variable bandpass), vowel oo/aa/ee from
  `model.bands`, tone↔breath from `voicedRatio`.
- **Unison swarm → archive-bloomed cluster:** voices sit near one collective
  pitch (`meanLogF0`); harmonic width tracks the archive's F0 spread (new model
  stat `meanLogF0Sq`), with a ~20% pull toward unison/octave/fifth.
- **Breathing** amplitude arcs (smoothed, ~3 s glide — no stepping) instead of a
  sustained pad; post-recording "chew" gesture. The amplitude **sidechain was
  removed** (MELOS no longer pumps with LOGOS); LOGOS events still open/aerate
  the *vowel*, not the volume.
- **Rare intelligible words (LOGOS):** a grain on any stream occasionally plays
  as a whole forward, normal-speed word (`LOGOS_WORD_PERCENT`, currently 25);
  every other grain is forced into cloud/micro deconstruction, so clear words
  emerge from the texture instead of dominating.
- **Timbral movement (Pink-Trombone-inspired):** nasality (a 4th murmur
  resonator), wandering constriction ("almost-consonants"), vowel micro-wander,
  and sub-choirs (some voices darker, some brighter). Autonomous but anchored to
  archive-set centers (currently exaggerated for tuning).
- **Smarter grain slicing** (hysteresis + quiet-point split + zero-crossing snap)
  so grains are syllable/word-like; serial `x` re-slices the whole archive.
- **Per-channel mastering** (`AudioMaster`: biquad EQ + soft limiter). The MELOS
  bus drives a surface exciter on a **31 cm / 1.5 mm steel cube** — only a
  protective 150 Hz high-pass; the metallic ring is kept on purpose (the cube is
  the instrument). DC blockers on both buses. Serial `y` sweeps the exciter to
  find cube resonances.
- `MODEL.BIN` version bumped to 2 (added `meanLogF0Sq` for pitch spread); an
  older model reinitializes (spread restarts at 0) while the recording archive
  and `GRAINS.CSV` are untouched.

### `ooho_combined_emergent/ooho_combined_emergent.ino`

Claude Code branch of the build, forked from `ooho_combined_test` (the stable
build, left untouched). Primary development of this project is done in Codex;
this build was made with Claude Code. Design spec:
[`docs/superpowers/specs/2026-06-03-ooho-combined-emergent-design.md`](docs/superpowers/specs/2026-06-03-ooho-combined-emergent-design.md).

Differences from `ooho_combined_test`:

- LOGOS (left): up to 4 overlapping streams (a crowd), denser scheduling
  (`0.5-6 s` active interval), ~30% reversed grains, ~20% rate-shifted grains
  (`0.70x` slower / `1.40x` faster), longer `40 ms` fades. Grains are loaded
  into PSRAM and played from RAM, so LOGOS no longer streams from SD.
- MELOS (right): real-time generative synthesis instead of a rendered
  `MELOS.WAV`. Harmonic partials of F0 + breath, colored by the model bands.
  The synth eases toward the model over a few seconds, so the collective voice
  drifts slowly and continuously with no loop seam, no render pause, no crunch.
  There is no MELOS file and no render step.
- Mixer routing: `logosQueue1..4 -> logosMixer -> leftMixer.0`; the diagnostic
  tone stays on `leftMixer.3` (an `AudioMixer4` has only 4 inputs).

Serial: same as `ooho_combined_test`, except `e` is a no-op (nothing to render).
Next step (not in this build): a live granular synthesis path for LOGOS.

### `ooho_combined_test/ooho_combined_test.ino`

Current stable Stage 8 lab build.

Behavior:

- hold pin `32` to record;
- release to process;
- recording writes only to PSRAM during capture;
- raw WAV is saved after release;
- `GRAINS.CSV` and `MODEL.BIN` update after release;
- playback continues during recording;
- one LOGOS stream plays real grains on left;
- MELOS v2 soft plays on right;
- MELOS render is deferred while playback is active.

Important note:

Same-card `MNEXT.WAV` double-buffer/crossfade was tested and disabled. It produced crunch and rhythmic modulation when rendering while playing from the same SD card. Do not re-enable casually.

Serial commands for bench debugging:

```text
r  start/stop recording
p  only available in record_raw_test for latest full WAV playback
s  stop playback or recording
t  diagnostic tone
m  print model summary
g  print grain/LOGOS status
```

Memory behavior:

- With PSRAM detected: 30 seconds at 44.1 kHz mono 16-bit in current field/test builds.
- Without PSRAM: 3 second internal RAM fallback.
- Global Teensy `AUDIO_BLOCK_SAMPLES` is not modified.

### `record_raw_test/record_raw_test.ino`

Record-only bring-up sketch.

Behavior:

- Hold pin `32` = record to PSRAM/RAM only.
- Release = save `Wxxxx.WAV`.
- Max record time = 30 seconds.
- Appends LOGOS grain candidates to `GRAINS.CSV`.
- Serial `p` plays the latest full WAV.
- Serial `s` stops playback or recording.
- No MELOS, no model, no idle playback.

Use this sketch before the full artistic firmware.

### `sd_smoke_test/sd_smoke_test.ino`

Built-in SD slot test.

Behavior:

```text
SD.begin(BUILTIN_SDCARD)
write TEST.TXT
read TEST.TXT
delete TEST.TXT
```

LED stays on when the test passes and blinks fast on failure.

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

### `pin_scan_test/pin_scan_test.ino`

Temporary hardware finder for button wiring.

It scans pins `28`, `29`, `30`, `32`, and `33` using `INPUT_PULLUP`.

This confirmed that the current record button is on pin `32`.

### `logos_one_stream_test/logos_one_stream_test.ino`

Manual LOGOS playback test.

```text
reads GRAINS.CSV
opens Wxxxx.WAV
manual seek into WAV data
AudioPlayQueue -> left channel
button pin 32 / Serial n = one grain now
```

### `melos_short_test/melos_short_test.ino`

Short MELOS render/playback test.

```text
loads MODEL.BIN or default model
renders MELOS.WAV at MELOS_SECONDS = 30
manual FsFile + AudioPlayQueue -> right channel
```

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

Local Arduino CLI path used during bring-up on this machine:

```sh
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' compile --fqbn teensy:avr:teensy41 record_raw_test
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' upload -p usb:132000 --fqbn teensy:avr:teensy41 record_raw_test
```

If upload fails with `Unable find Teensy Loader`, open the Teensy Loader app from the local Teensy package and repeat upload.

## Field Notes

Use the built-in Teensy 4.1 SD slot, not the Audio Shield SD slot. The built-in slot is native SDIO and is faster than the SPI slot on the shield.

Use short filenames:

```text
W0001.WAV
W0002.WAV
T0001.WAV temporary while saving
GRAINS.CSV
MODEL.BIN
MELOS.WAV
MNEXT.WAV experimental next MELOS render, disabled in stable build
```

The SD card should be formatted FAT32 for first field tests. Endurance cards are preferred for repeated writes.

Noise lessons learned:

- The earlier ticking artifact was present inside the WAV file, not just in playback.
- Internal sine tone playback was clean, so the output amp/speaker chain was not the cause.
- Powerbank recording did not remove the ticking.
- RAM-first recording removed the ticking.
- Conclusion: do not write to SD during recording.
- Same-card MELOS render while playback continues can produce crunch/modulation.
- `AUDIO_FEED_MIN_US = 2300` is a known-good queue feed interval in `ooho_combined_test`; a slower `3200 us` caused rhythmic modulation.

## Research Notes

The next artistic/technical direction is documented in:

- [`docs/collective_voice_research.md`](docs/collective_voice_research.md) - research map for the sealed metal cube, internal archive speaker, external surface exciter, and the "collective voice" model.
- [`docs/collective_voice_implementation_plan.md`](docs/collective_voice_implementation_plan.md) - implementation plan for LOGOS/MELOS Field v1.
- [`docs/bringup_pipeline.md`](docs/bringup_pipeline.md) - incremental hardware and firmware bring-up sequence.
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
│   ├── bringup_pipeline.md
│   └── project_knowledge.md
├── the_ooho_rev1/
│   └── the_ooho_rev1.ino
├── ooho_combined_test/
│   └── ooho_combined_test.ino
├── ooho_combined_emergent/
│   └── ooho_combined_emergent.ino
├── ooho_combined_voicebody/
│   └── ooho_combined_voicebody.ino
├── record_raw_test/
│   └── record_raw_test.ino
├── logos_one_stream_test/
│   └── logos_one_stream_test.ino
├── melos_short_test/
│   └── melos_short_test.ino
├── sd_smoke_test/
│   └── sd_smoke_test.ino
├── psram_memtest/
│   └── psram_memtest.ino
├── button_led_test/
│   └── button_led_test.ino
├── pin_scan_test/
│   └── pin_scan_test.ino
├── two_button_test/
│   └── two_button_test.ino
└── tone_button_test/
    └── tone_button_test.ino
```
