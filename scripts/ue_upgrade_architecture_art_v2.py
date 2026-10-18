"""Upgrade owned ASGlobalV1 architecture graphs in place; no node deletion.

Parent launches in UE5.8. This script never changes physics, assets' building
definitions, save data, geometry, module counts or canonical four-float layout.
Only owned material graph connections/parameters are changed and saved. Requires
-AllowCommandletRendering for final native shader statistics.
"""
from pathlib import Path
import json
import re
import traceback
import unreal

ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
OUT=ROOT/'Saved/PresentationValidation/architecture_art_v2_native.json'
L=unreal.MaterialEditingLibrary;A=unreal.EditorAssetLibrary
TAG='ASGlobalV2:'
REPORT={'schema_version':2,'valid':False,'materials':[],'node_deletion':False,
        'physics_modified':False,'canonical_cpd_layout_modified':False,
        'scope':'Owned material graph upgrade and compiled resources, not screenshot/GPU timing proof.'}

def checkpoint():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    OUT.write_text(json.dumps(REPORT,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def need(ok,message):
    if not ok:raise RuntimeError(message)
def enum(cls,key):
    norm=lambda x:re.sub('[^A-Z0-9]','',x.upper())
    for name in dir(cls):
        if norm(name)==norm(key):return getattr(cls,name)
    raise RuntimeError('Missing UE enum '+str(cls)+' '+key)
def node(m,name,kind,**props):
    n=L.create_material_expression(m,getattr(unreal,'MaterialExpression'+kind),0,0)
    need(n is not None,'Cannot create '+name);n.set_editor_property('desc',TAG+name)
    for key,value in props.items():n.set_editor_property(key,value)
    return n
def link(source,target,pin='',output=''):
    need(L.connect_material_expressions(source,output,target,pin),'Cannot connect '+pin)
def prop(m,source,name):
    need(L.connect_material_property(source,'',getattr(unreal.MaterialProperty,'MP_'+name)),'Cannot connect output '+name)
def c(m,name,value):return node(m,name,'Constant',r=float(value))
def vec(m,name,rgb):return node(m,name,'Constant3Vector',constant=unreal.LinearColor(*rgb,1))
def scalar(m,name,value):return node(m,name,'ScalarParameter',parameter_name='ArchitectureV2'+name,default_value=float(value))
def custom(m,name,code,inputs,width=3):
    n=node(m,name,'Custom',code=code,output_type=enum(unreal.CustomMaterialOutputType,'CMOTFLOAT'+str(width)))
    fields=[]
    for name in inputs:
        f=unreal.CustomInput();f.set_editor_property('input_name',name);fields.append(f)
    n.set_editor_property('inputs',fields)
    for name,source in inputs.items():link(source,n,name)
    return n
def inst(m,index,default):
    raw=node(m,'Canonical'+str(index),'PerInstanceCustomData',data_index=index,const_default_value=float(default))
    pixel=node(m,'PixelCanonical'+str(index),'VertexInterpolator');link(raw,pixel);return pixel
def reachable(m,roots):
    found={};queue=[r for r in roots if r]
    while queue:
        n=queue.pop();key=n.get_path_name()
        if key in found:continue
        found[key]=n;queue.extend(x for x in L.get_inputs_for_material_expression(m,n) if x)
    return list(found.values())
def compile_final(m):
    errors=list(L.recompile_material(m));stats=L.get_statistics(m)
    values={k:int(stats.get_editor_property(k)) for k in ('num_vertex_shader_instructions','num_pixel_shader_instructions','num_samplers')}
    need(not errors and values['num_vertex_shader_instructions']>0 and values['num_pixel_shader_instructions']>0,'Final material shader resource '+m.get_path_name()+' '+repr(errors)+' '+repr(values))
    return values

def upgrade(kind):
    path='/Game/Buildings/Architecture/Materials/M_Arch_'+kind
    m=unreal.load_asset(path);need(isinstance(m,unreal.Material),'Missing owned architecture master '+path)
    expressions=list(L.get_material_expressions(m));descs=[str(x.get_editor_property('desc')) for x in expressions]
    need('ASGlobalV1:Complete' in descs,'Preserve unowned/incomplete source graph '+path)
    if TAG+'Complete' in descs:
        REPORT['materials'].append({'path':path,'reused_completed_v2':True,'shader_statistics':compile_final(m)});checkpoint();return
    need(not any(d.startswith(TAG) for d in descs),'PartialV2 graph detected; preserve and repair authoring before retry '+path)
    need(m.get_editor_property('tangent_space_normal'),'Unknown source normal-space contract '+path)
    old={p:L.get_material_property_input_node(m,getattr(unreal.MaterialProperty,'MP_'+p)) for p in ('BASE_COLOR','ROUGHNESS','NORMAL','EMISSIVE_COLOR')}
    need(all(old[p] for p in ('BASE_COLOR','ROUGHNESS','NORMAL')),'Missing source output '+path)
    before_nodes=reachable(m,list(old.values()))
    old_indices={int(n.get_editor_property('data_index')) for n in before_nodes if isinstance(n,unreal.MaterialExpressionPerInstanceCustomData)}
    source_samples={str(n.get_editor_property('parameter_name')):n for n in expressions if isinstance(n,unreal.MaterialExpressionTextureSampleParameter2D)}
    preserved_cpd=[(n.get_path_name(),int(n.get_editor_property('data_index'))) for n in expressions if isinstance(n,(unreal.MaterialExpressionPerInstanceCustomData,unreal.MaterialExpressionPerInstanceCustomData3Vector))]
    damage=inst(m,0,0);retrofit=inst(m,1,0);power=inst(m,2,1);operation=inst(m,3,1);part=inst(m,7,0)
    position=node(m,'AbsoluteMetricWorld','WorldPosition',world_position_shader_offset=enum(unreal.WorldPositionIncludedOffsets,'WPTEXCLUDEALLSHADEROFFSETS'))
    ng=node(m,'GeometricNormalWS','VertexNormalWS')
    tile=scalar(m,'TileCm',250 if kind=='Roof' else 200)
    uv0=node(m,'OriginalPhysicalUV','TextureCoordinate',coordinate_index=0)
    baked_uv=custom(m,'BakedUVTileCalibration','return UV*(200.0/max(Tile,25.0));',{'UV':uv0,'Tile':tile},2)
    # Reconnect only the already-owned photo samplers. Baked geometry keeps its
    # native metricUV basis; Floors/Bays are never multiplied a second time.
    for sample in source_samples.values():link(baked_uv,sample,'UVs')
    projected_uv=custom(m,'BodyMetricDominantAxisUV',r'''
float3 n=normalize(N);float3 a=abs(n);float3 q=P/max(Tile,25.0);
if(a.z>=max(a.x,a.y))return float2(q.x*(n.z<0?-1:1),q.y);
if(a.x>=a.y)return float2(q.y*(n.x<0?-1:1),q.z);
return float2(q.x*(n.y<0?1:-1),q.z);
''',{'P':position,'N':ng,'Tile':tile},2)
    base=old['BASE_COLOR'];rough=old['ROUGHNESS'];normal=old['NORMAL']
    if 'SurfaceAlbedo' in source_samples:
        sample=source_samples['SurfaceAlbedo'];texture=sample.get_editor_property('texture')
        photo=node(m,'MetricAlbedoObject','TextureObjectParameter',parameter_name='ArchitectureV2Albedo',texture=texture,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        base=custom(m,'BodyMetricBasePreserveCanonical',r'''
float body=1-step(.5,abs(Kind));
float2 dx=ddx(UV),dy=ddy(UV);
float3 photo=Texture2DSampleGrad(Tex,TexSampler,UV,dx,dy).rgb;
// The previous neutral PBR graph already contains canonical damage/tint.
// Replace its UV sample through a bounded ratio; preserve those upstream effects.
float3 ratio=clamp(photo/max(OriginalPhoto,float3(.01,.01,.01)),.05,12.0);
return lerp(Original,saturate(Original*ratio),body);
''',{'Original':base,'OriginalPhoto':sample,'UV':projected_uv,'Tex':photo,'Kind':part})
    if 'SurfaceRoughness' in source_samples:
        texture=source_samples['SurfaceRoughness'].get_editor_property('texture')
        obj=node(m,'MetricRoughnessObject','TextureObjectParameter',parameter_name='ArchitectureV2Roughness',texture=texture,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
        rough=custom(m,'BodyMetricRoughness',r'''
float body=1-step(.5,abs(Kind));
return lerp(Original,Texture2DSampleGrad(Tex,TexSampler,UV,ddx(UV),ddy(UV)).r,body);
''',{'Original':rough,'UV':projected_uv,'Tex':obj,'Kind':part},1)
    if 'SurfaceNormal' in source_samples:
        texture=source_samples['SurfaceNormal'].get_editor_property('texture')
        obj=node(m,'MetricNormalObject','TextureObjectParameter',parameter_name='ArchitectureV2NormalDX',texture=texture,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        world=custom(m,'MetricBC5NormalWorld',r'''
float3 n=normalize(N);float3 a=abs(n);
float2 xy=Texture2DSampleGrad(Tex,TexSampler,UV,ddx(UV),ddy(UV)).rg*2-1;
float z=sqrt(saturate(1-dot(xy,xy)));
// The axes/signs are identical to BodyMetricDominantAxisUV. Reconstruction is
// required: Custom texture-object samples return raw BC5 RG, not tangent RGB.
if(a.z>=max(a.x,a.y)){float s=n.z<0?-1:1;return normalize(float3(s*xy.x,xy.y,s*z));}
if(a.x>=a.y){float s=n.x<0?-1:1;return normalize(float3(s*z,s*xy.x,xy.y));}
float s=n.y<0?-1:1;return normalize(float3(-s*xy.x,s*z,xy.y));
''',{'N':ng,'UV':projected_uv,'Tex':obj})
        tangent=node(m,'MetricWorldToExistingTangent','Transform',transform_source_type=enum(unreal.MaterialVectorCoordTransformSource,'TRANSFORMSOURCEWORLD'),transform_type=enum(unreal.MaterialVectorCoordTransform,'TRANSFORMTANGENT'))
        link(world,tangent)
        normal=custom(m,'BodyMetricNormalPreserveBaked',r'''
float body=1-step(.5,abs(Kind));
float3 detail=normalize(lerp(float3(0,0,1),Photo,saturate(Strength)));
return normalize(lerp(Original,detail,body));
''',{'Original':normal,'Photo':tangent,'Kind':part,'Strength':scalar(m,'BodyNormalStrength',.65)})
    gain=.22 if 3 in old_indices else .65
    crack_strength=scalar(m,'CrackStrength',gain if kind not in ('Metal','Roof') else gain*.18)
    base=custom(m,'CanonicalDamageRetrofitOperation',r'''
float d=saturate(Damage);float3 q=P/85.0;
float warped=q.x+sin(q.z*2.7)*.16+sin(q.x*.8+q.z*.5)*.11;
float crackDistance=abs(frac(warped)-.5);
float crack=(1-smoothstep(.006,.022,crackDistance))*smoothstep(.18,.8,d);
float segment=frac(sin(dot(floor(q.xz),float2(127.1,311.7)))*43758.5453);
crack*=step(.22,segment)*saturate(CrackStrength);
float3 colour=lerp(Original,Original*.25,crack);
// New wrappers fill missing oldV1 signals; a freshly provisioned graph that
// already reads1/3 does not receive the same retrofit/operation effect twice.
float band=1-smoothstep(3.0,9.0,abs(frac(P.z/300.0)*300.0-30.0));
colour=lerp(colour,colour*float3(.88,1.025,.96),saturate(Retrofit)*band*.18*AddRetrofit);
colour*=lerp(.88,1.0,saturate(Operation)*.98+.02)*AddOperation+(1-AddOperation);
return saturate(colour);
''',{'Original':base,'P':position,'Damage':damage,'Retrofit':retrofit,'Operation':operation,
       'CrackStrength':crack_strength,'AddRetrofit':c(m,'RetrofitOnlyIfMissing',0 if 1 in old_indices else 1),
       'AddOperation':c(m,'OperationOnlyIfMissing',0 if 3 in old_indices else 1)})
    old_emissive=old['EMISSIVE_COLOR'] or vec(m,'OriginalNoEmission',(0,0,0))
    emissive=custom(m,'CanonicalPowerOperationServiceIndicator',r'''
float door=1-step(.5,abs(Kind-3));
float band=1-smoothstep(4.0,12.0,abs(frac(P.z/220.0)*220.0-170.0));
float3 indicator=float3(.08,.17,.10)*saturate(Power)*saturate(Operation)*door*band;
// Existing glass emission retains its upstream Power2 connection; operation3
// closes an inactive business without inventing a second electrical allocation.
return Original*saturate(Operation)+indicator;
''',{'Original':old_emissive,'P':position,'Kind':part,'Power':power,'Operation':operation})
    prop(m,base,'BASE_COLOR');prop(m,rough,'ROUGHNESS');prop(m,normal,'NORMAL');prop(m,emissive,'EMISSIVE_COLOR')
    L.set_base_material_usage(m,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    L.layout_material_expressions(m);statistics=compile_final(m)
    current=list(L.get_material_expressions(m))
    need(all(any(n.get_path_name()==name and int(n.get_editor_property('data_index'))==index for n in current if isinstance(n,(unreal.MaterialExpressionPerInstanceCustomData,unreal.MaterialExpressionPerInstanceCustomData3Vector))) for name,index in preserved_cpd),'Original CPD nodes/index changed '+path)
    # Completion is committed only after shader AND preserved-CPD validation.
    node(m,'Complete','Constant',r=2)
    need(A.save_loaded_asset(m),'Cannot save V2 '+path)
    REPORT['materials'].append({'path':path,'reused_completed_v2':False,'tile_cm':250 if kind=='Roof' else 200,
        'body_world_metric':bool(source_samples),'baked_facade_uv_repeated_by_floor_or_bay':False,
        'normal_projection':'signed dominant-axis BC5 reconstruction then world-to-tangent; baked tangent input preserved outsideBody',
        'canonical_indices_added':[0,1,2,3],'original_cpd_nodes_preserved':len(preserved_cpd),
        'source_had_cpd1':1 in old_indices,'source_had_cpd3':3 in old_indices,
        'shader_statistics':statistics,'graph_version':2})
    checkpoint()

try:
    for kind in ('Plaster','Wood','Roof','Concrete','Brick','Metal','Glass'):upgrade(kind)
    REPORT['valid']=True;checkpoint();unreal.log_warning('AFTERSHOCK_ARCHITECTURE_ART_V2_COMPLETE '+str(len(REPORT['materials'])))
except Exception as exc:
    REPORT['error']=repr(exc);REPORT['traceback']=traceback.format_exc();checkpoint();raise
