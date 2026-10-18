"""Additive original world-scaled facade/roof albedo variation.

Run separately AFTER existing authoring. No levels/game startup. The exact
previous BaseColor output stays upstream, so palette UV0 and canonical damage
remain. Emissive, WPO, Normal, Roughness, Metallic and custom data stay untouched.
Idempotent tags reuse nodes and unwrap the previous Original pin on a rerun.
"""
from pathlib import Path
import json,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import unreal
from ue_author_transport_visuals import enum_value

ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'_Imports'/'Original'/'Architecture'
DEST='/Game/Buildings/Textures/Original'
TAG='AS_ARCHITECTURE_ALBEDO_V1:'
LIB=unreal.MaterialEditingLibrary;ASSETS=unreal.EditorAssetLibrary

def node(mat,key,cls,**props):
    found=[n for n in LIB.get_material_expressions(mat) if str(n.get_editor_property('desc'))==TAG+key]
    if len(found)>1:raise RuntimeError('Duplicate original albedo tag '+key)
    n=found[0]if found else LIB.create_material_expression(mat,cls)
    if n.get_class()!=cls.static_class():raise RuntimeError('Unexpected albedo expression class '+key)
    n.set_editor_property('desc',TAG+key)
    for k,v in props.items():n.set_editor_property(k,v)
    return n

def link(a,b,pin='',port=''):
    if not LIB.connect_material_expressions(a,port,b,pin):raise RuntimeError('Albedo link failed '+pin)

def snapshot(mat):
    return {k:LIB.get_material_property_input_node(mat,v)for k,v in [('Emissive',unreal.MaterialProperty.MP_EMISSIVE_COLOR),('WPO',unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET),('Normal',unreal.MaterialProperty.MP_NORMAL),('Roughness',unreal.MaterialProperty.MP_ROUGHNESS),('Metallic',unreal.MaterialProperty.MP_METALLIC),('Opacity',unreal.MaterialProperty.MP_OPACITY)]}

def import_texture():
    name='T_Architecture_SurfaceVariation';ASSETS.make_directory(DEST);task=unreal.AssetImportTask()
    for k,v in [('filename',str(SOURCE/(name+'.png'))),('destination_path',DEST),('destination_name',name),('automated',True),('replace_existing',True),('save',False)]:task.set_editor_property(k,v)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task]);tex=unreal.load_asset(DEST+'/'+name)
    if not isinstance(tex,unreal.Texture2D):raise RuntimeError('Original albedo texture import failed')
    tex.set_editor_property('srgb',False);tex.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_MASKS)
    tex.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property('address_x',unreal.TextureAddress.TA_WRAP);tex.set_editor_property('address_y',unreal.TextureAddress.TA_WRAP)
    if not ASSETS.save_loaded_asset(tex):raise RuntimeError('Original albedo texture save failed')
    return tex

