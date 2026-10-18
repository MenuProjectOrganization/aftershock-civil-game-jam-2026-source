"""Original175cm neutral-standing articulated static citizen for shared GPU gait.
OBJ UV0 temporarily encodes partID; UE authoring moves it to UV1. No skeletal actors.
"""
from pathlib import Path
import hashlib,json,math,shutil
from PIL import Image,ImageDraw
from original_mesh_tools import Mesh
ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'_Imports'/'Original'/'Citizens'
LEGACY=ROOT/'Content'/'Aftershock'/'KenneyMeshes'/'_Imports'/'SM_CitizenProxy.obj'
PARTS={0:('torso',120),1:('pelvis',93),2:('head',154),3:('hair',154),4:('neck',149),5:('left_upper_arm',145),6:('left_forearm',116),7:('left_hand',91),8:('right_upper_arm',145),9:('right_forearm',116),10:('right_hand',91),11:('left_thigh',93),12:('left_calf',53),13:('left_foot',9),14:('right_thigh',93),15:('right_calf',53),16:('right_foot',9),17:('brows',154),18:('eyes',154),19:('belt',93),20:('shirt_details',120)}
PALETTE=[(189,202,201),(46,62,77),(31,29,26),(185,136,103),(52,38,29),(51,44,36),(221,218,197),(62,68,71),(22,30,34),(210,205,195),(194,153,116),(112,74,50),(160,106,78),(238,187,151),(219,222,219),(77,80,83)]

def citizen(lod):
    n=[12,8,5][lod];limb_n=[8,6,4][lod];m=Mesh('SM_CitizenProxy'+('' if lod==0 else '_LOD%d'%lod));rings=[]
    for z,depth,width in [(96,10,15),(104,11.4,16),(125,12,18),(140,10,18.5),(145,7.5,16)]:
        rings.append([(depth*math.cos(k*2*math.pi/n),width*math.sin(k*2*math.pi/n),z) for k in range(n)])
    m.loft(rings,0,0);m.ellipsoid((0,0,94),(11,15.5,11),1,1,n,4);m.limb((0,0,145),(0,0,155),5,5.2,3,4,limb_n)
    m.ellipsoid((.5,0,164),(9.2,8.8,10.4),3,2,n,[8,5,3][lod]);hair=[]
    for z,rx,ry in [(165,9.2,9),(171,8.2,8.1),(174.5,4.2,4.1),(175,.5,.5)]:
        hair.append([(.1+rx*math.cos(k*2*math.pi/n),ry*math.sin(k*2*math.pi/n),z) for k in range(n)])
    m.loft(hair,4,3)
    if lod<2:m.ellipsoid((9.25,0,163.7),(2.2,2,2.5),3,2,max(4,limb_n),3)
    for side,upper,fore,hand,thigh,calf,foot in [(-1,5,6,7,11,12,13),(1,8,9,10,14,15,16)]:
        if lod<2:m.ellipsoid((.2,side*8.7,163.5),(2.4,1.6,3),3,2,max(4,limb_n),3)
        m.limb((0,side*19.5,145),(2,side*19.5,116),4.8,4,0,upper,limb_n,lod==2);m.limb((2,side*19.5,116),(3,side*19.8,91),3.9,2.8,0,fore,limb_n,lod==2)
        m.ellipsoid((4,side*19.8,87),(3.5,2.7,5),3,hand,limb_n,2 if lod==2 else 4)
        m.limb((0,side*8.5,95),(1,side*8.6,53),7.5,5.7,1,thigh,limb_n,lod==2);m.limb((1,side*8.6,54),(0,side*8.8,9),5.6,3.9,1,calf,limb_n,lod==2)
        if lod==2:m.box((6,side*8.8,5.2),(26,10.4,10.4),2,foot)
        else:m.round_box((6,side*8.8,5.2),(26,10.4,10.4),2.2,2,foot,max(1,limb_n//4))
        if lod<2:
            m.ellipsoid((8.8,side*3.5,166),(1,1.6,1.25),8,18,6,3);m.round_box((8.2,side*3.4,168),(1,3.7,.9),.2,4,17,1)
    if lod==2:m.box((0,0,101),(21,31,3),5,19)
    else:m.round_box((0,0,101),(21,31,3),.7,5,19,1)
    if lod==0:
        for z in (113,121,129,137):m.ellipsoid((11.6,0,z),(.8,.8,.8),6,20,6,3)
        m.round_box((10.6,-8,130),(1,6,7),.5,0,20,1)
    return m

def main():
    OUT.mkdir(parents=True,exist_ok=True);models=[]
    for lod in range(3):
        mesh=citizen(lod);mesh.write(OUT/(mesh.name+'.obj'),metadata=True);models.append(mesh.summary())
    LEGACY.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(OUT/'SM_CitizenProxy.obj',LEGACY)
    atlas=Image.new('RGB',(512,32));draw=ImageDraw.Draw(atlas)
    for i,color in enumerate(PALETTE):draw.rectangle((i*32,0,(i+1)*32-1,31),fill=color)
    atlas.save(OUT/'T_CitizenPalette.png')
    manifest={'version':2,'source':'Original AFTERSHOCK authoring code; no third-party character or likeness','license':'Original project work; same license as project','units':'centimetres; Z up; +X forward; soleZ0','height_cm':175,'shoulder_breadth_cm':models[0]['dimensions_cm'][1],'parts':{str(k):{'name':v[0],'pivot_z_cm':v[1]}for k,v in PARTS.items()},'uv_contract':'OBJ UV0.u=partID*2+(paletteColumn+.1+.8*surfaceU)/16. UE writes UV1=(partID,pivotZ/200),UV0=paletteU,surfaceV; never lightmap-generated UV1.','instance_data':['red','green','blue','phaseRadians','actualSpeedCmPerSec','lastUpdateWorldSeconds','run01'],'gait':{'walk_stride_cm':140,'run_stride_cm':170,'idle_speed_cutoff_cm_s':2,'phase_extrapolation_seconds_max':.16,'wpo_fade_start_cm':9000,'wpo_fade_end_cm':16000,'max_authored_displacement_cm':85},'models':models,'validation':'Source dimensions/triangle counts only; UE import, animation and GPU profile still required','files':[]}
    manifest['ue_assets']={'mesh':'/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy','shared_material':'/Game/Environment/Materials/M_CitizenActivity','palette':'/Game/Environment/Textures/Citizens/T_CitizenPalette'}
    for p in sorted(OUT.iterdir()):
        if p.suffix in('.obj','.png'):manifest['files'].append({'file':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
    (OUT/'SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8');print('AFTERSHOCK_CITIZEN_SOURCES '+json.dumps(models))

if __name__=='__main__':main()
