"""Proposal: matched three-tap PBR texture anti-tiling on existing owned nodes.

Parent executes through the coordinated RHI authoring process. This file does
not import textures, alter imagery/masks/geometry or launch any Engine process.
Existing GlobalGroundNormalQuality is the ONLY runtime quality authority:
low/medium/regional one tap; high/ultra/cinematic three matched stochastic taps.
"""
from pathlib import Path
import json
import traceback
import uuid
from datetime import datetime,timezone
import unreal

L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
TAG='AFTERSHOCK_REGIONAL_STYLE_V1:'
MARKER='// AS_GROUND_STOCHASTIC_V3'
OUT=ROOT/'Saved/PresentationValidation/ground_stochastic_v3.json'
REPORT={'valid':False,'in_progress':True,'run_id':str(uuid.uuid4()),
        'started_utc':datetime.now(timezone.utc).isoformat(),'materials':[],
        'acceptance_scope':'Native authoring/readback only; fresh-process native shader/error readback and real RHI quality/performance are separate gates.'}


def checkpoint():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    temporary=OUT.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
    temporary.replace(OUT)


PATHS=('/Game/Environment/Materials/Presentation/M_PresentationGroundPBR',
       '/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend')
PROPERTIES=(unreal.MaterialProperty.MP_BASE_COLOR,unreal.MaterialProperty.MP_NORMAL,
            unreal.MaterialProperty.MP_ROUGHNESS,unreal.MaterialProperty.MP_EMISSIVE_COLOR,
            unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,unreal.MaterialProperty.MP_OPACITY,
            unreal.MaterialProperty.MP_OPACITY_MASK,unreal.MaterialProperty.MP_METALLIC)

# Each triangular-lattice vertex owns a stateless90degree rotation+phase. The
# same3vertices/weights serve albedo, roughness and correctly reoriented normals.
# A shared lattice vertex has the same sample in each adjacent triangle, so
# blending inserts neither a grid-cell seam nor a discontinuous tile reset.
# Explicit gradients are evaluated BEFORE the uniform quality branch and rotate
# with each sample; hash/floor derivatives never create bad mip bands.
HELPER=r'''
// AS_GROUND_STOCHASTIC_V3
struct RegionalStochastic {
 float3 random3(float2 p) {
  float3 h=frac(float3(p.x,p.y,p.x)*float3(.1031,.11369,.13787));
  h+=dot(h,h.yzx+19.19);
  return frac(float3((h.x+h.y)*h.z,(h.x+h.z)*h.y,(h.y+h.z)*h.x));
 }
 float2 rotate90(float2 v,int r) {
  float2 q=float2(-v.y,v.x);
  return r==0?v:r==1?q:r==2?-v:-q;
 }
 float4 fetch(Texture2D Tex,SamplerState SS,float2 q,float quality,bool normalMap) {
  float2 dx=ddx(q),dy=ddy(q);
  // Existing policy: .15regional/low, .35medium, .60high, .85ultra,1cine.
  [branch] if(quality<.6) return Texture2DSampleGrad(Tex,SS,q,dx,dy);
  // A lattice interval is2 source repeats (~8m for the verified4m dry soil).
  // This is a rendering sample distribution, never a new parcel/biome grid.
  float2 skew=float2(q.x-q.y*.57735026919,q.y*1.15470053838)*.5;
  float2 cell=floor(skew),f=frac(skew),ids[3];float3 w;
  if(f.x+f.y<=1.) {
   ids[0]=cell;ids[1]=cell+float2(1,0);ids[2]=cell+float2(0,1);
   w=float3(1.-f.x-f.y,f.x,f.y);
  } else {
   ids[0]=cell+float2(1,1);ids[1]=cell+float2(0,1);ids[2]=cell+float2(1,0);
   w=float3(f.x+f.y-1.,1.-f.x,1.-f.y);
  }
  // Sharpen broad blend regions gently to retain scan contrast; weights stay
  // continuous and nonnegative across both triangle and lattice boundaries.
  w*=w;w/=max(w.x+w.y+w.z,1.e-6);
  float4 result=0.;
  [unroll] for(int i=0;i<3;++i) {
   float3 h=random3(ids[i]+float2(53.7,19.3));int r=min(3,(int)floor(h.z*4.));
   float2 uv=rotate90(q,r)+h.xy*37.1;
   float4 s=Texture2DSampleGrad(Tex,SS,uv,rotate90(dx,r),rotate90(dy,r));
   if(normalMap) {
    // UV'=R*worldXY: normal slope returns through transpose(R), not R.
    // Re-encode for the existing BC5 decode/normal-strength graph downstream.
    s.rg=rotate90(s.rg*2.-1.,(4-r)&3)*.5+.5;
   }
   result+=s*w[i];
  }
  return result;
 }
};
RegionalStochastic stochastic;
'''


