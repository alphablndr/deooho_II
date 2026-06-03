# Design: MELOS as Collective Vocal Body (`ooho_combined_voicebody`)

Date: 2026-06-03
Status: approved (pending spec review)
Base build: `ooho_combined_waveset/ooho_combined_waveset.ino` (current active build)
New sketch: `ooho_combined_voicebody/ooho_combined_voicebody.ino`

## Summary

Redesign the **MELOS** (right-channel) synthesizer so it reads as a *collective
vocal body* derived from the voice archive, instead of an autonomous "beautiful
synth chord." The **LOGOS** (left channel) engine and everything else in the
waveset build are left unchanged.

The problem with the current MELOS: it is an additive synth (harmonic partials
weighted by `model.bands`) playing a few fixed pitches with an autonomous
timbre-morph timer. The brain reads that as *a synthesizer playing a chord* —
authored harmony, detached from the archive.

The redesign rests on three decisions made during brainstorming:

1. **Identity:** music + collective + "our shared medium of voice." The music
   should *be the shape of the collective*, not the composer's hand.
2. **Pitch:** a near-unison **swarm** around the archive pitch that **blooms into
   harmony in proportion to how diverse the archive's recorded pitches are**.
   Bloom is a **microtonal cluster from the real pitch spread, with a gentle
   (~20%) pull toward consonance** (unison/octave/fifth) when the spread is wide.
3. **Engine:** **source–formant-filter** synthesis (glottal source → resonant
   formant filters), not additive partials and not (yet) a full waveguide tract.
   Real formants make it vocal; the architecture can later be upgraded to a full
   Kelly–Lochbaum tract (Pink Trombone) without changing the rest.

## Artistic Intent

- MELOS = the collective voice the cube has accumulated. It changes **only**
  because the archive changed, a new voice was just recorded, or LOGOS events
  occurred — never as autonomous "pretty movement."
- "Music" is emergent: pitch center, harmonic width, vowel, and tone/breath
  balance are all readouts of the archive. The more (and more varied) the voices
  recorded, the richer the collective harmony.
- It should breathe like a body, not sustain like a pad.

## Platform Context (unchanged)

- Teensy 4.1 + Audio Shield Rev D, 44.1 kHz, `AUDIO_BLOCK_SAMPLES = 128`.
- MELOS feeds `melosQueue -> rightMixer.0`. The `NON_STALLING` + held-block feed
  (`feedMelosQueue`, `melosBlockReady`) is **kept as-is** — do not reintroduce a
  `while(available())` loop (see the waveset/emergent spec for why).
- CPU note: the Teensy `AudioProcessorUsage` meter does **not** measure loop()
  synthesis; judge headroom by absence of queue underrun.

## What stays unchanged (out of scope)

- The entire **LOGOS** left-channel engine: 4 streams, cloud patterns
  (stutter/arp/scrub/scatter/waveset), density modes, compressor, recent-recording
  bias, smoothed density-gain.
- The held-block `NON_STALLING` queue feed for both channels.
- The LOGOS→MELOS coupling *hooks* (`exciteMelosFromLogos`, the
  `melosLogosOpen/Constrict/Stabilize/Density` excitation values, and the
  `followLogosEnvelope` sidechain) — they are **reused**, re-pointed at the new
  engine.
- **Full Kelly–Lochbaum waveguide tract** — deferred. The source–formant-filter
  is explicitly designed so the formant stage can later be swapped for a tract.
- **Per-voice formant tracts** — deferred; v1 uses one shared formant "mouth".

## What is replaced

The additive MELOS core is removed and rebuilt:

- Remove the autonomous vowel-mode timer (`chooseMelosMode` / `MELOS_MODE_*` /
  the random mode switching) — vowel motion becomes a direct readout of the
  archive, not a timer.
- Remove the additive partial loop (`MELOS_PARTIALS_PER_VOICE` harmonic sum
  weighted by bands) inside `melosNextSample` / `updateMelosVocalTract`.
