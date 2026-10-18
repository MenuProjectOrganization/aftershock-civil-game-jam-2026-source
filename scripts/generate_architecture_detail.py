"""Original tileable building micro surface textures, without external art.

Detail normals/ORM never replace the existing UV0 palette or damage/power data.
The seam pattern is deliberately small-scale (240 cm tile in UE), not a second
window atlas. Facades and roofs keep their source material identity.
"""
from pathlib import Path
import hashlib,json
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'_Imports'/'Original'/'Architecture'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    n=2048;rng=np.random.default_rng(2061017)
    # Periodic multi-scale aggregate: interpolation wraps exactly at the seam.
    y,x=np.mgrid[0:n,0:n].astype(np.float32)
    height=np.zeros((n,n),np.float32)
    for grid,amp in ((8,.10),(32,.05),(128,.024),(512,.008)):
        low=rng.random((grid,grid),dtype=np.float32)-.5
        gx=x*grid/n;gy=y*grid/n;ix=gx.astype(int);iy=gy.astype(int)
        fx=gx-ix;fy=gy-iy;fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy)
        h=(low[iy,ix]*(1-fx)+low[iy,(ix+1)%grid]*fx)*(1-fy)+(low[(iy+1)%grid,ix]*(1-fx)+low[(iy+1)%grid,(ix+1)%grid]*fx)*fy
        height+=h*amp
    # Subtle joints and fine pores, no large painted masonry forced onto roofs.
    joints=np.exp(-np.minimum(x%n,(n-x)%n)**2/7)+np.exp(-np.minimum(y%n,(n-y)%n)**2/7)
    pores=(rng.random((n,n),dtype=np.float32)>.9985).astype(np.float32)
    height-=joints*.018+pores*.028
    dx=(np.roll(height,-1,1)-np.roll(height,1,1))*6
    dy=(np.roll(height,-1,0)-np.roll(height,1,0))*6
    norm=np.stack((-dx,dy,np.ones_like(dx)),axis=-1);norm/=np.linalg.norm(norm,axis=-1,keepdims=True)
    normal=np.uint8(np.clip(norm*.5+.5,0,1)*255+.5)
    rough=np.clip(.66+height*.85+rng.normal(0,.012,(n,n)),.50,.80)
    ao=np.clip(1-pores*.18-joints*.05,.80,1)
    orm=np.stack((ao,rough,np.zeros_like(rough)),axis=-1)
    Image.fromarray(normal,'RGB').save(OUT/'T_Architecture_DetailNormal.png')
    Image.fromarray(np.uint8(orm*255+.5),'RGB').save(OUT/'T_Architecture_DetailORM.png')
    manifest={'version':1,'source':'Original deterministic AFTERSHOCK procedural periodic aggregate, seed2061017; no external photograph, model or texture','license':'Original project work; same license as project','resolution':[n,n],'normal_convention':'DirectX tangent normal (+green below); UE normal sampler decodes RGB','orm_channels':{'R':'micro occlusion','G':'roughness','B':'nonmetal zero'},'world_tile_cm':240,'usage':'Additive triplanar detail normal and subtle roughness only; original palette UV0, cracks, power, operation and WPO retained','files':[],'validation':'Source generation only; UE material compile and live render pending'}
    manifest['ue_assets']={'textures':['/Game/Buildings/Textures/Original/T_Architecture_DetailNormal','/Game/Buildings/Textures/Original/T_Architecture_DetailORM'],'shared_building_masters':['/Game/Buildings/Materials/M_Building_Base','/Game/Buildings/Materials/M_ServiceBuilding']}
    for name in('T_Architecture_DetailNormal.png','T_Architecture_DetailORM.png'):
        p=OUT/name;manifest['files'].append({'file':p.name,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
    (OUT/'SOURCE_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print('AFTERSHOCK_ORIGINAL_ARCHITECTURE_SOURCES '+json.dumps(manifest))

if __name__=='__main__':main()
