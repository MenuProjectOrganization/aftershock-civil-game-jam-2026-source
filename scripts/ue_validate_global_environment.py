"""Reload and validate actual UE5.8 owned-art resources; never import/save assets.

Run using UnrealEditor-Cmd, PythonScript and -AllowCommandletRendering. Shader
statistics synchronously finish native compilation; they are NOT GPU timings.
Optional -AftershockGlobalArtProofManifest="...json" also validates three REAL
gameplay metadata captures (quality0/3/4). No capture supplied => runtime proof
explicitly NOT executed. Never starts PIE, applies quality, saves preferences,
spawns a town, moves a camera or substitutes a fabricated component count.
"""
from pathlib import Path
import hashlib
import json
import math
import re
import traceback
import unreal

ROOT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE=ROOT/'_Imports/GlobalEnvironment'
OUT=ROOT/'Saved/PresentationValidation/global_environment_native_readback.json'
L=unreal.MaterialEditingLibrary
A=unreal.EditorAssetLibrary
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)
REPORT={'schema_version':1,'valid':False,'asset_validation_valid':False,
        'runtime_requested':False,'runtime_acceptance_complete':False,
        'scope':'Actual native assets/shader resources; optional actual-game JSON. No GPU timing, visibility or visual-quality certification.',
        'assets_imported_or_saved':False,'preferences_written':False,
        'textures':[],'materials':[],'meshes':[],'runtime_captures':[],
        'source_hashes_checked':0,'errors':[]}

