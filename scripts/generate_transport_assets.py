
"""Generate original compact traffic bodies and a shared citizen status atlas.

No network or third-party source art. Geometry is in centimetres, Z up; +X is
forward. Wheels are native engine cylinders driven by the timer fleet renderer.
"""
from pathlib import Path
import hashlib
import json
import math
from PIL import Image, ImageDraw

ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/"_Imports"/"Original"/"Transport"
PALETTE=[(190,190,190),(29,65,78),(32,36,39),(225,213,175),
         (232,189,94),(153,32,23),(229,229,218),(83,88,91)]


class Mesh:
    def __init__(self,name): self.name=name; self.verts=[]; self.faces=[]
    def solid(self,points,faces,color):
        start=len(self.verts); self.verts+=points
        self.faces += [(tuple(start+i for i in f),color) for f in faces]
    def box(self,x,y,z,w,d,h,color):
        self.solid([(x+a*w/2,y+b*d/2,z+c*h/2) for a,b,c in
                    [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
                     (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]],
                   [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)],color)
    def cabin(self,x,z,length,width,height,rooflength,roofwidth,color):
        self.solid([(x-length/2,-width/2,z),(x+length/2,-width/2,z),
                    (x+length/2,width/2,z),(x-length/2,width/2,z),
                    (x-rooflength/2,-roofwidth/2,z+height),(x+rooflength/2,-roofwidth/2,z+height),
                    (x+rooflength/2,roofwidth/2,z+height),(x-rooflength/2,roofwidth/2,z+height)],
                   [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)],color)
    def panel(self,points,color):
        self.solid(points,[tuple(range(len(points)))],color)
    def save(self):
        lines=["# AFTERSHOCK original transport body; cm; Z up; +X forward","o "+self.name]
        lines+=["v %.4f %.4f %.4f"%p for p in self.verts]
        faces=[]; uvindex=0
        for ids,color in self.faces:
            uv=[(0,0),(1,0),(1,1),(0,1)] if len(ids)==4 else [(.5+.48*math.cos(i*2*math.pi/len(ids)),.5+.48*math.sin(i*2*math.pi/len(ids))) for i in range(len(ids))]
            for u,v in uv: lines.append("vt %.7f %.7f"%((color+.12+.76*u)/8,.12+.76*v))
            faces.append("f "+" ".join("%d/%d"%(idx+1,uvindex+i+1) for i,idx in enumerate(ids))); uvindex+=len(ids)
        (OUT/(self.name+".obj")).write_text("\n".join(lines+faces)+"\n",encoding="ascii")


def car():
    m=Mesh("SM_ResidentCar")
    m.box(0,0,62,380,170,42,0);m.box(0,0,39,370,150,10,2)
    m.cabin(-20,83,230,160,62,150,136,0)
    # Side glass inset; divided rear/driver windows, dark frames.
    for side in (-1,1):
        glass=[(-121,side*79,91),(70,side*79,91),(43,side*68.5,134),(-91,side*68.5,134)]
        m.panel(glass if side<0 else list(reversed(glass)),1)
        m.box(-26,side*78,111,8,5,39,2)
        for x in (-77,28): m.box(x,side*85.8,82,23,3,4,7)
        m.box(63,side*94,99,19,19,10,0)
    m.panel([(91,-70,91),(91,70,91),(55,62,135),(55,-62,135)],1)
    m.panel([(-131,-70,91),(-95,-62,135),(-95,62,135),(-131,70,91)],1)
    for side in (-1,1):
        m.box(191,side*57,64,4,39,18,4);m.box(-191,side*57,66,4,38,18,5)
    m.box(193,0,46,6,142,11,7);m.box(-193,0,46,6,142,11,7)
    m.box(194,0,63,5,42,14,2);m.box(-194,0,59,5,41,13,6)
    m.save();return m


