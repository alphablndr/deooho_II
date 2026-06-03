# The Ooho II - Incremental Bring-Up Pipeline

Use these sketches one layer at a time. Do not debug the full LOGOS/MELOS firmware until the lower layers pass.

## Stage 0 - Button

Sketch:

```text
button_led_test
```

Checks:

- button between pin 32 and GND;
- built-in LED turns on while pressed;
- Serial prints `button pressed` and `button released`.

## Stage 1 - Audio Output

Sketch:

```text
tone_button_test
```

Checks:

- button on pin 32 still works;
- Audio Shield output reaches the amp/speaker/exciter;
- press = 440 Hz tone, release = silence.

## Stage 2 - SD And PSRAM

Sketches:

```text
sd_smoke_test
psram_memtest
```

Checks:

- `sd_smoke_test` initializes `BUILTIN_SDCARD`, writes, reads, and deletes `TEST.TXT`;
- `psram_memtest` reports detected PSRAM and passes memory patterns.

## Stage 3-5 - Record Raw, Playback, Grain Index

Sketch:

```text
record_raw_test
```

Checks:

- hold pin 32 button = record to PSRAM/RAM only;
- release = save `Wxxxx.WAV`;
- max recording time is 30 seconds;
- Serial `p` plays latest full WAV;
- Serial `s` stops playback or recording;
- `GRAINS.CSV` gets LOGOS grain candidates.

This sketch intentionally has no MELOS, no model, and no idle playback.

## Stage 6 - One LOGOS Stream

Sketch:

```text
logos_one_stream_test
```

Checks:

- reads existing `GRAINS.CSV`;
- counts valid grain rows;
- opens the referenced `Wxxxx.WAV`;
- seeks manually into the WAV data with `FsFile.seekSet()`;
- feeds `AudioPlayQueue`;
- routes LOGOS to the left channel only;
- button pin 32 or Serial `n` triggers one grain immediately.

Confirmed during bring-up:

```text
GRAINS count: 50+
LOGOS Wxxxx.WAV @ start_ms for duration_ms
```

Physical result: real word grains were audible on the left/internal speaker path.

## Stage 7 - Short MELOS

Sketch:

```text
melos_short_test
```

Checks:

- loads `MODEL.BIN` if present, otherwise uses a neutral default model;
- renders short `MELOS.WAV` with `MELOS_SECONDS = 30`;
- plays MELOS through manual `FsFile` + `AudioPlayQueue`;
- routes MELOS to the right channel only.

Confirmed during bring-up:

```text
Rendered MELOS.WAV.
MELOS playback started on right channel.
MELOS loop wrapped to start.
```

The first MELOS renderer sounded too digital and crunchy. The current approved test character is **MELOS v2 soft**:

- softer sine/harmonic body instead of sharp saw;
- slower random-walk pitch drift;
- smoother envelope;
- lower gain;
- a small amount of breath/noise.

Physical result: a single pitch center plus slight noise, much more pleasant than the first renderer.

## Stage 8 - Combined Short Test

Sketch:

```text
ooho_combined_test
```

Current stable baseline:

- button pin 32 hold = record;
- playback continues during recording;
- recording still writes only to PSRAM during capture;
- release = save raw WAV, append LOGOS grains, update `MODEL.BIN`;
- MELOS render is currently deferred while playback is active, to keep sound stable;
- one LOGOS stream plays on the left;
- MELOS v2 soft plays on the right;
- `MELOS_SECONDS = 30` for debug;
- audio queue feed interval is `2300 us`.

Confirmed during bring-up:

```text
W0023.WAV peak=0.92508, LOGOS grains added: 3
W0024.WAV LOGOS grains added: 6
W0025.WAV peak=0.79196, LOGOS grains added: 3
W0027.WAV LOGOS grains added: 1
```

Important lessons from Stage 8:

- queue feed interval `3200 us` caused audible rhythmic modulation/starvation; `2300 us` restored stable playback;
- attempting `MNEXT.WAV` double-buffer + crossfade on the same SD card caused unstable/rhythmic output and was disabled;
- rendering MELOS while playing from the same SD card can produce crunch/glitch after release;
- the stable choice for now is to keep playback alive and defer MELOS rendering.

Serial commands:

```text
r  record toggle
s  stop playback or stop recording
t  diagnostic tone
m  print model summary
g  print grain/LOGOS status
n  trigger one LOGOS grain now
e  manual MELOS render, for bench only
```

## Stage 9 - Full Field Build

Only after `record_raw_test` passes:

1. one LOGOS stream from `GRAINS.CSV`;
2. short 30-second MELOS render;
3. combined short test build;
4. final field build with 3 LOGOS streams and long MELOS.

Keep `MELOS_SECONDS = 30` until the whole loop is reliable.

Do not enable the 3-stream LOGOS scheduler, 480-second MELOS, or same-card MELOS crossfade until the Stage 8 baseline has soaked without glitches.

## Commands

Compile:

```sh
arduino-cli compile --fqbn teensy:avr:teensy41 <sketch_folder>
```

Upload:

```sh
arduino-cli upload -p usb:132000 --fqbn teensy:avr:teensy41 <sketch_folder>
```

Serial:

```text
115200 baud
```
