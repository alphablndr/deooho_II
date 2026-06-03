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
- **Breathing** amplitude arcs (smoothed ~3 s glide), not a sustained pad.
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

### LOGOS: rare intelligible words (new)

- `LOGOS_WORD_PERCENT` (currently 25): a grain on **any** stream may play as a
  whole **forward, rate-1.0** intelligible word; otherwise it is forced into
  cloud/micro deconstruction so clear words never slip through by accident. Words
  emerge from the texture; absolute frequency still breathes with the density
  mode.

## Model Change

`MODEL_VERSION` bumped to **2** (added `float meanLogF0Sq` after `meanLogF0`).
An older `MODEL.BIN` fails validation and reinitializes (pitch spread restarts at
0 and accumulates as new voices are recorded). The WAV archive and `GRAINS.CSV`
are not touched.

## Key Tunables

- MELOS: `MELOS_VOICE_COUNT` (6), `MELOS_SPREAD_MAX_SEMITONES` (7),
  `MELOS_CONSONANCE_PULL` (0.20), `MELOS_MAKEUP` (2.2), `MELOS_BREATH_EASE`
  (~3 s), `MELOS_GAIN` (0.525).
- LOGOS words: `LOGOS_WORD_PERCENT` (25). Lower = rarer words, higher = more.

## Fallback

If the vocal-body MELOS is wrong for the installation, `ooho_combined_waveset`
remains the prior build with the additive Pink-Trombone-lite MELOS.
