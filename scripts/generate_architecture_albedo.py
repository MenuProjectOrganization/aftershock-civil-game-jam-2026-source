"""Original neutral facade/roof detail packed texture; no borrowed photographs.

Four periodic grayscale patterns modulate the established palette. They do not
replace the source UV0 color, change a building's structural material, or claim
photogrammetry. The generated file is linear mask data, not sRGB color.
"""
from pathlib import Path
import hashlib,json
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'_Imports'/'Original'/'Architecture'

def smooth_noise(x,y,count,rng):
    tile=rng.random((count,count),dtype=np.float32)
    gx=x*count/x.shape[1];gy=y*count/y.shape[0];ix=gx.astype(int);iy=gy.astype(int)
    fx=gx-ix;fy=gy-iy;fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy)
    return (tile[iy,ix]*(1-fx)+tile[iy,(ix+1)%count]*fx)*(1-fy)+(tile[(iy+1)%count,ix]*(1-fx)+tile[(iy+1)%count,(ix+1)%count]*fx)*fy

def main():
    OUT.mkdir(parents=True,exist_ok=True);n=2048;rng=np.random.default_rng(2071017)
    y,x=np.mgrid[0:n,0:n].astype(np.float32);u=x/n;v=y/n
    fine=rng.random((n,n),dtype=np.float32)-.5;micro=smooth_noise(x,y,256,rng)-.5;coarse=smooth_noise(x,y,16,rng)-.5
    # 240cm world tile:24x8cm running bond.30courses means parity wraps.
    row=np.floor(v*30).astype(int);bx=np.mod(u*10+(row%2)*.5,1);by=np.mod(v*30,1)
    mortar=(np.minimum(bx,1-bx)<.013)|(np.minimum(by,1-by)<.036)
    brick_id=(np.floor(u*10+(row%2)*.5).astype(int)%10)+row*10
    brick_colors=rng.random(300,dtype=np.float32)-.5
    brick=.48+brick_colors[brick_id]*.16+micro*.05+fine*.032
    brick=np.where(mortar,.76+micro*.02,brick)
    # Cast plaster/concrete: pores, very soft long runoff, aggregate variation.
    streak=np.sin(u*2*np.pi*17+np.sin(v*2*np.pi*2)*.25)*.5+.5
    concrete=.51+coarse*.13+micro*.065+fine*.025-streak**12*.045
    pores=rng.random((n,n),dtype=np.float32)>.9975;concrete-=pores*.07
    # Neutral roof sheets/courses, no bright painted roof replacing the palette.
    rx=np.mod(u*6,1);ry=np.mod(v*16,1)
    seams=np.exp(-np.minimum(rx,1-rx)**2/.00045)+np.exp(-np.minimum(ry,1-ry)**2/.00070)*.45
    roof=.54+coarse*.08+micro*.035+fine*.02-seams*.18
    # Fine timber striation available only for existing warm dark wall colors.
    grain=np.sin(u*2*np.pi*47+np.sin(v*2*np.pi*3)*1.4)*.5+.5
    wood=.5+grain*.035+coarse*.09+fine*.02
    packed=np.uint8(np.clip(np.stack((brick,concrete,roof,wood),axis=-1),0,1)*255+.5)
    path=OUT/'T_Architecture_SurfaceVariation.png';Image.fromarray(packed,'RGBA').save(path)
    manifest={'version':1,'source':'Original deterministic AFTERSHOCK raster authoring code, seed2071017; no third-party art, game asset or photograph','license':'Original project work; same license as project','resolution':[n,n],'data_space':'linear masks, sRGB false; channels are neutral grayscale detail, not replacement albedo','channels':{'R':'24x8cm running-bond masonry tonal pattern','G':'fine plaster/concrete pores and subtle periodic runoff','B':'roof seam/course tonal variation','A':'fine timber striation for existing warm dark walls'},'world_tile_cm':240,'shader_range':'Original basecolor multiplied by0.78+0.44*pattern, excluding source glass swatches; existing palette/damage output remains upstream','material_assignment':'Visual palette/normal heuristics only; never alters gameplay construction material, damage, population or economy','files':[{'file':path.name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}],'proof_scope':'Generated source only; UE import, compile, scene visibility and profile must be separately verified'}
    manifest['ue_assets']={'texture':'/Game/Buildings/Textures/Original/T_Architecture_SurfaceVariation','shared_building_masters':['/Game/Buildings/Materials/M_Building_Base','/Game/Buildings/Materials/M_ServiceBuilding']}
    (OUT/'ALBEDO_SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    thumb=Image.new('RGB',(1024,1024));labels=('Masonry','Plaster / concrete','Roof seams / courses','Timber detail')
    from PIL import ImageDraw
    draw=ImageDraw.Draw(thumb)
    for i in range(4):
        channel=Image.fromarray(packed[:,:,i],'L').convert('RGB').resize((512,512));thumb.paste(channel,((i%2)*512,(i//2)*512));draw.text(((i%2)*512+12,(i//2)*512+12),labels[i],fill=(255,221,135))
    preview=ROOT/'Saved'/'PresentationValidation'/'original-architecture-patterns.png';preview.parent.mkdir(parents=True,exist_ok=True);thumb.save(preview)
    print('AFTERSHOCK_ARCHITECTURE_ALBEDO_SOURCE '+json.dumps(manifest))

if __name__=='__main__':main()
