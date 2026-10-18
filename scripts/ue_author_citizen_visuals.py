"""175cm static citizen with three authored LODs and shared GPU articulated gait.

Only mesh/material authoring. No world/game startup, gameplay, skeletal actors,
citizen ticks, private MIDs, asset deletion or material-expression deletion.
"""
from pathlib import Path
import json,math
import unreal
from ue_author_transport_visuals import enum_value

ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'_Imports'/'Original'/'Citizens'
MESH_PATH='/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy'
MAT_PATH='/Game/Environment/Materials/M_CitizenActivity'
TAG='AS_CITIZEN_GAIT_V2:'
LIB=unreal.MaterialEditingLibrary
ASSETS=unreal.EditorAssetLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
# Commandlets do not instantiate every UEditorSubsystem. These native methods
# operate only on the passed asset and are also callable on the class default.
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)

def node(mat,tag,cls,**props):
    found=[n for n in LIB.get_material_expressions(mat) if str(n.get_editor_property('desc'))==TAG+tag]
    if len(found)>1:raise RuntimeError('Duplicate citizen material tag '+tag)
    n=found[0] if found else LIB.create_material_expression(mat,cls,-500,0)
    if n.get_class()!=cls.static_class():raise RuntimeError('Citizen expression class changed '+tag)
    n.set_editor_property('desc',TAG+tag)
    for k,v in props.items():n.set_editor_property(k,v)
    return n

def link(a,b,pin='',port=''):
    if not LIB.connect_material_expressions(a,port,b,pin):raise RuntimeError('Citizen link failed '+pin)

def custom(mat,tag,code,inputs,kind='CMOTFLOAT3'):
    n=node(mat,tag,unreal.MaterialExpressionCustom,code=code,output_type=enum_value(unreal.CustomMaterialOutputType,kind))
    fields=[]
    for name in inputs:
        item=unreal.CustomInput();item.set_editor_property('input_name',name);fields.append(item)
    n.set_editor_property('inputs',fields)
    for key,value in inputs.items():link(value,n,key)
    return n

def output(mat,n,prop):
    if not LIB.connect_material_property(n,'',prop):raise RuntimeError('Citizen output failed '+str(prop))

def import_asset(source,path,options=None):
    folder,name=path.rsplit('/',1);ASSETS.make_directory(folder)
    task=unreal.AssetImportTask()
    for k,v in [('filename',str(source)),('destination_path',folder),('destination_name',name),('automated',True),('replace_existing',True),('save',False)]:task.set_editor_property(k,v)
    if options:task.set_editor_property('options',options)
    TOOLS.import_asset_tasks([task]);asset=unreal.load_asset(path)
    if not asset:raise RuntimeError('Citizen import missing '+path)
    return asset

