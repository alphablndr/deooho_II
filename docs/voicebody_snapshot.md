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

### 2026-06-04: Explicit articulation layer

Codex added a small Pink-Trombone-inspired articulation layer on top of the
Claude `voicebody` engine. This is still not a Pink Trombone port and uses no
external code; it only borrows the physical idea of a changing vocal tract:

- `melosTonguePos`: archive spectral centroid becomes a slow tongue position
  (`0 = back/dark`, `1 = front/bright`).
- `melosTongueDiameter`: voiced/open archives and clear LOGOS gestures open the
  mouth; dense/granular/reversed LOGOS gestures narrow it.
- `melosConstrBack`, `melosConstrMid`, `melosConstrFront`: transient constriction
  zones. Reversed LOGOS pushes back/nasal; granular/cloud LOGOS pushes mid/front
  and adds a little fricative air; clear whole words stabilize/open MELOS.
- The articulation changes formant targets, oral damping, nasality, and a small
  front-fricative noise component. It does **not** add phoneme sequencing or
  literal speech. Conceptually, LOGOS does not “control a synth parameter”; it
  briefly deforms the collective mouth.

A fallback copy of the previous build is saved in
`ooho_combined_voicebody_pre_articulation/` and documented in
`docs/voicebody_pre_articulation_snapshot.md`.

### 2026-06-04: First cleanup pass

Codex saved `ooho_combined_voicebody_pre_cleanup/`, then removed stale
constants/global state left over from older additive/vocal-mode MELOS versions.
The pass also updated the sketch header and added a section map. It did not
change the audio graph, recording path, model format, LOGOS behavior, or MELOS
sound design. The cleanup roadmap lives in
`docs/voicebody_debug_cleanup_plan.md`.

### 2026-06-04: Speech-ghost articulation bursts

Codex saved `ooho_combined_voicebody_pre_speechghost/`, then added a small
`speech ghost` layer to MELOS. The slow ambient vocal body remains the default,
but LOGOS events and post-recording learning gestures can briefly wake a faster
4-9 Hz jaw/tongue motor rhythm for roughly 1-4 seconds. This does not sequence
phonemes or create intelligible speech. It only adds temporary offsets to the
existing tongue position, tongue diameter, and back/mid/front constriction:

- clear words: occasional soft opening/stabilizing speech-memory;
- granular/cloud LOGOS: more frequent front/mid articulation bursts;
- reversed LOGOS: darker back/nasal bursts;
- dense LOGOS overlaps: slightly higher chance of a burst.

Serial command `m` now prints `speechGhost/rate` for tuning. The fallback before
this layer is documented in `docs/voicebody_pre_speechghost_snapshot.md`.

### 2026-06-04: More active MELOS murmur

After listening, MELOS still read as mostly ambient. Codex tuned the
speech-ghost layer to make the collective body more present without turning it
into constant speech:

- post-recording speech-ghost is stronger and lasts longer;
- LOGOS grains trigger speech-ghost more often, especially granular/reversed
  fragments and dense overlaps;
- speech-ghost tongue/diameter/constriction offsets are deeper;
- MELOS now has rare autonomous murmur gestures every ~18-45 seconds, so the
  body can remember speech even during quiet LOGOS gaps.

### 2026-06-04: More synthetic-voice MELOS, still ambient

Codex tuned MELOS halfway toward a clearer synthetic voice while preserving the
ambient object-body:

- reduced maximum pitch spread and increased consonance pull, so the six sources
  read less like an abstract cloud and more like one synthetic throat;
- added a very subtle 5 Hz vibrato/micro-instability, stronger during
  speech-ghost and post-recording gestures;
- raised formant Q/gain moderately for clearer vowel identity;
- made the glottal source slightly brighter and reduced aspiration noise;
- increased post-formant makeup slightly without changing output routing.

### 2026-06-04: Tab refactor baseline

The speech-ghost build was compiled, uploaded to Teensy 4.1, and reported as
sounding normal. Codex saved `ooho_combined_voicebody_pre_tab_refactor/` before
starting the Arduino tab split. The tab refactor is intended to change only
readability and debug navigation; no model format, SD layout, audio routing,
queue behavior, pins, or tuning constants should change.

### LOGOS: rare intelligible words (new)

- `LOGOS_WORD_PERCENT` (currently 25): a grain on **any** stream may play as a
  whole **forward, rate-1.0** intelligible word; otherwise it is forced into
  cloud/micro deconstruction so clear words never slip through by accident. Words
  emerge from the texture; absolute frequency still breathes with the density
  mode.

