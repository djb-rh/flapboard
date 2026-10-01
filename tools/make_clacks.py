#!/usr/bin/env python3
"""Synthesise split-flap clacks as 16-bit mono WAVs (for auditioning, and as
the sign's built-in sounds if one of them wins).

A flap landing is modelled as four parts, each a damped resonance or noise
burst with its own decay:
  impact   1-3 ms broadband click: the card edge hitting the stop
  flap     the card's own ring (a couple of modes, kHz range, 10-40 ms)
  body     the module housing / drum thump (hundreds of Hz, ~15 ms)
  bounce   a smaller copy of impact+flap 7-16 ms later (the card settling)
Each style has six variations with slightly different parameters, so a whole
board never plays the same waveform twice in a row.

Seeded, so re-running produces byte-identical files.
Usage: tools/make_clacks.py [outdir] [rate] [style prefix]
  defaults: sounds/generated 44100, every style named <style>_<n>.wav
  tools/make_clacks.py sounds/device 22050 solari clack   -> the firmware's clack_1..6.wav
"""
import math
import os
import random
import struct
import sys
import wave

OUT = sys.argv[1] if len(sys.argv) > 1 else "sounds/generated"
RATE = int(sys.argv[2]) if len(sys.argv) > 2 else 44100
ONLY = sys.argv[3] if len(sys.argv) > 3 else None
PREFIX = sys.argv[4] if len(sys.argv) > 4 else None

# name: impact (dur s, lowpass 0-1), flap modes [(Hz, tau s, gain)], body (Hz, tau, gain),
#       bounce (delay range s, gain), overall length s
STYLES = {
    # Airport/station board: plastic flaps, crisp tock, audible bounce.
    "solari": dict(impact=(0.0018, 0.55), flap=[(2300, 0.014, 0.55), (3900, 0.008, 0.30)],
                   body=(310, 0.016, 0.45), bounce=((0.009, 0.014), 0.38), length=0.09),
    # Modern home board: softer, more muffled, short.
    "vesta": dict(impact=(0.0025, 0.30), flap=[(1250, 0.010, 0.50), (2100, 0.006, 0.20)],
                  body=(220, 0.014, 0.55), bounce=((0.008, 0.011), 0.22), length=0.07),
    # Aluminium flaps (Pragotron-like): bright and ringy.
    "metal": dict(impact=(0.0012, 0.80), flap=[(4300, 0.028, 0.45), (6100, 0.018, 0.30), (2900, 0.020, 0.2)],
                  body=(380, 0.012, 0.30), bounce=((0.011, 0.016), 0.30), length=0.12),
    # Small desk flip clock: tiny and light.
    "clock": dict(impact=(0.0010, 0.70), flap=[(3600, 0.009, 0.45), (5200, 0.006, 0.25)],
                  body=(650, 0.008, 0.25), bounce=((0.006, 0.009), 0.25), length=0.05),
}


def hit(n, rng, p, gain):
    """One impact: click + flap modes + body thump, as a list of floats."""
    out = [0.0] * n
    idur, lp = p["impact"]
    ni = max(1, int(idur * RATE))
    prev = 0.0
    for i in range(ni):   # one-pole lowpassed noise with a fast decay
        x = rng.uniform(-1, 1)
        prev = prev + lp * (x - prev)
        out[i] += prev * math.exp(-i / (ni * 0.35)) * 0.9
    for f, tau, g in p["flap"]:
        f *= rng.uniform(0.94, 1.06)
        tau *= rng.uniform(0.85, 1.15)
        ph = rng.uniform(0, 2 * math.pi)
        for i in range(n):
            t = i / RATE
            out[i] += g * math.sin(2 * math.pi * f * t + ph) * math.exp(-t / tau)
    f, tau, g = p["body"]
    f *= rng.uniform(0.95, 1.05)
    for i in range(n):
        t = i / RATE
        # a tiny attack so the thump does not click on its own
        a = min(1.0, t / 0.0008)
        out[i] += g * a * math.sin(2 * math.pi * f * t) * math.exp(-t / tau)
    return [s * gain for s in out]


def clack(style, seed):
    rng = random.Random(seed)
    p = STYLES[style]
    n = int(p["length"] * RATE)
    buf = hit(n, rng, p, 1.0)
    (d0, d1), bg = p["bounce"]
    at = int(rng.uniform(d0, d1) * RATE)
    b = hit(n - at, rng, p, bg * rng.uniform(0.7, 1.1))
    for i, s in enumerate(b):
        buf[at + i] += s
    # fade the tail to zero, normalise to -3 dBFS
    fade = int(0.006 * RATE)
    for i in range(fade):
        buf[n - 1 - i] *= i / fade
    peak = max(abs(s) for s in buf) or 1.0
    return [s / peak * 0.707 for s in buf]


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767)) for s in samples))


def main():
    os.makedirs(OUT, exist_ok=True)
    n = 0
    for si, style in enumerate(STYLES):
        if ONLY and style != ONLY:
            continue
        for v in range(6):
            write(os.path.join(OUT, "%s_%d.wav" % (PREFIX or style, v + 1)), clack(style, 1000 * si + v))
            n += 1
    print("wrote %d clacks to %s at %d Hz" % (n, OUT, RATE))


if __name__ == "__main__":
    main()