def author(path,texture):
    mat=unreal.load_asset(path)
    if not isinstance(mat,unreal.Material):raise RuntimeError('Existing building master missing '+path)
    before=snapshot(mat);old=LIB.get_material_property_input_node(mat,unreal.MaterialProperty.MP_BASE_COLOR)
    if not old:raise RuntimeError('Building basecolor source missing; preserve graph '+path)
    if str(old.get_editor_property('desc'))==TAG+'PreservedPaletteWeathering':
        names=list(LIB.get_material_expression_input_names(old));values=list(LIB.get_inputs_for_material_expression(mat,old));old=values[names.index('Original')]
    if not old or str(old.get_editor_property('desc')).startswith(TAG):raise RuntimeError('Cannot unwrap original albedo safely')
    palettes=[n for n in LIB.get_material_expressions(mat)if n.get_class().get_name()=='MaterialExpressionTextureSampleParameter2D'and str(n.get_editor_property('parameter_name'))in('BuildingAlbedo','ServicePalette')]
    if len(palettes)!=1:raise RuntimeError('Expected one original source palette '+path)
    p=node(mat,'WorldPosition',unreal.MaterialExpressionWorldPosition,world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets,'WPTEXCLUDEALLSHADEROFFSETS'))
    n=node(mat,'VertexNormal',unreal.MaterialExpressionVertexNormalWS)
    tex=node(mat,'OriginalSurfacePatterns',unreal.MaterialExpressionTextureObjectParameter,parameter_name='ArchitectureSurfaceVariation',texture=texture,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    tile=node(mat,'WorldTileCentimetres',unreal.MaterialExpressionScalarParameter,parameter_name='ArchitectureAlbedoTileCm',default_value=240.)
    code=r'''
float3 ng=normalize(N);float3 w=pow(abs(ng),4);w/=max(w.x+w.y+w.z,.0001);float3 q=P/max(Tile,25);
float4 pattern=Texture2DSample(Surface,SurfaceSampler,q.yz)*w.x+Texture2DSample(Surface,SurfaceSampler,q.xz)*w.y+Texture2DSample(Surface,SurfaceSampler,q.xy)*w.z;
float glass=step(Palette.r*1.4,Palette.b)*step(Palette.r*1.25,Palette.g)*(1-step(.20,Palette.r));
float roof=smoothstep(.55,.92,abs(ng.z));
// Preserve existing wall/roof colors. Classification selects only a subtle
// neutral surface pattern; it does not assign a gameplay construction material.
float masonry=smoothstep(.055,.18,Palette.r-Palette.g)*smoothstep(.035,.10,Palette.r-Palette.b);
float timber=smoothstep(.025,.065,Palette.r-Palette.g)*(1-smoothstep(.12,.24,Palette.r))*(1-masonry);
float wall=lerp(pattern.g,pattern.r,masonry);wall=lerp(wall,pattern.a,timber);
float detail=lerp(wall,pattern.b,roof);
float modulation=.78+.44*detail;
return lerp(saturate(Original*modulation),Original,glass);
'''
    expr=node(mat,'PreservedPaletteWeathering',unreal.MaterialExpressionCustom,code=code,output_type=enum_value(unreal.CustomMaterialOutputType,'CMOTFLOAT3'))
    inputs={'Original':old,'Palette':palettes[0],'P':p,'N':n,'Surface':tex,'Tile':tile};fields=[]
    for name in inputs:
        f=unreal.CustomInput();f.set_editor_property('input_name',name);fields.append(f)
    expr.set_editor_property('inputs',fields)
    for key,value in inputs.items():link(value,expr,key)
    if not LIB.connect_material_property(expr,'',unreal.MaterialProperty.MP_BASE_COLOR):raise RuntimeError('Original albedo output link failed')
    if snapshot(mat)!=before:raise RuntimeError('Building non-albedo output changed; do not save')
    values=list(LIB.get_inputs_for_material_expression(mat,expr));names=list(LIB.get_material_expression_input_names(expr))
    if values[names.index('Original')]!=old:raise RuntimeError('Original palette/damage chain detached')
    errors=LIB.recompile_material(mat)
    if errors:raise RuntimeError('Original albedo shader compile failed '+str(list(errors)))
    if not ASSETS.save_loaded_asset(mat):raise RuntimeError('Original albedo material save failed')
    return {'material':path,'original_basecolor_preserved_upstream':old.get_path_name(),'source_palette_uv0':palettes[0].get_path_name(),'preserved_other_outputs':{k:v.get_path_name()if v else None for k,v in before.items()},'world_tile_cm':240,'source_glass_excluded':True,'custom_data_layout_unchanged':True,'compile_errors':[],'save_verified':True}

def main():
    texture=import_texture();report={'texture':texture.get_path_name(),'materials':[author(path,texture)for path in('/Game/Buildings/Materials/M_Building_Base','/Game/Buildings/Materials/M_ServiceBuilding')],'source_manifest':str(SOURCE/'ALBEDO_SOURCE_MANIFEST.json'),'proof_scope':'Native import, graph preservation, compile and save. Original neutral surface variation, not photogrammetry or a photoreal facade replacement; live render/profile separate.'}
    path=ROOT/'Saved'/'PresentationValidation'/'building_albedo_detail_assets.json';path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_BUILDING_ALBEDO_DETAIL_READY '+json.dumps(report))
    # No imports or graph mutations in this final reader. FinishCompilation
    # catches a null/failed render resource that an empty early error list cannot.
    import ue_validate_original_visuals
    ue_validate_original_visuals.main()

if __name__=='__main__':main()