### 2026-06-04: LOGOS call-and-response + reverse tails

Codex added two LOGOS deconstruction gestures:

- **Call-and-response:** after some clear words, another LOGOS stream answers
  180-820 ms later with a short reversed/micro fragment from the same grain.
- **Reverse tails:** after some clear words finish, the same stream may play the
  end of that word backwards as a quiet object-memory tail.

Both gestures reuse the existing PSRAM grain player and only start new SD reads
while idle, preserving the no-SD-write-during-recording rule and avoiding new
audio routing.

### 2026-06-04: LOGOS phrase ghosts + musical rates

Added two more LOGOS gestures on top of the response build:

- **Phrase ghosts:** automatic LOGOS playback can sometimes expand a selected
  word into a 1.5-4.0 s source window around it, then slice that longer window
  into a cloud of syllable/dust/scrub voices. The result should feel like a
  human phrase passing through the object, but without returning language as
  readable sentence meaning. Tunables: `LOGOS_PHRASE_GHOST_PERCENT`,
  `LOGOS_PHRASE_MIN_MS`, `LOGOS_PHRASE_MAX_MS`,
  `LOGOS_PHRASE_PREROLL_MAX_MS`.
- **Musical playback rates:** deconstructed LOGOS voices now choose from a more
  interval-like set of rates: 0.5, 0.667, 0.75, 1.0, 1.333, 1.5, 2.0. This is
  not true pitch shifting; speed and pitch move together, but the offsets are
  closer to octave/fifth/fourth relationships and should make stutters/arps feel
  more composed.

The fallback before this change is `ooho_combined_voicebody_pre_phrase_ghosts/`.
Because phrase ghosts can load up to four seconds into PSRAM, the per-stream
LOGOS buffer is now sized from `LOGOS_PHRASE_MAX_MS` instead of
`LOGOS_MAX_DURATION_MS`.

### 2026-06-04: LOGOS archive-pressure density

LOGOS density now follows archive size while preserving chance-heavy behavior at
small archive sizes:

- `logosArchivePressure()` maps `grainCount` to 0-100% against
  `LOGOS_ARCHIVE_FULL_GRAINS` (currently 240 grains).
- More archive pressure makes `swarm` mode more likely and `sparse` less likely,
  but swarm still has a baseline chance even with a small archive so early field
  versions can still suddenly feel crowded.
- More archive pressure shortens LOGOS event intervals and dormant waits.
- Cloud grains can add up to `LOGOS_ARCHIVE_VOICE_BOOST_MAX` extra voices on
  non-anchor streams, so a large archive feels more like a crowd rather than only
  a faster scheduler.
- Serial diagnostics (`d`) prints `archivePressure` next to grain count.

### MELOS reference: APEX / vocal tract modelling

`musikinformatik/apex-sc-dafx` is a SuperCollider/C++ vocal-tract modelling
project based around a KLVocalTract UGen and APEX data/examples. It is too large
and GPL-oriented to directly copy into this firmware, but the useful idea for
Ooho is conceptual and implementable: replace some of the current formant
wandering with a lightweight **moving tract resonator** model, where slow changes
in a few tract/area parameters produce vowel color by reflection/resonance rather
than only by moving filter frequencies. Future MELOS work can prototype this as
2-4 coupled resonator sections on Teensy, not a full port.

### 2026-06-04: Lightweight moving tract resonator

Implemented the first safe version of that idea in live MELOS:

- Current source/formant/nasal synthesis stays intact.
- A new three-section serial tract (`back / mid / front`) is inserted after the
  formant+nasal mouth output.
- Each section is a broad TPT state-variable resonator. The section frequencies
  move slowly from the existing tongue position, tongue diameter, and
  back/mid/front constriction state, so the sound gets cavity memory instead of
  only parallel formant peaks.
- This is not a direct Pink Trombone/APEX port. It is a lightweight object-body
  approximation designed for Teensy CPU safety.
- Main tunables: `MELOS_TRACT_BLEND`, `MELOS_TRACT_DRIVE`,
  `MELOS_TRACT_Q`, `MELOS_TRACT_EASE`.
- Serial `m` / `d` now print `tract Hz` for the three moving sections.

Fallback before this change: `ooho_combined_voicebody_pre_moving_tract/`.

### 2026-06-04: Pink-trombone-cpp articulation borrowings

