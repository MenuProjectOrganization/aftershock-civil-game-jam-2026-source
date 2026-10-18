"""Parent-run bounded native geometry merge. No whole building/source saves."""
from pathlib import Path
import hashlib,json,math,os,traceback,uuid
import unreal

HERE=Path(__file__).resolve().parent
REPO=next(p for p in HERE.parents if (p/'Source/JamProject').is_dir())
PROJECT=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
PREFIX='/Game/Aftershock/CitySample/Architecture'
RECEIPTS=Path(os.environ.get('AFTERSHOCK_CITY_ARCH_RECEIPTS',str(REPO/'Saved/PresentationValidation/CitySampleArchitecture'))).resolve()
if not RECEIPTS.is_relative_to(REPO/'Saved'):raise RuntimeError('Native receipts must remain inside project Saved')
RECEIPTS.mkdir(parents=True,exist_ok=True)
OUT=RECEIPTS/'native_facade_bake.json'
REPORT=dict(schema=1,valid=False,in_progress=True,run_id=str(uuid.uuid4()),source_assets_cc0=False,
    scope='16 native-derived real aperture facade bakes; geometry/LOD/dependency receipt only, no live graphics acceptance',meshes=[])
A=unreal.EditorAssetLibrary
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
ACTORS=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
REG=unreal.AssetRegistryHelpers.get_asset_registry()
DEPS=unreal.AssetRegistryDependencyOptions(include_soft_package_references=True,include_hard_package_references=True,
    include_searchable_names=False,include_soft_management_references=False,include_hard_management_references=False)
NAMES=('SM_CS_06_1st_Floor_Window_3x3_M','SM_CS_02_2nd_Floor_Kit')

def need(ok,msg):
    if not ok:raise RuntimeError(msg)
def checkpoint():
    t=OUT.with_suffix('.json.tmp');t.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8');t.replace(OUT)
def xyz(v):return [float(v.x),float(v.y),float(v.z)]
def face_rotation():
    # UE Python's native MakeRotator positional order differs from C++.
    # Set reflected axes explicitly: a pitch180 flips the source underground.
    r=unreal.Rotator()
    for axis,value in [('pitch',0.),('yaw',180.),('roll',0.)]:r.set_editor_property(axis,value)
    need(abs(r.pitch)<1.e-6 and abs(r.yaw-180.)<1.e-6 and abs(r.roll)<1.e-6,'Explicit yaw180 rotation failed')
    return r
def optional_imported_slot(slot):
    # ImportedMaterialSlotName is editor-only native metadata and is not a
    # reflected Python property in every UE5.8 build. It is never a gate.
    try:return dict(value=str(slot.get_editor_property('imported_material_slot_name')),unavailable_reason=None)
    except Exception as exc:return dict(value=None,unavailable_reason=type(exc).__name__+': '+str(exc))
def material_snapshot(mesh):
    slots=list(mesh.get_editor_property('static_materials'))
    return dict(mesh=mesh.get_path_name(),slots=[dict(index=i,name=str(s.material_slot_name),
        imported_name=optional_imported_slot(s),material=s.material_interface.get_path_name() if s.material_interface else None)
        for i,s in enumerate(slots)],lod_sections=[dict(lod=l,material_indices=[SM.get_lod_material_slot(mesh,l,j)
        for j in range(mesh.get_num_sections(l))]) for l in range(mesh.get_num_lods())])
def staged_actor_snapshot(obj):
    c=obj.static_mesh_component;mesh=c.static_mesh;b=mesh.get_bounding_box();t=c.get_world_transform()
    corners=[xyz(t.transform_location(unreal.Vector(x,y,z))) for x in (b.min.x,b.max.x)
        for y in (b.min.y,b.max.y) for z in (b.min.z,b.max.z)]
    r=obj.get_actor_rotation()
    return dict(mesh=mesh.get_path_name(),rotation_pitch_yaw_roll=[float(r.pitch),float(r.yaw),float(r.roll)],
        materials=[m.get_path_name() if m else None for m in c.get_materials()],
        world_bounds=dict(min_cm=[min(p[i] for p in corners) for i in range(3)],max_cm=[max(p[i] for p in corners) for i in range(3)]))
def box(mesh):
    b=mesh.get_bounding_box();return dict(min_cm=xyz(b.min),max_cm=xyz(b.max),size_cm=xyz(b.max-b.min))
def save(mesh):
    need(mesh.get_path_name().startswith(PREFIX+'/Baked/'),'Only owned baked packages may save')
    need(A.save_loaded_asset(mesh),'Native save failed')
