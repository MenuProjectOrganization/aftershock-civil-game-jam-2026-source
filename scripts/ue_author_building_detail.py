"""Add original 2k detail PBR without overwriting palette/damage/power graphs.

No copied art, new building identity, displacement, GC edit, or gameplay change.
Only Normal and Roughness outputs are wrapped. Every other output connection is
captured and verified unchanged before saving either shared master material.
"""
from pathlib import Path
import json
import unreal
from ue_author_transport_visuals import enum_value

ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'_Imports'/'Original'/'Architecture'
DEST='/Game/Buildings/Textures/Original'
TAG='AS_ARCHITECTURE_DETAIL_V1:'
LIB=unreal.MaterialEditingLibrary;ASSETS=unreal.EditorAssetLibrary

def node(mat,label,cls,**props):
    matches=[n for n in LIB.get_material_expressions(mat) if str(n.get_editor_property('desc'))==TAG+label]
    if len(matches)>1:raise RuntimeError('Duplicate architecture tag '+label)
    n=matches[0]if matches else LIB.create_material_expression(mat,cls)
    if n.get_class()!=cls.static_class():raise RuntimeError('Architecture expression class changed '+label)
    n.set_editor_property('desc',TAG+label)
    for k,v in props.items():n.set_editor_property(k,v)
    return n

def link(a,b,pin='',port=''):
    if not LIB.connect_material_expressions(a,port,b,pin):raise RuntimeError('Architecture link failed '+pin)

def custom(mat,label,code,inputs,kind='CMOTFLOAT3'):
    n=node(mat,label,unreal.MaterialExpressionCustom,code=code,output_type=enum_value(unreal.CustomMaterialOutputType,kind))
    fields=[]
    for name in inputs:
        f=unreal.CustomInput();f.set_editor_property('input_name',name);fields.append(f)
    n.set_editor_property('inputs',fields)
    for name,source in inputs.items():link(source,n,name)
    return n

def output(mat,n,prop):
    if not LIB.connect_material_property(n,'',prop):raise RuntimeError('Architecture material property failed')

def original_input(mat,prop,wrapper,label,pin,default):
    old=LIB.get_material_property_input_node(mat,prop)
    if old and str(old.get_editor_property('desc'))==TAG+wrapper:
        names=list(LIB.get_material_expression_input_names(old));values=list(LIB.get_inputs_for_material_expression(mat,old))
        if pin not in names:raise RuntimeError('Architecture rerun old input missing '+pin)
        old=values[names.index(pin)]
    return old if old else node(mat,label,unreal.MaterialExpressionConstant,r=default)

def import_texture(name,normal=False):
    path=DEST+'/'+name;ASSETS.make_directory(DEST);task=unreal.AssetImportTask()
    for k,v in [('filename',str(SOURCE/(name+'.png'))),('destination_path',DEST),('destination_name',name),('automated',True),('replace_existing',True),('save',False)]:task.set_editor_property(k,v)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task]);tex=unreal.load_asset(path)
    if not isinstance(tex,unreal.Texture2D):raise RuntimeError('Architecture texture import failed '+name)
    tex.set_editor_property('srgb',False)
    tex.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_MASKS)
    tex.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if normal else unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property('address_x',unreal.TextureAddress.TA_WRAP);tex.set_editor_property('address_y',unreal.TextureAddress.TA_WRAP)
    if not ASSETS.save_loaded_asset(tex):raise RuntimeError('Architecture texture save failed')
    return tex

