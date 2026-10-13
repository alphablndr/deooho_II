#!/usr/bin/env python3
"""Check an Ooho SD card archive (or a copy of it) for recording problems.

Per recording it reports level, clipping, the end-of-take button click, a
steady tone in the pauses (the cube's own MELOS bleeding into the mic), and
how many LOGOS grains GRAINS.CSV holds for it. A summary follows.

Usage:
  python3 tools/analyze_archive.py /Volumes/SDEAR
  python3 tools/analyze_archive.py /path/to/copy --from 90   # only W0090+

Needs numpy. Read-only: never writes to the archive.
"""
import argparse
import glob
import os
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required: python3 -m venv /tmp/oohovenv && /tmp/oohovenv/bin/pip install numpy")

SAMPLE_RATE = 44100
WINDOW = 661                 # ~15 ms, same window as the firmware grain slicer
CLIP_LEVEL = 32700
SHORT_TAKE_S = 0.5           # firmware should not save takes shorter than this
CLICK_TAIL_S = 0.3           # look for the button click in the last 300 ms
CLICK_WIN = 441              # 10 ms
TONE_MIN_HZ, TONE_MAX_HZ = 100.0, 1000.0
TONE_FLAG_DB = 15.0          # a peak this far above the median spectrum = steady tone
TONE_MIN_FLOOR = 0.003       # ...but only if the pauses are not near-digital silence (~-50 dBFS)
FFT_N = 8192