def create_material(palette):
    mat=unreal.load_asset(MAT_PATH)
    if not mat:
        folder,name=MAT_PATH.rsplit('/',1);ASSETS.make_directory(folder);mat=TOOLS.create_asset(name,folder,unreal.Material,unreal.MaterialFactoryNew())
    if not isinstance(mat,unreal.Material):raise RuntimeError('Expected citizen base material')
    mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property('shading_model',unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('two_sided',False);mat.set_editor_property('tangent_space_normal',False)
    mat.set_editor_property('max_world_position_offset_displacement',85.)
    uv=node(mat,'PaletteUV',unreal.MaterialExpressionTextureCoordinate,coordinate_index=0)
    meta=node(mat,'ArticulationUV',unreal.MaterialExpressionTextureCoordinate,coordinate_index=1)
    sample=node(mat,'ClothSkinPalette',unreal.MaterialExpressionTextureSampleParameter2D,parameter_name='CitizenPalette',texture=palette);link(uv,sample,'UVs')
    tint=node(mat,'ShirtRGB',unreal.MaterialExpressionPerInstanceCustomData3Vector,data_index=0,const_default_value=unreal.LinearColor(.5,.65,.7,1))
    tintpixel=node(mat,'ShirtRGBPixel',unreal.MaterialExpressionVertexInterpolator);link(tint,tintpixel)
    color=custom(mat,'PaletteTint','return lerp(Base,Base*max(Tint,float3(.08,.08,.08))*1.35,1-step(1.0/16.0,UV.x));',{'Base':sample,'Tint':tintpixel,'UV':uv})
    output(mat,color,unreal.MaterialProperty.MP_BASE_COLOR)
    rough=custom(mat,'ClothSkinRoughness','int i=(int)floor(UV.x*16);return i==3?.53:i==2?.66:i==8?.28:.84;',{'UV':uv},'CMOTFLOAT1')
    output(mat,rough,unreal.MaterialProperty.MP_ROUGHNESS)
    spec=node(mat,'ClothSpecular',unreal.MaterialExpressionConstant,r=.3);output(mat,spec,unreal.MaterialProperty.MP_SPECULAR)
    params={}
    for label,index,default in [('Phase',3,0.),('Speed',4,0.),('Last',5,0.),('Run',6,0.)]:
        params[label]=node(mat,label,unreal.MaterialExpressionPerInstanceCustomData,data_index=index,const_default_value=default)
    time=node(mat,'WorldTime',unreal.MaterialExpressionTime)
    localpos=node(mat,'RestLocalPosition',unreal.MaterialExpressionPreSkinnedPosition)
    localnormal=node(mat,'RestLocalNormal',unreal.MaterialExpressionPreSkinnedNormal)
    world=node(mat,'WorldPositionWithoutOffset',unreal.MaterialExpressionWorldPosition,
               world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets,'WPTEXCLUDEALLSHADEROFFSETS'))
    camera=node(mat,'Camera',unreal.MaterialExpressionCameraPositionWS)
    fade=custom(mat,'GaitDistanceFade','return 1-smoothstep(9000,16000,length(P-C));',{'P':world,'C':camera},'CMOTFLOAT1')
    # Shared code runs in the vertex shader. Rigid limb groups pivot about real
    # knees/hips/elbows, not a whole-person oscillation or random teleport.
    common=r'''
int part=(int)round(Meta.x);float run=saturate(Run);float speed=max(Speed,0);
float t=Phase+clamp(Time-Last,0,.16)*speed*6.28318530718/lerp(140,170,run);
float a=saturate((speed-2)/35)*saturate(Fade);float walk=lerp(.36,.68,run)*a;
float side=(part==5||part==6||part==7||part==11||part==12||part==13)?-1:1;
float legPhase=t+(side>0?3.14159265359:0);float leg=sin(legPhase)*walk;
float knee=max(0,-sin(legPhase))*(.52+run*.60)*a;
float arm=-sin(legPhase)*walk*.68;float elbow=(.14+run*.42+max(0,sin(legPhase))*.15)*a;
float3 p=P;float3 n=N;
// Local-Y rotation: x/z pitch. Inline to remain compatible with Custom HLSL.
float3 pivot=float3(0,side*8.5,93);float ang=0;
if(part>=11&&part<=16){
 if(part==12||part==13||part==15||part==16){
  float3 k=float3(1,side*8.6,53);float c=cos(knee),s=sin(knee);float3 q=p-k;p=k+float3(c*q.x+s*q.z,q.y,-s*q.x+c*q.z);n=float3(c*n.x+s*n.z,n.y,-s*n.x+c*n.z);
 }
 ang=leg;
}else if(part>=5&&part<=10){
 if(part==6||part==7||part==9||part==10){
  float3 e=float3(2,side*19.5,116);float c=cos(-elbow),s=sin(-elbow);float3 q=p-e;p=e+float3(c*q.x+s*q.z,q.y,-s*q.x+c*q.z);n=float3(c*n.x+s*n.z,n.y,-s*n.x+c*n.z);
 }
 pivot=float3(0,side*19.5,145);ang=arm;
}
float c=cos(ang),s=sin(ang);float3 q=p-pivot;p=pivot+float3(c*q.x+s*q.z,q.y,-s*q.x+c*q.z);n=float3(c*n.x+s*n.z,n.y,-s*n.x+c*n.z);
float bob=(.7+.65*run)*(1-cos(t*2))*a;
p.z+=bob;
// A purely visual foot contact bound. Canonical navigation Z stays untouched.
if(part==13||part==16)p.z=max(p.z,.10);
'''
    inputs={'P':localpos,'N':localnormal,'Meta':meta,'Time':time,'Fade':fade,**params}
    offset=custom(mat,'ArticulatedLocalOffset',common+'return p-P;',inputs)
    normal=custom(mat,'ArticulatedLocalNormal',common+'return normalize(n);',inputs)
    offsetworld=node(mat,'InstanceOffsetToWorld',unreal.MaterialExpressionTransform,
        transform_source_type=enum_value(unreal.MaterialVectorCoordTransformSource,'TRANSFORMSOURCEINSTANCE'),
        transform_type=enum_value(unreal.MaterialVectorCoordTransform,'TRANSFORMWORLD'))
    normalworld=node(mat,'InstanceNormalToWorld',unreal.MaterialExpressionTransform,
        transform_source_type=enum_value(unreal.MaterialVectorCoordTransformSource,'TRANSFORMSOURCEINSTANCE'),
        transform_type=enum_value(unreal.MaterialVectorCoordTransform,'TRANSFORMWORLD'))
    link(offset,offsetworld);link(normal,normalworld)
    normalpixel=node(mat,'AnimatedNormalPixel',unreal.MaterialExpressionVertexInterpolator);link(normalworld,normalpixel)
    normalized=node(mat,'NormalizeWorldNormal',unreal.MaterialExpressionNormalize);link(normalpixel,normalized)
    output(mat,offsetworld,unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    output(mat,normalized,unreal.MaterialProperty.MP_NORMAL)
    LIB.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
    errors=LIB.recompile_material(mat)
    if errors:raise RuntimeError('Citizen shader compile failed '+str(list(errors)))
    if not ASSETS.save_loaded_asset(mat):raise RuntimeError('Citizen material save failed')
    return mat

def main():
    provenance=json.loads((SOURCE/'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    palette=import_asset(SOURCE/'T_CitizenPalette.png','/Game/Environment/Textures/Citizens/T_CitizenPalette')
    palette.set_editor_property('srgb',True);palette.set_editor_property('filter',unreal.TextureFilter.TF_NEAREST)
    palette.set_editor_property('mip_gen_settings',unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    if not ASSETS.save_loaded_asset(palette):raise RuntimeError('Citizen palette save failed')
    options=unreal.FbxImportUI()
    for k,v in [('import_mesh',True),('import_materials',False),('import_textures',False),('automated_import_should_detect_type',False),('mesh_type_to_import',unreal.FBXImportType.FBXIT_STATIC_MESH)]:options.set_editor_property(k,v)
    data=unreal.FbxStaticMeshImportData()
    for k,v in [('combine_meshes',True),('auto_generate_collision',False),('generate_lightmap_u_vs',False),('normal_import_method',unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)]:data.set_editor_property(k,v)
    options.set_editor_property('static_mesh_import_data',data)
    mesh=import_asset(SOURCE/'SM_CitizenProxy.obj',MESH_PATH,options)
    if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError('Citizen OBJ import did not create static mesh')
    for lod in (1,2):
        if SM.import_lod(mesh,lod,str(SOURCE/('SM_CitizenProxy_LOD%d.obj'%lod)))!=lod:raise RuntimeError('Citizen authored LOD import failed '+str(lod))
    if SM.get_lod_count(mesh)!=3:raise RuntimeError('Citizen expected three LODs')
    descriptions=[];counts=[]
    for lod in range(3):
        settings=SM.get_lod_build_settings(mesh,lod)
        settings.set_editor_property('use_full_precision_u_vs',True)
        settings.set_editor_property('generate_lightmap_u_vs',False)
        settings.set_editor_property('recompute_normals',False)
        settings.set_editor_property('recompute_tangents',True)
        SM.set_lod_build_settings(mesh,lod,settings)
        while SM.get_num_uv_channels(mesh,lod)<2:
            if not SM.add_uv_channel(mesh,lod):raise RuntimeError('Citizen articulation UV channel creation failed')
        desc=mesh.get_static_mesh_description(lod);parts={};valid=0
        for i in range(desc.get_vertex_instance_count()):
            vi=unreal.VertexInstanceID();vi.set_editor_property('id_value',i)
            if not desc.is_vertex_instance_valid(vi):continue
            uv=desc.get_vertex_instance_uv(vi,0);part=int(math.floor(float(uv.x)/2+.0001))
            if str(part) not in provenance['parts']:raise RuntimeError('Citizen imported metadata corrupt '+str(uv))
            palette_u=float(uv.x)-part*2
            if not 0<=palette_u<=1:raise RuntimeError('Citizen UV0 outside palette')
            pivot=provenance['parts'][str(part)]['pivot_z_cm']
            desc.set_vertex_instance_uv(vi,unreal.Vector2D(part,pivot/200.),1)
            desc.set_vertex_instance_uv(vi,unreal.Vector2D(palette_u,uv.y),0)
            read=desc.get_vertex_instance_uv(vi,1)
            if abs(read.x-part)>.001:raise RuntimeError('Citizen UV1 readback failed')
            parts[part]=parts.get(part,0)+1;valid+=1
        if not all(i in parts for i in range(17)):raise RuntimeError('Citizen missing animated body groups '+repr(parts))
        descriptions.append(desc);counts.append({'lod':lod,'vertex_instances':valid,'parts':parts,'source_triangles':provenance['models'][lod]['triangles']})
    mesh.build_from_static_mesh_descriptions(descriptions,False,False)
    if not SM.set_lod_screen_sizes(mesh,[1.,.18,.055]):raise RuntimeError('Citizen LOD screen sizes failed')
    mat=create_material(palette)
    for slot in range(max(1,len(mesh.get_editor_property('static_materials')))):mesh.set_material(slot,mat)
    nanite=mesh.get_editor_property('nanite_settings');nanite.set_editor_property('enabled',False);mesh.set_editor_property('nanite_settings',nanite)
    # Bound limb motion without scaling the canonical 175cm source mesh.
    mesh.set_editor_property('positive_bounds_extension',unreal.Vector(75,3,6))
    mesh.set_editor_property('negative_bounds_extension',unreal.Vector(75,3,3))
    if not ASSETS.save_loaded_asset(mesh):raise RuntimeError('Citizen mesh save failed')
    # Check raw MeshDescription positions; mesh render bounds intentionally
    # include authored WPO extension and must not be mistaken for body height.
    desc=mesh.get_static_mesh_description(0);positions=[]
    for i in range(desc.get_vertex_count()):
        v=unreal.VertexID();v.set_editor_property('id_value',i)
        if desc.is_vertex_valid(v):
            p=desc.get_vertex_position(v);positions.append((p.x,p.y,p.z))
    low=[min(p[k]for p in positions)for k in range(3)];high=[max(p[k]for p in positions)for k in range(3)]
    if abs(high[2]-175)>.1 or abs(low[2])>.1 or not 45<=high[1]-low[1]<=50:raise RuntimeError('Citizen real-centimetre bounds failed '+repr([low,high]))
    report={'asset':mesh.get_path_name(),'material':mat.get_path_name(),'source_bounds_min_cm':low,'source_bounds_max_cm':high,'height_cm':high[2]-low[2],'shoulder_width_cm':high[1]-low[1],'lods':counts,'instance_data':provenance['instance_data'],'normal_source':'Original explicit OBJ normals; articulated in vertex shader and interpolated as world normal','gait':provenance['gait'],'compile_errors':[],'save_verified':True,'proof_scope':'UE native import/metadata/dimensions/compile/save. Runtime visual and GPU profile separately required.'}
    path=ROOT/'Saved'/'PresentationValidation'/'citizen_visual_assets.json';path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_CITIZEN_VISUALS_READY '+json.dumps(report))

if __name__=='__main__':main()
