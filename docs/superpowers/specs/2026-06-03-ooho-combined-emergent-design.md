# Design: `ooho_combined_emergent`

Date: 2026-06-03
Status: **implemented and verified on hardware** (Teensy 4.1, 2026-06-03)
Base build: `ooho_combined_test/ooho_combined_test.ino` (Stage 8 stable)

> **As-built note.** This document is the design plus the final as-built state.
> Where the implementation differs from the original plan, the as-built value is
> given and marked. Key as-built facts: LOGOS per-stream gain is **0.33** (raised
> from 0.22 by ear); MELOS is real-time synthesized (no file); and the audio play
> queues use **`NON_STALLING` behaviour with a held-block feed** — this is
> load-bearing, see "Audio queue feeding" below. On-device result: LOGOS crowd +
> live MELOS audible, no underrun modulation/bitcrush.

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
- Per-stream gain `0.33` (as-built; raised from 0.22 by ear). 4 *simultaneous*
  streams sum to ≈ 1.32, so dense 4-voice overlap can briefly hard-clip; in
  practice streams 1-3 are dormant most of the time so 1-2 voices is typical.
  Tunable: drop toward 0.28 or add a soft limiter on the LOGOS sum bus if peaks
  distort.
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
the per-sample multiply by `LOGOS_STREAM_GAIN` (0.33 as-built) inside the feed
loop. The `logosMixer` input gains are used only as
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

- Removed entirely: `renderMelosWav*()`, `startMelosLoop()`, `swapToNextMelos*`,
  the crossfade machinery, `MELOS.WAV` / `MTEMP.WAV` / `MNEXT.*` files and their
  name constants, and the deferred-render logic.
- `feedMelosQueue()` is repurposed: instead of streaming a WAV from SD it fills
  `melosQueue` blocks from the live synthesizer (`melosNextSample()`), in IDLE and
  during RECORDING/PROCESSING (MELOS keeps sounding while a person records).

### Synthesis (harmonic voice, from the model)

Per output sample, driven by the live `model`:

- Fundamental `f0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ)`.
- Generate harmonics: partial `n` at frequency `n * f0`, for `n = 1, 2, 3, ...`
  while `n * f0 < MELOS_PARTIAL_MAX_HZ` (8000 Hz), capped at `MELOS_MAX_PARTIALS`
  (24 as-built).
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
  internal working parameters (`melosF0`, `melosVoicedRatio`, `melosBandWeight[]`)
  toward the model's current values, so a model update never causes an audible
  jump. As-built ease rate `MELOS_PARAM_EASE = 1/(4·SAMPLE_RATE)` (~4 s time
  constant). The envelope shape is walked by a slow LFO,
  `MELOS_ENV_INC = 1/(28·SAMPLE_RATE)` (~28 s cycle). The result drifts on the
  timescale of minutes as voices accumulate — a constant ambient with no audible
  cycle and no render pause. Both constants are tunable by ear.
- After each recording, `processRecording()` calls `computeMelosEnvelope()` to
  refresh the smoothed envelope from the updated model; the synth then eases into
  the new tone. No render, no SD writes for MELOS, no playback dip.

### Gain

- Preserve current MELOS level intent (`MELOS_GAIN`, v2-soft character).
  `rightMixer` input 0 = MELOS synth, input 1 = diagnostic tone (unchanged).

---

## Audio queue feeding (as-built — load-bearing, do not "simplify")

Both MELOS and LOGOS push audio into `AudioPlayQueue`s from `loop()` (via
`servicePlaybackDuringWork`). Two non-obvious facts about `AudioPlayQueue` shape
this code and a regression we hit:

1. **`available()` reports the audio-memory POOL, not the queue ring.** It calls
   `allocate()` and returns true whenever the pool has a free block. The pool is
   large (`AudioMemory(360)`), the queue ring is small (`max_buffers` = 24/32). So
   `while (queue.available()) { … }` is effectively unbounded — it does **not**
   stop when the queue is full.
2. **Default behaviour is `ORIGINAL`, where `playBuffer()` busy-waits**
   (`while (tail == h);`) when the ring is full.

Combined, a naive "fill while available()" loop spins forever once you can
synthesize faster than realtime: it fills the ring, then busy-waits / re-fills
indefinitely, never returning to `loop()`. Observed symptom: MELOS kept sounding
(fed by the audio ISR) but `loop()` was stuck, so **LOGOS never triggered and
serial stopped responding**. This regressed an earlier attempt.

As-built fix (required):

- All five play queues are set to `AudioPlayQueue::NON_STALLING` in `setup()`, so
  `playBuffer()` returns `1` instead of busy-waiting when the ring is full.
- Each feeder **holds one filled block** (`melosBlockReady`, `LogosStream.blockReady`)
  and only retries `playBuffer()` when the ring is full — it never re-synthesizes a
  wasted block, and the loop always terminates at the ring boundary.

