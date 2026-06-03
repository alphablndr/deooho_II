# Design: `ooho_combined_emergent`

Date: 2026-06-03
Status: approved (pending spec review)
Base build: `ooho_combined_test/ooho_combined_test.ino` (Stage 8 stable)

## Summary

A new sketch, `ooho_combined_emergent/ooho_combined_emergent.ino`, forked from the
stable `ooho_combined_test`. The original `ooho_combined_test` is **not modified**.

The work changes what the installation sounds like, on both output channels:

- **Left channel (LOGOS, real words):** a crowd of up to 4 overlapping speech
  fragments, with reversed grains, playback-rate variation, and longer fades so
  words emerge from the texture instead of clicking in.
- **Right channel (MELOS, collective voice):** replace the pre-rendered
  `MELOS.WAV` loop with **real-time generative synthesis** driven by the voice
  model. The tone is harmonic (partials of F0 + breath), evolves continuously and
  imperceptibly as new recordings shift the model, never repeats, and has no
  render pause or loop seam.

This is an artist-driven build. The maintainer (Anton) is a sound artist, not a
Teensy specialist; internal architecture is chosen to serve the listening
experience. The primary work on this device is done in Codex; **this build is the
Claude Code branch**, which the README notes.

## Artistic Intent

- The cube is a sealed object. People approach, hold a button, whisper a
  recording. The recording joins the archive and contributes to a collective
  voice that lives inside the cube.
- LOGOS = the literal remembered words, fragmented and multiplied into a crowd.
- MELOS = the collective, synthesized voice: a constant ambient that is always
  present, never obviously loops, and drifts slowly and continuously (on the
  order of minutes) as new voices accumulate.

## Hardware / Platform Context (unchanged)

- Teensy 4.1 + Teensy Audio Shield Rev D, SGTL5000.
- 8 MB PSRAM soldered.
- Built-in Teensy 4.1 SD slot (SDIO), single card.
- Audio Library: 44.1 kHz, `AUDIO_BLOCK_SAMPLES = 128` (not modified).
- Output: `leftMixer -> i2sOut L` (LOGOS), `rightMixer -> i2sOut R` (MELOS).
- Record button on pin 32 (pressed = LOW).

### Why live synthesis is safe here

The known bottleneck on this device is **SD I/O contention**, not CPU. The
historical ticking/crunch came from SD writes (recording, MELOS render) competing
with SD reads (playback) on a single card. Live MELOS synthesis **removes MELOS
from the SD card entirely**, so it reduces SD contention rather than adding to it.

CPU budget: MELOS synthesis is ~10-15 partials per sample. Using a shared sine
**wavetable** (not `sinf()` per partial) this is on the order of 1-3% CPU at
44.1 kHz. Recording is a block copy into PSRAM (negligible). LOGOS playback is
SD reads + light per-sample processing. Running live MELOS + recording + 4 LOGOS
streams concurrently is well within budget.

## Out of Scope (explicitly deferred)

- **Live granular synthesis for LOGOS.** LOGOS words are real recordings that
  must be read from SD, so a full live granulator is a larger, separate effort.
  This build keeps LOGOS as grain playback (with the additions below). The
  reverse + rate + multi-stream work already moves LOGOS toward a granular
  texture. Recorded as the next step.
- Two-SD-card architecture.
- Any change to the original `ooho_combined_test`.

---

## Component 1 — LOGOS: up to 4 overlapping streams (left channel)

### Current state (combined_test)

- `LOGOS_STREAM_COUNT = 1`; only `logosStreams[0]` runs.
- 3 play queues exist (`logosQueue1/2/3`) wired to `leftMixer` inputs 0,1,2.
- `leftMixer` input 3 = `diagnosticTone`.
- `leftMixer` is `AudioMixer4` (hard limit: 4 inputs).
- Stream scheduling and a dormant/active state machine already exist but are
  effectively unused at count 1.

### Target

- 4 LOGOS streams. `logosStreams[0]` is the always-on stream; `logosStreams[1..3]`
  run independent dormant/active state machines (as the existing code already does
  for index > 0).
- Per-stream gain `0.22` (4 simultaneous ≈ 0.88, no clip).
- Density constants:
  - `LOGOS_ACTIVE_MIN_INTERVAL_MS = 500` (was 2000)
  - `LOGOS_ACTIVE_MAX_INTERVAL_MS = 6000` (was 8000)
- `LOGOS_STREAM_COUNT = 4`.

