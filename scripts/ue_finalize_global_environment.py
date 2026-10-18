"""Coordinated post-import finalization; separate native UV and RHI phases.

UV:  -AftershockEnvironmentFinalizePhase=UV -NullRHI
RHI: -AftershockEnvironmentFinalizePhase=RHI -AllowCommandletRendering

Run initial source imports separately ONCE. This finalizer refuses missing
assets and unsafe helper-main execution; it never reruns the mesh importer.
The only calibrated scalar change is Wood's physical150cm PBR tile.
"""
from pathlib import Path
import ast
import hashlib
import json
import math
import re
import runpy
import struct
from datetime import datetime,timezone

import unreal

SCRIPTS=Path(__file__).resolve().parent
PROJECT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE=PROJECT/'_Imports/GlobalEnvironment'
L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)
CLI=unreal.SystemLibrary.get_command_line()
match=re.search(r'(?:^|\s)-?AftershockEnvironmentFinalizePhase=([A-Za-z]+)',CLI)
PHASE=match.group(1).upper() if match else 'RHI'
OUT=PROJECT/('Saved/PresentationValidation/global_environment_finalize_'+PHASE.lower()+'.json')
REPORT={'schema_version':2,'valid':False,'phase':PHASE,'started_utc':datetime.now(timezone.utc).isoformat(),
        'steps':[],'mesh_source_imported_by_finalizer':False,'canonical_gameplay_modified':False,
        'source_fbx_modified':False,'render_frame_native_readback_required':True,
        'gpu_or_runtime_acceptance_claimed':False}


def need(value,message):
    if not value:raise RuntimeError(message)


