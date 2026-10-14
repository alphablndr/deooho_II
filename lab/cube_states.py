#!/usr/bin/env python3
"""SPIKE (throwaway, 2026-10-14): LOGOS in three states (Anton's model).

After every take the cube goes through
  STORM   15-30 s   fragments of syllables, fast pulse, sometimes a shared
                    grid across voices ("squared notebook"), ~30 % reversed,
                    ~40 % of fragments from the new take (its voice multiplies)
  SPEECH  1-2 min   words and short phrases of real speech, rubato, natural
                    pauses; whole words may be heard ("price of organicity")
  RARE    then      never silent: every 10-20 s a short episode (<= 10 s):
                    a phrase, one word, a reversed word, a burst of fragments
                    or a repeated word; then quiet again
A new take restarts the storm. Number of voices by archive size:
  < 10 recordings: 1, < 30: 2, < 80: 3, otherwise 4 (never more).
Every voice follows the same phases. MELOS is not part of this spike.

  python3 lab/cube_states.py <folder> <out> [--teensy-clean]
"""
import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logos_lab as L  # noqa: E402
from conversation import BAND_HI, BAND_LO, Drift, band  # noqa: E402

STORM_S = (15.0, 30.0)
SPEECH_S = (60.0, 120.0)
RARE_EVERY_S = (10.0, 20.0)
RARE_EPISODE_MAX_S = 10.0
NEW_TAKE_SHARE = 0.4
REGISTERS = (30, 70, 45, 60)
VOICE_GAIN = (1.0, 0.85, 0.7, 0.6)
VOICE_LOWPASS = (None, None, 6000.0, 5000.0)


def voices_for_archive(n):
    return 1 if n < 10 else 2 if n < 30 else 3 if n < 80 else 4


class Timeline:
    """Which phase the cube is in at time t (s), given take times."""

    def __init__(self, takes, rng):
        self.takes = sorted(takes)
        self.lengths = {t: (rng.uniform(*STORM_S), rng.uniform(*SPEECH_S)) for t in self.takes}

    def phase(self, t):
        past = [te for te in self.takes if te <= t]
        if not past:
            return "rare", 0.0, None
        te = past[-1]
        storm, speech = self.lengths[te]
        if t - te < storm:
            return "storm", (t - te) / storm, te
        if t - te < storm + speech:
            return "speech", (t - te - storm) / speech, te
        return "rare", 0.0, te


