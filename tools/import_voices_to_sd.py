#!/usr/bin/env python3
import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SAMPLE_RATE = 44100
MAX_SECONDS = 30
MODEL_MAGIC = 0x4F4F484F
MODEL_VERSION = 2
MODEL_WEIGHT = 0.10
DEFAULT_F0_HZ = 160.0
DEFAULT_VOICED_RATIO = 0.5
DEFAULT_RMS = 0.08
ENV_BINS = 64
BAND_COUNT = 8
LOGOS_WINDOW_SAMPLES = 661
LOGOS_GAP_TOLERANCE_MS = 60
LOGOS_MIN_DURATION_MS = 120
LOGOS_MAX_DURATION_MS = 1800
LOGOS_SPLIT_DURATION_MS = 1200
LOGOS_MIN_RMS_THRESHOLD = 0.008
LOGOS_MAX_RMS_THRESHOLD = 0.090
LOGOS_WINDOW_PEAK_FACTOR = 0.18
LOGOS_HYST_FACTOR = 0.45
LOGOS_HISS_CONTINUE_FACTOR = 1.6
LOGOS_SPLIT_SEARCH_MS = 260
LOGOS_ZC_SEARCH_SAMPLES = 256
ANALYSIS_RATE = 11025
F0_FRAME_SAMPLES = 1024
F0_HOP_SAMPLES = 512
F0_RMS_THRESHOLD = 0.012
F0_CONFIDENCE_THRESHOLD = 0.28
F0_MIN_HZ = 70.0
F0_MAX_HZ = 350.0
MAX_F0_VALUES = 256
MODEL_STRUCT = "<IH2xIffff64f8f"


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def find_sd_root():
    volumes = Path("/Volumes")
    if not volumes.exists():
        return None
    candidates = []
    for child in volumes.iterdir():
        if child.name in ("Macintosh HD", "com.apple.TimeMachine.localsnapshots"):
            continue
        if (child / "COUNTER.TXT").exists() or (child / "GRAINS.CSV").exists() or (child / "MODEL.BIN").exists():
            candidates.append(child)
    return candidates[0] if len(candidates) == 1 else None


def wav_sources(voices_dir):
    return sorted(p for p in voices_dir.iterdir() if p.is_file() and p.suffix.lower() == ".wav")


def convert_to_field_wav(src, dst):
    cmd = [
        "sox",
        str(src),
        "-b",
        "16",
        "-c",
        "1",
        "-r",
        str(SAMPLE_RATE),
        str(dst),
        "trim",
        "0",
        str(MAX_SECONDS),
    ]
    subprocess.run(cmd, check=True)


