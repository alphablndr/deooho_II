# The Ooho II - Collective Voice Research

This document maps artistic and technical strategies for the next firmware layer of The Ooho II.

Current installation premise:

- A wall-mounted ear has one button.
- A visitor presses, whispers, and cannot directly replay the result.
- The recording is stored inside a sealed metal box.
- One internal speaker plays specific recorded voices inside the box.
- One surface exciter, currently conceived as a Dayton DAEX58FP/DAEX58P-class exciter on the cube surface, produces an external collective/anonymous voice.
- The cube therefore has three listening zones: outside, inside, and the boundary gesture of touching/listening to the metal.

## 1. Core Artistic Reading

The strongest direction is not "voice effects", but a separation of speech into three ontological states:

1. **Archive voice**: the concrete recordings of specific people, stored as WAV files and played inside the sealed cube.
2. **Object voice**: an anonymous residue produced from all recordings; not language, but pitch, pressure, breath, rhythm, density, and vibration.
3. **Boundary voice**: the sound that exists only through contact with the wall/cube surface, turning listening into a bodily act.

This resonates with several sound-art and voice-theory lineages:

- Alvin Lucier's *I Am Sitting in a Room* repeatedly re-records speech until words dissolve into resonances of the room. The useful lesson is not the literal feedback loop, but the transformation of semantic speech into the acoustic signature of a container. Source: [MoMA on Lucier's I Am Sitting in a Room](https://www.moma.org/explore/inside_out/2015/01/20/collecting-alvin-luciers-i-am-sitting-in-a-room/).
- Michel Chion's acousmatic/acousmetre framing is useful because the listener hears a voice-being without seeing its source. The cube can become a hidden voice-body rather than a speaker box. Source: [Duke University Press, Sound: An Acoulogical Treatise](https://www.dukeupress.edu/sound/) and [Chion acousmetre excerpt](https://www.filmsound.org/chion/metre.htm).
- Dick Raaijmakers' *Cahier M* is relevant as a morphology of electric sound: signal is not neutral transport, but a shaped material. Source: [Orpheus Instituut, Cahier M](https://orpheusinstituut.be/en/publications/cahier-m).
- Mladen Dolar's voice-as-object is the conceptual anchor: the outside exciter should not transmit meaning, but the leftover object-character of speech. Source: [MIT Press, A Voice and Nothing More](https://mitpress.mit.edu/9780262541879/a-voice-and-nothing-more/).
- Corpus-based concatenative synthesis is a direct technical-artistic precedent: choose fragments from a corpus by descriptors such as pitch, energy, brightness, and roughness. Source: [IRCAM corpus-based concatenative synthesis](https://www.ircam.fr/projects/pages/synthese-concatenative-par-corpus).
- Granular/microsound approaches are relevant when the voice is broken into small particles rather than replayed as a message. Source: [Curtis Roads, Microsound](https://mitpress.mit.edu/9780262681544/microsound/).

## 2. Technical Baseline

Hardware already supports this:

- Teensy 4.1 with PSRAM
- Teensy Audio Shield Rev D / SGTL5000
- Audio recorded into PSRAM first, then written to built-in SD after stop
- Stereo line output available from Audio Shield `LINE OUT L/G/R`
- Current button on pin `32`
- Current LED on pin `31`

Relevant Teensy facts:

- Teensy Audio Library streams 16-bit, 44.1 kHz audio and supports analysis/effects/synthesis/routing. Source: [PJRC Teensy Audio Library](https://www.pjrc.com/teensy/td_libs_Audio.html).
- The Audio Shield supports stereo line-level input/output and simultaneous input/output. Source: [PJRC Audio Adaptor](https://www.pjrc.com/store/teensy3_audio.html).
- Teensy 4.1 `EXTMEM` places variables in optional PSRAM. Source: [PJRC Teensy 4.1](https://www.pjrc.com/store/teensy41.html).
- The Audio Library includes objects such as RMS analysis, note/frequency analysis, FFT, synthesis, mixing, filtering, and granular effects. Source: [PaulStoffregen/Audio](https://github.com/PaulStoffregen/Audio).

Important constraint:

- Do not write to SD while recording. We already proved this creates ticking in recorded WAV files. The PSRAM-first architecture is the correct foundation.

## 3. Recommended Output Topology

Use the two line-out channels as two different social spaces:

```text
LINE OUT L -> amplifier channel L -> ordinary speaker inside sealed cube
LINE OUT R -> amplifier channel R -> DAEX58FP/DAEX58P surface exciter on cube wall
LINE OUT G -> amplifier input ground / jack sleeve
```

Suggested behavior:

- **Left/internal speaker**: plays specific recordings softly, randomly, or in a slow archive ritual. These voices are not offered as a user-facing playback function; they are "living inside".
- **Right/exciter**: plays a continuously updated collective voice model. This is the public/anonymous surface voice.

One stereo amplifier with 3.5 mm input is acceptable:

```text
LINE OUT L -> jack tip
LINE OUT R -> jack ring
LINE OUT G -> jack sleeve
```

Do not use the Audio Shield headphone jack as the main source for the amplifier. Use `LINE OUT L/G/R`.

## 4. Implementation Families

### A. Collective Breath / Envelope Drone

**Artistic behavior**

The outside cube does not speak words. It breathes with the average pressure curve of all whispers.

**Technical implementation**

After each recording:

- Compute RMS envelope every 20-50 ms.
- Normalize the recording length into 64 or 128 envelope bins.
- Update a running average envelope.
- Estimate recording duration and overall loudness.
- Store/update `MODEL.BIN` and optionally `MODEL.CSV`.

Playback on right/exciter:

- A low, voice-like oscillator or filtered noise is amplitude-modulated by the average envelope.
- The envelope can be stretched to a slow cycle, e.g. 20-60 seconds.

Feasibility:

- Very high on Teensy.
- No heavy DSP.
- Strong conceptually because it preserves gesture and breath, not content.

Weakness:

- It may become too abstract unless paired with occasional grain/formant detail.

### B. Median F0 Voice Object

**Artistic behavior**

Each whisper contributes its pitch tendency. The outside surface slowly converges toward a collective fundamental frequency: neither male nor female, neither one person nor many.

**Technical implementation**

After each recording:

- Decimate analysis to around 11.025 kHz for CPU efficiency.
- Analyze frames of about 1024 samples with 512 hop.
- Estimate F0 using autocorrelation or a lightweight YIN-like method.
- Keep only voiced frames: RMS above threshold and autocorrelation confidence above threshold.
- Store median F0, voiced ratio, and F0 variance for the recording.
- Update a global log-domain mean/median:

```text
model.meanLogF0 = weighted running average of log(f0)
model.f0Spread  = running estimate of variance
```

Synthesis:

- Generate a pulse/saw/triangle source at `exp(model.meanLogF0)`.
- Add small random detuning based on `model.f0Spread`.
- Modulate amplitude with the collective envelope.
- Filter to a soft voice band, roughly 100-4000 Hz.

Feasibility:

- Medium-high.
- Autocorrelation is feasible in post-record analysis because recording has already stopped.
- YIN is a better-known F0 estimator for speech/music, but simple autocorrelation may be enough for whisper-adjacent voice. Source: [YIN paper, PubMed](https://pubmed.ncbi.nlm.nih.gov/12002874/). Praat recommends autocorrelation/cross-correlation variants for voice pitch analysis depending on the goal. Source: [Praat pitch method guide](https://praat.org/manual/how_to_choose_a_pitch_analysis_method.html).

Weakness:

- Whisper often has weak or absent periodic F0. The algorithm must allow "unvoiced" contributions.
- The output could become too synthetic unless breath/noise is included.

### C. Formant Ghost / Vocal-Tract Residue

**Artistic behavior**

Instead of averaging pitch, the cube averages the "mouth shape" of the archive: vowel color without words.

**Technical implementation**

After each recording:

- Compute FFT or band energies in 8-16 fixed frequency bands.
- Track spectral centroid, high-frequency noise/breath ratio, and rough spectral envelope.
- Update a global spectral profile.

Synthesis:

- Use a mixed source: low pulse train + breath noise.
- Pass it through 3-6 bandpass filters approximating broad formant zones.
- Drive the exciter with this slowly changing vowel-body.

Feasibility:

- Medium.
- Easier than full LPC, more robust than exact formant tracking.
- Teensy Audio Library has FFT/filter building blocks, and post-record analysis can run outside the audio interrupt.

Weakness:

- Real formant analysis is hard in noisy whispers.
- Fixed bands may be more robust and more artistic than "accurate".

### D. Granular Choir / Corpus Without Sentences

**Artistic behavior**

The outside surface emits particles of real voices, but too short and scattered to become messages. It is a community without identities.

**Technical implementation**

After each recording:

- Detect non-silent regions using RMS threshold.
- Choose short grains: 30-120 ms.
- Store grain metadata:

```text
filename, start_sample, length_samples, rms, brightness, maybe f0
```

Playback:

- Randomly select grains weighted by age, loudness, or similarity to the current model.
- Apply short fade-in/fade-out.
- Optionally pitch-shift only by crude playback-rate changes if implemented later.

Feasibility:

- Medium.
- Conceptually strong and very sound-art friendly.
- Direct random reads from SD can be clicky if too chaotic; safer approach is to pre-render a `COLLECT.WAV` after each new recording.

Weakness:

- Without careful grain fades it will click.
- Too many recognizable grains may break anonymity.

### E. Lucier-In-A-Box / Physical Resonance Averaging

**Artistic behavior**

The cube literally transforms voices into its own resonant body. This is the closest physical cousin to Lucier.

**Two possible versions**

1. **Without internal microphone**: simulate cube resonance by filtering the collective voice with fixed resonant filters measured from the cube.
2. **With internal microphone added later**: play a voice inside the sealed box, record the internal response, repeat over time, allowing the cube to overwrite speech with its resonances.

Feasibility:

- Version 1: medium-high.
- Version 2: future hardware iteration.

Weakness:

- True feedback/re-recording needs careful gain control to avoid runaway and accidental howl.

### F. Archive Weather / Statistical Sonification

**Artistic behavior**

The surface does not imitate a voice. It reports the social weather of the archive: how many voices, how recent, how intense, how long, how whispered.

Features:

- Number of recordings
- Last voice age
- Average duration
- Average loudness
- Voiced/unvoiced ratio
- Breathiness / spectral brightness

Synthesis:

- Low rumble = archive size.
- Slow pulse = arrival rate.
- Air noise = whisper/breath ratio.
- Pitch = median F0.
- Amplitude = collective envelope.

Feasibility:

- Very high.

Weakness:

- Less literally voice-like.

## 5. Best Direction For This Installation

Recommended combined implementation:

```text
Inside speaker / left channel:
  raw archive voices, played quietly and intermittently

Surface exciter / right channel:
  collective voice = envelope + median F0 + breath/noise + broad formant color
```

This gives three distinct layers:

- **Inside**: specific people remain specific.
- **Outside**: the archive becomes one anonymous body.
- **Boundary**: the metal wall mediates between them.

Primary technical plan:

1. Keep current one-button record-only behavior.
2. After recording stops, save `Wxxxx.WAV`.
3. Analyze the PSRAM buffer before clearing it.
4. Update `MODEL.BIN`:

```text
uint32_t count
float meanLogF0
float f0Variance
float meanRms
float meanDuration
float envelope[64]
float bands[12]
```

5. Append a human-readable line to `LOG.CSV`:

```text
index,duration_ms,rms,peak,voiced_ratio,median_f0,centroid
```

6. Generate or update a short `COLLECT.WAV` for the right channel.
7. In idle, play:

```text
left channel: occasional raw voice from archive
right channel: looped/generated collective voice
```

For the first field version, pre-rendering `COLLECT.WAV` after each recording is safer than fully live synthesis, because it makes playback deterministic and easy to inspect on a computer.

## 6. Teensy Implementation Architecture

The safest firmware architecture is:

```text
record button
  -> capture mono 44.1 kHz / 16-bit into PSRAM
  -> save raw Wxxxx.WAV to SD
  -> analyze the still-present PSRAM buffer
  -> update MODEL.BIN and LOG.CSV
  -> render COLLECT.WAV for the right/exciter channel
  -> return to idle playback state
```

This keeps the important rule intact: no SD writes during recording.

### Model file

Use a small binary model for firmware and a CSV/JSONL log for human inspection.

```cpp
struct CollectiveModel {
  uint32_t magic;          // 'OOHO'
  uint16_t version;        // start at 1
  uint16_t envBins;        // 64
  uint32_t voiceCount;
  float meanDurationSec;
  float meanRms;
  float meanPeak;
  float meanLogF0;
  float varLogF0;
  float voicedRatio;
  float envelope[64];
  float bands[12];
};
```

Use either a true cumulative average:

```text
new_mean = old_mean + (x - old_mean) / voice_count
```

or a slow adaptive average:

```text
new_mean = 0.92 * old_mean + 0.08 * x
```

The adaptive version is more alive in the field because recent visitors can slowly change the object; the cumulative version is more archival and stable.

### Envelope analysis

For `Collective Breath v1`, avoid frame scheduling complexity. Map every sample into one of 64 time-normalized bins:

```text
bin = sample_index * 64 / total_samples
envelope[bin] += sample * sample
```

After the pass:

```text
envelope[bin] = sqrt(envelope[bin] / samples_in_bin)
normalize by recording peak or recording RMS
```

Then update the global average envelope. This preserves the gesture of a whisper: the attack, hesitation, breath, decay, and silence.

### F0 analysis

For `Object Voice v2`, run F0 after the raw WAV has already been saved.

Practical approach:

- Downsample analysis only, not the stored WAV, by taking every 4th sample or low-pass + decimate if time allows.
- Use 1024-sample analysis frames at the decimated rate, with 50 percent overlap.
- Compute RMS first; skip quiet frames.
- Compute autocorrelation over plausible voice range, for example 70-350 Hz.
- Accept a frame only when the best autocorrelation peak is above a confidence threshold.
- Store median/log-mean F0, not arithmetic mean F0.

Whisper can be partly unvoiced, so `voicedRatio` matters as much as pitch. If `voicedRatio` is low, the collective surface should become breath/noise-heavy instead of forcing a fake pitch.

### Spectral-band analysis

For `Formant Ghost v2/v3`, do not attempt exact formant tracking first. Use fixed bands:

```text
80-150 Hz
150-250 Hz
250-400 Hz
400-650 Hz
650-1000 Hz
1.0-1.5 kHz
1.5-2.2 kHz
2.2-3.2 kHz
3.2-4.6 kHz
4.6-6.5 kHz
6.5-9.0 kHz
9.0-12.0 kHz
```

These bands are enough to make a changing vowel/body color without pretending to reconstruct a person.

### Rendering `COLLECT.WAV`

For the first build, render a 30-60 second mono WAV after every new voice. That file can be played on the right/exciter channel in idle.

Sound recipe:

```text
source = triangle/sine at collective F0
       + filtered noise controlled by unvoiced/breath ratio
       + slow random detune

amplitude = collective envelope, stretched to render length
filtering = broad voice-band filter + optional cube-resonance EQ
```

For DAEX58P/DAEX58FP on metal, start conservative:

- high-pass around 80-120 Hz so the exciter does not waste power on sub-bass;
- low-pass around 5-8 kHz if the cube becomes too metallic;
- use a soft limiter before writing the WAV;
- fade in/out every generated file by at least 20 ms.

### Left/right playback logic

Idle state can run a simple ritual:

```text
right/exciter: loop COLLECT.WAV quietly or in slow pulses
left/internal: occasionally play one raw Wxxxx.WAV inside the sealed box
```

For the internal speaker, avoid playing constantly. Sparse playback makes the cube feel inhabited rather than like a normal speaker.

Suggested intervals:

```text
every 20-90 seconds: play one internal archive voice
right surface layer: almost continuous but quiet
```

## 7. Test Protocol

Each field firmware version should pass these tests before going into the wall:

1. Record 10 short whispers and verify all `Wxxxx.WAV` files open on a laptop.
2. Confirm there is no SD ticking in the raw WAV files.
3. Confirm `MODEL.BIN` updates after each recording.
4. Confirm `LOG.CSV` has duration, RMS, peak, voiced ratio, and F0 fields, even if F0 is `0` or `nan`.
5. Confirm `COLLECT.WAV` changes audibly after several different voices.
6. Run for one hour from powerbank with idle playback active.
7. Power-cut during idle and during post-record saving; confirm old WAV files survive and temporary files can be ignored.

## 8. Concrete First Firmware Milestone

Milestone name:

**Collective Breath v1**

Features:

- Record-only button remains unchanged.
- LED on only during recording.
- After stop, save the WAV.
- Analyze RMS envelope into 64 bins.
- Update average envelope and average loudness.
- Generate a 30-second `COLLECT.WAV`:
  - source: low sine/triangle around 120 Hz or current mean F0 if available
  - add low-level filtered noise for breath
  - amplitude follows average envelope
  - write mono or stereo WAV for right/exciter playback

Why this first:

- Very likely to work on Teensy.
- Minimal risk.
- The result will already express the "average voice" idea through pressure and breath.
- F0/formant/granular layers can be added after the basic ritual works.

## 9. Second Firmware Milestone

Milestone name:

**Object Voice v2**

Add:

- F0 estimation using autocorrelation on decimated frames.
- Voiced/unvoiced classification.
- Running log-mean F0 and spread.
- Synth voice source follows model pitch.
- Breath noise level follows unvoiced ratio.

Expected result:

- The exciter stops being a generic drone and starts sounding like a collective larynx.

## 10. Third Firmware Milestone

Milestone name:

**Granular Archive v3**

Add:

- Grain index over stored WAV files.
- Non-silent snippet extraction.
- Fade-in/fade-out grains.
- Optional pre-rendered outside texture.

Expected result:

- The surface contains traces of real people, but not enough to become direct playback.
- The inside/outside distinction becomes sharper: inside has voices, outside has fragments and residue.

## 11. Practical Warnings

- Do not attempt machine-learning voice averaging on Teensy. It is unnecessary and would add fragility.
- Do not chase precise formants in the first field build. Broad spectral bands are more robust.
- Whisper often lacks reliable F0. Treat F0 as one contribution, not the whole identity of the voice.
- Preserve anonymity by making outside grains shorter than semantic units.
- Keep SD writes outside recording.
- Keep the output model inspectable as WAV/CSV so field failures are debuggable.
- Do not let the external surface layer become understandable language unless you explicitly want to break anonymity.
- Do not make the internal speaker too loud; if it leaks clearly through the cube, the inside/outside distinction collapses.

## 12. Short Conceptual Statement

The Ooho II does not replay a visitor's voice to them. It accepts the voice into a sealed social body. Inside the cube, voices remain concrete and plural. On the surface, the cube returns only an anonymous pressure: averaged breath, unstable pitch, shared resonance. The listener outside does not receive a message; they encounter the residue of speech after meaning has been removed.