def package_file(package):return REPO/'Content'/(package[6:]+'.uasset')
def size(package):
    p=package_file(package);return sum(q.stat().st_size for ext in ('.uasset','.uexp','.ubulk','.uptnl') if (q:=p.with_suffix(ext)).is_file())
def own_closure(package):
    seen=set();pending=[package]
    while pending:
        p=pending.pop()
        if p in seen:continue
        seen.add(p);need(len(seen)<=512,'Bounded dependency traversal exceeded')
        for d in REG.get_dependencies(unreal.Name(p),DEPS):
            d=str(d)
            need(not d.startswith('/Game/') or d.startswith(PREFIX+'/') or d.startswith('/Game/Buildings/Architecture/')
                 or d.startswith('/Game/Environment/Textures/') or d.startswith('/Game/Aftershock/CitySample/Textures/'),
                 'Unexpected original/unbounded Game dependency '+d)
            if d.startswith('/Game/'):pending.append(d)
    return sorted(seen)
def actor(mesh,location,scale,rotation,materials,staged):
    obj=ACTORS.spawn_actor_from_class(unreal.StaticMeshActor,location,rotation,transient=True)
    need(obj is not None,'Transient native mesh actor unavailable');staged.append(obj)
    c=obj.static_mesh_component;need(c.set_static_mesh(mesh),'Cannot bind actual module geometry')
    obj.set_actor_scale3d(scale)
    for i,m in enumerate(materials):c.set_material(i,m)
    actual=obj.get_actor_rotation()
    for axis in ('pitch','yaw','roll'):
        delta=(float(getattr(actual,axis))-float(getattr(rotation,axis))+180.)%360.-180.
        need(abs(delta)<1.e-4,'Native actor rotation readback differs on '+axis)
    return obj
def cube(cube_mesh,center,dimensions,material,staged):
    need(min(dimensions)>0.,'Invalid original seam/flashing dimensions')
    return actor(cube_mesh,unreal.Vector(*center),unreal.Vector(*(d/100. for d in dimensions)),unreal.Rotator(),[material],staged)
def reductions(mesh):
    opt=unreal.StaticMeshReductionOptions();opt.set_editor_property('auto_compute_lod_screen_size',False)
    rows=[]
    for percent,screen in ((1.,1.),(.35,.20),(.12,.065)):
        row=unreal.StaticMeshReductionSettings();row.set_editor_property('percent_triangles',percent);row.set_editor_property('screen_size',screen);rows.append(row)
    opt.set_editor_property('reduction_settings',rows)
    need(SM.set_lods(mesh,opt)==3,'Actual three classic LOD generation failed')
def validate(mesh,bays,floors):
    b=box(mesh);s=b['size_cm'];lo=b['min_cm']
    need(abs(s[0]-bays*300.)<.5 and abs(s[1]-75.)<.5 and abs(s[2]-floors*300.)<.5 and abs(lo[2])<.25
         and abs(lo[0]+bays*150.)<.25 and abs(lo[1])<.25,'Native whole-height bounds/pivot differ '+repr(b))
    need(mesh.get_num_lods()==3 and not SM.get_nanite_settings(mesh).enabled,'Not exactly3 classic non-Nanite LODs')
    tris=[mesh.get_num_triangles(i) for i in range(3)]
    need(all(t>0 for t in tris) and tris[0]<=50000 and tris[1]<=18000 and tris[2]<=6000,'Native bounded triangles failed '+repr(tris))
    slots=mesh.get_editor_property('static_materials');roles=[str(s.material_slot_name) for s in slots]
    need(len(slots)==3 and set(roles)=={'Wall','Glass','Metal'},'Three exact semantic roles required '+repr(roles))
    for lod in range(3):
        indices=[SM.get_lod_material_slot(mesh,lod,section) for section in range(mesh.get_num_sections(lod))]
        need(indices and all(0<=i<3 for i in indices),'Invalid native section/material remap')
        need(roles.index('Glass') in indices,'Actual glass section disappeared from native LOD'+str(lod))
    return b,tris,roles
