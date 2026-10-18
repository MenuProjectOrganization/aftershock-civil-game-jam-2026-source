"""Original chamfered city traffic: three distinct cars, two-axle minibus and wheel.
Centimetres, +X forward, Z up. No reference-game geometry, logo or branded design.
"""
from pathlib import Path
import hashlib,json,math
from PIL import Image,ImageDraw
from original_mesh_tools import Mesh
from generate_transport_assets import status_atlas
ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'_Imports'/'Original'/'Transport'
PALETTE=[(190,193,195),(27,49,61),(24,27,29),(226,219,194),(247,230,176),(153,27,23),(225,225,211),(132,141,148),
         (225,146,43),(18,22,25),(71,78,82),(209,216,215),(75,88,91),(88,96,100),(166,177,183),(239,230,210)]

def body_ring(x,width,low,top):
    return [(x,-width*.8,low),(x,width*.8,low),(x,width,low+4),(x,width,top-5),
            (x,width-7,top),(x,-width+7,top),(x,-width,top-5),(x,-width,low+4)]

def inset_glass(mesh,points,shrink=.94):
    mesh.panel(points,9)
    center=tuple(sum(p[i]for p in points)/len(points)for i in range(3))
    glass=[tuple(center[i]+(p[i]-center[i])*shrink for i in range(3))for p in points]
    # Offset along the outward normal avoids coplanar paint/seal/glass flicker.
    from original_mesh_tools import unit,cross,sub,add,mul
    normal=unit(cross(sub(points[1],points[0]),sub(points[2],points[0])))
    mesh.panel([add(p,mul(normal,.24))for p in glass],1)

def side_line(mesh,side,points,width=.65,color=9):
    for a,b in zip(points,points[1:]):
        mesh.panel([(a[0],side*a[1],a[2]-width),(b[0],side*b[1],b[2]-width),
                    (b[0],side*b[1],b[2]+width),(a[0],side*a[1],a[2]+width)][::(-side)],color)

def fender(mesh,x,side,y,radius,z):
    rings=[]
    for r in (radius+3,radius+6.3):
        rings.append([(x+r*math.cos(i*math.pi/16),side*y,z+r*math.sin(i*math.pi/16))for i in range(17)])
    for i in range(16):
        points=[rings[0][i],rings[0][i+1],rings[1][i+1],rings[1][i]]
        mesh.panel(points if side>0 else list(reversed(points)),0)