After inspecting `cutelabnyc/pink-trombone-cpp/src`, Ooho borrowed two small
ideas rather than porting the full 44-section waveguide:

- **Thinness/openness fricatives:** MELOS front/mid turbulence now follows a
  Pink-Trombone-style window: a constriction must be narrow enough to hiss, but
  not fully closed. This should make breath/noise feel more like articulating
  tissue and less like plain white noise.
- **Release transients:** when the moving tract releases from a strong
  obstruction, a very short decaying impulse is injected into the tract. This is
  a lightweight version of the transient behavior in Pink Trombone, meant to add
  subtle mouth-release events without literal consonants.

Debug `m` / `d` now also print tract `obstruction/transient`. Fallback before
this change: `ooho_combined_voicebody_pre_ptcpp_articulation/`.

### 2026-06-04: LF glottis lite

MELOS source synthesis now borrows the core **LF glottal waveform** idea from
Pink Trombone's `Glottis.cpp`:

- The old primary source was a summed harmonic buzz per voice. It made the
  formants speak sometimes, but the base character still read as synth.
- The new source computes lightweight LF coefficients once per audio block from
  `melosTenseness`, then each MELOS voice emits a glottal open/close pulse.
- A very small 2nd/3rd harmonic edge remains to keep the pulse present on the
  exciter, but the formants and moving tract are now driven by something closer
  to vocal-fold motion.
- Aspiration noise is modulated by glottal phase, so breath enters the tract in
  a more voice-like rhythm instead of as plain steady noise.

Main tunables: `MELOS_LF_TONE_GAIN`, `MELOS_LF_BRIGHT_GAIN`,
`MELOS_LF_ASPIRATION_GAIN`, `MELOS_LF_MIN_RD`, `MELOS_LF_MAX_RD`.
Fallback before this change: `ooho_combined_voicebody_pre_lf_glottis/`.

### 2026-06-04: Lead glottis + syllabic motor

After listening, LF glottis was too subtle because six equal MELOS voices
averaged away the glottal closure and read as a synth pad. This build keeps the
collective ambience but moves the voice identity into a foreground body:

- The source is now **one lead LF glottis plus two quiet shadows**, not six equal
  voices. `MELOS_LEAD_SOURCE_GAIN` carries the closure; shadows retain collective
  breadth without smearing the pulse.
- LF edge is stronger (`MELOS_LF_TONE_GAIN`, `MELOS_LF_BRIGHT_GAIN`) so the
  formants and steel exciter receive a more voice-like excitation.
- A short **syllabic motor** sometimes performs a 180-760 ms
  open -> constrict -> release gesture. It is not phoneme sequencing; it is a
  physical mouth movement that should make MELOS occasionally feel like it is
  trying to speak.
- LOGOS events and post-recording learning can trigger the syllabic motor, so
  the archive and grains visibly deform the collective voice.
- Serial `m` / `d` print `syllable amount/phase`.

Fallback before this change: `ooho_combined_voicebody_pre_lead_glottis/`.

### 2026-06-04: MELOS 2345 voice-body shaping

Implemented the next musical MELOS layer without adding A/B debug controls:

- **Vocal foreground events:** MELOS sometimes steps out of ambient mode for a
  few seconds. Foreground tightens pitch spread, makes the lead glottis more
  dominant than the shadows, raises formant presence, and increases moving-tract
  blend. LOGOS density and post-recording learning can trigger these events.
- **Vowel islands:** instead of endless vowel wandering, MELOS is gently pulled
  toward recognisable vocal zones (`oo` / dark, open `aa`, brighter/front vowel,
  etc.) for 7-22 second spans. The archive still biases the color, but the sound
  should have more memorable states.
- **Stronger assimilation:** after a recording is processed, MELOS now pulls
  harder toward the incoming F0/brightness, enters foreground, triggers a breath
  phrase, and sets the next vowel island from the dominant band of the new voice.
- **Breath phrases:** short inhale -> voiced pulse -> release shapes add
  human-like phrase energy to aspiration and amplitude without returning to a
  simple pumping LFO.

Debug `m` / `d` now print `foreground/vowelIsland/breathPhrase`. Fallback before
this change: `ooho_combined_voicebody_pre_melos_2345/`.

**Status:** reverted from the active firmware after listening. The previous
lead-glottis/syllabic-motor MELOS was preferred; the 2345 foreground/vowel
island/breath phrase layer made MELOS less desirable in the installation
context. Keep this section as a record of the experiment, not as current active
behavior.

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