def bake(bays,floors,modules,mats,cube_mesh):
    name=f'SM_CS_Facade_B{bays}_F{floors:02d}';package=PREFIX+'/Baked/'+name
    existing=unreal.load_asset(package)
    if existing is not None:
        need(A.get_metadata_tag(existing,'AftershockNativeFacadeVersion')=='1','Preserve unowned existing baked package')
        b,tris,roles=validate(existing,bays,floors)
        return dict(package=package,reused=True,bounds=b,triangles_per_lod=tris,semantic_slots=roles,package_bytes=size(package),native_dependencies=own_closure(package))
    panels=bays*floors;source_lod=None
    for lod in range(3):
        if panels*max(m.get_num_triangles(lod) for m in modules)+(bays+floors+2)*12<=49500:source_lod=lod;break
    need(source_lod is not None,'No source classic LOD fits retained aperture triangle budget')
    staged=[];merged_actor=None
    try:
        # Full-height real module repetition; uniform scale only. Actual QBA
        # identity instances at its +Y perimeter prove this kit faces+Y.
        # A proper180degree rotation restores projectfront−Y without mirroring.
        for floor in range(floors):
            source=modules[0 if floor==0 else 1];bb=source.get_bounding_box();ss=bb.max-bb.min;scale=300./ss.z
            need(0.94<=scale<=1.02 and ss.y*scale<=75.,'Source cannot retain real recess at300cm pitch')
            for bay in range(bays):
                cx=-bays*150.+(bay+.5)*300.
                p=unreal.Vector(cx+(bb.min.x+bb.max.x)*.5*scale,bb.max.y*scale,floor*300.-bb.min.z*scale)
                actor(source,p,unreal.Vector(scale,scale,scale),face_rotation(),[mats['Wall'],mats['Glass']],staged)
        # Native module width after uniform scaling is~289.77cm. Original
        # narrow wall joints fill the remaining gap, never a plane across glass.
        source_width=min((m.get_bounding_box().max.x-m.get_bounding_box().min.x)*300./(m.get_bounding_box().max.z-m.get_bounding_box().min.z) for m in modules)
        seam=300.-source_width;need(0.<seam<15.,'Unexpected native bay seam width')
        for edge in range(bays+1):
            width=seam*.5 if edge in (0,bays) else seam
            cx=-bays*150.+edge*300.+(width*.5 if edge==0 else -width*.5 if edge==bays else 0.)
            cube(cube_mesh,(cx,37.5,floors*150.),(width,75.,floors*300.),mats['Wall'],staged)
        # Real small floor-edge flashing adds the third honest material role.
        # Native QBA_Body frames remain masonry; do not relabel them as metal.
        for floor in range(floors):cube(cube_mesh,(0.,1.,floor*300.+2.),(bays*300.,2.,2.),mats['Metal'],staged)
        settings=unreal.MeshMergingSettings()
        for prop,value in dict(merge_materials=False,merge_equivalent_materials=False,merge_physics_data=False,
            generate_light_map_uv=False,lod_selection_type=unreal.MeshLODSelectionType.SPECIFIC_LOD,specific_lod=source_lod,
            pivot_type=unreal.MeshMergePivotType.WORLD_ORIGIN).items():settings.set_editor_property(prop,value)
        options=unreal.MergeStaticMeshActorsOptions()
        # MeshMergeUtilities.cpp prepends SM_ to the requested short name.
        merge_base=PREFIX+'/Baked/'+name.removeprefix('SM_')
        for prop,value in dict(destroy_source_actors=False,spawn_merged_actor=True,base_package_name=merge_base,new_actor_label=name,
                               mesh_merging_settings=settings).items():options.set_editor_property(prop,value)
        # Persist actual native component overrides and source-section mapping
        # BEFORE merge. NullRHI can cause FMeshMergeHelpers::ExtractSections to
        # substitute DefaultMaterial when GetMaterialResource is unavailable;
        # assigning names after that collapse would silently destroy glass.
        REPORT['current_merge']=dict(package=package,source_lod=source_lod,
            intended_materials={r:m.get_path_name() for r,m in mats.items()},
            source_meshes=[material_snapshot(m) for m in modules],
            actors=[staged_actor_snapshot(o) for o in staged])
        snapshots=REPORT['current_merge']['actors']
        lo=[min(a['world_bounds']['min_cm'][i] for a in snapshots) for i in range(3)]
        hi=[max(a['world_bounds']['max_cm'][i] for a in snapshots) for i in range(3)]
        REPORT['current_merge']['staged_union_bounds']=dict(min_cm=lo,max_cm=hi)
        checkpoint()
        need(all(math.isfinite(v) for v in lo+hi) and all(abs(a-z)<.5 for a,z in zip(lo,[-bays*150.,0.,0.]))
             and all(abs(a-z)<.5 for a,z in zip(hi,[bays*150.,75.,floors*300.])),
             'Staged native geometry escaped exact whole-height envelope before merge: '+repr(REPORT['current_merge']['staged_union_bounds']))
        merged_actor=SM.merge_static_mesh_actors(staged,options)
        need(isinstance(merged_actor,unreal.StaticMeshActor),'Native mesh merge did not return its output actor')
        mesh=merged_actor.static_mesh_component.static_mesh;need(isinstance(mesh,unreal.StaticMesh),'Merged native geometry missing')
        need(mesh.get_path_name()==package+'.'+name,'Native merge output namespace/name drift')
        REPORT['current_merge']['actual_merged']=material_snapshot(mesh);checkpoint()
        unreal.log_warning('AFTERSHOCK_NATIVE_FACADE_MATERIAL_READBACK '+json.dumps(REPORT['current_merge']['actual_merged']))
        slots=list(mesh.get_editor_property('static_materials'))
        for s in slots:
            role=next((r for r,m in mats.items() if s.material_interface==m),None)
            need(role is not None,'Unknown merged material: '+(s.material_interface.get_path_name() if s.material_interface else 'None')
                +'; inspect current_merge.actual_merged; never infer Glass after default-material collapse')
            s.set_editor_property('material_slot_name',unreal.Name(role))
        mesh.set_editor_property('static_materials',slots)
        need(SM.generate_box_uv_channel(mesh,0,0,unreal.Vector(),unreal.Rotator(),unreal.Vector(200,200,200)), 'Metric200cm native box UV authoring failed')
        build=SM.get_lod_build_settings(mesh,0)
        for prop,value in dict(recompute_normals=False,recompute_tangents=True,use_mikk_t_space=True,use_full_precision_u_vs=True).items():build.set_editor_property(prop,value)
        SM.set_lod_build_settings(mesh,0,build)
        nanite=SM.get_nanite_settings(mesh);nanite.set_editor_property('enabled',False);SM.set_nanite_settings(mesh,nanite,False)
        reductions(mesh);SM.remove_collisions(mesh);mesh.set_editor_property('allow_cpu_access',False)
        A.set_metadata_tag(mesh,'AftershockNativeFacadeVersion','1')
        A.set_metadata_tag(mesh,'AftershockNativeFacadeSource','CitySample02and06; actualQBA_Glass; native merge; uniform300cm floor pitch; original narrow seams/flashing')
        b,tris,roles=validate(mesh,bays,floors);save(mesh);file=package_file(package);REG.scan_files_synchronous([str(file)],True)
        return dict(package=package,reused=False,bounds=b,triangles_per_lod=tris,semantic_slots=roles,source_classic_lod=source_lod,
            source_panels=panels,source_uniform_scale=300./310.5911817550659,metric_uv_cm=200,front_axis='-Y',
            actual_native_window_geometry=True,body_inset_required_cm=75.,package_bytes=size(package),sha256=hashlib.sha256(file.read_bytes()).hexdigest(),native_dependencies=own_closure(package))
    finally:
        if merged_actor is not None:ACTORS.destroy_actor(merged_actor)
        for obj in staged:ACTORS.destroy_actor(obj)