def car(kind):
    name,length,width,height,roof_rear,roof_front=(
        ('SM_ResidentCar',450,182,144,-88,42) if kind==0 else
        ('SM_ResidentHatchback',410,178,153,-125,32) if kind==1 else
        ('SM_ResidentUtility',440,184,170,-125,43))
    m=Mesh(name);half=length/2;w=width/2;belt=85 if kind!=2 else 100
    xs=sorted(set([-half,-half+18,-180,-158,-145,-132,-120,-108,-95,-82,-55,0,55,82,95,108,120,132,145,158,180,half-18,half]))
    rings=[]
    for x in xs:
        end=max(0,(abs(x)-(half-35))/35);sidew=w-15*end
        cut=max([31+math.sqrt(max(0,38**2-(x-a)**2))if abs(x-a)<38 else 38 for a in(-120,120)])
        upper=belt-12*end;rings.append(body_ring(x,sidew,min(cut,upper-8),upper))
    m.loft(rings,0)
    cabin_rear=-154 if kind==0 else-half+28;cabin_front=106
    cabin=[]
    for x,top,roofw in [(cabin_rear,belt+6,w-8),(roof_rear-20,height-9,w-23),
                        (roof_rear,height,w-20),(roof_front,height,w-20),(roof_front+20,height-10,w-23),(cabin_front,belt+5,w-8)]:
        cabin.append([(x,-(w-8),belt),(x,w-8,belt),(x,w-8,belt+5),(x,roofw,top-4),
                      (x,roofw-5,top),(x,-roofw+5,top),(x,-roofw,top-4),(x,-(w-8),belt+5)])
    m.loft(cabin,0)
    # Real frame thickness around independently authored windshield/side panes.
    for side in(-1,1):
        y=w-7.4;top_y=w-18
        rear=[(cabin_rear+8,side*y,belt+7),(-24,side*y,belt+7),(-24,side*top_y,height-7),(roof_rear+1,side*top_y,height-7)]
        front=[(-17,side*y,belt+7),(cabin_front-10,side*y,belt+7),(roof_front+14,side*top_y,height-14),(-17,side*top_y,height-7)]
        inset_glass(m,rear if side<0 else list(reversed(rear)));inset_glass(m,front if side<0 else list(reversed(front)))
        for x in(-80,35):m.round_box((x,side*(w+.6),belt-1),(19,2.4,3.3),.8,7)
        m.round_box((84,side*(w+8),belt+18),(19,13,9),2.5,0)
        side_line(m,side,[(-23,w+.35,44),(-23,w+.35,belt),(-23,w-19,height-6)],.55)
        side_line(m,side,[(cabin_rear+5,w+.4,44),(cabin_rear+5,w+.4,belt+3)],.55)
        side_line(m,side,[(-half+28,w-1,44),(half-28,w-1,44)],.75,10)
        for axle in(-120,120):fender(m,axle,side,w+.7,31,31)
        # Front and rear optical housings, white/amber/red lenses and DRL edges.
        m.round_box((half-1,side*(w*.59),belt-16),(5,39,16),2.2,9)
        m.round_box((half+1.4,side*(w*.59),belt-15),(2.3,34,11),1.2,4)
        m.round_box((half+2.8,side*(w*.59),belt-20),(1.1,32,2.2),.3,11)
        m.round_box((-half-1,side*(w*.65),belt-25),(4,34,16),2,5)
        m.round_box((half+2.2,side*(w*.81),belt-15),(2.2,5,11),.5,8)
        side_line(m,side,[(half-8,w-14,belt+1),(115,w-2,belt+1)],.35)
    # Derive windshield planes from the actual roof loft. A guessed sloped
    # rectangle can sit inside the solid cabin despite having valid normals.
    # These seals sit1.1cm outside the authored paint plane, not in its interior.
    from original_mesh_tools import unit,cross,sub,add,mul
    for rearward in(False,True):
        bottom=(cabin_rear,belt+6,w-13)if rearward else(cabin_front,belt+5,w-13)
        top=(roof_rear-20,height-9,w-28)if rearward else(roof_front+20,height-10,w-28)
        points=[]
        for t,side in((.10,-1),(.10,1),(.90,1),(.90,-1)):
            x=bottom[0]+(top[0]-bottom[0])*t;z=bottom[1]+(top[1]-bottom[1])*t;halfwidth=(bottom[2]+(top[2]-bottom[2])*t)*.94
            points.append((x,side*halfwidth,z))
        if rearward:points=list(reversed(points))
        normal=unit(cross(sub(points[1],points[0]),sub(points[2],points[0])))
        inset_glass(m,[add(p,mul(normal,1.1))for p in points])
    m.round_box((half+.8,0,51),(5,width-23,15),2,10);m.round_box((-half-.8,0,51),(5,width-23,15),2,10)
    m.round_box((half+2,0,66),(3,65,18),1.5,9)
    for z in(61,66,71):m.round_box((half+3.8,0,z),(1.3,58,1.2),.3,7)
    for x in(-half-3.2,half+4.3):m.round_box((x,0,51),(1,39,10),.4,6)
    if kind==2:
        for side in(-1,1):m.round_box((-25,side*61,height+3),(175,4,5),1,7)
    return m

def bus():
    m=Mesh('SM_CityMinibus');rings=[]
    xs=sorted(set([-325,-312,-280,-230,-210,-200,-180,-160,-140,-110,0,110,160,180,190,210,230,250,280,312,325]))
    for x in xs:
        cut=max([43+math.sqrt(max(0,49**2-(x-a)**2))if abs(x-a)<49 else 48 for a in(-180,210)])
        w=115-max(0,(abs(x)-300)*.24)
        rings.append([(x,-w*.87,cut),(x,w*.87,cut),(x,w,cut+5),(x,w,233),(x,w-4,248),(x,w-12,256),
                      (x,-w+12,256),(x,-w+4,248),(x,-w,233),(x,-w,cut+5)])
    m.loft(rings,0)
    for side in(-1,1):
        for x in(-246,-145,-44,57,158):
            points=[(x-43,side*115.8,151),(x+43,side*115.8,151),(x+43,side*115.8,228),(x-43,side*115.8,228)]
            inset_glass(m,points if side<0 else list(reversed(points)),.94)
        side_line(m,side,[(-300,115.5,128),(298,115.5,128)],3.5,3)
        side_line(m,side,[(-300,115.5,114),(298,115.5,114)],.8,10)
        for x in(222,267):side_line(m,side,[(x,115.7,60),(x,115.7,233)],.7)
        inset_glass(m,[(224,side*116,148),(264,side*116,148),(264,side*116,224),(224,side*116,224)][::(-side)])
        m.round_box((310,side*132,199),(19,22,29),2,9)
        for axle in(-180,210):fender(m,axle,side,115.7,43,43)
    inset_glass(m,[(326.2,-98,157),(326.2,98,157),(326.2,98,231),(326.2,-98,231)])
    inset_glass(m,[(-326.2,96,157),(-326.2,-96,157),(-326.2,-96,225),(-326.2,96,225)])
    m.round_box((326,0,242),(3,182,16),1,9)
    m.round_box((-16,0,262),(201,154,16),5,10)
    for side in(-1,1):
        m.round_box((326,side*83,112),(5,29,21),2,4);m.round_box((-326,side*89,119),(5,21,31),2,5)
    for x in(-327,327):m.round_box((x,0,65),(6,211,16),3,10)
    m.round_box((329,0,108),(2,83,31),2,9)
    for z in(98,105,112,119):m.round_box((330.4,0,z),(1,76,1.4),.3,7)
    for x in(-331,331):m.round_box((x,0,78),(1,44,12),.3,6)
    return m

