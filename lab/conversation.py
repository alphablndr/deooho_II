#!/usr/bin/env python3
"""SPIKE (throwaway, 2026-10-14): two "collective speakers" in conversation.

Builds on logos_lab.py (syllable corpus, background-matched choice, own air).
Anton's asks for this round:
  - band-limit the voice: gentle high-pass ~70 Hz (keeps low fundamentals),
    low-pass ~12 kHz (above it there is only hiss)
  - alive, not programmed: every speaker's tempo, register and phrase length
    drift slowly at random; each phrase gets its own melody shape; units are
    one syllable or sometimes 2-3 neighbouring syllables of one take
  - two voices in different "spaces" (near: full band; far: darker, quieter)
    that answer, interrupt, continue, echo each other or fall silent

Writes mono renders and a stereo preview (near left, far right).

  python3 lab/conversation.py ../RECSREWIRE ../lab_renders/x
  python3 lab/conversation.py <teensy folder> ../lab_renders/y --teensy-clean
"""
import argparse
import glob
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logos_lab as L  # noqa: E402

BAND_LO, BAND_HI = 70.0, 12000.0
FAR_LOWPASS = 4500.0
FAR_GAIN = 0.65
CHUNK_PROB = 0.25          # a unit is 2-3 neighbouring syllables of one take
ECHO_PROB = 0.15           # an answer starts with the other's last syllable
CONTOURS = ("fall", "rise", "arch", "flat", "wobble")


def band(x, sr, lo, hi):
    """Zero-phase band-limit with raised-cosine edges (offline, FFT)."""
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    gain = np.ones_like(f)
    if lo > 0:
        ramp = np.clip((f - lo * 0.7) / (lo * 0.3), 0, 1)
        gain *= 0.5 - 0.5 * np.cos(np.pi * ramp)
    if hi < sr / 2:
        ramp = np.clip((hi * 1.15 - f) / (hi * 0.3), 0, 1)
        gain *= 0.5 - 0.5 * np.cos(np.pi * ramp)
    return np.fft.irfft(spec * gain, n=len(x))


class Drift:
    """Slow random walk around a mean (Ornstein-Uhlenbeck): never repeats."""

    def __init__(self, rng, mean, sd, tau):
        self.rng, self.mean, self.sd, self.tau = rng, mean, sd, tau
        self.x = mean + sd * rng.standard_normal()

    def step(self, dt):
        a = min(1.0, dt / self.tau)
        self.x += (self.mean - self.x) * a + self.sd * np.sqrt(2 * a) * self.rng.standard_normal()
        return self.x


def contour(kind, k, n, rng):
    """Pitch offset in octaves for syllable k of n."""
    u = k / max(1, n - 1)
    if kind == "fall":
        return 0.25 - 0.55 * u
    if kind == "rise":
        return -0.2 + 0.5 * u ** 1.5
    if kind == "arch":
        return 0.3 * np.sin(np.pi * u) - 0.1
    if kind == "wobble":
        return 0.18 * np.sin(2 * np.pi * u * rng.uniform(1.0, 2.0))
    return 0.04 * rng.standard_normal()


class Speaker:
    def __init__(self, rng, corpus, by_file, files, audio, sr, register):
        self.rng, self.corpus, self.by_file, self.files, self.audio, self.sr = rng, corpus, by_file, files, audio, sr
        f0s = np.array([c["f0"] for c in corpus if c["f0"] > 0])
        base = np.percentile(f0s, 30 if register == "low" else 70) if len(f0s) else 150.0
        self.register = Drift(rng, np.log2(base), 0.25, 25.0)        # octaves
        self.tempo = Drift(rng, 4.2, 1.1, 18.0)                       # syllables per second
        self.phrase_len = Drift(rng, 6.0, 2.2, 22.0)
        self.bright = Drift(rng, np.log2(np.median([c["bright"] for c in corpus])), 0.4, 20.0)
        self.voiced_share = np.mean([c["f0"] > 0 for c in corpus])
        self.snr_median = float(np.median([c["snr"] for c in corpus]))
        self.target_level = float(np.median([c["rms"] for c in corpus]))
        self.prev = rng.choice(corpus)
        self.used = set()

    def drift(self, dt):
        for d in (self.register, self.tempo, self.phrase_len, self.bright):
            d.step(dt)

    def unit(self, c):
        """One syllable, or with CHUNK_PROB the next 1-2 syllables of the same take too."""
        last = c
        if self.rng.random() < CHUNK_PROB:
            nxt = self.by_file[c["file"]]
            for _ in range(int(self.rng.integers(1, 3))):
                j = last["idx"] + 1
                if j < len(nxt) and nxt[j]["start"] - last["end"] < 0.25 * self.sr:
                    last = nxt[j]
        return c["start"], last["end"], last

    def phrase(self, t, echo=None):
        """Returns pieces [(start_sample, audio)] and the end time (samples)."""
        rng, sr = self.rng, self.sr
        n = max(1, int(round(self.phrase_len.x + rng.normal(0, 1))))
        kind = CONTOURS[rng.integers(len(CONTOURS))]
        voiced = rng.random() < self.voiced_share
        pieces = []
        for k in range(n):
            f0_t = 2 ** (self.register.x + contour(kind, k, n, rng)) if voiced else 0.0
            dur_t = max(0.08, (1.0 / max(self.tempo.x, 1.5)) * rng.uniform(0.5, 1.3))
            if k == 0 and echo is not None:
                c = echo
            else:
                c = L.pick(self.corpus, rng, {self.prev["file"]}, self.used, f0_t, 2 ** self.bright.x, dur_t,
                           0.9 if voiced else 0.2, snr_t=self.prev["snr"], snr_median=self.snr_median)
            self.used.add(id(c))
            s, e, last = self.unit(c)
            x = self.audio[c["file"]]
            pre, post = L.air_pads(self.files, c["file"], s, e, sr, len(x))
            seg = x[s - pre:e + post]
            if rng.random() < L.REVERSE_B:
                seg, pre, post = seg[::-1], post, pre
            gain = min(self.target_level / max(c["rms"], 1e-5) * rng.uniform(0.75, 1.15), 8.0)
            pieces.append((t - pre, L.shaped(seg, pre, post, sr) * gain))
            t += (e - s) + int(sr * max(0.0, (1.0 / max(self.tempo.x, 1.5)) - (e - s) / sr) * rng.uniform(0.3, 1.4))
            self.prev = last
        return pieces, t