- Replace with the source–formant-filter engine below.

---

## Component 1 — Voice engine: source–formant-filter

### Glottal sources (the crowd)

- **`MELOS_VOICE_COUNT = 6`** glottal sources (was 3).
- Each source is a glottal-pulse oscillator at its own frequency (see Pitch
  below): a phase accumulator producing a glottal waveform (band-limited
  sawtooth / Rosenberg-style pulse via the existing sine wavetable + shaping).
- **Tenseness** per source morphs the source between a breathy, rounded pulse and
  a tenser, brighter pulse; aspiration noise is mixed in inversely to tenseness.
  Tenseness is driven by `model.voicedRatio` (+ LOGOS coupling).

### Shared formant "mouth"

- The 6 glottal sources are **summed**, then passed through **3 resonant bandpass
  formant filters** (biquads) in parallel: F1, F2, F3.
- This is one collective mouth: many voices, one vowel — exactly the "crowd on
  one vowel at a spread of pitches" identity.
- F3 ~ fixed (≈ 2600–2900 Hz, slight jitter). F1/F2 trace the vowel locus
  (Component 3). Bandwidths/gains are vowel-appropriate constants.

### Output

- `melosOut = softClip(mouthOut * breathAmp * MELOS_GAIN)`, fed block-by-block via
  the existing held-block `feedMelosQueue`.

CPU: 6 phase-accumulator sources + noise + 3 biquads per sample ≈ trivial on the
M7 FPU. Memory: a handful of floats; no new large buffers.

## Component 2 — Pitch: swarm → archive-bloomed cluster

- **Center:** `melosF0 = eased(constrain(expf(model.meanLogF0), F0_MIN, F0_MAX))`
  (existing easing kept).
- **Spread:** a new model statistic gives the standard deviation of recorded
  log-F0 (see Component 6). Map it to a maximum voice spread `spreadSemitones`
  (clamp e.g. 0 → ±7 semitones).
- **Per-voice pitch:** voice `v` sits at `melosF0 * 2^(offset_v / 12)`, where
  `offset_v` is distributed across `[-spread, +spread]` (e.g. symmetric around 0,
  voice 0 ≈ center). Narrow archive → all offsets ≈ 0 → near unison.
- **Humanizing:** each voice adds constant micro-detune jitter (±5–15 cents) and
  a slow independent random-walk drift, so even at unison there is beating and
  life (no two voices phase-locked).
- **Consonance pull (~20%):** when `spread` is wide, each voice's `offset_v` is
  pulled a fraction `MELOS_CONSONANCE_PULL` (≈0.2) toward the nearest of
  {0, ±12 (octave), ±7 (fifth)} semitones. Mostly a raw detuned cluster;
  occasionally "finds a chord." Pull strength is a tunable.

## Component 3 — Vowel from the archive

- Compute a **vowel position** (1-D, dark→bright) from the spectral centroid of
  `model.bands` (low bands dominant → dark/`oo`; high bands → bright/`ee`;
  mid → open/`aa`). Ease it slowly (a few seconds); it moves only when the model
  changes.
- Map vowel position to formant targets along the locus:
  - `oo` (dark/back): F1 ≈ 300, F2 ≈ 870 Hz
  - `aa` (open):      F1 ≈ 700, F2 ≈ 1220 Hz
  - `ee` (bright):    F1 ≈ 270, F2 ≈ 2300 Hz
- The biquad center frequencies ease toward these targets (no zipper).

## Component 4 — Tone vs breath

- `model.voicedRatio` sets the global **tenseness/aspiration balance**: high →
  tonal choir; low → breathy/whisper body (more aspiration noise, looser pulse).
- Eased; audible but smooth.

## Component 5 — Breathing dynamics (no pad)

- A global **breath amplitude** follows slow arcs: rise/fall over
  `MELOS_BREATH_MIN_MS..MELOS_BREATH_MAX_MS` (≈ 4–8 s) with random variation, and
  occasional deeper near-silent "inhales." Implemented as a smoothed
  random-target envelope (not a fixed LFO), so it never sounds metronomic.
