"""Software orthographic source geometry proof, explicitly NOT a UE screenshot."""
from pathlib import Path
import math
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from original_mesh_tools import Mesh
from generate_citizen_v2 import citizen,PALETTE as CP
from generate_transport_v2 import car,bus,wheel,PALETTE as VP

ROOT=Path(__file__).resolve().parent.parent

def with_wheels(body,kind):
    source=wheel();radius=43 if kind=='bus' else 31;ys=107 if kind=='bus' else 78;xs=(-180,210)if kind=='bus'else(-120,120)
    for x in xs:
        for side in(-1,1):
            start=len(body.vertices)
            body.vertices.extend((px*radius/50+x,-pz*.20+side*ys,py*radius/50+radius)for px,py,pz in source.vertices)
            body.faces.extend((tuple(i+start for i in ids),c,p,smooth)for ids,c,p,smooth in source.faces)
    return body

def render(mesh,palette,yaw=-.65,elevation=.50,size=620):
    pts=np.array(mesh.vertices,float);ca,sa=math.cos(yaw),math.sin(yaw)
    right=np.array((-sa,ca,0));forward=np.array((ca,sa,0));up=np.array((-ca*math.sin(elevation),-sa*math.sin(elevation),math.cos(elevation)))
    depthdir=np.cross(right,up);projected=np.stack((pts@right,-pts@up,pts@depthdir),axis=1)
    lo=projected[:,:2].min(0);hi=projected[:,:2].max(0);scale=(size-70)/max(hi-lo)
    projected[:,:2]=(projected[:,:2]-(lo+hi)*.5)*scale+size/2
    pixels=np.zeros((size,size,3),np.uint8);pixels[:]=(27,34,42);depth=np.full((size,size),-1e30)
    light=np.array((.35,-.48,.80));light/=np.linalg.norm(light)
    for ids,color,part,smooth in mesh.faces:
        a0,b0,c0=pts[list(ids[:3])];n=np.cross(b0-a0,c0-a0);n/=max(np.linalg.norm(n),1e-12)
        # Source contact sheet is intentionally double-sided to reveal all
        # authored panels; native Unreal normals/culling are validated separately.
        shade=.40+.60*max(0,float(n@light));rgb=np.clip(np.array(palette[color])*shade,0,255).astype(np.uint8)
        for i in range(1,len(ids)-1):
            a,b,c=projected[[ids[0],ids[i],ids[i+1]]];ab=b[:2]-a[:2];ac=c[:2]-a[:2];area=ab[0]*ac[1]-ab[1]*ac[0]
            if abs(area)<1e-7:continue
            x0,y0=np.maximum(0,np.floor(np.minimum(np.minimum(a[:2],b[:2]),c[:2]))).astype(int)
            x1,y1=np.minimum(size-1,np.ceil(np.maximum(np.maximum(a[:2],b[:2]),c[:2]))).astype(int)
            if x1<x0 or y1<y0:continue
            yy,xx=np.mgrid[y0:y1+1,x0:x1+1];u=((b[0]-xx)*(c[1]-yy)-(b[1]-yy)*(c[0]-xx))/area
            v=((c[0]-xx)*(a[1]-yy)-(c[1]-yy)*(a[0]-xx))/area;w=1-u-v;z=u*a[2]+v*b[2]+w*c[2]
            sub=depth[y0:y1+1,x0:x1+1];mask=(u>=0)&(v>=0)&(w>=0)&(z>sub);sub[mask]=z[mask];pixels[y0:y1+1,x0:x1+1][mask]=rgb
    return Image.fromarray(pixels)

def main():
    panels=[(citizen(0),CP,'Citizen175cm LOD0:1948tri',-.25,.15),(citizen(2),CP,'Citizen175cm LOD2:344tri',-.65,.18)]
    panels +=[(with_wheels(car(i),'car'),VP,label,-.68,.40)for i,label in enumerate(('Sedan body3604tri','Hatchback body3604tri','Utility body3852tri'))]
    panels.append((with_wheels(bus(),'bus'),VP,'Minibus2axles body2724tri',-.68,.40))
    sheet=Image.new('RGB',(1240,2010),(27,34,42));draw=ImageDraw.Draw(sheet)
    draw.text((20,16),'AFTERSHOCK ORIGINAL SOURCE GEOMETRY — software preview, not Unreal render',(235,231,218))
    for i,(mesh,pal,title,yaw,elev)in enumerate(panels):
        x=(i%2)*620;y=55+(i//2)*650;sheet.paste(render(mesh,pal,yaw,elev),(x,y));draw.text((x+22,y+621),title,(227,224,211))
    out=ROOT/'Saved'/'PresentationValidation'/'original-visual-source-contact-sheet.png';out.parent.mkdir(parents=True,exist_ok=True);sheet.save(out);print(out)

if __name__=='__main__':main()