This keeps the queue rings full (cushion: MELOS `setMaxBuffers(32)` ≈ 93 ms,
each LOGOS `setMaxBuffers(24)` ≈ 70 ms; pool `AUDIO_MEMORY_BLOCKS = 360`). The
large cushion absorbs the **blocking SD read** done when a LOGOS grain starts
(whole grain loaded into PSRAM at once), which was the cause of the
"modulation / bitcrush" the artist first reported. Grain starts and the
`GRAINS.CSV` scan are gated to `state == IDLE` so no bulk SD read competes with
the recording-save writes during `PROCESSING`.

> Lesson: the original `ooho_combined_test` bounded its feed with a `maxBlocks`
> cap. That cap was not just throttling — it was also what kept the feed loop
> finite given the pool-based `available()`. Do not remove the bound without
> `NON_STALLING` + a held-block (or explicit count) replacement.

---

## Serial / Bench Behavior

- `r` record, `s` stop, `t` diagnostic tone, `m` model summary, `g` LOGOS status,
  `n` one LOGOS grain now: unchanged.
- `e` (manual MELOS render) is a no-op: prints `"MELOS is live-synthesized;
  nothing to render."` There is no render path.
- MELOS no longer prints "render deferred" messages.

## Constants Summary (new build)

| Constant | combined_test | emergent (as-built) |
|---|---|---|
| `LOGOS_STREAM_COUNT` | 1 | 4 |
| `LOGOS_STREAM_GAIN` (per stream) | 0.28 | 0.33 |
| `LOGOS_FADE_MS` | 8 | 40 |
| `LOGOS_ACTIVE_MIN_INTERVAL_MS` | 2000 | 500 |
| `LOGOS_ACTIVE_MAX_INTERVAL_MS` | 8000 | 6000 |
| reverse chance (`LOGOS_REVERSE_PERCENT`) | n/a | 30% |
| rate-shift (`LOGOS_RATE_PERCENT` / factors) | n/a | 20% @ 0.70x or 1.40x |
| `AUDIO_MEMORY_BLOCKS` | 220 | 360 |
| LOGOS `setMaxBuffers` | 18 | 24 |
| MELOS `setMaxBuffers` | 24 | 32 |
| play queue behaviour | ORIGINAL (default) | NON_STALLING |
| `MELOS_MAX_PARTIALS` | n/a | 24 |
| `MELOS_PARAM_EASE` | n/a | 1/(4·SR) |
| `MELOS_ENV_INC` | n/a | 1/(28·SR) |
| MELOS | rendered `MELOS.WAV` | real-time synth |

PSRAM (as-built, EXTRAM ≈ 3.05 MB of 8): `psramRecordBuffer` (2.65 MB) +
`logosGrainBuffers[4][LOGOS_MAX_GRAIN_SAMPLES]` (≈ 0.4 MB).

## README

Add a short note that `ooho_combined_emergent` is the **Claude Code** branch of
the build (primary development is done in Codex), forked from
`ooho_combined_test`, with the LOGOS crowd/reverse/rate changes and live MELOS
synthesis. Add the new sketch to the Repository Layout and Bring-Up sections.

## Open tuning knobs / things still to judge by ear

- **LOGOS clipping at 0.33:** dense 4-voice overlap can hard-clip. Drop toward
  0.28 or add a soft limiter on the LOGOS sum bus if peaks distort. (Pending the
  artist's ear after the +50% bump.)
- **Crowd density:** streams 1-3 currently spend long stretches dormant
  (`LOGOS_DORMANT_*` 60-120 s), so the crowd builds slowly. Wake them sooner /
  more often for a fuller hum.
- **MELOS timbre / evolution:** harmonic harshness, breath amount, drift speed
  (`MELOS_PARAM_EASE`), right-channel level (`MELOS_GAIN`) — all by ear.
- **Reverse/rate ratios** (30% / 20%) — by ear.
- **CPU headroom under worst case** (4 LOGOS + live MELOS + recording): expected
  fine; can confirm via `AudioProcessorUsageMax()` after a record cycle.

## Testing (verified on hardware 2026-06-03)

Firmware for custom hardware; verification is on-device by listening plus serial.

1. ✅ Compiles for `teensy:avr:teensy41`, no errors. EXTRAM ≈ 3.05 MB / 8 MB.
2. ✅ Boots; banner + `MODEL.BIN loaded` + `LOGOS grains: 71`; reaches IDLE.
3. ✅ LOGOS crowd audible on left; serial shows varied grains, e.g.
   `LOGOS 1 W0028.WAV fwd rate=1.40`, `LOGOS 1 W0018.WAV REV` — reverse and
   rate-shift both firing. MELOS live on right.
4. ✅ **No underrun modulation/bitcrush** after the NON_STALLING + held-block
   feed fix (confirmed by the artist).
5. ⏳ Record-cycle model drift and tone evolution: not yet A/B'd over minutes.
6. ✅ Original `ooho_combined_test` unchanged (only new files committed).

### Regression history

- v1: copied the old `maxBlocks`/`AUDIO_FEED_MIN_US` feed → audible
  modulation/bitcrush (queue cushion too thin vs. blocking grain read).
- v2: removed the bound, used `while(available())` → infinite feed loop, LOGOS
  and serial dead (see "Audio queue feeding").
- v3 (current): `NON_STALLING` + held-block feed, full rings → clean.
