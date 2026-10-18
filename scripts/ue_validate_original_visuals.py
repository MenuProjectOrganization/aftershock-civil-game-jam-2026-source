"""Reload native assets and synchronously validate their final render resources.

Requires -AllowCommandletRendering. Never imports, saves, deletes, loads maps or
starts gameplay. Statistics calls FinishCompilation in the UE5.8 native API,
so a missing shader resource/failed material cannot pass as an empty error list.
"""
from pathlib import Path
import json,math
import unreal

ROOT=Path(__file__).resolve().parent.parent
LIB=unreal.MaterialEditingLibrary
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)

def material_resource(path):
    mat=unreal.load_asset(path)
    if not isinstance(mat,unreal.Material):raise RuntimeError('Render validation missing base material '+path)
    errors=list(LIB.recompile_material(mat))
    stats=LIB.get_statistics(mat) # Native implementation calls FinishCompilation.
    data={name:int(stats.get_editor_property(name))for name in('num_vertex_shader_instructions','num_pixel_shader_instructions','num_samplers','num_vertex_texture_samples','num_pixel_texture_samples','num_uv_scalars','num_interpolator_scalars')}
    if errors or data['num_vertex_shader_instructions']<=0 or data['num_pixel_shader_instructions']<=0:
        raise RuntimeError('Final material render resource invalid '+path+' '+repr(errors)+' '+repr(data))
    return {'material':path,'final_compile_errors':errors,'shader_statistics':data,'statistics_are_not_runtime_timings':True}

def main():
    source=json.loads((ROOT/'_Imports'/'Original'/'Citizens'/'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    mesh=unreal.load_asset('/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy')
    if not isinstance(mesh,unreal.StaticMesh)or SM.get_lod_count(mesh)!=3:raise RuntimeError('Saved citizen mesh/threeLODs missing')
    lods=[]
    for lod in range(3):
        d=mesh.get_static_mesh_description(lod);parts=set();count=0
        if SM.get_num_uv_channels(mesh,lod)<2:raise RuntimeError('Saved UV1 missing')
        for i in range(d.get_vertex_instance_count()):
            vi=unreal.VertexInstanceID();vi.set_editor_property('id_value',i)
            if not d.is_vertex_instance_valid(vi):continue
            a=d.get_vertex_instance_uv(vi,0);b=d.get_vertex_instance_uv(vi,1);part=int(round(b.x))
            if str(part)not in source['parts']or abs(b.x-part)>.001 or abs(b.y-source['parts'][str(part)]['pivot_z_cm']/200)>.001 or not(0<=a.x<=1 and 0<=a.y<=1):raise RuntimeError('Saved citizen articulation metadata corrupt')
            parts.add(part);count+=1
        if not all(i in parts for i in range(17)):raise RuntimeError('Saved citizen parts missing')
        triangles=d.get_triangle_count()
        if triangles!=source['models'][lod]['triangles']:raise RuntimeError('Authored citizen LOD changed unexpectedly')
        settings=SM.get_lod_build_settings(mesh,lod)
        if not settings.get_editor_property('use_full_precision_u_vs')or settings.get_editor_property('generate_lightmap_u_vs'):raise RuntimeError('Citizen metadata UV build precision/settings changed')
        lods.append({'lod':lod,'native_triangles':triangles,'render_vertices':SM.get_number_verts(mesh,lod),'uv_channels':SM.get_num_uv_channels(mesh,lod),'part_ids':sorted(parts),'vertex_instances':count})
    models=[];vehicle_source=json.loads((ROOT/'_Imports'/'Original'/'Transport'/'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    for entry in vehicle_source['models']:
        path='/Game/Environment/Transport/'+entry['name'];m=unreal.load_asset(path)
        if not isinstance(m,unreal.StaticMesh):raise RuntimeError('Saved vehicle missing '+path)
        b=m.get_bounds().box_extent;actual=[b.x*2,b.y*2,b.z*2]
        if any(abs(a-e)>.05 for a,e in zip(actual,entry['dimensions_cm'])):raise RuntimeError('Saved vehicle dimensions differ '+path)
        d=m.get_static_mesh_description(0)
        if d.get_triangle_count()!=entry['triangles']:raise RuntimeError('Saved vehicle triangle count mismatch '+path)
        if m.get_material(0).get_path_name()!='/Game/Environment/Transport/Materials/M_VehicleBody.M_VehicleBody':raise RuntimeError('Saved vehicle shared material mismatch')
        models.append({'asset':path,'native_triangles':d.get_triangle_count(),'dimensions_cm':actual,'source_bounds_equal':True})
    materials=[material_resource(p)for p in('/Game/Environment/Materials/M_CitizenActivity','/Game/Environment/Transport/Materials/M_VehicleBody','/Game/Environment/Transport/Materials/M_VehicleRubber','/Game/Environment/Transport/Materials/M_ResidentEmotions','/Game/Buildings/Materials/M_Building_Base','/Game/Buildings/Materials/M_ServiceBuilding')]
    report={'valid':True,'proof_scope':'Reloaded native sourceLOD/UV1/material assignments and synchronously compiled real render resources; not gameplay, visual animation or GPU timing proof','citizen_lods':lods,'lod_screen_sizes':[float(x)for x in SM.get_lod_screen_sizes(mesh)],'models':models,'materials':materials}
    out=ROOT/'Saved'/'PresentationValidation'/'original_visuals_native_readback.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    unreal.log_warning('AFTERSHOCK_ORIGINAL_VISUALS_NATIVE_READBACK '+json.dumps(report))

if __name__=='__main__':main()
