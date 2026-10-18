"""Original deterministic avalanche/debris sound design, not field recordings.

Separate snow shear/air roar and coarse stone contacts. No external samples.
The production owner controls timing and gain from the physical event clock.
"""
from pathlib import Path
import hashlib, json, math, random, struct, wave

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "_Imports/Original/MassMovementAudio"
RATE, SECONDS = 44100, 16


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    receipt = {"schema": 1, "original_project_sound_design": True,
               "external_recordings": False, "files": []}
    for name, seed, snow in [("S_Avalanche_FlowBed", 701337, True),
                             ("S_Landslide_DebrisBed", 801337, False)]:
        rng = random.Random(seed)
        low = middle = impact = 0.
        values = []
        for i in range(RATE * SECONDS):
            t = i / RATE
            noise = rng.uniform(-1., 1.)
            low += .009 * (noise - low)
            middle += .11 * (noise - middle)
            modulation = .65 + .2 * math.sin(t * .7) + .15 * math.sin(t * 1.73)
            if rng.random() < (.0003 if snow else .0013):
                impact += rng.uniform(.15, .8)
            impact *= .993 if snow else .981
            if snow:
                v = (1.4 * low + .32 * middle + .035 * noise) * modulation + .15 * impact * middle
            else:
                v = 1.1 * low * modulation + .2 * middle + .65 * impact * noise
                v += .055 * math.sin(t * 2. * math.pi * 43.) * impact
            values.append(v)
        # Circular overlap joins the tail to the head without a periodic silent
        # gap. The consumed overlap shortens the original16s source by150ms.
        overlap=round(RATE*.15)
        joined=[]
        for i in range(overlap):
            phase=i/(overlap-1)
            weight=phase*phase*(3.-2.*phase)
            joined.append(values[-overlap+i]*(1.-weight)+values[i]*weight)
        values=values[overlap:-overlap]+joined
        peak = max(abs(v) for v in values)
        gain = .65 / max(1e-9, peak)
        data = b"".join(struct.pack("<h", round(v * gain * 32767)) for v in values)
        path = OUT / (name + ".wav")
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            output.writeframes(data)
        receipt["files"].append({"name": name, "file": str(path.relative_to(ROOT)),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "seconds": len(values)/RATE, "rate": RATE, "channels": 1,
            "loop_join": "150ms circular smoothstep overlap; no silent gap",
            "peak": .65, "seed": seed, "clipped_samples": 0})
    (OUT / "SOURCE_MANIFEST.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    build()