def wheel():
    m=Mesh('SM_VehicleWheel');m.tube_z(0,0,[(-50,42),(-46,47),(-30,50),(30,50),(46,47),(50,42)],2,segments=32,caps=False)
    for side in(-1,1):
        start_face=len(m.faces)
        m.tube_z(0,0,[(side*49,42),(side*50,30),(side*49,27)],2,segments=32,caps=False)
        m.tube_z(0,0,[(side*49,29),(side*50.2,27.8),(side*50.2,25.3),(side*49,24)],7,segments=32,caps=False)
        m.tube_z(0,0,[(side*48,24),(side*48.2,22)],10,segments=24)
        m.tube_z(0,0,[(side*49,8),(side*51,8)],7,segments=16)
        for j in range(5):
            a=j*2*math.pi/5;u=(math.cos(a),math.sin(a));v=(-u[1],u[0]);z=side*50.4
            m.panel([(u[0]*r+v[0]*w,u[1]*r+v[1]*w,z)for r,w in[(7,-2.5),(26,-4),(26,4),(7,2.5)]],7)
            m.tube_z(12*u[0],12*u[1],[(side*50.7,1.8),(side*51.3,1.8)],14,segments=6)
        if side<0:
            # Mirroring Z changes handedness. Reverse every mirrored sidewall,
            # rim and bolt face, so both outboard sides retain outward normals.
            m.faces[start_face:]=[(tuple(reversed(ids)),c,p,smooth)for ids,c,p,smooth in m.faces[start_face:]]
    # Raised diagonal tread bars rotate with the shared wheel instance.
    for j in range(32):
        a=j*2*math.pi/32
        p=[((50.2+r)*math.cos(a+t),(50.2+r)*math.sin(a+t),z)for t,z,r in[(-.025,-29,0),(.025,-22,0),(.045,26,0),(-.005,29,0)]]
        m.panel(p,10)
    return m

def main():
    OUT.mkdir(parents=True,exist_ok=True);models=[car(0),car(1),car(2),bus(),wheel()]
    for m in models:m.write(OUT/(m.name+'.obj'))
    palette=Image.new('RGB',(512,32));d=ImageDraw.Draw(palette)
    for i,c in enumerate(PALETTE):d.rectangle((i*32,0,(i+1)*32-1,31),fill=c)
    palette.save(OUT/'T_TransportPalette.png');status_atlas()
    manifest={'version':2,'source':'Original AFTERSHOCK chamfer/loft models and code-drawn icons; no third-party vehicle, brand, badge or game art','license':'Original project work; same license as project','coordinates':'centimetres; Z up; +X forward; body pivot at roadZ0; body excludes separately timer-instanced wheels','glass':'Dark tinted opaque PBR panes for a bounded shared-material city renderer; no transparent cabin simulation claim','models':[m.summary()for m in models],
              'wheel_contract':{'asset':'SM_VehicleWheel','source_axis':'Z','source_radius_cm':50,'source_width_cm':102.6,'car':{'axle_x_cm':[-120,120],'half_width_cm':78,'radius_cm':31,'axles':2},'bus':{'axle_x_cm':[-180,210],'half_width_cm':107,'radius_cm':43,'axles':2},'render_scale':'XY=radius/50,Z=.20, followed by localX90deg and car yaw; wheel shared Body material preserves rim/rubber palette'},'palette_columns':16,'icons':['happy','sad','hungry','power_outage','emergency','sick','reserved','reserved'],'validation':'Source generation only; Unreal import/render and GPU profile not yet run','files':[]}
    manifest['ue_assets']={'models':['/Game/Environment/Transport/'+m.name for m in models],'shared_body_and_wheel_material':'/Game/Environment/Transport/Materials/M_VehicleBody','emotion_material':'/Game/Environment/Transport/Materials/M_ResidentEmotions','palette':'/Game/Environment/Transport/Textures/T_TransportPalette','emotion_atlas':'/Game/Environment/Transport/Textures/T_ResidentEmotions'}
    for p in sorted(OUT.iterdir()):
        if p.suffix in('.obj','.png'):manifest['files'].append({'file':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
    (OUT/'SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8');print('AFTERSHOCK_TRANSPORT_V2_SOURCES '+json.dumps(manifest['models']))

if __name__=='__main__':main()
