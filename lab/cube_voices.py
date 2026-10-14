#!/usr/bin/env python3
"""SPIKE (throwaway, 2026-10-14): the cube's voices, round 2.

Anton on round 1 (conversation.py): "like a squared notebook, tu-tu-tu" -
units were single short syllables placed on an even grid. Here:

  - units are stretches of real speech: from one syllable up to a short
    phrase of one take, keeping its own internal rhythm; slow pace -> long
    units, fast pace -> short ones
  - no grid: pauses come from the archive's real pauses between words and
    breathe with a slowly drifting rubato; neighbours may overlap in their
    own air (legato)
  - the number of voices follows the cube's activity (as in firmware stage
    C): asleep = one slow voice, after a take = 3-4 denser voices, then it
    calms down again
  - in mono the voices separate by register, level and rhythm; voices 3-4
    are a bit darker and quieter

  python3 lab/cube_voices.py <teensy folder> <out> --teensy-clean
"""
import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logos_lab as L  # noqa: E402
from conversation import BAND_HI, BAND_LO, Drift, band  # noqa: E402

MAX_VOICES = 4
REGISTERS = (30, 70, 45, 60)            # pitch percentile per voice
VOICE_GAIN = (1.0, 0.85, 0.6, 0.5)
VOICE_LOWPASS = (None, None, 6000.0, 5000.0)
SLEEP, HALF_LIFE_S = 0.15, 40.0         # demo: faster decay than the firmware's 8 min


def runs_by_file(corpus, sr):
    """Per take: syllables in order, plus their real gaps (for pauses)."""
    by_file = {}
    for c in corpus:
        by_file.setdefault(c["file"], []).append(c)
    gaps = []
    for sylls in by_file.values():
        for a, b in zip(sylls, sylls[1:]):
            g = (b["start"] - a["end"]) / sr
            if 0.03 <= g <= 1.2:
                gaps.append(g)
    return by_file, np.array(gaps) if gaps else np.array([0.15])


def activity_at(t, events):
    a = SLEEP
    for te in events:
        if t >= te:
            a = min(1.0, a + 0.7 * (1.0 - a))
            a = SLEEP + (a - SLEEP) * 2 ** (-(t - te) / HALF_LIFE_S)
    return a


class Voice:
    def __init__(self, k, rng, corpus, by_file, gaps, files, audio, sr):
        self.k, self.rng, self.corpus, self.by_file, self.gaps = k, rng, corpus, by_file, gaps
        self.files, self.audio, self.sr = files, audio, sr
        f0s = np.array([c["f0"] for c in corpus if c["f0"] > 0])
        self.register = Drift(rng, np.log2(np.percentile(f0s, REGISTERS[k])) if len(f0s) else 7.3, 0.2, 30.0)
        self.rubato = Drift(rng, 0.0, 0.35, 12.0)        # log2 tempo factor, slow and smooth
        self.voiced_share = np.mean([c["f0"] > 0 for c in corpus])
        self.snr_median = float(np.median([c["snr"] for c in corpus]))
        self.level = float(np.median([c["rms"] for c in corpus])) * VOICE_GAIN[k]
        self.prev = rng.choice(corpus)
        self.used = set()
        self.t = 0.0                                     # seconds: when this voice is free

    def unit(self, start, target_s):
        """A stretch of one take from `start`: whole syllables while they stay
        close together and the stretch is shorter than target_s."""
        sylls = self.by_file[start["file"]]
        last = start
        while True:
            j = last["idx"] + 1
            if j >= len(sylls):
                break
            nxt = sylls[j]
            if (nxt["start"] - last["end"]) / self.sr > 0.35 or (nxt["end"] - start["start"]) / self.sr > target_s:
                break
            last = nxt
        return start["start"], last["end"], last

    def phrase(self, act):
        """1-5 units; pace follows activity and this voice's own rubato."""
        rng, sr = self.rng, self.sr
        pace = 2 ** (self.rubato.x + 1.2 * (act - 0.5))           # >1 faster, <1 slower
        n_units = int(np.clip(rng.poisson(1.5 + 2.5 * act), 1, 5))
        voiced = rng.random() < self.voiced_share
        shape = rng.choice(("fall", "rise", "arch", "flat"))
        pieces, t = [], int(self.t * sr)
        for u in range(n_units):
            frac = u / max(1, n_units - 1)
            offset = {"fall": 0.2 - 0.45 * frac, "rise": -0.15 + 0.4 * frac,
                      "arch": 0.25 * np.sin(np.pi * frac), "flat": 0.0}[shape]
            f0_t = 2 ** (self.register.x + offset) if voiced else 0.0
            target_s = float(np.clip(rng.lognormal(np.log(0.9 / pace), 0.45), 0.15, 2.5))
            c = L.pick(self.corpus, rng, {self.prev["file"]}, self.used, f0_t, self.prev["bright"],
                       min(target_s, 0.5), 0.9 if voiced else 0.2, k=8,
                       snr_t=self.prev["snr"], snr_median=self.snr_median)
            self.used.add(id(c))
            s, e, last = self.unit(c, target_s)
            x = self.audio[c["file"]]
            pre, post = L.air_pads(self.files, c["file"], s, e, sr, len(x))
            seg = x[s - pre:e + post]
            if rng.random() < 0.15 and e - s < 0.6 * sr:          # reverse only short units
                seg, pre, post = seg[::-1], post, pre
            seg_rms = L.rms(x[s:e])
            gain = min(self.level / max(seg_rms, 1e-5) * rng.uniform(0.8, 1.1), 8.0)
            pieces.append((max(0, t - pre), L.shaped(seg, pre, post, sr) * gain))
            gap = rng.choice(self.gaps) / pace * rng.uniform(0.7, 1.3)
            if rng.random() < 0.2:
                gap = -min(pre + post, int(0.08 * sr)) / sr     # legato: overlap in own air
            t += (e - s) + int(gap * sr)
            self.prev = last
        dur = t / sr - self.t
        self.rubato.step(dur)
        self.register.step(dur)
        return pieces, t / sr


