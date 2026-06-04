# Ooho Voicebody Snapshot

Saved 2026-06-03. This is the Claude Code branch that rebuilds **MELOS** as a
collective vocal body. Full design + as-built tuning:
[`docs/superpowers/specs/2026-06-03-melos-collective-voice-body-design.md`](superpowers/specs/2026-06-03-melos-collective-voice-body-design.md).

## Sketch

- Active build: `ooho_combined_voicebody/ooho_combined_voicebody.ino`
- Forked from: `ooho_combined_waveset/ooho_combined_waveset.ino` (its fallback)

## Lineage

```
ooho_combined_emergent (Claude Code base, NON_STALLING held-block feed)
  -> ooho_combined_emergent_v3 (Codex: granular crowd + Pink-Trombone-lite MELOS)
       -> ooho_combined_coupled / _density_snapshot (Codex: coupling, clouds, density)
            -> ooho_combined_waveset (Codex: CDP zero-crossing waveset)
                 -> ooho_combined_voicebody (this: MELOS rebuilt as vocal body)
```

The LOGOS left channel is inherited from waveset **unchanged** (4 streams, cloud
patterns stutter/arp/scrub/scatter/waveset, density modes, compressor,
recent-recording bias, smoothed density gain) and the `NON_STALLING` + held-block
queue feed is kept. Only the MELOS right channel was replaced, plus the rare-words
LOGOS tweak below.

## What Changed vs Waveset

### MELOS: additive partials -> source-formant-filter (the point of this build)

- Was: a harmonic-partial additive synth weighted by `model.bands`, with discrete
  vocal "modes" on a timer. It read as an autonomous synth chord.
- Now: **6 glottal sources** (a crowd near one collective pitch) summed into **one
  shared 3-formant "mouth"** (TPT state-variable bandpass). Vowel (oo/aa/ee) comes
  from the `model.bands` centroid; tone vs breath from `voicedRatio`. Aspiration
  noise shares the formant filter, so low voicedRatio = whispered vowels.
- **Pitch = unison swarm blooming into an archive cluster.** Voices sit near
  `meanLogF0`; the cluster width tracks the archive's F0 spread (new model stat
  `meanLogF0Sq`), with a ~20% pull toward unison/octave/fifth. Sparse archive =
  near unison; varied archive = wider, occasionally consonant cluster.
- **Breathing** amplitude arcs: narrow range (~0.66..0.86, no near-silence dips),
  smoothed ~5 s glide, reduced envelope movement — a calm, quiet body. MELOS is
  deliberately quiet (`MELOS_GAIN = 0.13`) as a supporting layer under LOGOS.
- After a recording, MELOS briefly "chews" that voice's F0/vowel/breath, then
  dissolves it into the collective.
- The LOGOS->MELOS **amplitude sidechain was removed** (no volume pumping). LOGOS
  coupling now only nudges the vowel/openness and pitch blur, not loudness.
- Vocal-mode timer, additive partial loop, `archiveBandWeight`, `setMelosMode`,
  `chooseMelosMode`, `updateMelosVocalTract` are gone. New core:
  `melosBlockUpdate()` (per-block easing + formant coeffs), `melosNextSample()`
  (per-sample sources + SVF), `melosVowelPosition()`, `melosFormantTargets()`,
  `melosNearestConsonant()`, `melosComputeFormantCoeffs()`. The coupling hooks
  (`exciteMelosFromLogos`, `followLogosEnvelope`) are reused.

### MELOS: timbral movement (Pink-Trombone-inspired)

Autonomous but anchored to archive-set centers (archive defines *where* the body
sits; slow autonomous drift gives life *around* it):

- **Nasality (velum):** a 4th resonator (`MELOS_NASAL_FREQ` ~270 Hz murmur pole),
  state `melosNasal`, blended into the output as `mouthOut = formantOut*(1-0.6*n)
  + nasalBP*2.0*n`. Drifts toward an archive center (darker/hummed = more nasal).
- **Wandering constriction:** `melosConstrictAuto` — occasional autonomous swells
  raise F3 + breath ("almost-consonants"); adds to the LOGOS-driven constriction.
- **Vowel micro-wander:** `melosVowelWander` random-walks around the archive vowel
  (`MELOS_VOWEL_WANDER`).
- **Sub-choirs:** `melosVoiceBright[v]` biases half the 6 voices darker, half
  brighter (varied mouths) — static richness, not movement.

NOTE (as of this snapshot): these are tuned **exaggerated** so the movement is
clearly audible while tuning (the small/uniform archive otherwise keeps the
archive-driven centers near-static). Expect to dial vowel wander / nasal depth /
constriction frequency **down** toward a calm body once confirmed audible.

### LOGOS: rare intelligible words (new)

- `LOGOS_WORD_PERCENT` (currently 25): a grain on **any** stream may play as a
  whole **forward, rate-1.0** intelligible word; otherwise it is forced into
  cloud/micro deconstruction so clear words never slip through by accident. Words
  emerge from the texture; absolute frequency still breathes with the density
  mode.

