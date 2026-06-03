# Ooho Combined Emergent v3 Snapshot

Saved before the MELOS/LOGOS coupling experiment on 2026-06-03.

## Sketch

- Snapshot sketch: `ooho_combined_emergent_v3/ooho_combined_emergent_v3.ino`
- Working fork for the next experiment: `ooho_combined_coupled/ooho_combined_coupled.ino`
- Stable incremental baseline remains: `ooho_combined_test/ooho_combined_test.ino`

## What v3 Does

LOGOS is a four-stream PSRAM granular crowd on the left channel:

- stream 1 mostly plays recognizable word grains;
- streams 2-4 produce denser micro-granular fragments;
- grains can be reversed;
- grains can play at musical rate ratios: `0.5x`, `0.75x`, `1.333x`, `1.5x`;
- active stream timing follows a loose breath-pulse instead of purely uniform randomness;
- dormant intervals shrink as the archive grows.

MELOS is a live right-channel collective voice, not a WAV file:

- three harmonic voices around the archive-derived collective F0;
- five slow vocal modes inspired by Pink Trombone:
  `oo/body`, `aa/open`, `ee/bright`, `whisper/air`, `nasal/narrow`;
- archive `model.bands` still colors the voice, so accumulated recordings remain the source of the body;
- rare short constriction events add almost-consonant movement without making words;
- no reverb, delay, or SD playback is used for MELOS.

## Why It Was Saved

This is the last uncoupled version: LOGOS and MELOS coexist as independent layers.
The next fork explores coupling, where LOGOS grain events excite and reshape MELOS in real time.