def read_samples(path):
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getframerate() != SAMPLE_RATE:
            die(f"{path} is not mono 16-bit 44100 WAV after conversion")
        frames = wav.readframes(wav.getnframes())
    return list(struct.unpack("<" + "h" * (len(frames) // 2), frames))


def rms_window(samples, start, count):
    ss = 0.0
    end = min(len(samples), start + count)
    if end <= start:
        return 0.0
    for s in samples[start:end]:
        x = s / 32768.0
        ss += x * x
    return math.sqrt(ss / (end - start))


def snap_to_zero(samples, pos, radius):
    total = len(samples)
    if total == 0:
        return 0
    best = min(pos, total - 1)
    best_abs = abs(samples[best])
    lo = pos - radius if pos > radius else 0
    hi = min(total - 1, pos + radius)
    for i in range(lo, hi + 1):
        a = abs(samples[i])
        if a < best_abs:
            best_abs = a
            best = i
            if a == 0:
                break
    return best


def emit_region_grains(samples, filename, s0, s1):
    grains = []
    total = len(samples)
    samples_per_ms = SAMPLE_RATE // 1000
    s0 = snap_to_zero(samples, s0, LOGOS_ZC_SEARCH_SAMPLES)
    s1 = snap_to_zero(samples, s1, LOGOS_ZC_SEARCH_SAMPLES)
    if s1 <= s0:
        return grains
    min_samples = LOGOS_MIN_DURATION_MS * samples_per_ms
    max_samples = LOGOS_MAX_DURATION_MS * samples_per_ms
    split_samples = LOGOS_SPLIT_DURATION_MS * samples_per_ms

    def append(start, end):
        duration_ms = (end - start) * 1000 // SAMPLE_RATE
        if LOGOS_MIN_DURATION_MS <= duration_ms <= LOGOS_MAX_DURATION_MS:
            grains.append((filename, start * 1000 // SAMPLE_RATE, duration_ms))

    if s1 - s0 <= max_samples:
        append(s0, s1)
        return grains

    cursor = s0
    while s1 - cursor > max_samples:
        target = cursor + split_samples
        search = LOGOS_SPLIT_SEARCH_MS * samples_per_ms
        lo = target - search if target > cursor + min_samples + search else cursor + min_samples
        hi = min(s1 - min_samples, target + search)
        if hi <= lo:
            break
        best = lo
        best_rms = None
        step = LOGOS_WINDOW_SAMPLES
        p = lo
        while p <= hi:
            r = rms_window(samples, p, step)
            if best_rms is None or r < best_rms:
                best_rms = r
                best = p
            p += step
        cut = snap_to_zero(samples, best, LOGOS_ZC_SEARCH_SAMPLES)
        if cut <= cursor + min_samples:
            cut = cursor + max_samples
        append(cursor, min(cut, total))
        cursor = min(cut, total)
    if s1 > cursor:
        append(cursor, s1)
    return grains


def segment_grains(samples, filename):
    if len(samples) < LOGOS_WINDOW_SAMPLES:
        return []
    window_count = len(samples) // LOGOS_WINDOW_SAMPLES
    rms = []
    total_energy = 0.0
    max_rms = 0.0
    for w in range(window_count):
        base = w * LOGOS_WINDOW_SAMPLES
        r = rms_window(samples, base, LOGOS_WINDOW_SAMPLES)
        rms.append(r)
        max_rms = max(max_rms, r)
        total_energy += r * r * LOGOS_WINDOW_SAMPLES
    start_thr = clamp(max_rms * LOGOS_WINDOW_PEAK_FACTOR, LOGOS_MIN_RMS_THRESHOLD, LOGOS_MAX_RMS_THRESHOLD)
    cont_thr = start_thr * LOGOS_HYST_FACTOR
    # Same as firmware: the continue threshold stays above the take's hiss
    # (10th-percentile window), so quiet whispers do not become one hiss grain.
    hiss = sorted(rms)[len(rms) // 10]
    cont_thr = max(cont_thr, hiss * LOGOS_HISS_CONTINUE_FACTOR)
    start_thr = max(start_thr, cont_thr * 1.05)
    gap_windows = max(1, (LOGOS_GAP_TOLERANCE_MS * (SAMPLE_RATE // 1000)) // LOGOS_WINDOW_SAMPLES)

    grains = []
    w = 0
    while w < window_count:
        if rms[w] <= cont_thr:
            w += 1
            continue
        run_start = w
        last_above = w
        has_core = False
        gap = 0
        while w < window_count:
            if rms[w] > cont_thr:
                if rms[w] > start_thr:
                    has_core = True
                last_above = w
                gap = 0
            else:
                gap += 1
                if gap > gap_windows:
                    break
            w += 1
        if has_core:
            s0 = run_start * LOGOS_WINDOW_SAMPLES
            s1 = min(len(samples), (last_above + 1) * LOGOS_WINDOW_SAMPLES)
            grains.extend(emit_region_grains(samples, filename, s0, s1))
    return grains


def analyze_envelope_rms(samples):
    total_energy = 0.0
    bin_energy = [0.0] * ENV_BINS
    bin_counts = [0] * ENV_BINS
    n = len(samples)
    for i, s in enumerate(samples):
        x = s / 32768.0
        e = x * x
        total_energy += e
        b = min(ENV_BINS - 1, (i * ENV_BINS) // n)
        bin_energy[b] += e
        bin_counts[b] += 1
    rms = math.sqrt(total_energy / n) if n else 0.0
    env = []
    max_env = 0.0001
    for e, c in zip(bin_energy, bin_counts):
        v = math.sqrt(e / c) if c else 0.0
        env.append(v)
        max_env = max(max_env, v)
    env = [v / max_env for v in env]
    return env, rms


def estimate_f0(samples):
    dec = samples[::4]
    if len(dec) < F0_FRAME_SAMPLES:
        return 0.0, 0.0
    min_lag = int(ANALYSIS_RATE / F0_MAX_HZ)
    max_lag = int(ANALYSIS_RATE / F0_MIN_HZ)
    accepted = []
    total_frames = 0
    voiced_frames = 0
    for start in range(0, len(dec) - F0_FRAME_SAMPLES + 1, F0_HOP_SAMPLES):
        frame = [x / 32768.0 for x in dec[start : start + F0_FRAME_SAMPLES]]
        total_frames += 1
        energy = sum(x * x for x in frame)
        rms = math.sqrt(energy / F0_FRAME_SAMPLES)
        if rms < F0_RMS_THRESHOLD:
            continue
        voiced_frames += 1
        best_corr = 0.0
        best_lag = min_lag
        for lag in range(min_lag, max_lag + 1):
            ac = 0.0
            e0 = 0.0
            e1 = 0.0
            for i in range(F0_FRAME_SAMPLES - lag):
                a = frame[i]
                b = frame[i + lag]
                ac += a * b
                e0 += a * a
                e1 += b * b
            denom = math.sqrt(e0 * e1) if e0 > 0 and e1 > 0 else 0.0
            corr = ac / denom if denom > 0 else 0.0
            if corr > best_corr:
                best_corr = corr
                best_lag = lag
        if best_corr > F0_CONFIDENCE_THRESHOLD and len(accepted) < MAX_F0_VALUES:
            accepted.append(ANALYSIS_RATE / best_lag)
    voiced_ratio = voiced_frames / total_frames if total_frames else 0.0
    if not accepted:
        return 0.0, voiced_ratio
    accepted.sort()
    return accepted[len(accepted) // 2], voiced_ratio


def analyze_bands(samples):
    centers = [175.0, 375.0, 700.0, 1200.0, 2000.0, 3250.0, 5000.0, 8500.0]
    total = len(samples)
    stride = max(1, total // 4096)
    count = total // stride
    if count < 32:
        return [1.0 / BAND_COUNT] * BAND_COUNT
    energies = []
    for center in centers:
        omega = 2.0 * math.pi * center * stride / SAMPLE_RATE
        coeff = 2.0 * math.cos(omega)
        q1 = q2 = 0.0
        for i in range(count):
            s = samples[i * stride] / 32768.0
            q0 = coeff * q1 - q2 + s
            q2 = q1
            q1 = q0
        energies.append(max(0.0, q1 * q1 + q2 * q2 - coeff * q1 * q2))
    total_energy = sum(energies)
    if total_energy <= 0:
        return [1.0 / BAND_COUNT] * BAND_COUNT
    return [e / total_energy for e in energies]


def default_model():
    mean = math.log(DEFAULT_F0_HZ)
    env = [0.35 + 0.25 * math.sin((i / (ENV_BINS - 1)) * math.pi) for i in range(ENV_BINS)]
    bands = [1.0 / BAND_COUNT] * BAND_COUNT
    return {
        "magic": MODEL_MAGIC,
        "version": MODEL_VERSION,
        "voiceCount": 0,
        "meanLogF0": mean,
        "meanLogF0Sq": mean * mean,
        "voicedRatio": DEFAULT_VOICED_RATIO,
        "meanRms": DEFAULT_RMS,
        "envelope": env,
        "bands": bands,
    }


def load_model(path):
    if not path.exists() or path.stat().st_size < struct.calcsize(MODEL_STRUCT):
        return default_model()
    values = struct.unpack(MODEL_STRUCT, path.read_bytes()[: struct.calcsize(MODEL_STRUCT)])
    model = {
        "magic": values[0],
        "version": values[1],
        "voiceCount": values[2],
        "meanLogF0": values[3],
        "meanLogF0Sq": values[4],
        "voicedRatio": values[5],
        "meanRms": values[6],
        "envelope": list(values[7 : 7 + ENV_BINS]),
        "bands": list(values[7 + ENV_BINS : 7 + ENV_BINS + BAND_COUNT]),
    }
    if model["magic"] != MODEL_MAGIC or model["version"] != MODEL_VERSION:
        return default_model()
    return model


def save_model(path, model):
    values = [
        MODEL_MAGIC,
        MODEL_VERSION,
        int(model["voiceCount"]),
        float(model["meanLogF0"]),
        float(model["meanLogF0Sq"]),
        float(model["voicedRatio"]),
        float(model["meanRms"]),
        *[float(x) for x in model["envelope"]],
        *[float(x) for x in model["bands"]],
    ]
    path.write_bytes(struct.pack(MODEL_STRUCT, *values))


def update_model(model, samples):
    env, rms = analyze_envelope_rms(samples)
    median_f0, incoming_voiced = estimate_f0(samples)
    bands = analyze_bands(samples)
    w = MODEL_WEIGHT
    model["voiceCount"] += 1
    model["meanRms"] = model["meanRms"] * (1.0 - w) + rms * w
    model["voicedRatio"] = clamp(model["voicedRatio"] * (1.0 - w) + incoming_voiced * w, 0.0, 1.0)
    if median_f0 > 0 and math.isfinite(median_f0):
        lf = math.log(median_f0)
        model["meanLogF0"] = model["meanLogF0"] * (1.0 - w) + lf * w
        model["meanLogF0Sq"] = model["meanLogF0Sq"] * (1.0 - w) + lf * lf * w
    model["envelope"] = [a * (1.0 - w) + b * w for a, b in zip(model["envelope"], env)]
    model["bands"] = [a * (1.0 - w) + b * w for a, b in zip(model["bands"], bands)]
    return median_f0, incoming_voiced, rms


def next_index(sd_root):
    counter = sd_root / "COUNTER.TXT"
    if counter.exists():
        try:
            return int(counter.read_text().strip())
        except ValueError:
            pass
    max_idx = 0
    for path in sd_root.glob("W*.WAV"):
        name = path.name
        if len(name) == 9 and name[0] == "W" and name[1:5].isdigit():
            max_idx = max(max_idx, int(name[1:5]))
    return max_idx + 1


def main():
    parser = argparse.ArgumentParser(description="Import desktop VOICES WAV files into an Ooho SD card.")
    parser.add_argument("--voices", default="/Users/menemene/Desktop/OOHO/VOICES")
    parser.add_argument("--sd-root", default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    voices_dir = Path(args.voices)
    if not voices_dir.exists():
        die(f"voices folder missing: {voices_dir}")
    sd_root = Path(args.sd_root) if args.sd_root else find_sd_root()
    if sd_root is None:
        die("SD card not found. Insert/mount it, or pass --sd-root /Volumes/NAME")
    if not sd_root.exists():
        die(f"SD root does not exist: {sd_root}")
    if not shutil.which("sox"):
        die("sox is required but was not found")

    sources = wav_sources(voices_dir)
    if not sources:
        die(f"no .wav files found in {voices_dir}")

    start = next_index(sd_root)
    model = load_model(sd_root / "MODEL.BIN")
    grains_path = sd_root / "GRAINS.CSV"
    planned = []
    all_new_grains = []

    with tempfile.TemporaryDirectory() as tmp:
      tmpdir = Path(tmp)
      for offset, src in enumerate(sources):
        filename = f"W{start + offset:04d}.WAV"
        converted = tmpdir / filename
        convert_to_field_wav(src, converted)
        samples = read_samples(converted)
        grains = segment_grains(samples, filename)
        median_f0, voiced, rms = update_model(model, samples)
        planned.append((src.name, filename, len(samples) / SAMPLE_RATE, len(grains), median_f0, voiced, rms, converted))
        all_new_grains.extend(grains)

      print(f"SD: {sd_root}")
      print(f"Importing {len(planned)} WAV files starting at W{start:04d}.WAV")
      for src_name, filename, dur, grain_count, median_f0, voiced, rms, _converted in planned:
        print(f"  {filename}: {dur:.2f}s grains={grain_count} f0={median_f0:.1f} voiced={voiced:.3f} rms={rms:.4f} <- {src_name}")

      if args.dry_run:
        print("dry run; no files written")
        return

      for _src_name, filename, _dur, _grain_count, _median_f0, _voiced, _rms, converted in planned:
        shutil.copyfile(converted, sd_root / filename)

      with grains_path.open("a", newline="\n") as grains_file:
        for filename, start_ms, duration_ms in all_new_grains:
          grains_file.write(f"{filename},{start_ms},{duration_ms}\n")

      save_model(sd_root / "MODEL.BIN", model)
      (sd_root / "COUNTER.TXT").write_text(f"{start + len(planned)}\n")
      print(f"Done. Added {len(planned)} WAV files and {len(all_new_grains)} grains.")
      print(f"COUNTER.TXT -> {start + len(planned)}")


if __name__ == "__main__":
    main()