class Voice:
    def __init__(self, k, rng, ctx):
        self.k, self.rng, self.ctx = k, rng, ctx
        c = ctx["corpus"]
        f0s = np.array([s["f0"] for s in c if s["f0"] > 0])
        self.register = Drift(rng, np.log2(np.percentile(f0s, REGISTERS[k])) if len(f0s) else 7.3, 0.2, 30.0)
        self.rubato = Drift(rng, 0.0, 0.35, 12.0)
        self.level = ctx["level"] * VOICE_GAIN[k]
        self.prev = rng.choice(c)
        self.used = set()
        self.t = 0.0
        self.tracks = []

    # -- material ---------------------------------------------------------
    def place(self, t, c, s, e, air=True, reverse=False, gain_scale=1.0):
        sr, x = self.ctx["sr"], self.ctx["audio"][c["file"]]
        pre, post = L.air_pads(self.ctx["files"], c["file"], s, e, sr, len(x)) if air else (0, 0)
        seg = x[s - pre:e + post]
        if reverse:
            seg, pre, post = seg[::-1], post, pre
        gain = min(self.level / max(L.rms(x[s:e]), 1e-5) * gain_scale, 8.0)
        self.tracks.append((max(0, int(t * sr) - pre), L.shaped(seg, pre, post, sr) * gain))
        return (e - s) / sr

    def word(self, f0_t=0.0, target_s=0.9, fresh=None):
        """A stretch of real speech of about target_s, chosen to follow the last one."""
        ctx, rng = self.ctx, self.rng
        if fresh is not None:
            c = fresh[rng.integers(len(fresh))]
        else:
            c = L.pick(ctx["corpus"], rng, {self.prev["file"]}, self.used, f0_t, self.prev["bright"],
                       min(target_s, 0.5), 0.9 if f0_t > 0 else 0.3, k=8,
                       snr_t=self.prev["snr"], snr_median=ctx["snr_median"])
        self.used.add(id(c))
        sylls, last = ctx["by_file"][c["file"]], c
        while last["idx"] + 1 < len(sylls):
            nxt = sylls[last["idx"] + 1]
            if (nxt["start"] - last["end"]) / ctx["sr"] > 0.35 or (nxt["end"] - c["start"]) / ctx["sr"] > target_s:
                break
            last = nxt
        self.prev = last
        return c, c["start"], last["end"]

    def fragment(self, fresh):
        """A cut of one syllable, 60-250 ms (the storm's material)."""
        ctx, rng, sr = self.ctx, self.rng, self.ctx["sr"]
        pool = fresh if (fresh and rng.random() < NEW_TAKE_SHARE) else ctx["corpus"]
        c = pool[rng.integers(len(pool))]
        length = int(rng.uniform(0.06, 0.25) * sr)
        span = c["end"] - c["start"]
        if span <= length:
            return c, c["start"], c["end"]
        s = c["start"] + int(rng.integers(0, span - length))
        return c, s, s + length

    # -- behaviour per phase --------------------------------------------------
    def storm(self, t, progress, fresh):
        """A burst of fragments; sometimes locked to the shared grid."""
        rng = self.rng
        rate = rng.uniform(5.0, 8.0) * (1.0 - 0.3 * progress)           # calms toward speech
        grid = self.ctx["grid"] if rng.random() < 0.45 else None
        end = t + rng.uniform(1.0, 3.0)
        while t < end:
            if grid:
                t = np.ceil(t / grid) * grid                            # snap to the common pulse
            c, s, e = self.fragment(fresh)
            self.place(t, c, s, e, air=False, reverse=rng.random() < 0.3, gain_scale=0.8)
            t += (grid if grid else 1.0 / rate) * rng.uniform(0.85, 1.25)
        return t + rng.uniform(0.1, 0.5)

    def speech(self, t, progress):
        """A phrase of 1-4 word-level units; pauses grow toward the end."""
        rng, sr = self.rng, self.ctx["sr"]
        pace = 2 ** self.rubato.x
        shape = rng.choice(("fall", "rise", "arch", "flat"))
        voiced = rng.random() < self.ctx["voiced_share"]
        n = int(np.clip(rng.poisson(2.2), 1, 4))
        for u in range(n):
            frac = u / max(1, n - 1)
            off = {"fall": 0.2 - 0.45 * frac, "rise": -0.15 + 0.4 * frac,
                   "arch": 0.25 * np.sin(np.pi * frac), "flat": 0.0}[shape]
            target = float(np.clip(rng.lognormal(np.log(0.9 / pace), 0.4), 0.4, 1.5))
            c, s, e = self.word(2 ** (self.register.x + off) if voiced else 0.0, target)
            t += self.place(t, c, s, e, reverse=rng.random() < 0.1)
            t += rng.choice(self.ctx["gaps"]) / pace * rng.uniform(0.7, 1.3)
        self.rubato.step(3.0)
        self.register.step(3.0)
        return t + rng.lognormal(np.log(0.8 + 2.5 * progress), 0.5)

    def episode(self, t, fresh):
        """RARE: one short event, <= RARE_EPISODE_MAX_S."""
        rng = self.rng
        kind = rng.choice(("phrase", "word", "reversed", "burst", "repeat"), p=(0.3, 0.25, 0.15, 0.15, 0.15))
        start = t
        if kind == "phrase":
            t = self.speech(t, 0.0) - 0.8
        elif kind in ("word", "reversed"):
            c, s, e = self.word(target_s=rng.uniform(0.4, 1.2))
            t += self.place(t, c, s, e, reverse=(kind == "reversed"))
        elif kind == "burst":
            t = self.storm(t, 0.5, None)
        else:
            c, s, e = self.word(target_s=rng.uniform(0.3, 0.8))
            for _ in range(int(rng.integers(2, 4))):
                t += self.place(t, c, s, e) + rng.uniform(0.15, 0.6)
        return min(t, start + RARE_EPISODE_MAX_S), kind


