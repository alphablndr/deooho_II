#!/usr/bin/env python3
"""SPIKE (throwaway, 2026-10-14): LOGOS experiments on a computer.

Question: if real voices are cut into syllables (not into pauses-delimited
chunks) and recombined by how they sound, do we get "almost-speech without
words" that is more alive than random grain playback?

Only original material: no speed change, no pitch shift. Units are cut,
reordered, faded, level-matched and sometimes reversed (Anton: keep reverse).

  A  corpus     cut every recording into syllables + descriptors
                (pitch, voiced share, brightness, loudness, length)
  B  collective one continuous speaker made of syllables of many people,
                each next syllable continuing the melody/timbre of the last
  C  transplant the rhythm and melody of one person's phrase, every syllable
                replaced by a matching syllable of someone else

Writes WAVs for listening (e.g. in Ableton) and a corpus CSV. Needs numpy.

  python3 lab/logos_lab.py ../RECSREWIRE ../lab_renders/2026-10-14
"""
import argparse
import csv
import glob
import os
import wave

import numpy as np

HOP_S = 0.010             # envelope hop
WIN_S = 0.030             # envelope window
BAND = (250.0, 3800.0)    # speech band for the envelope (ignores rumble and hiss)
MIN_SYL_S, MAX_SYL_S = 0.06, 0.70
PEAK_OVER_FLOOR = 3.0     # a syllable nucleus is at least this much above the hiss
EDGE_OVER_FLOOR = 1.8     # a syllable ends where the envelope falls below this
MIN_PEAK_GAP_S = 0.11     # two nuclei closer than this are one syllable
DIP_RATIO = 0.70          # the valley between two nuclei must dip below 70 % of the smaller one
FADE_S = 0.010
REVERSE_B = 0.20          # share of syllables played backwards in B
REVERSE_C = 0.10          # ...and in C (lower: keeps the phrase's melody readable)
AIR_MAX_S = 0.12          # 'own air': quiet edge of its own take kept around a syllable
AIR_QUIET = 1.6           # ...only while the take stays below this x its background
F0_RANGE = (70.0, 400.0)


# ---------------------------------------------------------------- audio io

def load_mono(path):
    with wave.open(path, "rb") as w:
        sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x - x.mean(), sr