def render(corpus, files, audio, sr, seconds, events, rng):
    by_file, gaps = runs_by_file(corpus, sr)
    voices = [Voice(k, rng, corpus, by_file, gaps, files, audio, sr) for k in range(MAX_VOICES)]
    tracks = [[] for _ in voices]
    for v in voices:
        v.t = 0.3 + rng.uniform(0, 2.0) * v.k
    while True:
        v = min(voices, key=lambda q: q.t)
        if v.t >= seconds:
            break
        act = activity_at(v.t, events)
        active = 1 + int(round((MAX_VOICES - 1) * (act - SLEEP) / (1.0 - SLEEP)))
        if v.k >= active:
            v.t += 0.5                                   # not needed right now
            continue
        pieces, end = v.phrase(act)
        tracks[v.k] += pieces
        pause = rng.lognormal(np.log(2.6 - 2.2 * act), 0.5)   # asleep: long pauses
        v.t = end + pause
    length = int(seconds * sr) + int(3 * sr)
    out = np.zeros(length)
    for k, tr in enumerate(tracks):
        if not tr:
            continue
        y = L.mix(tr, length)[:length]
        if VOICE_LOWPASS[k] and VOICE_LOWPASS[k] < sr / 2:
            y = band(y, sr, 0, VOICE_LOWPASS[k])
        out += y
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--teensy-clean", action="store_true")
    ap.add_argument("--seed", type=int, default=21)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.source, "*.WAV")) + glob.glob(os.path.join(args.source, "*.wav")))
    corpus, audio, sr, files = L.build_corpus(paths, args.teensy_clean)
    for name in audio:
        audio[name] = band(audio[name], sr, BAND_LO, min(BAND_HI, sr / 2 * 0.95))
    rng = np.random.default_rng(args.seed)
    renders = {
        "F_arc_3min": (180.0, [40.0]),       # asleep -> a take at 0:40 -> talks -> calms down
        "F_asleep_30s": (30.0, []),
        "F_awake_30s": (30.0, [0.0]),
    }
    for name, (seconds, events) in renders.items():
        y = render(corpus, files, audio, sr, seconds, events, rng)
        L.save(os.path.join(args.out, f"{name}.wav"), y, sr)
        print(f"{name}: {seconds:.0f} s, take events at {events}")


if __name__ == "__main__":
    main()
