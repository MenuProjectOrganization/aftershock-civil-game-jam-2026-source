"""Deterministic offline weather beds from hash-verified CC0 field recordings.

Run with the Codex bundled Python 3.12; soundfile/numpy may be installed in
.tools/audio-python. No network, Unreal process, saved game or source mutation.
The quake bed is original procedural sound design, not a field recording.
"""
from __future__ import annotations

import hashlib
import json
import math
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIVATE_DEPS = ROOT / ".tools" / "audio-python"
if PRIVATE_DEPS.is_dir():
    sys.path.insert(0, str(PRIVATE_DEPS))
import numpy as np
import soundfile as sf

SOURCE = ROOT / "_Imports" / "OpenGameArt" / "WeatherAudio"
OUT = ROOT / "Content" / "Aftershock" / "Audio" / "Generated"
RATE = 44100
SECONDS = 30
FRAMES = RATE * SECONDS
FADE = int(RATE * 1.5)
WORK_FRAMES = FRAMES + FADE
SEED = 20261017


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(audio, dtype=np.float64))))


def normalize(audio: np.ndarray, target: float) -> np.ndarray:
    return audio * (target / max(1e-8, rms(audio)))


def band(audio: np.ndarray, low: float, high: float, brown=False) -> np.ndarray:
    """Smooth FFT shelves, zero phase; circular construction avoids filter starts."""
    freq = np.fft.rfftfreq(len(audio), 1.0 / RATE)
    gain = 1.0 / np.sqrt(1.0 + (freq / high) ** 8)
    if low:
        gain *= 1.0 / np.sqrt(1.0 + (low / np.maximum(freq, 1e-6)) ** 8)
    if brown:
        gain /= np.sqrt(1.0 + (freq / 28.0) ** 2)
    gain[0] = 0.0
    return np.fft.irfft(np.fft.rfft(audio, axis=0) * gain[:, None], n=len(audio), axis=0)


def close_loop(audio: np.ndarray) -> np.ndarray:
    """31.5s source -> 30s loop with a 1.5s raised-cosine seam overlap.

    The seam joins adjacent original samples at N-1/N; the overlap transitions
    back to the beginning over 1.5s, rather than fading the entire bed to silence.
    """
    if len(audio) != WORK_FRAMES:
        raise ValueError("Expected 31.5 seconds before seam construction")
    result = audio[:FRAMES].copy()
    blend = np.sin(np.linspace(0.0, math.pi / 2, FADE, endpoint=True)) ** 2
    result[:FADE] = audio[FRAMES:FRAMES + FADE] * (1.0 - blend[:, None]) + audio[:FADE] * blend[:, None]
    return result


def excerpt(audio: np.ndarray, offset_seconds: float) -> np.ndarray:
    offset = round(offset_seconds * RATE)
    if offset + WORK_FRAMES > len(audio):
        raise ValueError("Source too short for a continuous rain excerpt")
    return audio[offset:offset + WORK_FRAMES].copy()


def circular_add(out: np.ndarray, grain: np.ndarray, start: int, gain=1.0, pan=0.0):
    """Overlapping grains wrap, preserving all tails and deterministic timing."""
    if len(grain) >= len(out):
        raise ValueError("Grain exceeds output")
    grain = grain * np.array([1.0 - max(0.0, pan) * 0.45, 1.0 + min(0.0, pan) * 0.45]) * gain
    start %= len(out)
    end = min(len(out), start + len(grain))
    out[start:end] += grain[:end - start]
    if end - start < len(grain):
        out[:len(grain) - (end - start)] += grain[end - start:]


