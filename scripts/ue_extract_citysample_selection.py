"""Native, bounded City Sample derivative export in an isolated scratch project.

Scratch project must be named CitySampleExtract and have real Content/Aftershock
plus read-only source top-folder junctions. Never run against the Vault project.
The output uses only its own /Game/Aftershock/CitySample references. Epic assets
remain subject to their acquisition license, including private source sharing.
"""
from pathlib import Path
import hashlib, importlib.util, json
import unreal

ROOT=Path(__file__).resolve().parents[1]
PROJECT=Path(unreal.Paths.get_project_file_path()).resolve()
CONTENT=PROJECT.parent/'Content'
PREFIX='/Game/Aftershock/CitySample'
A=unreal.EditorAssetLibrary
L=unreal.MaterialEditingLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
SOURCE=(CONTENT/'Prop').resolve().parent
assert PROJECT.stem=='CitySampleExtract' and 'vaultcache' not in str(CONTENT.resolve()).lower()
assert not (CONTENT/'Aftershock').is_symlink()
assert 'vaultcache' in str(SOURCE).lower()
REPORT={'schema_version':1,'source_product':'City Sample 5.8, Epic Games','cost_tl':0,
 'license_urls':['https://www.fab.com/listings/4898e707-7855-404b-af0e-a505ee690e68','https://www.unrealengine.com/eula/content'],
 'source_assets_cc0':False,'source_sharing':'Private project authorized collaborators only under applicable acquisition terms',
 'output_prefix':PREFIX,'assets':[],'valid':False}
out=ROOT/'Saved/CitySampleAudit/native_extraction.json'
out.parent.mkdir(parents=True,exist_ok=True)

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def disk(package,base):return base/(package[len('/Game/'):]+'.uasset')
def duplicate(source,destination):
    assert destination.startswith(PREFIX+'/'), destination
    source_asset=unreal.load_asset(source)
    if not source_asset:raise RuntimeError('Missing native source '+source)
    asset=unreal.load_asset(destination) if A.does_asset_exist(destination) else TOOLS.duplicate_asset(destination.rsplit('/',1)[1],destination.rsplit('/',1)[0],source_asset)
    if not asset:raise RuntimeError('Native duplicate failed '+destination)
    return asset,{'source_package':source,'source_sha256':sha(disk(source,SOURCE)),'destination_package':destination}

textures={}
for source,name,normal,srgb in (
 ('/Game/Prop/Kit_bench_RR/Texture/T_bench_C','T_Bench_C',False,True),
 ('/Game/Prop/Kit_bench_RR/Texture/T_bench_N','T_Bench_N',True,False),
 ('/Game/Prop/Kit_bench_RR/Texture/T_bench_M','T_Bench_M',False,False),
 ('/Game/Prop/Kit_roof_brick_RR/Texture/T_roof_bricks_C','T_Brick_C',False,True),
 ('/Game/Prop/Kit_roof_brick_RR/Texture/T_roof_bricks_N','T_Brick_N',True,False),
 ('/Game/Prop/Kit_roof_brick_RR/Texture/T_roof_bricks_M','T_Brick_M',False,False),
 ('/Game/Building/Texture/ConcreteDirty/T_ConcreteDirty_C','T_Concrete_C',False,True),
 ('/Game/Building/Texture/ConcreteDirty/T_ConcreteDirty_N','T_Concrete_N',True,False),
 ('/Game/Prop/Kit_StreetLamp_B/Texture/T_StreetLamp_B_C','T_Lamp_C',False,True)):
    asset,row=duplicate(source,PREFIX+'/Textures/'+name)
    assert isinstance(asset,unreal.Texture2D)
    asset.set_editor_property('max_texture_size',2048)
    asset.set_editor_property('never_stream',False)
    asset.set_editor_property('virtual_texture_streaming',False)
    asset.set_editor_property('srgb',srgb)
    asset.set_editor_property('lod_bias',0)
    asset.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if normal else unreal.TextureGroup.TEXTUREGROUP_WORLD)
    asset.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT if srgb else unreal.TextureCompressionSettings.TC_MASKS)
    assert A.save_loaded_asset(asset)
    row.update(class_name='Texture2D',runtime_max_size=2048,source_bulk_resized=False,streaming=True,srgb=srgb,normal_map=normal)
    REPORT['assets'].append(row);textures[source]=asset