def main():
    checkpoint();need(PROJECT==REPO,'Bake only in actual project after proper native migration/binding')
    need(SM is not None and ACTORS is not None,'Full editor lifecycle and actual editor subsystems required')
    binding=json.loads((RECEIPTS/'native_modules_bind.json').read_text());need(binding.get('valid') and len(binding.get('bound_modules',[]))==2,'Accepted2module binding receipt required')
    modules=[unreal.load_asset(PREFIX+'/Meshes/'+n) for n in NAMES]
    need(all(isinstance(m,unreal.StaticMesh) and m.get_num_lods()==3 for m in modules),'Actual native3LOD modules missing')
    mats={r:unreal.load_asset('/Game/Buildings/Architecture/Materials/M_Arch_'+n) for r,n in [('Wall','Plaster'),('Glass','Glass'),('Metal','Metal')]}
    need(all(isinstance(m,unreal.MaterialInterface) for m in mats.values()),'Existing canonical shared materials missing')
    cube_mesh=unreal.load_asset('/Engine/BasicShapes/Cube');need(isinstance(cube_mesh,unreal.StaticMesh),'Original seam geometry cube absent')
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    for floors in range(1,5):
        for bays in range(1,5):REPORT['meshes'].append(bake(bays,floors,modules,mats,cube_mesh));checkpoint()
    total=sum(r['package_bytes'] for r in REPORT['meshes']);need(total<=64*1024*1024,'16facade package budget exceeded64MiB')
    REPORT.update(valid=True,in_progress=False,total_baked_package_bytes=total,normal_tangent_native_readback_pending=True,
        live_renderer_material_cpd_screenshot_acceptance_pending=True,canonical_grid_building_save_writes=False)
    checkpoint();unreal.log_warning('AFTERSHOCK_NATIVE_FACADES_BAKED '+str(OUT))
if __name__=='__main__':
    try:main()
    except Exception:
        REPORT.update(valid=False,in_progress=False,error=traceback.format_exc());checkpoint();raise
