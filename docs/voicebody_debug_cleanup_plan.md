# Voicebody Debug + Cleanup Plan

Saved 2026-06-04 after the first safe cleanup pass.

> **Snapshot folders moved to git history (2026-10-13).** Every
> `ooho_combined_voicebody_pre_*/` folder mentioned below was removed from the
> working tree; all of them are preserved in commit `6649f1d`. To restore one:
> `git checkout 6649f1d -- ooho_combined_voicebody_pre_<name>/`

## Current Active Build

- Sketch: `ooho_combined_voicebody/ooho_combined_voicebody.ino`
- Fallback before cleanup: `ooho_combined_voicebody_pre_cleanup/`
- Earlier fallback before explicit articulation:
  `ooho_combined_voicebody_pre_articulation/`
- Fallback before Arduino tab split:
  `ooho_combined_voicebody_pre_tab_refactor/`

## First Cleanup Pass

Done:

- Updated the file header so it describes the current `voicebody` build instead
  of the older coupled/additive build.
- Added a section map at the top of the sketch.
- Removed unused MELOS constants from older additive/vocal-mode versions.
- Removed unused MELOS global state arrays and drift/mode variables.
- Kept the audio graph, queues, recording path, model format, LOGOS behavior,
  and MELOS sound engine behavior intact.
- Recompiled successfully for Teensy 4.1.

## Debugging Philosophy

Do not split or rewrite the whole sketch while tuning sound. This firmware is
timing-sensitive, and the stable parts are valuable. Work in small reversible
passes:

1. Preserve a snapshot folder before each structural change.
2. Make one kind of change at a time.
3. Compile after every pass.
4. Upload only after compile is clean.
5. Listen for audio starvation, clicks, or SD contention before continuing.

## Recommended Next Passes

### Pass 2: Debug flags

Add compile-time flags:

- `OOHO_DEBUG_SERIAL`
- `OOHO_DEBUG_LOGOS_EVENTS`
- `OOHO_DEBUG_MODEL`
- `OOHO_DEBUG_AUDIO_CPU`

Goal: keep field Serial quieter without deleting useful diagnostics.

### Pass 3: File/module split, still Arduino-friendly

Started after the speech-ghost build was uploaded and reported as sounding
normal. Arduino can compile multiple `.ino` tabs in a single sketch folder, so
use tabs before introducing `.cpp/.h` complexity:

- `00_config.ino`: constants, structs, forward declarations
- `10_audio_graph.ino`: Audio objects and routing
- `20_model_sd.ino`: model, counter, WAV helpers, SD helpers
- `30_recording.ino`: button, record queue, raw WAV save
- `40_analysis.ino`: segmentation, F0, bands, envelope
- `50_melos.ino`: live vocal body
- `60_logos.ino`: streams, grain loading, queue feeding
- `90_app.ino`: serial commands, setup, loop

Goal: readability only. No behavior changes in the split pass.

### Pass 4: Runtime diagnostics

Done: Serial command `d` prints:

- state;
- Audio CPU max;
- AudioMemory max;
- active LOGOS streams;
- queue freshness flags;
- model summary;
- articulation summary.

Goal: one command for field debugging instead of remembering `m`, `g`, and CPU
print locations.

### Pass 5: Tuning tables

Move artistic parameters into clearly named groups:

- LOGOS density and cloud behavior;
- LOGOS transformation probabilities;
- MELOS pitch/body behavior;
- MELOS articulation behavior;
- mastering/output gain.

Goal: fast tuning during installation without hunting through the whole file.