def load(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("not 16-bit")
        frames = w.readframes(w.getnframes())
        x = np.frombuffer(frames, dtype="<i2").astype(np.float64)
        if w.getnchannels() > 1:
            x = x.reshape(-1, w.getnchannels())[:, 0]
        return x, w.getframerate()


def window_rms(x):
    n = len(x) // WINDOW
    if n == 0:
        return np.zeros(0)
    seg = x[: n * WINDOW].reshape(n, WINDOW)
    seg = seg - seg.mean(axis=1, keepdims=True)
    return np.sqrt(np.mean(seg ** 2, axis=1))


def end_click_ms(x, body_rms):
    """Length of a loud burst at the very end of the take (0 = none)."""
    tail_n = int(CLICK_TAIL_S * SAMPLE_RATE)
    if len(x) < tail_n * 2:
        return 0
    tail = x[-tail_n:]
    wins = tail[: (len(tail) // CLICK_WIN) * CLICK_WIN].reshape(-1, CLICK_WIN)
    r = np.sqrt(np.mean(wins ** 2, axis=1))
    loud = np.where(r > max(3.0 * body_rms, 0.15))[0]
    if len(loud) == 0:
        return 0
    return int((len(r) - loud[0]) * CLICK_WIN * 1000 / SAMPLE_RATE)


def pause_floor(rms):
    """Level of the pauses: median RMS of the quietest fifth of the windows."""
    if len(rms) == 0:
        return 0.0
    return float(np.median(np.sort(rms)[: max(1, len(rms) // 5)]))


def pause_tone(x, rms):
    """Strongest steady tone in the quietest fifth of the take: (Hz, dB over median)."""
    if len(rms) < 8:
        return 0.0, 0.0
    quiet = np.sort(np.argsort(rms)[: max(8, len(rms) // 5)])
    seg = np.concatenate([x[i * WINDOW:(i + 1) * WINDOW] for i in quiet])
    seg = seg - seg.mean()
    if len(seg) < FFT_N:
        seg = np.pad(seg, (0, FFT_N - len(seg)))
    spec = np.zeros(FFT_N // 2 + 1)
    hop = FFT_N // 2
    count = 0
    for s in range(0, len(seg) - FFT_N + 1, hop):
        spec += np.abs(np.fft.rfft(seg[s:s + FFT_N] * np.hanning(FFT_N))) ** 2
        count += 1
    spec /= max(count, 1)
    f = np.fft.rfftfreq(FFT_N, 1.0 / SAMPLE_RATE)
    band = (f >= TONE_MIN_HZ) & (f <= TONE_MAX_HZ)
    ref = (f >= TONE_MIN_HZ) & (f <= 4000.0)
    median = np.median(spec[ref]) + 1e-20
    i = np.argmax(spec[band])
    return float(f[band][i]), float(10.0 * np.log10(spec[band][i] / median))


def read_grains(root):
    grains = {}
    path = os.path.join(root, "GRAINS.CSV")
    if not os.path.exists(path):
        return grains, 0
    bad = 0
    with open(path, newline="") as fh:
        for line in fh:
            parts = line.strip().split(",")
            if len(parts) != 3:
                if line.strip():
                    bad += 1
                continue
            grains.setdefault(parts[0], []).append(int(parts[2]))
    return grains, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("root", help="SD card mount point or a copy of it")
    ap.add_argument("--from", dest="first", type=int, default=1, help="first recording number to analyze")
    args = ap.parse_args()

    grains, bad_lines = read_grains(args.root)
    files = sorted(p for p in glob.glob(os.path.join(args.root, "W[0-9][0-9][0-9][0-9].WAV")))
    files = [p for p in files if int(os.path.basename(p)[1:5]) >= args.first]
    if not files:
        sys.exit("no Wxxxx.WAV recordings found")

    print(f"{'file':10} {'sec':>5} {'rms':>6} {'floor':>6} {'peak':>5} {'clip':>5} {'click':>6} {'tone Hz/dB':>12} {'grains':>6}")
    rows = []
    for p in files:
        name = os.path.basename(p)
        try:
            x, sr = load(p)
        except Exception as exc:  # unreadable file is itself a finding
            print(f"{name:10} UNREADABLE: {exc}")
            continue
        if sr != SAMPLE_RATE:
            print(f"{name:10} unexpected sample rate {sr}")
        xs = x / 32768.0
        rms = window_rms(xs)
        body = float(np.sqrt(np.mean((xs - xs.mean()) ** 2)))
        clipped = int(np.sum(np.abs(x) >= CLIP_LEVEL))
        click = end_click_ms(xs, body)
        floor = pause_floor(rms)
        tone_hz, tone_db = pause_tone(xs, rms)
        has_tone = tone_db >= TONE_FLAG_DB and floor >= TONE_MIN_FLOOR
        g = grains.get(name, [])
        dur = len(x) / SAMPLE_RATE
        rows.append(dict(name=name, dur=dur, click=click, tone=has_tone, grains=len(g), clipped=clipped))
        tone = f"{tone_hz:5.0f}/{tone_db:4.1f}" + ("*" if has_tone else " ")
        print(f"{name:10} {dur:5.1f} {body:6.3f} {floor:6.4f} {np.max(np.abs(xs)):5.2f} {clipped:5d} "
              f"{click:4d}ms {tone:>12} {len(g):6d}")

    n = len(rows)
    total_grains = sum(r["grains"] for r in rows)
    missing = [f for f in grains if not os.path.exists(os.path.join(args.root, f))]
    print("\nSUMMARY")
    print(f"  recordings analyzed:        {n}")
    print(f"  end-of-take click:          {sum(r['click'] > 0 for r in rows)}")
    print(f"  steady tone in pauses (*):  {sum(r['tone'] for r in rows)}  (>= {TONE_FLAG_DB:.0f} dB over median, pauses >= {TONE_MIN_FLOOR})")
    print(f"  shorter than {SHORT_TAKE_S} s:          {sum(r['dur'] < SHORT_TAKE_S for r in rows)}")
    print(f"  with clipped samples:       {sum(r['clipped'] > 0 for r in rows)}")
    print(f"  with zero grains:           {sum(r['grains'] == 0 for r in rows)}")
    print(f"  grains total:               {total_grains}")
    if total_grains:
        top = sorted(rows, key=lambda r: -r["grains"])[:5]
        print("  largest grain shares:       " + ", ".join(
            f"{r['name']} {100 * r['grains'] / total_grains:.0f}%" for r in top))
    print(f"  GRAINS.CSV bad lines:       {bad_lines}")
    print(f"  GRAINS.CSV -> missing WAV:  {len(missing)} {missing[:5]}")


if __name__ == "__main__":
    main()
