"""Original centimetre mesh primitives. Z-up, +X forward; no external source art."""
from pathlib import Path
import math

def add(a,b): return tuple(x+y for x,y in zip(a,b))
def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def mul(a,s): return tuple(x*s for x in a)
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def unit(a): return mul(a,1/max(1e-12,math.sqrt(dot(a,a))))

class Mesh:
    def __init__(self,name): self.name=name;self.vertices=[];self.faces=[]
    def solid(self,points,faces,color=0,part=0,smooth=True):
        start=len(self.vertices);self.vertices.extend(points)
        self.faces.extend((tuple(start+i for i in face),color,part,smooth) for face in faces)
    def panel(self,points,color=0,part=0): self.solid(points,[tuple(range(len(points)))],color,part,False)
    def box(self,center,size,color=0,part=0):
        rings=[[(center[0]+x*size[0]/2,center[1]+y*size[1]/2,center[2]+z*size[2]/2)for x,y in[(-1,-1),(1,-1),(1,1),(-1,1)]]for z in(-1,1)]
        self.loft(rings,color,part,False)
    def loft(self,rings,color=0,part=0,smooth=True,caps=True):
        n=len(rings[0]);points=[p for ring in rings for p in ring]
        faces=[(j*n+i,j*n+(i+1)%n,(j+1)*n+(i+1)%n,(j+1)*n+i) for j in range(len(rings)-1) for i in range(n)]
        if caps:faces += [tuple(reversed(range(n))),tuple((len(rings)-1)*n+i for i in range(n))]
        self.solid(points,faces,color,part,smooth)
    def round_box(self,center,size,bevel=2,color=0,part=0,segments=3):
        hx,hy,hz=[v/2 for v in size];bevel=min(bevel,hx*.7,hy*.7,hz*.7);rings=[]
        for z,inset in [(-hz,bevel*.6),(-hz+bevel,0),(hz-bevel,0),(hz,bevel*.6)]:
            rx,ry=hx-inset,hy-inset;radius=min(bevel,rx*.8,ry*.8);ring=[]
            for sx,sy,base in [(1,1,0),(-1,1,90),(-1,-1,180),(1,-1,270)]:
                for k in range(segments+1):
                    a=math.radians(base+k*90/segments);ring.append(add(center,(sx*(rx-radius)+radius*math.cos(a),sy*(ry-radius)+radius*math.sin(a),z)))
            rings.append(ring)
        self.loft(rings,color,part)
    def ellipsoid(self,center,radii,color=0,part=0,segments=12,rings=7):
        points=[add(center,(0,0,-radii[2]))]
        for j in range(1,rings+1):
            theta=math.pi*j/(rings+1)
            for k in range(segments):
                phi=2*math.pi*k/segments;points.append(add(center,(radii[0]*math.sin(theta)*math.cos(phi),radii[1]*math.sin(theta)*math.sin(phi),-radii[2]*math.cos(theta))))
        top=len(points);points.append(add(center,(0,0,radii[2])))
        faces=[(0,1+(k+1)%segments,1+k) for k in range(segments)]
        faces += [(1+j*segments+k,1+j*segments+(k+1)%segments,1+(j+1)*segments+(k+1)%segments,1+(j+1)*segments+k) for j in range(rings-1) for k in range(segments)]
        faces += [(top,1+(rings-1)*segments+k,1+(rings-1)*segments+(k+1)%segments) for k in range(segments)]
        self.solid(points,faces,color,part)
    def limb(self,p0,p1,r0,r1,color=0,part=0,segments=8,simple=False):
        axis=unit(sub(p1,p0));u=unit(cross(axis,(0,1,0)))
        if dot(u,u)<.5:u=unit(cross(axis,(1,0,0)))
        v=unit(cross(axis,u));rings=[]
        for t,taper in ([(0,.8),(1,.8)]if simple else[(0,.62),(.12,1),(.8,1),(1,.62)]):
            c=add(p0,mul(sub(p1,p0),t));r=(r0+(r1-r0)*t)*taper
            rings.append([add(c,add(mul(u,r*math.cos(k*2*math.pi/segments)),mul(v,r*math.sin(k*2*math.pi/segments)))) for k in range(segments)])
        self.loft(rings,color,part)
    def tube_z(self,cx,cy,sections,color=0,part=0,segments=24,caps=True):
        self.loft([[(cx+r*math.cos(k*2*math.pi/segments),cy+r*math.sin(k*2*math.pi/segments),z) for k in range(segments)] for z,r in sections],color,part,caps=caps)
    def write(self,path,palette_slots=16,metadata=False):
        path=Path(path);path.parent.mkdir(parents=True,exist_ok=True);normals=[(0.,0.,0.) for _ in self.vertices]
        for ids,_,_,smooth in self.faces:
            normal=cross(sub(self.vertices[ids[1]],self.vertices[ids[0]]),sub(self.vertices[ids[2]],self.vertices[ids[0]]))
            if smooth:
                for i in ids:normals[i]=add(normals[i],normal)
        lines=['# Original AFTERSHOCK source; cm; +X forward; Z up','o '+self.name]+['v %.6f %.6f %.6f'%p for p in self.vertices];face_lines=[];counter=0
        for ids,color,part,smooth in self.faces:
            normal=unit(cross(sub(self.vertices[ids[1]],self.vertices[ids[0]]),sub(self.vertices[ids[2]],self.vertices[ids[0]])));corners=[]
            for j,i in enumerate(ids):
                u,v=.5+.44*math.cos(j*2*math.pi/len(ids)),.5+.44*math.sin(j*2*math.pi/len(ids))
                encoded_u=part*2+(color+.1+.8*u)/palette_slots if metadata else (color+.1+.8*u)/palette_slots
                lines.append('vt %.8f %.8f'%(encoded_u,.1+.8*v));n=unit(normals[i]) if smooth and dot(normals[i],normals[i])>1e-12 else normal
                lines.append('vn %.8f %.8f %.8f'%n);counter+=1;corners.append('%d/%d/%d'%(i+1,counter,counter))
            face_lines.append('f '+' '.join(corners))
        path.write_text('\n'.join(lines+face_lines)+'\n',encoding='ascii')
    def summary(self):
        low=[min(p[i] for p in self.vertices) for i in range(3)];high=[max(p[i] for p in self.vertices) for i in range(3)]
        return {'name':self.name,'vertices':len(self.vertices),'polygons':len(self.faces),'triangles':sum(len(f[0])-2 for f in self.faces),'bounds_min_cm':low,'bounds_max_cm':high,'dimensions_cm':[b-a for a,b in zip(low,high)]}