bench,bench_row=duplicate('/Game/Prop/Kit_bench_RR/Material/M_bench',PREFIX+'/Materials/M_Bench')
changed=0
for expression in L.get_material_expressions(bench):
    try:texture=expression.get_editor_property('texture')
    except Exception:continue
    if texture:
        key=texture.get_path_name().split('.')[0]
        if key in textures:
            derived=textures[key]
            expression.set_editor_property('texture',derived)
            compression=derived.get_editor_property('compression_settings')
            sampler=(unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if compression==unreal.TextureCompressionSettings.TC_NORMALMAP
                     else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if compression==unreal.TextureCompressionSettings.TC_MASKS
                     else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if derived.get_editor_property('srgb')
                     else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            expression.set_editor_property('sampler_type',sampler)
        elif key not in {t.get_path_name().split('.')[0] for t in textures.values()}:raise RuntimeError('Unmapped bench texture '+key)
        changed+=1
assert changed>=3
L.set_base_material_usage(bench,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
errors=L.recompile_material(bench)
assert not errors,errors
assert A.save_loaded_asset(bench)
bench_row.update(class_name='Material',original_graph_preserved_except_owned_texture_references_and_matching_sampler_types=True,retargeted_texture_nodes=changed)
REPORT['assets'].append(bench_row)

lamp_path=PREFIX+'/Materials/M_StreetLamp'
lamp=unreal.load_asset(lamp_path) if A.does_asset_exist(lamp_path) else TOOLS.create_asset('M_StreetLamp',PREFIX+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
assert lamp
for old_node in list(L.get_material_expressions(lamp)):L.delete_material_expression(lamp,old_node)
for label,prop,texture,normal in (
 ('Albedo',unreal.MaterialProperty.MP_BASE_COLOR,textures['/Game/Prop/Kit_StreetLamp_B/Texture/T_StreetLamp_B_C'],False),):
    node=L.create_material_expression(lamp,unreal.MaterialExpressionTextureSample)
    node.set_editor_property('texture',texture)
    if normal:node.set_editor_property('sampler_type',unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    assert L.connect_material_property(node,'RGB',prop)
for value,prop in ((.48,unreal.MaterialProperty.MP_ROUGHNESS),(.35,unreal.MaterialProperty.MP_METALLIC)):
    node=L.create_material_expression(lamp,unreal.MaterialExpressionConstant);node.set_editor_property('r',value)
    assert L.connect_material_property(node,'',prop)
L.set_base_material_usage(lamp,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
assert not L.recompile_material(lamp)
assert A.save_loaded_asset(lamp)
REPORT['assets'].append({'destination_package':PREFIX+'/Materials/M_StreetLamp','class_name':'Material','original_project_shader':True,
 'roughness':.48,'metallic':.35,'source_packed_channels_guessed':False,'scope':'Bounded project shading using City Sample albedo and geometry normals; unused 8K source normal omitted, not measured material reconstruction'})

for source,name,material in (
 ('/Game/Prop/Kit_bench_RR/mesh/SM_street_bench','SM_StreetBench',bench),
 ('/Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B','SM_StreetLamp',lamp)):
    mesh,row=duplicate(source,PREFIX+'/Meshes/'+name)
    assert isinstance(mesh,unreal.StaticMesh)
    materials=list(mesh.get_editor_property('static_materials'))
    for slot in materials:slot.set_editor_property('material_interface',material)
    mesh.set_editor_property('static_materials',materials)
    box=mesh.get_bounding_box()
    assert A.save_loaded_asset(mesh)
    row.update(class_name='StaticMesh',source_geometry_scale_changed=False,bounds_cm={'min':[box.min.x,box.min.y,box.min.z],'max':[box.max.x,box.max.y,box.max.z]},
               material_slots=len(materials),owned_material=material.get_path_name())
    REPORT['assets'].append(row)

facade_script=ROOT/'Saved/CitySampleAudit/ue_author_citysample_facade.py'
if facade_script.is_file():
    spec=importlib.util.spec_from_file_location('facade_source_proof_only',facade_script)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    try:proof=module.native_source_channel_proof()
    except Exception as error:proof={'proven':False,'reason':str(error),'source_material':module.SOURCE_BRICK_MATERIAL}
    (ROOT/'Saved/CitySampleAudit/extracted_brick_channel_proof.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
    REPORT['brick_channel_proof']=proof

reg=unreal.AssetRegistryHelpers.get_asset_registry()
options=unreal.AssetRegistryDependencyOptions(include_hard_package_references=True,include_soft_package_references=True,include_searchable_names=False,include_hard_management_references=False,include_soft_management_references=False)
owned={row['destination_package'] for row in REPORT['assets']}
for row in REPORT['assets']:
    target=disk(row['destination_package'],CONTENT)
    assert target.resolve().is_relative_to((CONTENT/'Aftershock').resolve())
    reg.scan_files_synchronous([str(target)],True)
    deps=sorted(str(p) for p in reg.get_dependencies(unreal.Name(row['destination_package']),options))
    game_deps=[p for p in deps if p.startswith('/Game/')]
    assert all(p in owned for p in game_deps),(row['destination_package'],game_deps)
    row.update(package_sha256=sha(target),package_bytes=target.stat().st_size,dependencies=deps)
    if row.get('source_package'):assert sha(disk(row['source_package'],SOURCE))==row['source_sha256']
REPORT.update(valid=True,asset_count=len(owned),total_package_bytes=sum(r['package_bytes'] for r in REPORT['assets']),source_assets_unchanged=True,
              dependency_scope='Every hard/soft Game package reference belongs to this exact exported inventory; native Engine/Script references retained. Runtime/cook/visual proof separate.')
assert REPORT['total_package_bytes']<=128*1024*1024
out.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
unreal.log_warning('AFTERSHOCK_CITYSAMPLE_EXTRACTION_READY '+json.dumps({'valid':True,'assets':len(owned),'bytes':REPORT['total_package_bytes']}))