def save(path, x, sr):
    peak = np.max(np.abs(x)) if len(x) else 0.0
    if peak > 0.98:
        x = x * (0.98 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((x * 32767.0).astype("<i2").tobytes())


def clean_teensy_take(x, sr):
    """What the current firmware does to a fresh take (stage A), for older
    Teensy recordings: drop the last 100 ms (button click) and subtract the
    344.5 Hz block-rate hum, using the quieter half of the 128-sample blocks."""
    x = x[:max(0, len(x) - int(0.100 * sr))]
    n = len(x) // 128
    if n >= 8:
        blocks = x[:n * 128].reshape(n, 128)
        energy = (blocks ** 2).sum(axis=1)
        hum = blocks[energy <= np.median(energy)].mean(axis=0)
        x = x - np.tile(hum, len(x) // 128 + 1)[:len(x)]
    return x - x.mean()


def bandpass(x, sr, lo, hi):
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    mask = np.clip((f - lo * 0.7) / (lo * 0.3), 0, 1) * np.clip((hi * 1.2 - f) / (hi * 0.2), 0, 1)
    return np.fft.irfft(spec * mask, n=len(x))


def faded(seg, sr):
    seg = seg.copy()
    n = min(int(FADE_S * sr), len(seg) // 2)
    if n > 0:
        ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
        seg[:n] *= ramp
        seg[-n:] *= ramp[::-1]
    return seg


def rms(x):
    return float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0


# ---------------------------------------------------------------- A: syllables

def envelope(x, sr):
    xb = bandpass(x, sr, *BAND)
    hop, win = int(HOP_S * sr), int(WIN_S * sr)
    n = max(0, (len(xb) - win) // hop + 1)
    env = np.array([rms(xb[i * hop:i * hop + win]) for i in range(n)])
    if len(env) >= 3:
        env = np.convolve(env, np.ones(3) / 3, mode="same")
    return env, hop


def find_syllables(x, sr):
    """Syllable = one loudness nucleus, cut at the valleys between nuclei."""
    env, hop = envelope(x, sr)
    if len(env) < 5:
        return []
    floor = np.percentile(env, 10) + 1e-9
    min_gap = int(MIN_PEAK_GAP_S / HOP_S)
    cand = [i for i in range(1, len(env) - 1)
            if env[i] >= env[i - 1] and env[i] > env[i + 1] and env[i] > floor * PEAK_OVER_FLOOR]
    peaks = []
    for i in cand:  # keep the louder of two nuclei that are too close
        if peaks and i - peaks[-1] < min_gap:
            if env[i] > env[peaks[-1]]:
                peaks[-1] = i
        else:
            peaks.append(i)
    merged = []
    for i in peaks:  # neighbours without a real dip between them are one syllable
        if merged and env[merged[-1]:i + 1].min() > DIP_RATIO * min(env[merged[-1]], env[i]):
            if env[i] > env[merged[-1]]:
                merged[-1] = i
            continue
        merged.append(i)
    sylls = []
    for k, p in enumerate(merged):
        left_lim = merged[k - 1] if k > 0 else 0
        right_lim = merged[k + 1] if k + 1 < len(merged) else len(env) - 1
        a = left_lim + int(np.argmin(env[left_lim:p + 1])) if k > 0 else p
        if k == 0 or env[a] < floor * EDGE_OVER_FLOOR:
            a = p
            while a > left_lim and env[a] > floor * EDGE_OVER_FLOOR:
                a -= 1
        b = p + int(np.argmin(env[p:right_lim + 1])) if k + 1 < len(merged) else p
        if k + 1 == len(merged) or env[b] < floor * EDGE_OVER_FLOOR:
            b = p
            while b < right_lim and env[b] > floor * EDGE_OVER_FLOOR:
                b += 1
        s, e = a * hop, b * hop + int(WIN_S * sr)
        dur = (e - s) / sr
        if dur < MIN_SYL_S:
            continue
        if dur > MAX_SYL_S:  # very long nucleus: keep a centred slice
            c = p * hop
            s, e = max(0, c - int(MAX_SYL_S * sr / 2)), min(len(x), c + int(MAX_SYL_S * sr / 2))
        sylls.append((s, min(e, len(x))))
    return sylls


def pitch_and_voicing(seg, sr):
    """Median F0 of voiced 40 ms frames (normalized autocorrelation) and voiced share."""
    frame, hop = int(0.040 * sr), int(0.010 * sr)
    lo, hi = int(sr / F0_RANGE[1]), int(sr / F0_RANGE[0])
    f0s, frames = [], 0
    for s in range(0, len(seg) - frame, hop):
        fr = seg[s:s + frame] * np.hanning(frame)
        frames += 1
        ac = np.correlate(fr, fr, mode="full")[frame - 1:]
        if ac[0] <= 0 or hi >= len(ac):
            continue
        ac = ac / ac[0]
        lag = lo + int(np.argmax(ac[lo:hi]))
        if lo < lag < hi - 1 and ac[lag] > 0.5:  # a peak at the search edge is not a pitch
            f0s.append(sr / lag)
    voiced = len(f0s) / frames if frames else 0.0
    return (float(np.median(f0s)) if len(f0s) >= 2 else 0.0), voiced


def centroid(seg, sr):
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    f = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return float((mag * f).sum() / (mag.sum() + 1e-12))


def background(x, sr):
    """Full-band background level of a recording and its quiet stretches
    (>= 120 ms below 1.3 x that level): the raw material for room tone."""
    hop, win = int(HOP_S * sr), int(WIN_S * sr)
    n = max(0, (len(x) - win) // hop + 1)
    env = np.array([rms(x[i * hop:i * hop + win]) for i in range(n)])
    if n == 0:
        return 1e-4, [], env
    floor = float(np.percentile(env, 10)) + 1e-7
    quiet, start = [], None
    for i, v in enumerate(np.append(env, np.inf)):
        if v < floor * 1.3:
            start = i if start is None else start
        elif start is not None:
            if (i - start) * HOP_S >= 0.12:
                quiet.append((start * hop, (i - 1) * hop + win))
            start = None
    return floor, quiet, env


def build_corpus(paths, teensy_clean=False):
    corpus, audio, sr_all, files = [], {}, None, {}
    for path in paths:
        x, sr = load_mono(path)
        if teensy_clean:
            x = clean_teensy_take(x, sr)
        sr_all = sr_all or sr
        name = os.path.basename(path)
        audio[name] = x
        floor, quiet, env = background(x, sr)
        files[name] = dict(floor=floor, quiet=quiet, env=env)
        prev_end = None
        for idx, (s, e) in enumerate(find_syllables(x, sr)):
            seg = x[s:e]
            f0, voiced = pitch_and_voicing(seg, sr)
            corpus.append(dict(file=name, idx=idx, start=s, end=e, dur=(e - s) / sr,
                               rms=rms(seg), f0=f0, voiced=voiced, bright=centroid(seg, sr),
                               gap_before=(s - prev_end) / sr if prev_end is not None else None,
                               floor=floor, snr=rms(seg) / floor))
            prev_end = e
    return corpus, audio, sr_all, files


# ---------------------------------------------------------------- B and C: choosing syllables

def distance(c, f0_target, bright_target, dur_target, voiced_target):
    d = 0.0
    if f0_target > 0 and c["f0"] > 0:
        d += 2.0 * abs(np.log2(c["f0"] / f0_target))          # octaves
    elif (f0_target > 0) != (c["f0"] > 0):
        d += 0.8                                               # voiced vs whispered mismatch
    d += 0.8 * abs(np.log2(max(c["bright"], 50) / max(bright_target, 50)))
    d += 0.6 * abs(np.log2(c["dur"] / max(dur_target, 0.03)))
    d += 0.3 * abs(c["voiced"] - voiced_target)
    return d


def seam_cost(c, snr_t, snr_median):
    """Background match: once level-matched, a syllable's background sits at
    level / snr, so neighbours with similar snr have similar backgrounds; noisy
    takes (low snr) are avoided."""
    return 0.7 * abs(np.log2(c["snr"] / snr_t)) + 0.5 * max(0.0, np.log2(snr_median / c["snr"]))


def pick(corpus, rng, exclude_files, used, f0_t, br_t, dur_t, v_t, k=6, snr_t=None, snr_median=None):
    pool = [c for c in corpus if c["file"] not in exclude_files and id(c) not in used]
    if not pool:
        pool = corpus
    if snr_t:
        pool.sort(key=lambda c: distance(c, f0_t, br_t, dur_t, v_t) + seam_cost(c, snr_t, snr_median))
    else:
        pool.sort(key=lambda c: distance(c, f0_t, br_t, dur_t, v_t))
    top = pool[:k]
    w = np.linspace(1.0, 0.3, len(top))
    return top[rng.choice(len(top), p=w / w.sum())]


def air_pads(files, name, s, e, sr, n_audio):
    """How much quiet edge of its own take a syllable may carry (samples)."""
    info = files[name]
    env, floor, hop = info["env"], info["floor"], int(HOP_S * sr)
    limit = int(AIR_MAX_S / HOP_S)
    k, pre = s // hop, 0
    while pre < limit and k - pre - 1 >= 0 and env[k - pre - 1] < floor * AIR_QUIET:
        pre += 1
    k, post = e // hop, 0
    while post < limit and k + post + 1 < len(env) and env[k + post + 1] < floor * AIR_QUIET:
        post += 1
    return min(pre * hop, s), min(post * hop, n_audio - e)


def shaped(seg, lead, tail, sr):
    """Equal-power fades over the air on each side (at least FADE_S)."""
    seg = seg.copy()
    for n, at_start in ((max(lead, int(FADE_S * sr)), True), (max(tail, int(FADE_S * sr)), False)):
        n = min(n, len(seg) // 2)
        if n <= 0:
            continue
        ramp = np.sin(np.linspace(0, np.pi / 2, n))
        if at_start:
            seg[:n] *= ramp
        else:
            seg[-n:] *= ramp[::-1]
    return seg


def mix(pieces, length=None):
    end = max(st + len(sg) for st, sg in pieces)
    out = np.zeros(max(end, length or 0))
    for st, sg in pieces:
        out[st:st + len(sg)] += sg
    return out


def render_collective(corpus, audio, sr, seconds, rng, seam=False, files=None, air=False):
    """B: one impossible speaker. Phrases with a falling melody, each syllable
    from a different person than the one before, continuing pitch and timbre."""
    gaps = [c["gap_before"] for c in corpus if c["gap_before"] is not None and 0 <= c["gap_before"] < 0.35]
    voiced_f0 = [c["f0"] for c in corpus if c["f0"] > 0]
    target_level = float(np.median([c["rms"] for c in corpus]))
    snr_median = float(np.median([c["snr"] for c in corpus]))
    pieces, used, log, chosen = [], set(), [], []
    t = int(AIR_MAX_S * sr)                                     # timeline position of the next core
    prev = rng.choice(corpus)
    while t < seconds * sr:
        n_syll = int(rng.integers(4, 11))                       # one phrase
        f0_hi = float(np.percentile(voiced_f0, 70)) if voiced_f0 else 0.0
        for k in range(n_syll):
            fall = 2 ** (-0.6 * k / max(1, n_syll - 1))          # ~0.6 octave declination
            f0_t = f0_hi * fall if prev["f0"] > 0 or rng.random() < 0.5 else 0.0
            c = pick(corpus, rng, {prev["file"]}, used, f0_t, prev["bright"], prev["dur"], prev["voiced"],
                     snr_t=prev["snr"] if seam else None, snr_median=snr_median)
            used.add(id(c))
            chosen.append(c)
            x = audio[c["file"]]
            pre, post = air_pads(files, c["file"], c["start"], c["end"], sr, len(x)) if air else (0, 0)
            seg = x[c["start"] - pre:c["end"] + post]
            reversed_ = rng.random() < REVERSE_B
            if reversed_:
                seg, pre, post = seg[::-1], post, pre
            gain = target_level / max(c["rms"], 1e-5) * (0.85 + 0.3 * rng.random())
            pieces.append((t - pre, shaped(seg, pre, post, sr) * min(gain, 8.0)))
            gap = rng.choice(gaps) if gaps else 0.05
            t += (c["end"] - c["start"]) + int(gap * sr)
            log.append(f'{c["file"]}#{c["idx"]}' + ("R" if reversed_ else ""))
            prev = c
        t += int(rng.uniform(0.45, 1.2) * sr)                    # pause between phrases
    return mix(pieces, t), log


def render_transplant(corpus, audio, sr, source_file, rng, seam=False, files=None, air=False):
    """C: keep the rhythm and melody of one phrase, replace every syllable with
    the closest-sounding syllable of someone else (no pitch shifting: chosen)."""
    src = [c for c in corpus if c["file"] == source_file]
    x_src = audio[source_file]
    length = src[-1]["end"] + int(0.3 * sr)
    pieces = [(0, np.zeros(length))]
    snr_median = float(np.median([c["snr"] for c in corpus]))
    used, log, prev_file = set(), [], None
    for i, s in enumerate(src):
        slot_end = src[i + 1]["start"] if i + 1 < len(src) else s["end"]
        # seam-aware: replacements at least as clean as the source syllable
        c = pick(corpus, rng, {source_file, prev_file}, used, s["f0"], s["bright"], s["dur"], s["voiced"], k=3,
                 snr_t=max(s["snr"], snr_median) if seam else None, snr_median=snr_median)
        used.add(id(c))
        prev_file = c["file"]
        x = audio[c["file"]]
        max_core = int((slot_end - s["start"]) * 1.25) if i + 1 < len(src) else c["end"] - c["start"]
        core_end = c["start"] + max(min(max_core, c["end"] - c["start"]), int(0.05 * sr))
        pre, post = air_pads(files, c["file"], c["start"], c["end"], sr, len(x)) if air else (0, 0)
        if core_end < c["end"]:
            post = 0                                           # cut short: no tail air
        seg = x[c["start"] - pre:core_end + post]
        if rng.random() < REVERSE_C:
            seg, pre, post = seg[::-1], post, pre
        gain = min(s["rms"] / max(c["rms"], 1e-5), 8.0)
        pieces.append((max(0, s["start"] - pre), shaped(seg, pre, post, sr) * gain))
        log.append(f'{s["f0"]:.0f}Hz->{c["file"]}#{c["idx"]}({c["f0"]:.0f}Hz)')
    out = mix(pieces, length)[:length]
    original = x_src[:length]
    return original, out, log


def render_syllable_check(corpus, audio, sr, name):
    """A check: the syllables of one recording, separated by short silences."""
    parts = []
    for c in corpus:
        if c["file"] == name:
            parts += [faded(audio[name][c["start"]:c["end"]], sr), np.zeros(int(0.35 * sr))]
    return np.concatenate(parts) if parts else np.zeros(1)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", help="folder with WAV recordings")
    ap.add_argument("out", help="folder for renders (outside git)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--teensy-clean", action="store_true", help="apply the firmware's click trim and hum removal first")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    paths = sorted(glob.glob(os.path.join(args.source, "*.WAV")) + glob.glob(os.path.join(args.source, "*.wav")))
    corpus, audio, sr, files = build_corpus(paths, args.teensy_clean)
    with open(os.path.join(args.out, "A_corpus.csv"), "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["file", "idx", "start_s", "dur_s", "rms", "f0_hz", "voiced", "bright_hz"])
        for c in corpus:
            wr.writerow([c["file"], c["idx"], f'{c["start"] / sr:.3f}', f'{c["dur"]:.3f}', f'{c["rms"]:.5f}',
                         f'{c["f0"]:.1f}', f'{c["voiced"]:.2f}', f'{c["bright"]:.0f}'])
    per_file = {}
    for c in corpus:
        per_file[c["file"]] = per_file.get(c["file"], 0) + 1
    voiced = [c for c in corpus if c["f0"] > 0]
    print(f"A: {len(corpus)} syllables from {len(per_file)}/{len(paths)} recordings at {sr} Hz; "
          f"voiced {len(voiced)} ({100 * len(voiced) / max(1, len(corpus)):.0f} %), "
          f"median length {np.median([c['dur'] for c in corpus]):.2f} s")

    richest = sorted(per_file, key=lambda f: -per_file[f])
    for name in richest[:3]:
        save(os.path.join(args.out, f"A_syllables_{name[:-4]}.wav"), render_syllable_check(corpus, audio, sr, name), sr)

    for i in range(3):
        seed = args.seed + i
        y3, log = render_collective(corpus, audio, sr, 20.0, np.random.default_rng(seed), seam=True, files=files)
        y4, _ = render_collective(corpus, audio, sr, 20.0, np.random.default_rng(seed), seam=True, files=files, air=True)
        save(os.path.join(args.out, f"B{i + 1}_3_seams.wav"), y3, sr)
        save(os.path.join(args.out, f"B{i + 1}_4_seams_air.wav"), y4, sr)
        print(f"B{i + 1}: {len(log)} syllables from {len(set(l.split('#')[0] for l in log))} recordings, "
              f"{sum(l.endswith('R') for l in log)} reversed")

    for name in richest[:4]:
        original, y3, _ = render_transplant(corpus, audio, sr, name, np.random.default_rng(args.seed), seam=True, files=files)
        _, y4, _ = render_transplant(corpus, audio, sr, name, np.random.default_rng(args.seed), seam=True, files=files, air=True)
        gap = np.zeros(int(0.8 * sr))
        save(os.path.join(args.out, f"C_{name[:-4]}_3_seams.wav"), np.concatenate([original, gap, y3]), sr)
        save(os.path.join(args.out, f"C_{name[:-4]}_4_seams_air.wav"), np.concatenate([original, gap, y4]), sr)
        print(f"C {name}: original, then transplant")


if __name__ == "__main__":
    main()