### LOGOS: smarter grain slicing (`appendLogosGrains`)

The post-recording analysis was upgraded from a plain energy gate to:

- **Hysteresis** detection — a high `startThreshold` begins a region, a lower
  `continueThreshold` (`= start * LOGOS_HYST_FACTOR`) keeps it alive, so soft
  consonants / onsets / tails are captured (words keep their consonants).
- **Quiet-point splitting** — long regions are split at the **lowest-RMS window
  near the target length** (a syllable/word seam) instead of a fixed time grid.
- **Zero-crossing snap** — every grain boundary is nudged to the nearest zero
  crossing (`LOGOS_ZC_SEARCH_SAMPLES`) for clean cuts.

Per-window RMS is precomputed into `logosWindowRms[LOGOS_MAX_WINDOWS]`. Tunables:
`LOGOS_HYST_FACTOR`, `LOGOS_SPLIT_SEARCH_MS`, `LOGOS_ZC_SEARCH_SAMPLES`. Next
planned step (not done): per-grain brightness/voiced tags in `GRAINS.CSV` so
playback can route grains to matching stream roles.

**Archive re-slice (serial `x`):** `rebuildGrainsFromArchive()` re-runs the
current detector over every `Wxxxx.WAV` and rewrites `GRAINS.CSV` from scratch
(via `loadWavIntoRecordBuffer`). New slicing only affects newly recorded voices
otherwise, so run `x` once after changing the detector to migrate the whole
archive. Playback is stopped during the rebuild; MELOS keeps humming (no SD).

## Mastering: per-channel EQ + limiter (`AudioMaster`)

A custom `AudioStream` (`AudioMaster`, with RBJ `MasterBiquad` stages + a
peak-following soft limiter) is inserted on each output bus:
`leftMixer -> masterL -> i2sOut.0` and `rightMixer -> masterR -> i2sOut.1`.

- **LOGOS -> internal speaker (`masterL`):** HP 90 Hz + a +2.5 dB presence bell at
  3 kHz (word clarity). Limiter ceiling 0.92.
- **MELOS -> surface exciter on the steel cube (`masterR`):** HP **150 Hz only**.
  The cube is **31 cm x 31 cm x 31 cm, 1.5 mm steel**; its panel bending
  fundamental is ~130-180 Hz (it barely radiates below that), so the HP is purely
  protective. The metallic ring (2-6 kHz) is **intentionally NOT tamed** — the
  artist wants the steel to sing; the cube is part of the instrument. Limiter
  ceiling 0.90.
- DC blockers (`DC_BLOCK_R`, ~3.5 Hz) on each LOGOS stream (`LogosStream.dcX1/Y1`)
  and on MELOS (`melosDcX1/Y1`). LOGOS needs it because the SGTL5000 ADC high-pass
  is disabled, so recordings can carry DC into the exciter.

Do NOT re-add harshness EQ on `masterR` without checking with the artist — the
ring is wanted. If a specific resonance is ever too much, add a narrow notch at
that exact frequency, not a broad cut.

## Serial commands (added in this build)

- `x` = rebuild `GRAINS.CSV` from the whole archive with the current slicer.
- `y` = 30 s log sweep 60 Hz -> 8 kHz on the right (exciter) channel, printing
  the frequency, to find the cube's resonances by ear (e.g. to notch, or to tune
  MELOS pitch toward them so the steel rings in sympathy — a future idea).

## Model Change

`MODEL_VERSION` bumped to **2** (added `float meanLogF0Sq` after `meanLogF0`).
An older `MODEL.BIN` fails validation and reinitializes (pitch spread restarts at
0 and accumulates as new voices are recorded). The WAV archive and `GRAINS.CSV`
are not touched.

## Key Tunables

- MELOS level/body: `MELOS_GAIN` (0.13), `MELOS_MAKEUP` (2.2),
  `MELOS_BREATH_EASE` (~5 s), `MELOS_VOICE_COUNT` (6),
  `MELOS_SPREAD_MAX_SEMITONES` (7), `MELOS_CONSONANCE_PULL` (0.20).
- MELOS timbral movement (currently exaggerated): `MELOS_VOWEL_WANDER` (0.40),
  `MELOS_NASAL_EASE`, nasal blend depth in `mouthOut`, constriction interval /
  depth in `melosBlockUpdate`, `melosVoiceBright` split (±0.22).
- LOGOS words: `LOGOS_WORD_PERCENT` (25).
- LOGOS slicing: `LOGOS_HYST_FACTOR`, `LOGOS_SPLIT_SEARCH_MS`,
  `LOGOS_ZC_SEARCH_SAMPLES`.
- Mastering: per-bus EQ + limiter `ceiling` in `setup()`; exciter EQ kept
  minimal on purpose (steel ring is wanted).

## Fallback

If the vocal-body MELOS is wrong for the installation, `ooho_combined_waveset`
remains the prior build with the additive Pink-Trombone-lite MELOS.