def grain_window(count: int, attack=0.12, release=0.45):
    env = np.ones(count, dtype=np.float64)
    a, r = min(round(attack * RATE), count // 3), min(round(release * RATE), count // 2)
    env[:a] = np.sin(np.linspace(0, math.pi / 2, a)) ** 2
    env[-r:] = np.cos(np.linspace(0, math.pi / 2, r)) ** 2
    return env[:, None]


def rain_bed(sources):
    # Both layers are long actual recordings. No rapidly repeated noise loop.
    a = normalize(excerpt(sources["ylmir_3.ogg"], 5.25), 0.10)
    b = normalize(excerpt(sources["ylmir_4.ogg"], 2.10), 0.10)
    t = np.arange(WORK_FRAMES) / RATE
    macro = 0.90 + 0.06 * np.sin(2 * math.pi * t / 15.0) + 0.04 * np.sin(2 * math.pi * t / 6.0)
    return band((a * 0.78 + b * 0.26) * macro[:, None], 65.0, 12500.0)


def water_bed(sources, rng, include_rain=True):
    out = np.zeros((WORK_FRAMES, 2), dtype=np.float64)
    waves = [sources["transitking_wave_01.flac"], sources["transitking_wave_02.flac"]]
    for event in range(58):
        original = waves[0 if event % 4 == 0 else 1]
        speed = float(rng.uniform(0.86, 1.14))
        positions = np.arange(0, len(original) - 1, speed)
        grain = np.column_stack([np.interp(positions, np.arange(len(original)), original[:, c]) for c in range(2)])
        grain = normalize(grain, 0.075) * grain_window(len(grain), 0.15, 0.55)
        start = int((event + rng.uniform(-0.35, 0.35)) * WORK_FRAMES / 58)
        circular_add(out, grain, start, float(rng.uniform(0.52, 0.95)), float(rng.uniform(-0.75, 0.75)))
    # River-flood wash may contain quiet rain. Marine tsunami audio must not.
    if include_rain:
        out += normalize(excerpt(sources["ylmir_4.ogg"], 2.0), 0.018)
    return band(out, 48.0, 6800.0)


def wind_bed(sources, rng):
    wind = sources["bashar3a_wind_background.wav"]
    size = RATE * 8
    ranked = []
    for start in range(0, len(wind) - size, RATE):
        grain = wind[start:start + size]
        # Reject sharp high-frequency bag rustles, then select clean long sections.
        blocks = grain[:len(grain) // 1024 * 1024].reshape(-1, 1024, 2)
        levels = np.sqrt(np.mean(blocks * blocks, axis=(1, 2)))
        transient = float(np.percentile(levels, 98) / max(1e-6, np.median(levels)))
        sharp = float(np.mean(np.diff(grain, axis=0) ** 2) / max(1e-8, np.mean(grain ** 2)))
        ranked.append((transient + 8 * sharp, start))
    ranked.sort()
    starts = [start for _, start in ranked[:12]]
    rng.shuffle(starts)
    out = np.zeros((WORK_FRAMES, 2), dtype=np.float64)
    for i in range(7):
        grain = wind[starts[i % len(starts)]:starts[i % len(starts)] + size].copy()
        grain = normalize(grain, 0.10) * grain_window(len(grain), 1.5, 1.5)
        circular_add(out, grain, round(i * WORK_FRAMES / 7), float(rng.uniform(0.78, 1.08)), float(rng.uniform(-0.4, 0.4)))
    t = np.arange(WORK_FRAMES) / RATE
    gust = 0.76 + 0.15 * np.sin(2 * math.pi * t / 10.0) + 0.09 * np.sin(2 * math.pi * t / 6.0 + 0.7)
    # Broad low-pass removes handling clicks while retaining audible wind body.
    return band(out * gust[:, None], 36.0, 2700.0)


def quake_bed(rng):
    # Entirely original procedural low rumble, resonant structure and debris.
    rumble = normalize(band(rng.standard_normal((WORK_FRAMES, 2)), 22.0, 185.0, brown=True), 0.10)
    t = np.arange(WORK_FRAMES) / RATE
    rumble *= (0.75 + 0.20 * np.sin(2 * math.pi * t / 6.0) ** 2)[:, None]
    out = rumble
    for start in (1.2, 4.6, 8.8, 14.3, 18.1, 24.9, 28.0):
        duration = float(rng.uniform(0.60, 1.35))
        gtime = np.arange(round(duration * RATE)) / RATE
        f0, f1 = float(rng.uniform(105, 210)), float(rng.uniform(210, 430))
        phase = 2 * math.pi * (f0 * gtime + (f1 - f0) * gtime ** 2 / (2 * duration))
        creak = np.sin(phase) * 0.013 + np.sin(phase * 2.013) * 0.004
        grain = np.column_stack([creak, np.roll(creak, 103)]) * grain_window(len(gtime), 0.12, 0.32)
        circular_add(out, grain, round(start * RATE), pan=float(rng.uniform(-0.7, 0.7)))
    for _ in range(28):
        count = round(float(rng.uniform(0.025, 0.095)) * RATE)
        debris = band(rng.standard_normal((count, 2)), 700.0, 5100.0)
        debris = normalize(debris, float(rng.uniform(0.006, 0.019))) * grain_window(count, 0.006, 0.05)
        circular_add(out, debris, int(rng.integers(WORK_FRAMES)))
    return out


def write_bed(name, audio, target_rms, used_sources, recipe):
    audio = close_loop(audio)
    audio -= np.mean(audio, axis=0)
    audio = normalize(audio, target_rms)
    # Broad headroom, no brickwall clipping. Peak reduction is one transparent gain.
    peak = float(np.max(np.abs(audio)))
    audio *= min(1.0, 0.78 / max(peak, 1e-8))
    if not np.all(np.isfinite(audio)):
        raise ValueError("Nonfinite audio samples")
    pcm = np.rint(audio * 32767.0).astype("<i2")
    path = OUT / (name + ".wav")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(RATE)
        wav.writeframes(pcm.tobytes())
    measured = pcm.astype(np.float64) / 32768.0
    peak = float(np.max(np.abs(measured)))
    level = rms(measured)
    delta = float(np.max(np.abs(measured[0] - measured[-1])))
    step_p99 = float(np.percentile(np.abs(np.diff(measured, axis=0)), 99.0))
    if peak > 0.8 or np.any(np.abs(pcm.astype(np.int32)) >= 32767):
        raise RuntimeError("Headroom check failed: " + name)
    result = {
        "name": name, "file": path.relative_to(ROOT).as_posix(), "sha256": sha256(path),
        "sample_rate_hz": RATE, "channels": 2, "pcm_bits": 16, "frames": len(pcm),
        "duration_seconds": len(pcm) / RATE, "peak_linear": round(peak, 6),
        "rms_linear": round(level, 6), "rms_dbfs": round(20 * math.log10(max(level, 1e-10)), 2),
        "clipped_samples": 0, "seam_boundary_delta": round(delta, 6), "sample_step_p99": round(step_p99, 6),
        "seam_crossfade_seconds": 1.5, "sources": used_sources, "processing": recipe,
        "provenance": "CC0 field-recording mix" if used_sources else "Original project-authored procedural sound design; no field recording",
    }
    print(json.dumps(result, ensure_ascii=False))
    return result


def main():
    source_manifest = SOURCE / "SOURCE_LICENSE_MANIFEST.json"
    manifest = json.loads(source_manifest.read_text(encoding="utf-8-sig"))
    sources, source_credits = {}, {}
    # Decode only the actual selected sources; reject changed bytes or unclear licenses.
    required = {"ylmir_3.ogg", "ylmir_4.ogg", "transitking_wave_01.flac", "transitking_wave_02.flac", "bashar3a_wind_background.wav"}
    for asset in manifest["assets"]:
        if asset["license"] != "CC0-1.0":
            raise ValueError("Unsupported source license")
        for item in asset["files"]:
            if item["file"] not in required:
                continue
            path = SOURCE / item["file"]
            if sha256(path) != item["sha256"]:
                raise ValueError("Source checksum mismatch: " + item["file"])
            audio, sample_rate = sf.read(path, dtype="float64", always_2d=True)
            if sample_rate != RATE or audio.shape[1] != 2:
                raise ValueError("Expected verified 44.1kHz stereo source")
            sources[item["file"]] = audio - np.mean(audio, axis=0)
            source_credits[item["file"]] = {"file": item["file"], "sha256": item["sha256"], "credit": item["credit"], "license": asset["license"], "license_url": asset["license_url"], "source_page": asset["source_page"]}
    if set(sources) != required:
        raise RuntimeError("Missing verified source files")
    OUT.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    outputs = [
        write_bed("S_Flood_RainBed", rain_bed(sources), 0.12, ["ylmir_3.ogg", "ylmir_4.ogg"], "Two long continuous rain recordings; restrained slow level variation, DC/high-pass filtering; 1.5s raised-cosine seam."),
        write_bed("S_Flood_WaterBed", water_bed(sources, rng), 0.12, ["transitking_wave_01.flac", "transitking_wave_02.flac", "ylmir_4.ogg"], "58 overlapping variable-rate, variable-level stereo wave grains plus quiet recorded rain wash; circular tails and 1.5s seam."),
        write_bed("S_Storm_WindBed", wind_bed(sources, rng), 0.13, ["bashar3a_wind_background.wav"], "Ranked low-transient 8s recording sections, seven long overlapped stereo layers, slow gust modulation, gentle spectral filtering, 1.5s seam."),
        write_bed("S_Earthquake_RumbleBed", quake_bed(rng), 0.14, [], "Seeded original brown-shaped 22-185Hz rumble, seven resonant structural creaks and 28 restrained debris cracks; circular overlaps and 1.5s seam."),
    ]
    # Independent stream and append-only generation keep the four established
    # bed WAVs byte-identical. Only verified ocean-wave recordings enter this mix.
    marine = water_bed(sources, np.random.default_rng(SEED ^ 0x5453554E), include_rain=False)
    marine_time = np.arange(WORK_FRAMES) / RATE
    marine *= (0.78 + 0.22 * np.sin(2 * math.pi * marine_time / 10.0) ** 2)[:, None]
    outputs.append(write_bed("S_Tsunami_SurgeBed", marine, 0.13,
                            ["transitking_wave_01.flac", "transitking_wave_02.flac"],
                            "Marine-only CC0 wave recordings: 58 overlapping variable-rate/level/pan grains, long 10s swell modulation and 1.5s seam. No rain, wind, river-rain filler or synthesized recording claim."))
    report = {"schema_version": 1, "generator": "scripts/build_weather_audio.py", "generator_sha256": sha256(Path(__file__)), "seed": SEED,
              "source_manifest": source_manifest.relative_to(ROOT).as_posix(), "source_manifest_sha256": sha256(source_manifest),
              "sources": [source_credits[k] for k in sorted(source_credits)], "outputs": outputs,
              "limits": "Peak/RMS/format checks are objective signal checks; final perceptual loudness and in-game balance need listening on actual speakers/headphones."}
    (OUT / "WEATHER_AUDIO_MANIFEST.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    credits = ["AFTERSHOCK weather audio source credits", "", "All third-party recordings below: CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/", "Attribution retained voluntarily; original files/hash evidence remain in _Imports/OpenGameArt/WeatherAudio.", ""]
    credits.extend(dict.fromkeys(source_credits[k]["credit"] for k in sorted(source_credits)))
    credits += ["", "S_Earthquake_RumbleBed: original project-authored procedural sound design generated by build_weather_audio.py; not a real earthquake recording.", "Mixing modifications, per-output sources, sample checks and SHA256: WEATHER_AUDIO_MANIFEST.json", ""]
    (OUT / "WEATHER_AUDIO_LICENSES.txt").write_text("\n".join(credits), encoding="utf-8")


if __name__ == "__main__":
    main()