def connection(material,prop):
    return (L.get_material_property_input_node(material,prop),
            str(L.get_material_property_input_node_output_name(material,prop)))


def tagged(material,label):
    nodes=[n for n in L.get_material_expressions(material)
           if str(n.get_editor_property('desc'))==TAG+label]
    if len(nodes)!=1:raise RuntimeError('Expected exactly one existing owned node '+label)
    return nodes[0]


def textures(material):
    return {n.get_path_name():n.get_editor_property('texture').get_path_name()
            for n in L.get_material_expressions(material)
            if n.get_class().get_name() in ('MaterialExpressionTextureObjectParameter','MaterialExpressionTextureSampleParameter2D')
            and n.get_editor_property('texture') is not None}


def quality_parameter(material):
    values=[n for n in L.get_material_expressions(material)
            if isinstance(n,unreal.MaterialExpressionScalarParameter)
            and str(n.get_editor_property('parameter_name'))=='GlobalGroundNormalQuality']
    if len(values)!=1:raise RuntimeError('Existing unique GlobalGroundNormalQuality required; run quality author first')
    return values[0]


def connect_quality(expression,quality):
    inputs=list(expression.get_editor_property('inputs'))
    matches=[i for i in inputs if str(i.get_editor_property('input_name'))=='StochasticQuality']
    if len(matches)>1:raise RuntimeError('Duplicate stochastic quality input')
    if not matches:
        value=unreal.CustomInput();value.set_editor_property('input_name','StochasticQuality')
        inputs.append(value)
        # UE5.8 PyWrapperObject SetEditorProperty explicitly supports NEVER.
        # Suspend PostEditChange while a pin is temporarily unconnected.
        expression.set_editor_property('inputs',inputs,
            notify_mode=unreal.PropertyAccessChangeNotifyMode.NEVER)
    if not L.connect_material_expressions(quality,'',expression,'StochasticQuality'):
        raise RuntimeError('Could not connect existing canonical graphics quality scalar')



def require_reachable(material,prop,target):
    root=L.get_material_property_input_node(material,prop)
    queue=[(root,[])];visited=set()
    while queue:
        node,path=queue.pop(0)
        if node is None:continue
        key=node.get_path_name()
        if key in visited:continue
        visited.add(key)
        if len(visited)>4096:raise RuntimeError('Unexpected material graph traversal budget')
        route=path+[key]
        if node==target:return route
        for source in L.get_inputs_for_material_expression(material,node):
            queue.append((source,route))
    raise RuntimeError('Owned PBR wrapper is NOT reachable from active output: '+target.get_path_name())