def bus():
    m=Mesh("SM_CityMinibus")
    m.box(0,0,144,650,230,180,0);m.box(0,0,242,638,222,18,6)
    m.box(0,0,55,628,212,15,2);m.box(-30,0,260,210,150,25,7)
    for side in (-1,1):
        for x in (-232,-122,-12,98,208):
            m.box(x,side*116,187,94,4,75,1)
        m.box(0,side*116,105,630,4,12,6)
        m.box(220,side*117,135,65,4,163,2)
        m.box(220,side*119,176,55,3,73,1)
        m.box(220,side*119,98,4,3,147,7)
        m.box(50,side*116,141,85,4,10,6)
        m.box(313,side*134,190,18,23,23,2)
    m.box(326,0,187,4,206,82,1);m.box(-326,0,182,4,192,68,1)
    m.box(328,0,233,5,190,19,2)
    for side in (-1,1):
        m.box(328,side*77,95,5,40,22,4);m.box(-328,side*82,97,5,25,30,5)
    m.box(329,0,64,8,210,16,7);m.box(-329,0,64,8,210,16,7)
    m.box(331,0,107,4,83,33,2);m.box(-331,0,80,4,48,14,6)
    m.save();return m


def status_atlas():
    size=128; atlas=Image.new("RGBA",(8*size,size),(0,0,0,0))
    # Original code-drawn pictograms; alpha border prevents atlas bleed.
    for index in range(6):
        tile=Image.new("RGBA",(size,size),(0,0,0,0));d=ImageDraw.Draw(tile)
        color=[(235,204,100),(130,164,190),(226,161,86),(109,154,183),(211,100,83),(157,177,146)][index]
        d.ellipse((12,12,116,116),fill=(19,27,32,235))
        d.ellipse((17,17,111,111),fill=(*color,255))
        ink=(34,40,43,255)
        if index in (0,1,5):
            d.ellipse((39,43,47,53),fill=ink);d.ellipse((81,43,89,53),fill=ink)
            if index==0:d.arc((38,47,91,88),0,180,fill=ink,width=6)
            elif index==1:d.arc((39,76,90,104),180,360,fill=ink,width=6)
            else:
                d.line((40,78,88,78),fill=ink,width=6)
                d.rectangle((60,27,69,49),fill=(240,239,221,255));d.rectangle((53,33,76,42),fill=(240,239,221,255))
        elif index==2:
            # Fork and plate.
            d.ellipse((53,38,94,87),outline=ink,width=6)
            d.line((37,38,37,91),fill=ink,width=6)
            for x in (29,37,45):d.line((x,36,x,56),fill=ink,width=4)
            d.line((29,56,45,56),fill=ink,width=4)
        elif index==3:
            d.polygon([(68,27),(42,67),(60,67),(53,101),(88,53),(68,53)],fill=ink)
            d.line((30,29,99,98),fill=(237,232,210,255),width=7)
        else:
            d.polygon([(64,29),(101,96),(27,96)],fill=ink)
            d.line((64,49,64,74),fill=(243,227,190,255),width=7)
            d.ellipse((60,82,68,90),fill=(243,227,190,255))
        atlas.alpha_composite(tile,(index*size,0))
    atlas.save(OUT/"T_ResidentEmotions.png")


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    models=[car(),bus()]
    image=Image.new("RGB",(256,32))
    draw=ImageDraw.Draw(image)
    for i,c in enumerate(PALETTE): draw.rectangle((i*32,0,(i+1)*32-1,31),fill=c)
    image.save(OUT/"T_TransportPalette.png");status_atlas()
    manifest={"source":"Original AFTERSHOCK project-authored polygon meshes and code-drawn icons; no third-party assets",
              "coordinates":"centimetres; Z up; positive X forward; body mesh excludes timer-instanced wheels",
              "license":"Original project work; same license as project",
              "models":[{"name":m.name,"vertices":len(m.verts),"polygons":len(m.faces),
                         "bounds_min_cm":[min(p[i] for p in m.verts) for i in range(3)],
                         "bounds_max_cm":[max(p[i] for p in m.verts) for i in range(3)],
                         "dimensions_cm":[max(p[i] for p in m.verts)-min(p[i] for p in m.verts) for i in range(3)]} for m in models],
              "icons":["happy","sad","hungry","power_outage","emergency","sick","reserved","reserved"],"files":[]}
    for p in sorted(OUT.iterdir()):
        if p.suffix in (".obj",".png"):manifest["files"].append({"file":p.name,"sha256":hashlib.sha256(p.read_bytes()).hexdigest()})
    (OUT/"SOURCE_MANIFEST.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print("AFTERSHOCK_TRANSPORT_SOURCES "+json.dumps(manifest["models"]))


if __name__=="__main__":main()