def render(corpus, files, audio, sr, seconds, rng):
    by_file = {}
    for c in corpus:
        by_file.setdefault(c["file"], []).append(c)
    speakers = [Speaker(rng, corpus, by_file, files, audio, sr, "low"),
                Speaker(rng, corpus, by_file, files, audio, sr, "high")]
    tracks = [[], []]
    t = int(0.3 * sr)
    turn = int(rng.integers(2))
    echo = None
    events = []
    while t < seconds * sr:
        sp = speakers[turn]
        pieces, end = sp.phrase(t, echo)
        tracks[turn] += pieces
        dt = (end - t) / sr
        for s in speakers:
            s.drift(dt)
        last = sp.prev
        r = rng.random()
        if r < 0.50:                                   # the other answers after a breath
            events.append("answer")
            turn, t = 1 - turn, end + int(sr * rng.uniform(0.15, 0.7))
        elif r < 0.70:                                 # the other interrupts
            events.append("overlap")
            turn, t = 1 - turn, t + int((end - t) * rng.uniform(0.4, 0.85))
        elif r < 0.88:                                 # same speaker goes on
            events.append("continue")
            t = end + int(sr * rng.uniform(0.2, 0.6))
        else:                                          # both fall silent
            events.append("silence")
            turn, t = int(rng.integers(2)), end + int(sr * rng.uniform(1.0, 2.5))
        echo = last if (events[-1] in ("answer", "overlap") and rng.random() < ECHO_PROB) else None
    length = max(max((st + len(sg) for st, sg in tr), default=0) for tr in tracks)
    near = L.mix(tracks[0], length) if tracks[0] else np.zeros(length)
    far = L.mix(tracks[1], length) if tracks[1] else np.zeros(length)
    far = band(far, sr, 0, min(FAR_LOWPASS, sr / 2 * 0.95)) * FAR_GAIN
    return near, far, events


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--teensy-clean", action="store_true")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=30.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(args.source, "*.WAV")) + glob.glob(os.path.join(args.source, "*.wav")))
    corpus, audio, sr, files = L.build_corpus(paths, args.teensy_clean)
    for name in audio:  # Anton: voice band only
        audio[name] = band(audio[name], sr, BAND_LO, min(BAND_HI, sr / 2 * 0.95))
    print(f"corpus: {len(corpus)} syllables from {len(set(c['file'] for c in corpus))} recordings at {sr} Hz")
    for i in range(args.count):
        rng = np.random.default_rng(args.seed + i)
        near, far, events = render(corpus, files, audio, sr, args.seconds, rng)
        L.save(os.path.join(args.out, f"E_conversation_{i + 1}.wav"), near + far, sr)
        stereo = np.stack([near + 0.35 * far, 0.35 * near + far], axis=1)
        peak = np.max(np.abs(stereo))
        if peak > 0.98:
            stereo *= 0.98 / peak
        with wave.open(os.path.join(args.out, f"E_conversation_{i + 1}_stereo.wav"), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes((stereo * 32767).astype("<i2").tobytes())
        counts = {k: events.count(k) for k in ("answer", "overlap", "continue", "silence")}
        print(f"E{i + 1}: {len(events)} phrases, {counts}")


if __name__ == "__main__":
    main()
