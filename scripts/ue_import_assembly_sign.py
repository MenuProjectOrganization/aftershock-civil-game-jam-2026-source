"""Import original assembly wayfinding; parent coordinates native Editor run."""
import hashlib,json,math
from pathlib import Path
import unreal
ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE=ROOT/'_Imports/GlobalEnvironment'
if not(SOURCE/'ASSEMBLY_SIGN_GEOMETRY_MANIFEST.json').exists():SOURCE=ROOT/'Saved/global-art-source'
MANIFEST=json.loads((SOURCE/'ASSEMBLY_SIGN_GEOMETRY_MANIFEST.json').read_text(encoding='utf-8'))
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)
A=unreal.EditorAssetLibrary;L=unreal.MaterialEditingLibrary
PATH=MANIFEST['target_package']
REPORT={'valid':False,'semantic':'EmergencyAssembly wayfinding, not transit','canonical_changes':False}
OUT=ROOT/'Saved/PresentationValidation/assembly_sign_native.json'

def main():
    for source in MANIFEST['lods']:
        path=SOURCE/'assembly_sign_geometry'/source['source_file']
        if hashlib.sha256(path.read_bytes()).hexdigest()!=source['sha256']:raise RuntimeError('Sign source hash '+str(path))
    mesh=unreal.load_asset(PATH) if A.does_asset_exist(PATH)else None
    if mesh is None:
        options=unreal.FbxImportUI()
        for k,v in [('import_mesh',True),('import_materials',False),('import_textures',False),
                    ('automated_import_should_detect_type',False),('mesh_type_to_import',unreal.FBXImportType.FBXIT_STATIC_MESH)]:
            options.set_editor_property(k,v)
        data=unreal.FbxStaticMeshImportData()
        for k,v in [('combine_meshes',True),('auto_generate_collision',False),
                    ('generate_lightmap_u_vs',False),('normal_import_method',unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS)]:
            data.set_editor_property(k,v)
        options.set_editor_property('static_mesh_import_data',data)
        task=unreal.AssetImportTask()
        for k,v in [('filename',str(SOURCE/'assembly_sign_geometry'/MANIFEST['lods'][0]['source_file'])),
                    ('destination_path',PATH.rsplit('/',1)[0]),('destination_name',PATH.rsplit('/',1)[1]),
                    ('automated',True),('save',True),('options',options)]:
            task.set_editor_property(k,v)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        mesh=unreal.load_asset(PATH)
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError('Sign FBX did not create native mesh')
        A.set_metadata_tag(mesh,'AftershockOriginalAssemblySign','V1')
    elif str(A.get_metadata_tag(mesh,'AftershockOriginalAssemblySign'))!='V1':
        raise RuntimeError('Preserve unowned existing sign asset')
    else:
        old_hash=str(A.get_metadata_tag(mesh,'AftershockOriginalAssemblySignSourceHash')or'')
        if old_hash and old_hash!=MANIFEST['lods'][0]['sha256']:
            raise RuntimeError('Owned sign source changed: explicit coordinated reimport/version required; do not relabel stale native geometry')
    for lod in (1,2):
        if SM.get_lod_count(mesh)<=lod:
            if SM.import_lod(mesh,lod,str(SOURCE/'assembly_sign_geometry'/MANIFEST['lods'][lod]['source_file']))!=lod:
                raise RuntimeError('Assembly sign explicitLOD failed '+str(lod))
    if SM.get_lod_count(mesh)!=3:raise RuntimeError('Sign must have3nativeLODs')
    for slot in MANIFEST['material_slots']:
        mat=unreal.load_asset(slot['existing_material'])
        if not isinstance(mat,unreal.MaterialInterface):raise RuntimeError('Sign shared material missing '+slot['existing_material'])
        mesh.set_material(slot['slot'],mat)
        # Existing owned material parents already support instancing; set only
        # if this is the owned base material itself, no new material/MID graph.
        if isinstance(mat,unreal.Material):
            L.set_base_material_usage(mat,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
            errors=L.recompile_material(mat)
            if errors:raise RuntimeError('Assembly sign material compile '+str(errors))
            if not A.save_loaded_asset(mat):raise RuntimeError('Sign material save failed')
    lods=[]
    for lod in range(3):
        b=SM.get_lod_build_settings(mesh,lod)
        for k,v in [('generate_lightmap_u_vs',False),('recompute_normals',False),
                    ('recompute_tangents',True),('use_mikk_t_space',True),('use_full_precision_u_vs',True)]:
            b.set_editor_property(k,v)
        SM.set_lod_build_settings(mesh,lod,b)
        desc=mesh.get_static_mesh_description(lod)
        if desc.get_triangle_count()!=MANIFEST['lods'][lod]['triangles']:raise RuntimeError('SignLOD triangle mismatch')
        bad=0
        for i in range(desc.get_triangle_count()):
            tid=unreal.TriangleID();tid.set_editor_property('id_value',i)
            uv=[desc.get_vertex_instance_uv(desc.get_triangle_vertex_instance(tid,j),0)for j in range(3)]
            det=(uv[1].x-uv[0].x)*(uv[2].y-uv[0].y)-(uv[1].y-uv[0].y)*(uv[2].x-uv[0].x)
            if not math.isfinite(det)or abs(det)<1.e-10:bad+=1
        if bad:raise RuntimeError('Sign degenerate native UV '+str(bad))
        lods.append({'lod':lod,'triangles':desc.get_triangle_count(),'zero_uv_triangles':bad})
    if not SM.set_lod_screen_sizes(mesh,[1.,.10,.025]):raise RuntimeError('Sign native screenLOD settings failed')
    ns=mesh.get_editor_property('nanite_settings');ns.set_editor_property('enabled',False);mesh.set_editor_property('nanite_settings',ns)
    box=mesh.get_bounding_box()
    actual=[box.max.x-box.min.x,box.max.y-box.min.y,box.max.z-box.min.z]
    expected=MANIFEST['lods'][0]['bounds_cm']['size']
    if any(abs(a-e)>.15 for a,e in zip(actual,expected))or abs(box.min.z)>.1:raise RuntimeError('Sign realcm bbox/pivot mismatch '+str(actual))
    A.set_metadata_tag(mesh,'AftershockOriginalAssemblySignSourceHash',MANIFEST['lods'][0]['sha256'])
    if not A.save_loaded_asset(mesh):raise RuntimeError('Sign native save failed')
    REPORT.update(valid=True,asset=mesh.get_path_name(),bounds_cm=actual,pivot_min_z_cm=box.min.z,lods=lods,
        material_slots=[mesh.get_material(i).get_path_name()for i in range(2)],
        ground_pad=False,native_scale_required=[1,1,1],native_import_valid=True,runtime_render_acceptance_pending=True)

try:
    main()
except Exception as exc:
    REPORT['error']=repr(exc);raise
finally:
    OUT.parent.mkdir(parents=True,exist_ok=True);OUT.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_ASSEMBLY_SIGN_NATIVE '+json.dumps(REPORT))
