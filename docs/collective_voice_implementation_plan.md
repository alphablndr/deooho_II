# The Ooho II - Collective Voice Implementation Plan

## Summary

Target artistic milestone: **LOGOS/MELOS Field v1**.

Practical rule: bring up the system incrementally before running the full firmware. See `docs/bringup_pipeline.md`.

The public interaction remains simple: a visitor records a whisper, but does not get direct replay. The device stores the voice inside the sealed body and updates a slowly changing collective surface voice.

Core behavior:

```text
record
  -> save Wxxxx.WAV
  -> analyze PSRAM buffer
  -> update MODEL.BIN
  -> append GRAINS.CSV
  -> render or schedule MELOS.WAV update
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
GRAINS.CSV                 LOGOS grain index: filename,start_ms,duration_ms
MODEL.BIN                  binary collective model
MODEL.TMP                  temporary model while saving
MELOS.WAV                  rendered surface voice
MTEMP.WAV                  temporary MELOS render while saving
MNEXT.WAV                  experimental next MELOS render, disabled in current stable build
MNEXT.TMP                  temporary next MELOS render, disabled in current stable build
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
7. Append LOGOS candidates to `GRAINS.CSV`.

Current `GRAINS.CSV` columns:

```text
filename,start_ms,duration_ms
```

Reserved future `LOG.CSV` analysis columns, if a separate human-readable analysis log is added:

```text
index,filename,duration_ms,rms,peak,ignored_for_model,model_voice_count
voiced_ratio,median_f0,centroid
```

## Rendering `MELOS.WAV`

Render a mono 44.1 kHz / 16-bit WAV after each valid model update.

Use `MELOS_SECONDS = 30` for debug builds. Restore the intended long field render, `MELOS_SECONDS = 480`, only after the short build passes the whole pipeline.

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

The implementation can render `MELOS.WAV` directly to SD after recording, because this happens after capture and cannot contaminate the recorded voice.

Current Stage 8 decision:

```text
If MELOS is already playing, do not render MELOS.WAV immediately.
Print: MELOS render deferred to keep playback stable.
```

Reason: rendering on the same SD card while reading playback caused crunch and rhythmic modulation. The model still updates immediately, but the audible MELOS file is not automatically regenerated while continuous playback is prioritized.

## Idle Playback

Implement an idle scheduler after `MELOS.WAV` exists.

MELOS/right surface channel:

```text
right/exciter: loop MELOS.WAV quietly
```

LOGOS/left internal channel:

```text
left/internal speaker: play word-length real grains indexed in GRAINS.CSV
```

Current LOGOS field behavior:

```text
stream 1: always active, grains every 45-90 sec
stream 2/3: independently switch active/dormant every 30-90 sec
  dormant interval: 60-120 sec
  active interval: 2-8 sec
25% chance of one deconstructed repeat at +80-200 ms inside the word
```

Current tested behavior: playback may continue during recording. This is acceptable because the cube speaker/exciter is physically far from the recording ear in the current installation layout.

Processing after release may either:

- keep playback running and defer MELOS rendering;
- or intentionally pause/update MELOS if the field version accepts that gesture.

Do not write to SD during recording in either case.

## Audio Routing Changes

Current firmware mirrors one mixer to both stereo outputs. Collective voice requires independent routing.

Planned routing:

```text
AudioPlayQueue LOGOS streams -> left mixer  -> AudioOutputI2S left
AudioPlayQueue MELOS player  -> right mixer -> AudioOutputI2S right
diagnostic tone              -> both mixers
```

Use manual `FsFile`/SdFat seeking plus `AudioPlayQueue` for LOGOS grains and MELOS random-start playback. `AudioPlaySdWav` is acceptable in small test sketches for full-file playback only.

Acceptance test before installation:

```text
left output  = archived voices only
right output = collective surface voice only
```

## Test Plan

Bench tests:

1. Pass `button_led_test`.
2. Pass `tone_button_test`.
3. Pass `sd_smoke_test`.
4. Pass `psram_memtest`.
5. Pass `record_raw_test`: record 5-10 whispers and verify each `Wxxxx.WAV` opens on a laptop.
6. Confirm raw WAV files do not contain SD ticking.
7. Confirm `GRAINS.CSV` gets plausible word-length rows.
8. Compile and run short `the_ooho_rev1` with `MELOS_SECONDS = 30`.
9. Confirm `MODEL.BIN` appears and changes after valid recordings.
10. Confirm `MELOS.WAV` appears and changes after several different voices.
11. Confirm recording either keeps idle playback stable or intentionally pauses it according to the selected field behavior.
12. Confirm left/right channel separation with headphones or stereo amp.
13. Restore long MELOS only after the short build is stable.
14. Run from powerbank for at least 30-60 minutes.
15. Power-cut during idle and after recording; old final files should remain usable.

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

**LOGOS Grain Expansion**

- Keep indexing word-length grains from raw WAV files.
- Expand or tune the three-stream scheduler after one-stream playback is stable.
- Keep grains sparse enough that silence remains structural.

**MELOS Update Strategy**

- Same-card `MNEXT.WAV` double-buffer/crossfade was tested in Stage 8 and disabled.
- It caused crunch/glitch after release and rhythmic modulation across both output channels.
- Next attempt should either use two SD cards or a simpler intentional update pause.
- Candidate two-card split:
  - built-in Teensy SD: recording archive, `GRAINS.CSV`, `MODEL.BIN`, processing writes;
  - Audio Shield SPI SD: playback-only `MELOS.WAV`.
