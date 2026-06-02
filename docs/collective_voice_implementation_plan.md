# The Ooho II - Collective Voice Implementation Plan

## Summary

Target first milestone: **Collective Breath + Playback v1**.

The public interaction remains simple: a visitor records a whisper, but does not get direct replay. The device stores the voice inside the sealed body and updates a slowly changing collective surface voice.

Core behavior:

```text
record
  -> save Wxxxx.WAV
  -> analyze PSRAM buffer
  -> update MODEL.BIN
  -> append LOG.CSV
  -> render COLLECT.WAV
  -> return to idle playback
```

Stereo output becomes two different social spaces:

```text
LINE OUT L -> internal speaker -> concrete archived voices
LINE OUT R -> surface exciter  -> collective anonymous voice
```

## Data Files

Use short names for Teensy Audio Library compatibility and simple SD recovery.

```text
W0001.WAV, W0002.WAV, ...  raw archived recordings
T0001.WAV                  temporary raw recording while saving
MODEL.BIN                  binary collective model
MODEL.TMP                  temporary model while saving
LOG.CSV                    human-readable analysis log
COLLECT.WAV                rendered surface voice
CTEMP.WAV                  temporary collective render while saving
```

Write temp files first, close/sync them, then rename to final names. If power is lost, old final files should remain usable and temp files can be ignored or replaced.

## Model

Model v1 stores the slowly changing surface layer. Raw voices remain archived as WAV files.

```cpp
struct CollectiveModel {
  uint32_t magic;          // 'OOHO'
  uint16_t version;        // 1
  uint16_t envBins;        // 64
  uint32_t voiceCount;
  float meanDurationSec;
  float meanRms;
  float meanPeak;
  float meanLogF0;         // reserved in v1
  float varLogF0;          // reserved in v1
  float voicedRatio;       // reserved in v1
  float envelope[64];
  float bands[12];         // reserved in v1
};
```

For v1, implement RMS/peak/envelope fully. Initialize F0 and bands as reserved fields so v2 can add pitch and spectral color without changing the file shape immediately.

Use adaptive memory for the surface layer:

```text
new_value = old_value * 0.92 + incoming_value * 0.08
```

This means the surface changes noticeably but slowly. It behaves like an organism, while the raw archive remains exact and historical.

Ignore recordings for the surface model if they are too short or nearly silent:

```text
duration < 0.5 sec
or peak below a conservative silence threshold
```

Still save the raw WAV and write a log row with `ignored_for_model=1`.

## Analysis

Run analysis after recording has stopped and after the raw WAV is saved. Do not write to SD while recording.

For **Collective Breath v1**:

1. Read the captured samples from `activeRecordBuffer`.
2. Compute total duration, RMS, and peak.
3. Map all samples into 64 time-normalized bins:

```text
bin = sample_index * 64 / total_samples
envelope[bin] += sample * sample
```

4. Convert each bin to RMS:

```text
envelope[bin] = sqrt(envelope[bin] / samples_in_bin)
```

5. Normalize the envelope shape by peak or RMS, while storing loudness separately.
6. Update `MODEL.BIN`.
7. Append `LOG.CSV`.

Suggested CSV columns:

```text
index,filename,duration_ms,rms,peak,ignored_for_model,model_voice_count
```

Reserved future columns:

```text
voiced_ratio,median_f0,centroid
```

## Rendering `COLLECT.WAV`

Render a 30-second mono 44.1 kHz / 16-bit WAV after each valid model update.

Sound recipe for v1:

```text
source = low triangle/sine around 120 Hz
       + low-level pseudo-random breath noise

amplitude = collective envelope stretched across render length
output    = soft-limited 16-bit PCM
```

Apply:

- 20 ms fade in/out;
- conservative soft limiting;
- high-pass behavior by avoiding sub-bass-heavy synthesis;
- low enough output level that the DAEX surface layer does not become harsh or rattly.

The first implementation can render `COLLECT.WAV` directly to SD after recording, because this happens after capture and cannot contaminate the recorded voice.

## Idle Playback

Implement an idle scheduler after `COLLECT.WAV` exists.

Surface channel:

```text
right/exciter: loop or pulse COLLECT.WAV quietly
```

Internal channel:

```text
left/internal speaker: play real Wxxxx.WAV files in organism-like conversation clusters
```

Conversation behavior:

```text
cluster phase: 60-150 sec
  play 3-7 random voices
  gaps between voices: 2-10 sec

rest phase: 90-240 sec
  mostly silent
  small chance of one isolated voice
```

Recording must interrupt all idle playback immediately.

## Audio Routing Changes

Current firmware mirrors one mixer to both stereo outputs. Collective voice requires independent routing.

Planned routing:

```text
AudioPlaySdWav archivePlayer -> left mixer  -> AudioOutputI2S left
AudioPlaySdWav collectPlayer -> right mixer -> AudioOutputI2S right
diagnostic tone              -> both mixers
```

The first implementation may keep raw playback mono for testing, but the acceptance test requires channel separation before installation:

```text
left output  = archived voices only
right output = collective surface voice only
```

## Test Plan

Bench tests:

1. Compile `the_ooho_rev1` for Teensy 4.1.
2. Record 5-10 whispers; verify each `Wxxxx.WAV` opens on a laptop.
3. Confirm raw WAV files do not contain SD ticking.
4. Confirm `MODEL.BIN` appears and changes after valid recordings.
5. Confirm `LOG.CSV` gets one row per recording.
6. Confirm `COLLECT.WAV` appears and changes after several different voices.
7. Confirm recording interrupts idle playback cleanly.
8. Confirm left/right channel separation with headphones or stereo amp.
9. Run from powerbank for at least 30-60 minutes.
10. Power-cut during idle and after recording; old final files should remain usable.

Field acceptance:

- A visitor cannot directly replay their own recording.
- The box contains and sometimes plays concrete voices internally.
- The surface exciter produces a changing collective layer, not intelligible speech.
- The system stays stable without reboot during a field session.

## Future Milestones

**Object Voice v2**

- Add F0 estimation using post-record autocorrelation.
- Track voiced/unvoiced ratio.
- Use log-mean F0 to tune the synthetic source.
- Increase breath/noise when a recording is mostly unvoiced.

**Formant Ghost v2/v3**

- Add fixed spectral-band analysis.
- Use broad filter color rather than exact formant tracking.

**Granular Archive v3**

- Index non-silent grains from raw WAV files.
- Render short anonymous fragments into the outside layer.
- Keep grains shorter than semantic units to preserve anonymity.