- Breath phase also slightly raises aspiration at the start of each arc.

## Component 6 — Archive binding & events (reuse existing hooks)

- **New model field for spread.** Maintain a running mean of `logF0` (already
  `meanLogF0`) and a running mean of `logF0²` (`meanLogF0Sq`). Variance =
  `meanLogF0Sq − meanLogF0²`; `spread = sqrt(max(0, variance))`. Updated in
  `updateCollectiveModel` with the same `MODEL_WEIGHT` blend.
- **Model version bump + migration.** Bump `MODEL_VERSION`. On a version/size
  mismatch, the model reinitializes (spread starts at 0 = unison and accumulates
  as new voices arrive). The recording archive (`Wxxxx.WAV`, `GRAINS.CSV`) is
  untouched; only the collective model statistic restarts.
- **"Chew" gesture.** For `MELOS_CHEW_MS` (≈ 10–20 s) after a recording, bias the
  center pitch, vowel position, and tenseness toward *that* recording's detected
  F0 / band centroid / voicedRatio, then ease back into the collective means.
- **LOGOS coupling (reused).** Re-point the existing excitation values onto the
  new engine:
  - swarm / dense LOGOS → temporarily widen pitch spread + raise aspiration
    (blurs MELOS, airier);
  - clear forward words → pull voices toward unison + open the vowel toward `aa`
    (stabilize);
  - the `followLogosEnvelope` sidechain still ducks MELOS so the recorded voice
    comes forward.

## Proposed constants (all tunable by ear)

| Constant | Value | Meaning |
|---|---|---|
| `MELOS_VOICE_COUNT` | 6 | glottal sources in the crowd |
| `MELOS_FORMANTS` | 3 | F1/F2/F3 resonators (shared mouth) |
| `MELOS_SPREAD_MAX_SEMITONES` | 7 | widest bloom at max archive spread |
| `MELOS_DETUNE_CENTS` | 5–15 | per-voice humanizing detune |
| `MELOS_CONSONANCE_PULL` | 0.20 | pull toward octave/fifth when wide |
| `MELOS_VOWEL_EASE` | ~1/(3·SR) | vowel morph time (~3 s) |
| `MELOS_BREATH_MIN/MAX_MS` | 4000 / 8000 | breath arc period range |
| `MELOS_CHEW_MS` | 15000 | post-recording "chew" duration |
| `MELOS_GAIN` | from waveset | right-channel level (re-judge by ear) |

## Risks / things to verify on hardware

- **Formant filter stability/levels.** Resonant biquads at high Q can ring or
  clip when summed across 6 sources; keep Q moderate and gain-stage before
  `softClip`. Verify no distortion at swarm + chew overlap.
- **Spread starts at 0.** Until several new voices accumulate, MELOS will be near
  unison (intended). The artist should confirm the bloom feels right as the
  archive grows; the spread→semitones mapping is a tunable.
- **CPU.** Expected trivial, but confirm no queue underrun under MELOS + full
  LOGOS swarm.
- **Vowel legibility.** Confirm oo/aa/ee actually read as vowels through the cube
  exciter; adjust formant freqs/bandwidths by ear.

## Testing (on-device, by listening + serial)

1. Compiles for `teensy:avr:teensy41`; memory within budget.
2. Boots; right channel is a breathing vocal body, not a static chord/pad.
3. With a sparse archive: near-unison crowd (beating, alive), recognizable vowel.
4. Record several varied voices: pitch spread audibly widens into a cluster that
   occasionally leans consonant; vowel shifts as `model.bands` shift.
5. Just after a recording: the new voice is briefly foregrounded (chew), then
   dissolves.
6. LOGOS swarm blurs/aerates MELOS; clear words stabilize it — no crunch,
   modulation, or dropout.
7. `ooho_combined_waveset` unchanged (fallback intact).
