"""Create original deterministic disaster cues using only Python's stdlib.

These lightweight WAVs are imported into Unreal as SoundWave assets. They are
authored for this project and do not depend on a third-party music/SFX source.
"""

import math
import random
import struct
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "Content" / "Aftershock" / "Audio" / "Generated"
RATE = 44100
DURATION = 5.0
N = int(RATE * DURATION)


def smooth_fade(t, duration, fade=0.18):
    return min(1.0, t / fade, (duration - t) / fade)


def generate(kind, seed):
    rng = random.Random(seed)
    noise = [rng.uniform(-1.0, 1.0) for _ in range(N)]
    out = [0.0] * N
    low = 0.0
    mid = 0.0

    for i in range(N):
        t = i / RATE
        env = max(0.0, min(1.0, smooth_fade(t, DURATION)))
        if kind == "warning":
            # Alternating 440/554 Hz warning tone, with crisp 70 ms transitions.
            frequency = 440.0 if int(t / 0.42) % 2 == 0 else 554.37
            phase = 2.0 * math.pi * frequency * t
            pulse = 1.0 if (t % 1.68) < 1.34 else 0.0
            edge = min(1.0, (t % 1.68) / 0.04, (1.34 - (t % 1.68)) / 0.04) if pulse else 0.0
            out[i] = 0.42 * math.sin(phase) * max(0.0, edge)
        elif kind == "earthquake":
            low += 0.035 * (noise[i] - low)
            wobble = 0.70 + 0.30 * math.sin(2.0 * math.pi * 0.8 * t)
            tone = 0.45 * math.sin(2.0 * math.pi * 46.0 * t) + 0.20 * math.sin(2.0 * math.pi * 73.0 * t)
            out[i] = env * wobble * (0.75 * low + tone)
        elif kind == "flood":
            low += 0.11 * (noise[i] - low)
            mid += 0.26 * (noise[i] - mid)
            flow = 0.68 + 0.24 * math.sin(2.0 * math.pi * 0.31 * t) + 0.08 * math.sin(2.0 * math.pi * 0.77 * t)
            bubbles = 0.12 * math.sin(2.0 * math.pi * (190.0 + 30.0 * math.sin(2.0 * math.pi * 0.5 * t)) * t)
            out[i] = env * flow * (0.60 * mid + 0.25 * low + bubbles)
        elif kind == "wind":
            low += 0.075 * (noise[i] - low)
            gust = 0.52 + 0.35 * (0.5 + 0.5 * math.sin(2.0 * math.pi * 0.23 * t + 0.8))
            howl = math.sin(2.0 * math.pi * (155.0 + 38.0 * math.sin(2.0 * math.pi * 0.19 * t)) * t)
            out[i] = env * gust * (0.72 * low + 0.20 * howl)

    peak = max(max(abs(sample) for sample in out), 1e-8)
    scale = 0.82 / peak
    return [max(-1.0, min(1.0, sample * scale)) for sample in out]


def write_wav(path, samples):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(b"".join(struct.pack("<h", int(sample * 32767)) for sample in samples))


def main():
    sounds = {
        "S_DisasterWarning": ("warning", 17),
        "S_EarthquakeRumble": ("earthquake", 29),
        "S_FloodSurge": ("flood", 43),
        "S_StormGust": ("wind", 61),
    }
    for name, (kind, seed) in sounds.items():
        path = OUT / (name + ".wav")
        write_wav(path, generate(kind, seed))
        print("Wrote %s (%d bytes)" % (path, path.stat().st_size))


if __name__ == "__main__":
    main()