def upgrade(material):
    before={p:connection(material,p) for p in PROPERTIES}
    sources=textures(material)
    parameter=quality_parameter(material)
    rows=[]
    roles={'BaseColor':unreal.MaterialProperty.MP_BASE_COLOR,
           'Normal':unreal.MaterialProperty.MP_NORMAL,
           'Roughness':unreal.MaterialProperty.MP_ROUGHNESS}
    # Prove every target is reachable BEFORE changing any owned Custom body.
    reachable={role:require_reachable(material,prop,tagged(material,'Wrap'+role))
               for role,prop in roles.items()}
    for role in ('BaseColor','Normal','Roughness'):
        node=tagged(material,'Wrap'+role)
        if not isinstance(node,unreal.MaterialExpressionCustom):raise RuntimeError('Owned ground node is no longer Custom')
        # Complete the input binding before introducing a code symbol that
        # depends on it. No transient missing-input material compilation.
        connect_quality(node,parameter)
        code=str(node.get_editor_property('code'))
        if MARKER not in code:
            if '// AS_GROUND_MICROVARIATION_V2' not in code:raise RuntimeError('Accepted V2 ground refinement required first')
            anchor='RegionalDomain domain;'
            if code.count(anchor)!=1:raise RuntimeError('Owned domain anchor changed')
            code=code.replace(anchor,anchor+'\n'+HELPER,1)
            sampler='GetMaterialSharedSampler(BaseSampler,Material.Wrap_WorldGroupSettings)'
            old='Texture2DSample(Base,'+sampler+',qb.xy)'
            if code.count(old)!=1:raise RuntimeError('Expected exact one shared base sample '+role)
            normal='true' if role=='Normal' else 'false'
            new='stochastic.fetch(Base,'+sampler+',P.xy/max(BaseTile,50.),StochasticQuality,'+normal+')'
            code=code.replace(old,new,1)
            node.set_editor_property('code',code,
                notify_mode=unreal.PropertyAccessChangeNotifyMode.NEVER)
        rows.append({'owned_role':role,'stochastic_v3':MARKER in code,
                     'active_output_to_wrapper_path':reachable[role]})
    if {p:connection(material,p) for p in PROPERTIES}!=before:raise RuntimeError('A material output connection changed')
    if textures(material)!=sources:raise RuntimeError('A source texture binding changed')
    return {'material':material.get_path_name(),'roles':rows,'source_bindings_unchanged':True,
            'all_material_output_connections_unchanged':True,'new_scalar_parameters':0,
            'quality_parameter':'GlobalGroundNormalQuality','quality_one_tap_below':.6,
            'detail_base_taps_per_pbr_role':3,'maximum_added_texture_fetches_detail':6,
            'new_texture_objects':0,'new_sampler_objects':0,'normal_rotation':'inverse90deg',
            'gradients':'computed before uniform quality branch; each source rotation applied to gradients'}


def main():
    # Clear a previous successful receipt before loading or mutating anything.
    checkpoint()
    materials=[unreal.load_asset(p) for p in PATHS]
    if not all(isinstance(m,unreal.Material) for m in materials):raise RuntimeError('Existing ground material pair missing')
    rows=[upgrade(m) for m in materials]
    REPORT['materials']=rows;checkpoint()
    for material,row in zip(materials,rows):
        errors=list(L.recompile_material(material));stats=L.get_statistics(material)
        values={key:int(stats.get_editor_property(key)) for key in
                ('num_samplers','num_pixel_shader_instructions','num_vertex_shader_instructions','num_pixel_texture_samples')}
        if errors or not 1<=values['num_samplers']<=16 or min(values['num_pixel_shader_instructions'],values['num_vertex_shader_instructions'])<=0:
            raise RuntimeError('V3 compile/resource gate failed; material pair not saved '+material.get_path_name()+' '+repr(errors)+' '+repr(values))
        row['native_statistics']=values;row['compile_errors']=[]
    for material in materials:
        if not A.save_loaded_asset(material):raise RuntimeError('Stochastic ground save failed '+material.get_path_name())
    REPORT.update({'valid':True,'in_progress':False,'materials':rows,
        'preserved':'geology,source land-use/forest/agriculture/water masks,imagery,physics,geometry,WPO,save state',
        'source_physical_scale':'Existing runtime RegionalBaseTileCm unchanged; drysoil400cm, verifiedgreenmeadow140cm after sourcebinding upgrade',
        'native_final_compile_error_readback_required':True})
    checkpoint()
    unreal.log_warning('AFTERSHOCK_GROUND_STOCHASTIC_V3 '+json.dumps(REPORT))


if __name__=='__main__':
    try:main()
    except Exception as exc:
        REPORT.update({'valid':False,'in_progress':False,'error':repr(exc),'traceback':traceback.format_exc()})
        checkpoint()
        raise