### Mixer routing (the 4-input constraint)

`AudioMixer4` has only 4 inputs and input 3 currently carries the diagnostic
tone. To fit a 4th LOGOS stream without losing the tone test, **cascade a second
mixer**:

```
logosQueue1 -> logosMixer.0
logosQueue2 -> logosMixer.1
logosQueue3 -> logosMixer.2
logosQueue4 -> logosMixer.3
logosMixer  -> leftMixer.0      (summed LOGOS crowd)
diagnosticTone -> leftMixer.3   (tone test preserved)
```

Gain model (matching the current code's split): the per-stream **level** stays in
the per-sample multiply by `LOGOS_STREAM_GAIN` (0.22) inside the feed loop, so 4
simultaneous streams sum to ≈ 0.88. The `logosMixer` input gains are used only as
the existing **0/1 fade gate** (`setLogosStreamGain` toggles a stream's mixer
input between 0.0 and 1.0 to gate fresh audio in cleanly). `setLogosStreamGain`
therefore moves from `leftMixer` to `logosMixer`. `leftMixer.0` gain = 1.0 for
the LOGOS sum; `leftMixer.3` keeps the existing tone behavior.

Internal only; no effect on the two-channel (L=LOGOS / R=MELOS) layout the artist
works with.

## Component 2 — LOGOS: reversed grain playback

- For each grain start, decide independently per stream:
  `bool reversed = (nextRandomU32() % 100) < 30;` (~30%).
- Implementation: load the grain's samples into a dedicated PSRAM buffer, reverse
  in place, then feed reversed samples into the play queue.
  - Max grain ≈ 1100 ms ≈ 48,510 samples ≈ ~97 KB at 16-bit. A dedicated PSRAM
    grain buffer (e.g. `EXTMEM` array sized for the max grain) is well within
    8 MB. This buffer is **separate** from the record buffer (which is in use
    during recording).
- Reversed grains use the same fade-in/fade-out as forward grains.
- Applies to any of the 4 streams independently.

## Component 3 — LOGOS: playback-rate variation (crude pitch-shift)

- After the reverse decision, decide independently:
  `bool rateShift = (nextRandomU32() % 100) < 20;` (~20%). If shifting, pick
  randomly between `0.70x` and `1.40x`.
- Combines with reverse (rare: reversed + slow).
- Implementation via linear interpolation while feeding the queue:
  - `0.70x`: per 10 output samples, read 7 source samples, interpolate the gaps
    (slower, lower pitch).
  - `1.40x`: per 10 output samples, read 14 source samples, skipping ~every 3rd
    (faster, higher pitch).
- Not true time-stretch (pitch follows speed); acceptable artifact for voice
  fragments, adds texture.
- Because reverse already requires the full grain in a PSRAM buffer, rate
  variation reads from that same buffer with a fractional read index, which keeps
  both features on one code path (buffer-fed playback) rather than mixing
  buffer-fed and SD-streamed paths.

### Note on the grain feed path

Reverse and rate both require random access to grain samples. The cleanest design
is: when a grain starts, read the whole grain into the PSRAM grain buffer once,
then feed the play queue from that buffer for the grain's lifetime (forward or
reversed, integer or fractional step). This replaces the current per-block SD
streaming for LOGOS grains. It also reduces per-block SD reads during playback.

## Component 4 — LOGOS: longer fades

- `LOGOS_FADE_MS = 40` (was 8). Words emerge from the texture rather than
  clicking in. Applied to all streams, forward and reversed.

---

## Component 5 — MELOS: real-time generative synthesis (right channel)

### Replace the file path

Remove the pre-rendered MELOS file path for this build:

- Delete use of `renderMelosWav()`, `MELOS.WAV` / `MTEMP.WAV` reads, the
  `startMelosLoop()` / `feedMelosQueue()` file-streaming logic, and all the
  deferred-render / `MNEXT` / crossfade machinery.
- MELOS audio is produced by a synthesis function that fills `melosQueue` blocks
  directly each audio cycle, in IDLE and during RECORDING/PROCESSING (MELOS keeps
  sounding while a person records, as today).

### Synthesis (harmonic voice, from the model)

Per output sample, driven by the live `model`:

- Fundamental `f0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ)`.
- Generate harmonics: partial `n` at frequency `n * f0`, for `n = 1, 2, 3, ...`
  while `n * f0 < 8000 Hz` (typically 10-15 partials).