def checkpoint():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    OUT.write_text(json.dumps(REPORT,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def need(condition,message):
    if not condition:raise RuntimeError(message)

def read_json(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def path_of(obj):return obj.get_path_name() if obj else None

def checked_source(path,expected):
    need(path.is_file(),'Missing source file '+str(path))
    actual=hashlib.sha256(path.read_bytes()).hexdigest()
    need(actual==expected,'Source SHA256 mismatch '+str(path))
    REPORT['source_hashes_checked']+=1
    return actual

def reachable_nodes(material,roots):
    queue=list(roots);found={}
    while queue:
        item=queue.pop()
        if not item or item.get_path_name() in found:continue
        found[item.get_path_name()]=item
        queue.extend(x for x in L.get_inputs_for_material_expression(material,item) if x)
    return list(found.values())

def check_material(path,kind,leaf=False,ghost=False):
    material=unreal.load_asset(path)
    need(isinstance(material,unreal.Material),'Missing base material '+path)
    # UE5.8 retains the reflected read-only usage flag, although C++ callers
    # should prefer GetUsageByFlag. We never trigger automatic usage mutation.
    need(bool(material.get_editor_property('used_with_instanced_static_meshes')),'Missing explicit ISM shader usage '+path)
    properties=['BASE_COLOR','ROUGHNESS','NORMAL'] if not ghost else ['EMISSIVE_COLOR','OPACITY']
    if kind not in ('Leaf','Bark','Ghost'):properties.append('EMISSIVE_COLOR')
    if leaf or kind=='Bark':properties.append('WORLD_POSITION_OFFSET')
    roots=[L.get_material_property_input_node(material,getattr(unreal.MaterialProperty,'MP_'+p)) for p in properties]
    need(all(roots),'Disconnected material outputs / partial owned graph '+path+' '+str(properties))
    nodes=reachable_nodes(material,roots)
    if not ghost and path.startswith('/Game/Buildings/Architecture/'):
        need(any(str(n.get_editor_property('desc'))=='ASGlobalV2:Complete' for n in L.get_material_expressions(material)),'Architecture graph V2 incomplete '+path)
    scalar=[];vectors=[];parameters=[]
    for n in nodes:
        if isinstance(n,unreal.MaterialExpressionPerInstanceCustomData):
            scalar.append({'index':int(n.get_editor_property('data_index')),'default':float(n.get_editor_property('const_default_value'))})
        if isinstance(n,unreal.MaterialExpressionPerInstanceCustomData3Vector):
            c=n.get_editor_property('const_default_value')
            vectors.append({'index':int(n.get_editor_property('data_index')),'default':[c.r,c.g,c.b,c.a]})
        if isinstance(n,unreal.MaterialExpressionVectorParameter):parameters.append(str(n.get_editor_property('parameter_name')))
        if isinstance(n,unreal.MaterialExpressionTextureSampleParameter2D):
            t=n.get_editor_property('texture');sampler=n.get_editor_property('sampler_type')
            need(t is not None,'Unbound texture sample '+path)
            if sampler==unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL:
                need(not t.get_editor_property('srgb') and t.get_editor_property('compression_settings')==unreal.TextureCompressionSettings.TC_NORMALMAP,'Normalmap/sampler mismatch '+path_of(t))
            if sampler==unreal.MaterialSamplerType.SAMPLERTYPE_MASKS:need(not t.get_editor_property('srgb'),'sRGB mask sample '+path_of(t))
    if ghost:
        need('GhostTint' in parameters,'GhostTint is not connected '+path)
        need(not scalar and not vectors,'Ghost unexpectedly reads canonical/presentation CPD '+path)
    elif kind not in ('Leaf','Bark'):
        need(any(x['index']==0 and abs(x['default'])<1e-6 for x in scalar),'Damage0 not connected/default0 '+path)
        need({0,1,2,3}.issubset({x['index'] for x in scalar}),'V2 canonical0..3 not all reachable '+path)
        if kind not in ('Metal','Roof','Glass'):
            need(any(x['index']==8 and all(abs(c-1)<1e-6 for c in x['default'][:3]) for x in vectors),'RGB8..10 missing/incorrect default1 '+path)
        if kind=='Glass':need(any(x['index']==2 and abs(x['default']-1)<1e-6 for x in scalar),'Window power2 missing/default1 '+path)
    if leaf:
        need(material.get_editor_property('two_sided'),'Leaf material must be two-sided '+path)
        need(material.get_editor_property('shading_model')==unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE,'Wrong foliage lighting model '+path)
        need(L.get_material_property_input_node(material,unreal.MaterialProperty.MP_SUBSURFACE_COLOR) is not None,'Foliage subsurface disconnected '+path)
    errors=list(L.recompile_material(material))
    stats=L.get_statistics(material) # Native GetStatistics calls FinishCompilation.
    statistics={k:int(stats.get_editor_property(k)) for k in ('num_vertex_shader_instructions','num_pixel_shader_instructions','num_samplers','num_pixel_texture_samples','num_interpolator_scalars')}
    need(not errors and statistics['num_vertex_shader_instructions']>0 and statistics['num_pixel_shader_instructions']>0,'Invalid final shader resource '+path+' '+repr(errors)+' '+repr(statistics))
    REPORT['materials'].append({'path':path,'kind':kind,'compiled_errors':errors,'statistics':statistics,
        'connected_scalar_cpd':scalar,'connected_vector_cpd':vectors,
        'structural_cpd1_or3_read':any(x['index'] in (1,3) for x in scalar),
        'two_sided':bool(material.get_editor_property('two_sided')),'statistics_are_runtime_timings':False})
    checkpoint()

def check_textures(source):
    for row in source['files']:
        checked_source(SOURCE/row['relative_path'],row['sha256'])
        suffix={'diffuse':'C','normal_dx':'N','roughness':'R'}[row['role']]
        path=row.get('native_texture_path','/Game/Environment/Textures/GlobalPBR/T_'+row['asset_id']+'_'+suffix)
        t=unreal.load_asset(path);need(isinstance(t,unreal.Texture2D),'Missing native texture '+path)
        normal=row['role']=='normal_dx';srgb=row['role']=='diffuse'
        compression=unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT if srgb else unreal.TextureCompressionSettings.TC_MASKS
        need(bool(t.get_editor_property('srgb'))==srgb,'Wrong texture sRGB '+path)
        need(t.get_editor_property('compression_settings')==compression,'Wrong compression '+path)
        need(not t.get_editor_property('never_stream'),'Unbounded never-stream texture '+path)
        need(int(t.get_editor_property('max_texture_size'))==int(row['dimensions_px'][0]),'Texture resolution cap mismatch '+path)
        import_data=t.get_editor_property('asset_import_data')
        imported=Path(import_data.get_first_filename()) if import_data else None
        need(imported and imported.is_file(),'Missing native import source '+path)
        need(hashlib.sha256(imported.read_bytes()).hexdigest()==row['sha256'],'Native texture import filename points at different source '+path)
        REPORT['textures'].append({'path':path,'role':row['role'],'srgb':srgb,'streaming':True,
            'max_texture_size':int(t.get_editor_property('max_texture_size')),'source_sha256':row['sha256'],
            'import_filename_matches_current_source':True,
            'native_pixel_hash_not_measured':True})
    checkpoint()

def check_meshes(filename,folder,environment):
    manifest=read_json(SOURCE/filename)
    for row in manifest['assets']:
        sources=row['lods'] if environment else [row]
        for src in sources:checked_source(SOURCE/folder/src['source_file'],src['sha256'])
        mesh=unreal.load_asset(row['target_package'])
        need(isinstance(mesh,unreal.StaticMesh),'Missing native static mesh '+row['target_package'])
        count=SM.get_lod_count(mesh);need(count==3,'Native three-LOD requirement '+row['asset_name']+' '+str(count))
        triangles=[int(mesh.get_num_triangles(i)) for i in range(count)]
        need(all(n>0 for n in triangles),'Empty real renderLOD '+row['asset_name'])
        need(triangles[0]==sources[0]['triangles'],'LOD0 triangle/source mismatch '+row['asset_name'])
        if environment:need(triangles==[int(s['triangles']) for s in sources],'Explicit environment LOD triangles mismatch '+row['asset_name'])
        elif row['asset_name'].startswith('SM_Arch_Facade_'):
            need(triangles[0]>triangles[1]>triangles[2],'Facade reduction failed / duplicate costly LOD '+row['asset_name']+' '+str(triangles))
        sizes=list(float(x) for x in SM.get_lod_screen_sizes(mesh))
        need(len(sizes)==3 and all(math.isfinite(x) and x>0 for x in sizes) and sizes[0]>sizes[1]>sizes[2],'Invalid LOD screen sizes '+row['asset_name'])
        box=mesh.get_bounding_box();expected=sources[0]['bounds_cm'];lo=[box.min.x,box.min.y,box.min.z];hi=[box.max.x,box.max.y,box.max.z]
        # UE FBX ConvertPos(X,Y,Z)=(X,-Y,Z); compare full bounds in native basis.
        native_expected_min=[expected['min'][0],-expected['max'][1],expected['min'][2]]
        native_expected_max=[expected['max'][0],-expected['min'][1],expected['max'][2]]
        for a,b,e in zip(lo+hi,native_expected_min+native_expected_max,expected['size']*2):need(abs(a-b)<=max(.5,e*.001),'Native bounds/pivot mismatch '+row['asset_name']+' actual='+str(lo+hi)+' source='+str(expected))
        mats=[path_of(mesh.get_material(i)) for i in range(len(mesh.get_editor_property('static_materials')))]
        arch='/Game/Buildings/Architecture/Materials/M_Arch_'
        if environment:
            family='Grass' if row['family']=='GrassClump' else row['family']
            expected_mats=['/Game/Environment/Materials/Realistic/M_EnvironmentBark.M_EnvironmentBark',f'/Game/Environment/Materials/Realistic/M_Environment{family}.M_Environment{family}']
        else:
            # Native FBX import removes UNUSED source slots. Canopy has noGlass:
            # its remaining index1 is FrameMetal, not the original source index1.
            authored_roles=[r for r in row['material_roles'] if row['material_triangle_counts'].get(r,0)>0]
            role_material={'Wall':'Plaster','Glass':'Glass','MetalFrame':'Metal','Roof':'Roof','MetalGutter':'Metal','WallGable':'Plaster'}
            roles=[role_material[r] for r in authored_roles]
            expected_mats=[arch+r+'.M_Arch_'+r for r in roles]
        need(mats==expected_mats,'Native material slot order mismatch '+row['asset_name']+' '+str(mats))
        for lod in range(count):
            build=SM.get_lod_build_settings(mesh,lod)
            need(not build.get_editor_property('generate_lightmap_u_vs'),'Unrequested lightmap UV build '+row['asset_name'])
            need(mesh.get_num_tex_coords(lod)>=1,'Missing physical UV0 '+row['asset_name'])
        need(not mesh.get_editor_property('nanite_settings').get_editor_property('enabled'),'Unexpected Nanite enable '+row['asset_name'])
        REPORT['meshes'].append({'path':row['target_package'],'bounds_min_cm':lo,'bounds_max_cm':hi,'lod_count':count,
            'actual_render_triangles':triangles,'screen_sizes':sizes,'material_slots':mats,
            'automatic_LOD_selection_in_actual_game_not_measured':True})
        checkpoint()

def option(name):
    line=unreal.SystemLibrary.get_command_line()
    match=re.search(r'(?:^|\s)-'+re.escape(name)+r'=(?:"([^"]+)"|(\S+))',line,re.I)
    return (match.group(1) or match.group(2)) if match else None

def runtime_proofs(manifest_path):
    REPORT['runtime_requested']=True
    manifest=read_json(manifest_path);need(manifest.get('schema_version')==1,'Runtime manifest schema')
    groups={}
    for capture in manifest['captures']:
        p=Path(capture['path']);p=p if p.is_absolute() else ROOT/p
        d=read_json(p);g=d.get('global_environment',{});quality=int(capture['quality'])
        need(quality in (0,3,4),'Expected low0/ultra3/cinematic4')
        need(g.get('schema_version')==1 and g.get('requested_quality')==quality,'Missing actual game global_environment metadata '+str(p))
        need(g.get('game_foliage_quality')==quality,'GameUserSettings foliage differs from request '+str(p))
        need(int(g.get('cvars',{}).get('sg.FoliageQuality',-1))==quality,'Applied foliage cvar mismatch '+str(p))
        need(g.get('canonical_cpd_floats_valid') is True and g.get('derived_cpd_floats_valid') is True,'Canonical4/derived11 data contract failed '+str(p))
        need(int(g.get('module_cpd_values_checked',0))>0 and g.get('module_cpd_mismatches')==0,'No real ID-to-module canonical CPD comparison '+str(p))
        need(re.fullmatch(r'[0-9a-fA-F]{64}',g.get('canonical_buildings_sha256','')) is not None,'Missing canonical snapshot SHA256 '+str(p))
        components=d.get('render_components',[])
        architecture=[c for c in components if '/Buildings/Architecture/Meshes/' in c.get('mesh','') and c.get('component_visible')]
        environment=[c for c in components if '/Environment/Meshes/Realistic/' in c.get('mesh','') and c.get('component_visible')]
        arch_count=sum(int(c['nonzero_transform_instances']) for c in architecture)
        env_count=sum(int(c['nonzero_transform_instances']) for c in environment)
        need(arch_count>0,'No actual architectural component instances '+str(p))
        for c in architecture:
            need(c.get('num_custom_data_floats')==11,'Wrong derived CPD stride '+str(p))
            need(c.get('collision_enabled')==0 and c.get('tick_enabled') is False,'Module collision/tick introduced '+str(p))
            need(c.get('material_slots') and all('/Buildings/Architecture/Materials/' in str(s.get('material','')) for s in c['material_slots']),'Wrong actual module materials '+str(p))
            need(len(c.get('triangles_per_render_lod',[]))==3,'Actual instance render mesh lacks3LODs '+str(p))
        row={'path':str(p),'quality':quality,'region_id':d.get('region_id'),'focus_id':d.get('focus_id'),
             'terrain_seed':d.get('terrain_seed'),'canonical_buildings':d.get('canonical_buildings'),
             'living':d.get('canonical_living_residents'),'canonical_buildings_sha256':g['canonical_buildings_sha256'],
             'architecture_instances':arch_count,'environment_instances':env_count,
             'gpu_visibility_not_measured':True,'automatic_GPU_LOD_selection_not_measured':True}
        key=(row['region_id'],row['focus_id'],d.get('stage'))
        need(quality not in groups.setdefault(key,{}),'Duplicate runtime quality capture')
        groups[key][quality]=row;REPORT['runtime_captures'].append(row)
    need(groups,'No runtime capture groups')
    for key,group in groups.items():
        need(set(group)=={0,3,4},'Incomplete low/ultra/cinematic actual captures '+str(key))
        baseline=group[0]
        for q in (3,4):
            for field in ('terrain_seed','canonical_buildings','living','canonical_buildings_sha256'):
                need(group[q][field]==baseline[field],'Presentation quality altered canonical state '+field+' '+str(key))
        need(group[3]['environment_instances']>0 and group[4]['environment_instances']>0,'No actual environment instance proof '+str(key))
        need(group[0]['environment_instances']<group[3]['environment_instances']<=group[4]['environment_instances'],'Foliage quality has no expected actual count response '+str(key))
    REPORT['runtime_acceptance_complete']=True

def main():
    need(SM is not None,'StaticMeshEditorSubsystem unavailable')
    source=read_json(SOURCE/'SOURCE_MANIFEST.json');check_textures(source)
    for kind in ('Plaster','Wood','Roof','Concrete','Brick','Metal','Glass'):check_material('/Game/Buildings/Architecture/Materials/M_Arch_'+kind,kind)
    check_material('/Game/Environment/Materials/Realistic/M_EnvironmentBark','Bark')
    for family in ('Grass','Shrub','Conifer','Palm','Broadleaf'):check_material('/Game/Environment/Materials/Realistic/M_Environment'+family,'Leaf',leaf=True)
    check_material('/Game/Buildings/Materials/M_BuildingGhost','Ghost',ghost=True)
    check_meshes('ARCHITECTURE_GEOMETRY_MANIFEST.json','architecture_geometry',False)
    check_meshes('ENVIRONMENT_GEOMETRY_MANIFEST.json','environment_geometry',True)
    check_meshes('BROADLEAF_GEOMETRY_MANIFEST.json','broadleaf_geometry',True)
    need(len(REPORT['meshes'])==82,'Expected67 architecture +15 environment native meshes')
    REPORT['asset_validation_valid']=True
    manifest=option('AftershockGlobalArtProofManifest')
    if manifest:runtime_proofs(manifest)
    REPORT['valid']=True;checkpoint()
    unreal.log_warning('AFTERSHOCK_GLOBAL_ENVIRONMENT_NATIVE_READBACK '+json.dumps({'valid':True,'textures':len(REPORT['textures']),'materials':len(REPORT['materials']),'meshes':len(REPORT['meshes']),'runtime_acceptance_complete':REPORT['runtime_acceptance_complete']}))

try:main()
except Exception as exc:
    REPORT['errors'].append(repr(exc));REPORT['traceback']=traceback.format_exc();checkpoint();raise
