# Voicebody Pre-Articulation Snapshot

Saved 2026-06-04 before adding the explicit Pink-Trombone-inspired articulation
layer.

## Build

- Snapshot folder: `ooho_combined_voicebody_pre_articulation/`
- Active fork after this snapshot: `ooho_combined_voicebody/`

## Why It Was Saved

The pre-articulation build is the last Claude Code `voicebody` version before
Codex added explicit tongue position, tongue diameter, and back/mid/front
constriction gestures. It already had:

- live MELOS source-formant-filter synthesis;
- six glottal sub-voices into one shared vocal tract;
- archive-driven F0, voiced ratio, envelope, and band centroid;
- nasality, vowel wander, and autonomous constriction;
- four-stream LOGOS with waveset/cloud/micro/word behavior.

Use this folder if the newer articulation build feels too consonant, too noisy,
or too obviously "mouth-simulated" in the cube.