- For each partial, find which of the 8 model bands its frequency falls into;
  weight it by `sqrt(model.bands[band] * BAND_COUNT)`.
- `voiced_source = sum of weighted partials`, normalized by partial count.
- `breath_source = filtered noise` (carry over the existing smoothed-noise breath).
- `mix = voiced_source * model.voicedRatio + breath_source * (1 - voicedRatio)`.
- Keep slow random-walk pitch **drift**, the smoothed **envelope** shaping, and
  **soft clip** from the existing render math. There is no per-file fade-in/out
  and no loop boundary (the stream is continuous).
- Use a shared **sine wavetable** for partial oscillators (phase accumulator per
  partial, or a single phase with integer-multiple lookups) to keep CPU low.

This is the same harmonic concept as the originally requested `renderMelosWav()`
rewrite (Fix 1), relocated into a real-time generator.

### Continuous, imperceptible evolution

- After each recording, the model updates as it does today (`updateModel`):
  small blended shift of `meanLogF0`, `voicedRatio`, `bands`, `envelope`
  (governed by the existing `MODEL_WEIGHT`).
- The synthesizer reads **smoothed/interpolated** model parameters: it eases its
  internal working parameters toward the model's current values over a few
  seconds, so a model update never causes an audible jump. The result drifts on
  the timescale of minutes as voices accumulate — a constant ambient with no
  audible cycle and no render pause.
- No render, no SD writes for MELOS, no playback dip.

### Gain

- Preserve current MELOS level intent (`MELOS_GAIN`, v2-soft character).
  `rightMixer` input 0 = MELOS synth, input 1 = diagnostic tone (unchanged).

---

## Serial / Bench Behavior

- `r` record, `s` stop, `t` diagnostic tone, `m` model summary, `g` LOGOS status:
  unchanged.
- `e` (manual MELOS render) is removed/repurposed — there is no render. It may
  print a short "MELOS is live-synthesized; no render" notice or be dropped.
- MELOS no longer prints "render deferred" messages.

## Constants Summary (new build)

| Constant | combined_test | emergent |
|---|---|---|
| `LOGOS_STREAM_COUNT` | 1 | 4 |
| `LOGOS_STREAM_GAIN` (per stream) | 0.28 | 0.22 |
| `LOGOS_FADE_MS` | 8 | 40 |
| `LOGOS_ACTIVE_MIN_INTERVAL_MS` | 2000 | 500 |
| `LOGOS_ACTIVE_MAX_INTERVAL_MS` | 8000 | 6000 |
| reverse chance | n/a | 30% |
| rate-shift chance / factors | n/a | 20% @ 0.70x or 1.40x |
| MELOS | rendered `MELOS.WAV` | real-time synth |

## README

Add a short note that `ooho_combined_emergent` is the **Claude Code** branch of
the build (primary development is done in Codex), forked from
`ooho_combined_test`, with the LOGOS crowd/reverse/rate changes and live MELOS
synthesis. Add the new sketch to the Repository Layout and Bring-Up sections.

## Risks / Things to verify on hardware

- **CPU headroom under worst case:** 4 LOGOS streams active + live MELOS +
  recording. Expected fine; confirm no audio glitching via Audio Library CPU/
  memory reporting during a stress test.
- **PSRAM grain buffer sizing:** confirm the max-grain buffer plus the record
  buffer both fit in PSRAM (they do at ~97 KB + record buffer << 8 MB).
- **MELOS evolution rate feels right:** the parameter-easing time constant is a
  tunable; adjust by ear so change is perceptible over minutes but never jumps.
- **LOGOS density not muddy:** 4 streams at 0.22 with 0.5 s minimum spacing; if
  too dense/clippy, tune gain or interval by ear.
- **Reverse/rate ratios** (30% / 20%) are tunable by ear.

## Testing

This is firmware for custom hardware; verification is on-device by listening plus
serial diagnostics:

1. Compiles for `teensy:avr:teensy41` with no errors.
2. Boots; MELOS ambient audible on right channel immediately, continuous, no
   loop seam, no render pause.
3. LOGOS crowd audible on left; multiple overlapping fragments; some reversed;
   some pitch/speed shifted; smooth fades.
4. Record a voice; it saves `Wxxxx.WAV`, appends `GRAINS.CSV`, updates model;
   MELOS tone drifts gradually afterward with no dip or crunch.
5. Diagnostic tone (`t`) still audible on both channels.
6. Original `ooho_combined_test` unchanged (git diff shows only new files).