def checkpoint():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    OUT.write_text(json.dumps(REPORT,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def read_json(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def existing(path,cls):
    need(A.does_asset_exist(path),'Finalization requires existing native asset; run the separate INITIAL import stage: '+path)
    value=unreal.load_asset(path)
    need(isinstance(value,cls),'Unexpected native class '+path)
    return value


def manifest_meshes():
    result=[]
    for name,kind in (('ARCHITECTURE_GEOMETRY_MANIFEST.json','architecture'),
                      ('ENVIRONMENT_GEOMETRY_MANIFEST.json','environment'),
                      ('BROADLEAF_GEOMETRY_MANIFEST.json','environment'),
                      ('ASSEMBLY_SIGN_GEOMETRY_MANIFEST.json','sign')):
        m=read_json(SOURCE/name)
        for row in m.get('assets',[m]):
            result.append((row['target_package'],kind,row))
    need(len(result)==83,'Expected67architecture+15flora+1sign source inventory')
    return result


def verify_helper_import_guard():
    # Road authoring imports these function definitions via runpy under a
    # non-main name. A historical unguarded main() reimported native LOD1/2.
    tree=ast.parse((SCRIPTS/'ue_author_global_environment.py').read_text(encoding='utf-8-sig'))
    def main_guard(statement):
        if not isinstance(statement,ast.If):return False
        test=statement.test
        return (isinstance(test,ast.Compare) and isinstance(test.left,ast.Name)
                and test.left.id=='__name__' and len(test.comparators)==1
                and isinstance(test.ops[0],ast.Eq) and isinstance(test.comparators[0],ast.Constant)
                and test.comparators[0].value=='__main__')
    for statement in tree.body:
        if isinstance(statement,(ast.FunctionDef,ast.AsyncFunctionDef,ast.ClassDef)) or main_guard(statement):continue
        need(not any(isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id=='main'
                     for n in ast.walk(statement)),
             'Unsafe source importer main() on helper-load; guard ue_author_global_environment.py before road finalization')
    REPORT['global_import_helper_main_guard_verified']=True


def preflight_assets(rows):
    # All potentially importing subordinate scripts receive existing complete
    # assets. They can validate/update owned settings without opening FBX tasks.
    for path,kind,row in rows:
        mesh=existing(path,unreal.StaticMesh)
        need(SM.get_lod_count(mesh)==3,'Initial native3LOD import incomplete; finalizer will not import: '+path)
    texture_paths=[]
    for folder in (SOURCE,SOURCE/'RoadSurface'):
        m=read_json(folder/'SOURCE_MANIFEST.json')
        for row in m['files']:
            suffix={'diffuse':'C','normal_dx':'N','roughness':'R'}[row['role']]
            texture_paths.append(row.get('native_texture_path','/Game/Environment/Textures/GlobalPBR/T_'+row['asset_id']+'_'+suffix))
    for path in texture_paths:existing(path,unreal.Texture2D)
    for kind in ('Plaster','Wood','Roof','Concrete','Brick','Metal','Glass'):
        existing('/Game/Buildings/Architecture/Materials/M_Arch_'+kind,unreal.Material)
    existing('/Game/Environment/Materials/Presentation/M_EmergencyAssembly',unreal.Material)


def mesh_contract(rows,uv_fingerprint=False):
    result=[]
    for path,kind,source in rows:
        mesh=existing(path,unreal.StaticMesh);box=mesh.get_bounding_box()
        row={'asset':path,'lod_count':SM.get_lod_count(mesh),
             'bounds_cm':[box.min.x,box.min.y,box.min.z,box.max.x,box.max.y,box.max.z],
             'source_lod0_triangles':mesh.get_static_mesh_description(0).get_triangle_count()}
        if uv_fingerprint and kind in ('environment','sign'):
            lods=[]
            for lod in range(3):
                d=mesh.get_static_mesh_description(lod);digest=hashlib.sha256();bad=0
                for i in range(d.get_vertex_count()):
                    vertex=unreal.VertexID();vertex.set_editor_property('id_value',i)
                    if not d.is_vertex_valid(vertex):continue
                    p=d.get_vertex_position(vertex);digest.update(struct.pack('<i3d',i,p.x,p.y,p.z))
                for i in range(d.get_triangle_count()):
                    tri=unreal.TriangleID();tri.set_editor_property('id_value',i)
                    if not d.is_triangle_valid(tri):continue
                    vis=[d.get_triangle_vertex_instance(tri,j)for j in range(3)]
                    uv=[d.get_vertex_instance_uv(vi,0)for vi in vis]
                    det=(uv[1].x-uv[0].x)*(uv[2].y-uv[0].y)-(uv[1].y-uv[0].y)*(uv[2].x-uv[0].x)
                    if not math.isfinite(det)or abs(det)<=1.e-10:bad+=1
                    for vi,value in zip(vis,uv):
                        digest.update(struct.pack('<i2d',int(vi.get_editor_property('id_value')),value.x,value.y))
                need(bad==0,'Native UV0 is not repaired before RHI finalization; run UV phase in a SEPARATE NullRHI process: '+path+' LOD'+str(lod))
                lods.append({'lod':lod,'triangles':d.get_triangle_count(),'geometry_uv0_sha256':digest.hexdigest(),'zero_uv_triangles':bad})
            row['explicit_source_lods']=lods
        result.append(row)
    return result


def run_step(script,receipt=None,list_receipt=False):
    start=datetime.now(timezone.utc).isoformat()
    runpy.run_path(str(SCRIPTS/script),run_name='__main__')
    row={'script':script,'completed_utc':datetime.now(timezone.utc).isoformat(),'started_utc':start}
    if receipt:
        path=PROJECT/'Saved/PresentationValidation'/receipt
        need(path.is_file(),'Missing stage receipt '+receipt)
        data=read_json(path)
        need(bool(data) if list_receipt else data.get('valid')is True,'Stage receipt failed '+receipt)
        row['receipt']=str(path.relative_to(PROJECT))
    REPORT['steps'].append(row);checkpoint()


def calibrate_wood_tile():
    manifest=read_json(SOURCE/'SOURCE_MANIFEST.json')
    sources=[a for a in manifest['assets']if a['asset_id']=='wood_planks']
    need(len(sources)==1,'Unique verified wood source required')
    dimensions=sources[0]['physical_tile_dimensions_cm']
    need(len(dimensions)==2 and abs(float(dimensions[0])-float(dimensions[1]))<.1,'Wood source needs separate U/V calibration')
    tile=round(float(dimensions[0]),3)
    need(149.9<=tile<=150.1,'Unexpected physical wood source size; do not guess '+str(tile))
    path='/Game/Buildings/Architecture/Materials/M_Arch_Wood'
    material=existing(path,unreal.Material)
    expressions=list(L.get_material_expressions(material))
    matches=[n for n in expressions if isinstance(n,unreal.MaterialExpressionScalarParameter)
             and str(n.get_editor_property('desc'))=='ASGlobalV2:TileCm'
             and str(n.get_editor_property('parameter_name'))=='ArchitectureV2TileCm']
    need(len(matches)==1,'Unique owned V2 tile node required')
    samplers=[n for n in expressions if isinstance(n,unreal.MaterialExpressionTextureSampleParameter2D)
              and str(n.get_editor_property('parameter_name'))=='SurfaceAlbedo']
    need(len(samplers)==1 and samplers[0].get_editor_property('texture').get_path_name().split('.')[0]
         =='/Game/Environment/Textures/GlobalPBR/T_wood_planks_C','Wood calibration refuses unrelated albedo')
    properties=[getattr(unreal.MaterialProperty,'MP_'+p)for p in
                ('BASE_COLOR','NORMAL','ROUGHNESS','EMISSIVE_COLOR','WORLD_POSITION_OFFSET','OPACITY','METALLIC')]
    before={p:(L.get_material_property_input_node(material,p),
               str(L.get_material_property_input_node_output_name(material,p)))for p in properties}
    old=float(matches[0].get_editor_property('default_value'))
    if abs(old-tile)>.001:
        matches[0].set_editor_property('default_value',tile)
        errors=list(L.recompile_material(material));need(not errors,'Wood scalar compile failed '+str(errors))
        need(all((L.get_material_property_input_node(material,p),
                  str(L.get_material_property_input_node_output_name(material,p)))==value
                 for p,value in before.items()),'Wood calibration changed output ownership')
        need(A.save_loaded_asset(material),'Wood scalar save failed')
    REPORT['wood_physical_tile_calibration']={'parameter':'ArchitectureV2TileCm','old_default_cm':old,
       'source_tile_cm':tile,'actual_default_cm':float(matches[0].get_editor_property('default_value')),
       'source_url':sources[0]['source_url'],'node_created_or_deleted':False,
       'output_connections_unchanged':True,'geometry_cpd_or_canonical_material_modified':False}
    checkpoint()


def verify_final_shader_resources():
    paths=['/Game/Buildings/Architecture/Materials/M_Arch_'+k for k in ('Plaster','Wood','Roof','Concrete','Brick','Metal','Glass')]
    paths+=['/Game/Environment/Materials/Realistic/M_Environment'+k for k in ('Bark','Grass','Shrub','Conifer','Palm','Broadleaf','Asphalt','LaneMarkings','Sidewalk')]
    paths+=['/Game/Environment/Materials/Presentation/'+k for k in
            ('M_PresentationGroundPBR','M_AftershockRegionalBlend','M_PresentationWater','M_CoastalSea','M_EmergencyAssembly')]
    paths+=['/Game/FX/Aftershock/Materials/'+k for k in ('M_DisasterFloodVolume','M_DisasterTsunamiWave')]
    paths+=['/Game/Buildings/Materials/M_BuildingGhost']
    rows=[]
    for path in paths:
        material=existing(path,unreal.Material)
        errors=list(L.recompile_material(material));stats=L.get_statistics(material)
        row={key:int(stats.get_editor_property(key))for key in
             ('num_vertex_shader_instructions','num_pixel_shader_instructions','num_samplers','num_pixel_texture_samples')}
        need(not errors and row['num_vertex_shader_instructions']>0 and row['num_pixel_shader_instructions']>0,
             'Final actual shader resource missing '+path+' '+str(errors)+' '+str(row))
        need(row['num_samplers']<=16,'Final sampler budget exceeded '+path+' '+str(row))
        rows.append(dict(material=path,**row))
    REPORT['final_shader_resources']=rows
    REPORT['shader_scope']='Compiled native resources in current RHI, not GPU timings or cross-platform acceptance'
    checkpoint()


def main():
    need(PHASE in ('UV','RHI'),'Choose explicit UV or RHI finalization phase')
    need(SM is not None,'StaticMeshEditorSubsystem unavailable')
    rows=manifest_meshes()
    if PHASE=='UV':
        need('nullrhi'in CLI.lower() and 'allowcommandletrendering'not in CLI.lower(),
             'UV repair requires -NullRHI WITHOUT -AllowCommandletRendering, in its own process')
        before=mesh_contract(rows)
        run_step('ue_repair_environment_tangents.py','environment_uv_repair.json')
        after=mesh_contract(rows)
        need(before==after,'UV-only source repair changed mesh bbox/LOD/triangle contract')
        REPORT['mesh_geometry_contract_unchanged']=True
        REPORT['scope']='Native source UV repair only; no material authoring/shader/render/GPU claim'
    else:
        need('allowcommandletrendering'in CLI.lower() and 'nullrhi'not in CLI.lower(),
             'RHI phase requires -AllowCommandletRendering WITHOUT -NullRHI')
        verify_helper_import_guard();preflight_assets(rows)
        before=mesh_contract(rows,uv_fingerprint=True)
        run_step('ue_fix_architecture_semantic_slots.py','architecture_semantic_slots.json',True)
        run_step('ue_import_assembly_sign.py','assembly_sign_native.json') # Existing3LODs only; preflight forbids import branch.
        run_step('ue_upgrade_architecture_art_v2.py','architecture_art_v2_native.json')
        calibrate_wood_tile() # Also corrects reused/completedV2 assets, without new graph/output nodes.
        run_step('ue_author_environment_quality_surfaces.py','environment_quality_surfaces.json')
        run_step('ue_author_road_surface_materials.py','road_pbr_native.json') # Existing21textures, guarded helper import.
        run_step('ue_refine_ground_surface.py','ground_surface_refinement.json')
        run_step('ue_refine_ground_microvariation_v2.py','ground_surface_microvariation_v2.json')
        run_step('ue_refine_ground_stochastic_v3.py','ground_stochastic_v3.json')
        after=mesh_contract(rows,uv_fingerprint=True)
        need(before==after,'RHI material finalization changed source geometry/UV0; preserve receipts and investigate')
        REPORT['mesh_geometry_and_repaired_uv0_unchanged']=True
        verify_final_shader_resources()
        run_step('ue_validate_global_environment.py','global_environment_native_readback.json')
        REPORT['scope']='Post-import semantic/material/physical-tile/shader/native-asset acceptance; native render-frame and gameplay/GPU/RHI-image acceptance remain separate'
    REPORT['valid']=True;REPORT['completed_utc']=datetime.now(timezone.utc).isoformat();checkpoint()
    unreal.log_warning('AFTERSHOCK_GLOBAL_ENVIRONMENT_FINALIZE '+json.dumps({'valid':True,'phase':PHASE,'steps':len(REPORT['steps']),'source_mesh_imports':False}))


try:main()
except Exception as exc:
    REPORT['error']=repr(exc);checkpoint();raise