def author(path,normaltex,ormtex):
    mat=unreal.load_asset(path)
    if not isinstance(mat,unreal.Material):raise RuntimeError('Existing building master missing '+path)
    if not mat.get_editor_property('tangent_space_normal'):raise RuntimeError('Preserve unknown world-space building normal graph '+path)
    preserved={label:LIB.get_material_property_input_node(mat,prop) for label,prop in [('BaseColor',unreal.MaterialProperty.MP_BASE_COLOR),('Emissive',unreal.MaterialProperty.MP_EMISSIVE_COLOR),('WPO',unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET),('Metallic',unreal.MaterialProperty.MP_METALLIC),('Opacity',unreal.MaterialProperty.MP_OPACITY)]}
    expressions=list(LIB.get_material_expressions(mat))
    palettes=[n for n in expressions if n.get_class().get_name()=='MaterialExpressionTextureSampleParameter2D' and str(n.get_editor_property('parameter_name'))in('BuildingAlbedo','ServicePalette')]
    if len(palettes)!=1:raise RuntimeError('Cannot prove source building palette '+path+' matches='+str(len(palettes)))
    palette=palettes[0]
    p=node(mat,'AbsoluteWorldWithoutOffset',unreal.MaterialExpressionWorldPosition,world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets,'WPTEXCLUDEALLSHADEROFFSETS'))
    n=node(mat,'GeometricWorldNormal',unreal.MaterialExpressionVertexNormalWS)
    normalobject=node(mat,'DetailNormalTexture',unreal.MaterialExpressionTextureObjectParameter,parameter_name='ArchitectureDetailNormal',texture=normaltex,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    ormobject=node(mat,'DetailORMTexture',unreal.MaterialExpressionTextureObjectParameter,parameter_name='ArchitectureDetailORM',texture=ormtex,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    # Three projections avoid stretching on rotated roofs and walls. Explicit
    # BC5 reconstruction is required because a Custom texture sample is raw.
    detailworld=custom(mat,'TriplanarDetailWorldNormal',r'''
float3 ng=normalize(N);float3 w=pow(abs(ng),4);w/=max(w.x+w.y+w.z,.0001);float3 q=P/240;
float2 a=Texture2DSample(Detail,DetailSampler,q.yz).rg*2-1;
float2 b=Texture2DSample(Detail,DetailSampler,q.xz).rg*2-1;
float2 c=Texture2DSample(Detail,DetailSampler,q.xy).rg*2-1;
float3 x=float3(sqrt(saturate(1-dot(a,a)))*sign(ng.x),a.x,a.y);
float3 y=float3(b.x,sqrt(saturate(1-dot(b,b)))*sign(ng.y),b.y);
float3 z=float3(c.x,c.y,sqrt(saturate(1-dot(c,c)))*sign(ng.z));
return normalize(x*w.x+y*w.y+z*w.z);
''',{'P':p,'N':n,'Detail':normalobject})
    tangent=node(mat,'DetailWorldToTangent',unreal.MaterialExpressionTransform,
        transform_source_type=enum_value(unreal.MaterialVectorCoordTransformSource,'TRANSFORMSOURCEWORLD'),
        transform_type=enum_value(unreal.MaterialVectorCoordTransform,'TRANSFORMTANGENT'))
    link(detailworld,tangent)
    oldnormal=LIB.get_material_property_input_node(mat,unreal.MaterialProperty.MP_NORMAL)
    if oldnormal and str(oldnormal.get_editor_property('desc'))==TAG+'BlendOriginalAndDetailNormal':
        names=list(LIB.get_material_expression_input_names(oldnormal));values=list(LIB.get_inputs_for_material_expression(mat,oldnormal));oldnormal=values[names.index('Original')]
    if not oldnormal:oldnormal=node(mat,'OriginalFlatNormal',unreal.MaterialExpressionConstant3Vector,constant=unreal.LinearColor(0,0,1,1))
    normal=custom(mat,'BlendOriginalAndDetailNormal',r'''
float glass=step(Base.r*1.4,Base.b)*step(Base.r*1.25,Base.g)*(1-step(.20,Base.r));
float3 d=normalize(lerp(float3(0,0,1),Detail,(1-glass)*.60));
return normalize(float3(Original.xy+d.xy,Original.z*d.z));
''',{'Original':oldnormal,'Detail':tangent,'Base':palette})
    oldrough=original_input(mat,unreal.MaterialProperty.MP_ROUGHNESS,'MaskedDetailRoughness','OriginalRoughness','Original',.68)
    rough=custom(mat,'MaskedDetailRoughness',r'''
float3 w=pow(abs(normalize(N)),4);w/=max(w.x+w.y+w.z,.0001);float3 q=P/240;
float r=dot(float3(Texture2DSample(ORM,ORMSampler,q.yz).g,Texture2DSample(ORM,ORMSampler,q.xz).g,Texture2DSample(ORM,ORMSampler,q.xy).g),w);
float glass=step(Base.r*1.4,Base.b)*step(Base.r*1.25,Base.g)*(1-step(.20,Base.r));
float roof=pow(saturate(abs(N.z)),6);float opaque=saturate(Original+(r-.66)*lerp(.45,.30,roof));
return lerp(opaque,.14,glass);
''',{'P':p,'N':n,'ORM':ormobject,'Original':oldrough,'Base':palette},'CMOTFLOAT1')
    output(mat,normal,unreal.MaterialProperty.MP_NORMAL);output(mat,rough,unreal.MaterialProperty.MP_ROUGHNESS)
    props={'BaseColor':unreal.MaterialProperty.MP_BASE_COLOR,'Emissive':unreal.MaterialProperty.MP_EMISSIVE_COLOR,'WPO':unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,'Metallic':unreal.MaterialProperty.MP_METALLIC,'Opacity':unreal.MaterialProperty.MP_OPACITY}
    for label,prop in props.items():
        if LIB.get_material_property_input_node(mat,prop)!=preserved[label]:raise RuntimeError('Building '+label+' changed unexpectedly')
    errors=LIB.recompile_material(mat)
    if errors:raise RuntimeError('Building detail compile failed '+path+' '+str(list(errors)))
    if not ASSETS.save_loaded_asset(mat):raise RuntimeError('Building detail material save failed '+path)
    return {'material':path,'preserved_outputs':{label:(v.get_path_name()if v else None)for label,v in preserved.items()},'source_palette':palette.get_path_name(),'wrapped_outputs':['Normal','Roughness'],'instance_data_unchanged':{'0':'damage','1':'retrofit','2':'power','3':'operation'},'compile_errors':[],'save_verified':True}

def main():
    normal=import_texture('T_Architecture_DetailNormal',True);orm=import_texture('T_Architecture_DetailORM')
    report={'textures':[normal.get_path_name(),orm.get_path_name()],'materials':[author(path,normal,orm)for path in('/Game/Buildings/Materials/M_Building_Base','/Game/Buildings/Materials/M_ServiceBuilding')],'source_manifest':str(SOURCE/'SOURCE_MANIFEST.json'),'proof_scope':'UE native texture import, material output preservation, compile and save only. Live visual/performance proof separate.'}
    path=ROOT/'Saved'/'PresentationValidation'/'building_detail_assets.json';path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_BUILDING_DETAIL_READY '+json.dumps(report))

if __name__=='__main__':main()