def render(ctx, seconds, takes, fresh_file, n_voices, rng):
    tl = Timeline(takes, rng)
    voices = [Voice(k, rng, ctx) for k in range(n_voices)]
    for v in voices:
        v.t = rng.uniform(0, 1.5)
    fresh = ctx["by_file"].get(fresh_file)
    next_episode = rng.uniform(2.0, 6.0)
    log = []
    while True:
        v = min(voices, key=lambda q: q.t)
        if v.t >= seconds:
            break
        phase, progress, _ = tl.phase(v.t)
        if phase == "storm":
            v.t = v.storm(v.t, progress, fresh)
        elif phase == "speech":
            v.t = v.speech(v.t, progress)
        else:
            if v.t < next_episode:
                v.t = next_episode + (0.0 if v.k == 0 else rng.uniform(0.2, 3.0))
                if v.k != 0 and rng.random() > 0.3:          # others only sometimes join in
                    v.t = next_episode + rng.uniform(*RARE_EVERY_S)
                continue
            end, kind = v.episode(v.t, fresh)
            log.append((round(v.t, 1), kind))
            next_episode = end + rng.uniform(*RARE_EVERY_S)
            v.t = end + 0.5
    sr = ctx["sr"]
    length = int((seconds + 3) * sr)
    out = np.zeros(length)
    for k, v in enumerate(voices):
        if v.tracks:
            y = L.mix(v.tracks, length)[:length]
            if VOICE_LOWPASS[k] and VOICE_LOWPASS[k] < sr / 2:
                y = band(y, sr, 0, VOICE_LOWPASS[k])
            out += y
    return out, tl, log


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--teensy-clean", action="store_true")
    ap.add_argument("--seed", type=int, default=31)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.source, "*.WAV")) + glob.glob(os.path.join(args.source, "*.wav")))
    corpus, audio, sr, files = L.build_corpus(paths, args.teensy_clean)
    for name in audio:
        audio[name] = band(audio[name], sr, BAND_LO, min(BAND_HI, sr / 2 * 0.95))
    by_file = {}
    for c in corpus:
        by_file.setdefault(c["file"], []).append(c)
    gaps = np.array([(b["start"] - a["end"]) / sr for s in by_file.values() for a, b in zip(s, s[1:])
                     if 0.03 <= (b["start"] - a["end"]) / sr <= 1.2]) if by_file else np.array([0.15])
    ctx = dict(corpus=corpus, audio=audio, sr=sr, files=files, by_file=by_file, gaps=gaps,
               level=float(np.median([c["rms"] for c in corpus])),
               snr_median=float(np.median([c["snr"] for c in corpus])),
               voiced_share=float(np.mean([c["f0"] > 0 for c in corpus])),
               grid=float(np.random.default_rng(args.seed).uniform(0.12, 0.2)))
    richest = sorted(by_file, key=lambda f: -len(by_file[f]))
    for archive in (8, 25, 60, 150):
        n_voices = voices_for_archive(archive)
        rng = np.random.default_rng(args.seed + archive)
        y, tl, log = render(ctx, 240.0, [30.0], richest[0], n_voices, rng)
        storm, speech = tl.lengths[30.0]
        name = f"G_archive{archive:03d}_{n_voices}voice{'s' if n_voices > 1 else ''}.wav"
        L.save(os.path.join(args.out, name), y, sr)
        print(f"{name}: take at 0:30, storm {storm:.0f} s, speech {speech:.0f} s, "
              f"rare episodes: {[k for _, k in log]}")


if __name__ == "__main__":
    main()
