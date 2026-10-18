"""Original deterministic environmental sound design; no external recordings.

16-second 16-bit mono beds. Runtime envelope controls volume; no claim that these
are authentic field recordings of a wildfire, eruption or drought.
"""
from pathlib import Path
import hashlib,json,math,random,struct,wave
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"_Imports/Original/EnvironmentalAudio"
OUT.mkdir(parents=True,exist_ok=True)
RATE=44100;SECONDS=16;N=RATE*SECONDS
manifest={"schema_version":1,"author":"AFTERSHOCK original procedural sound design","external_sources":[],"field_recordings":False,"files":[]}
for kind,name,seed in [("fire","S_ForestFire_CrackleBed",43881),("volcano","S_Volcano_RoarBed",55153),("drought","S_Drought_DryBed",67319)]:
    randomizer=random.Random(seed);low=mid=crackle=0.;samples=[]
    for i in range(N):
        t=i/RATE;noise=randomizer.uniform(-1,1)
        low+=.012*(noise-low);mid+=.18*(noise-mid)
        if kind=="fire":
            if randomizer.random()<.0011: crackle+=randomizer.uniform(.12,.8)
            crackle*=.982
            v=mid*.22+low*.38+crackle*noise*.75
        elif kind=="volcano":
            burst=.55+.45*(.5+.5*math.sin(t*.77))**3
            v=low*1.4+mid*.18*burst+.13*math.sin(t*2*math.pi*38)*burst+.07*math.sin(t*2*math.pi*62)
        else:
            gust=.35+.2*math.sin(t*.83)+.15*math.sin(t*1.37)
            insect=.018*math.sin(t*2*math.pi*2750)*max(0,math.sin(t*7))**8
            v=low*.5*gust+(noise-mid)*.018+insect
        # Quiet equal endpoints avoid clicks when a SoundWave loops.
        fade=min(1.,t/.25,(SECONDS-t)/.25)
        samples.append(v*max(0,fade))
    peak=max(max(abs(v) for v in samples),1e-8);gain=(.65 if kind!="drought" else .25)/peak
    path=OUT/(name+".wav")
    with wave.open(str(path),"wb") as stream:
        stream.setnchannels(1);stream.setsampwidth(2);stream.setframerate(RATE)
        stream.writeframes(b''.join(struct.pack('<h',int(max(-1,min(1,v*gain))*32767)) for v in samples))
    manifest['files'].append({"file":path.name,"sha256":hashlib.sha256(path.read_bytes()).hexdigest(),"seconds":SECONDS,"sample_rate":RATE,"peak":.65 if kind!="drought" else .25,"original_project_asset":True})
    print(name,path.stat().st_size,flush=True)
(OUT/'SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
