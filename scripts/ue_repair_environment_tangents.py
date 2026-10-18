"""Native UV-only repair of original environment MeshDescription assets.

Parent runs this in its single coordinated UE Editor process. This script never
imports/re-exports source FBX, edits source topology/positions/colors, toggles a
plugin or changes materials. All45 sourceLOD plans preflight before mutation.
Python cannot inspect native render tangent buffers: separate C++ readback is
required before claiming the generated tangent frames finite/nonzero.
"""
import hashlib
import json
import math
import struct
from pathlib import Path

import unreal

if 'AllowCommandletRendering' in unreal.SystemLibrary.get_command_line():raise RuntimeError('UV source repair requires NullRHI, without AllowCommandletRendering: prevent live render-resource replacement')
ROOT=Path(unreal.Paths.project_dir()).resolve()
SOURCE=ROOT/'_Imports/GlobalEnvironment'
OUT=ROOT/'Saved/PresentationValidation/environment_uv_repair.json'
SM=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.get_default_object(unreal.StaticMeshEditorSubsystem)
ASSETS=unreal.EditorAssetLibrary
EPS=1.e-10
TILE_CM=200.
REPORT={'valid':False,'proof_scope':'Native sourceLOD UV/position/topology/bounds/material/LOD preservation; not render-buffer tangent proof',
        'source_fbx_modified':False,'render_tangent_buffers_verified':False,'lods':[]}
HELD=[]


def checkpoint():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    OUT.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')


def eid(cls,value):
    result=cls();result.set_editor_property('id_value',value);return result


def index(value):return int(value.get_editor_property('id_value'))


def sub(a,b):return tuple(x-y for x,y in zip(a,b))


def cross(a,b):return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])


def determinant(uv):
    a,b,c=uv
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])


def finite(values):return all(math.isfinite(x)for x in values)


def snapshot(desc):
    digest=hashlib.sha256()
    for i in range(desc.get_vertex_count()):
        v=eid(unreal.VertexID,i)
        if not desc.is_vertex_valid(v):continue
        p=desc.get_vertex_position(v)
        if not finite((p.x,p.y,p.z)):raise RuntimeError('Nonfinite source position')
        digest.update(struct.pack('<i3d',i,p.x,p.y,p.z))
    for i in range(desc.get_triangle_count()):
        t=eid(unreal.TriangleID,i)
        if not desc.is_triangle_valid(t):continue
        vis=[desc.get_triangle_vertex_instance(t,j)for j in range(3)]
        digest.update(struct.pack('<i3i',i,*(index(desc.get_vertex_instance_vertex(v))for v in vis)))
    return {'vertices':desc.get_vertex_count(),'vertex_instances':desc.get_vertex_instance_count(),
            'triangles':desc.get_triangle_count(),'geometry_sha256':digest.hexdigest()}


def plan(desc):
    bad=[];incidence={};positions={};allrows=[];minimum=math.inf
    def position(vi):
        vertex=desc.get_vertex_instance_vertex(vi);key=index(vertex)
        if key not in positions:
            p=desc.get_vertex_position(vertex);positions[key]=(float(p.x),float(p.y),float(p.z))
        return positions[key]
    for i in range(desc.get_triangle_count()):
        t=eid(unreal.TriangleID,i)
        if not desc.is_triangle_valid(t):continue
        vis=[desc.get_triangle_vertex_instance(t,j)for j in range(3)]
        uv=[desc.get_vertex_instance_uv(v,0)for v in vis]
        uv=[(float(v.x),float(v.y))for v in uv]
        if not all(finite(v)for v in uv):raise RuntimeError('Nonfinite native UV')
        uvarea=abs(determinant(uv))
        row={'triangle':i,'vis':vis,'uv':uv}
        for vi in vis:incidence.setdefault(index(vi),set()).add(i)
        if uvarea<=EPS:
            points=[position(v)for v in vis]
            normal=cross(sub(points[1],points[0]),sub(points[2],points[0]))
            if not finite(normal)or sum(x*x for x in normal)<=1.e-14:
                raise RuntimeError('UV repair refuses geometrically degenerate triangle '+str(i))
            axis=max(range(3),key=lambda a:abs(normal[a]))
            axes=[a for a in range(3)if a!=axis]
            projected=[(p[axes[0]]/TILE_CM,p[axes[1]]/TILE_CM)for p in points]
            if abs(determinant(projected))<=EPS:
                raise RuntimeError('Dominant-plane projection insufficient for triangle '+str(i))
            row['projected']=projected;bad.append(row)
        else:minimum=min(minimum,uvarea)
        allrows.append(row)
    badids={r['triangle']for r in bad}
    proposals={}
    for row in bad:
        for vi,uv in zip(row['vis'],row['projected']):
            key=index(vi)
            # Never change an already valid neighbouring face through a shared VI.
            if incidence[key]-badids:
                raise RuntimeError('Safe UV-only repair requires isolated cap corner; native VI shared with valid triangle: '+str(key))
            if key in proposals and max(abs(a-b)for a,b in zip(proposals[key][1],uv))>1.e-7:
                raise RuntimeError('Shared degenerate corner needs conflicting face projections '+str(key))
            proposals[key]=(vi,uv)
    return {'bad':bad,'proposals':proposals,'minimum_nonzero_uv_doublearea':minimum}


def bounds(mesh):
    box=mesh.get_bounding_box()
    return [box.min.x,box.min.y,box.min.z,box.max.x,box.max.y,box.max.z]


def material_paths(mesh):
    return [mesh.get_material(i).get_path_name() if mesh.get_material(i)else None
            for i in range(len(mesh.get_editor_property('static_materials')))]


def main():
    # Preflight whole selected set before changing any native UV/material/package.
    for filename in ('ENVIRONMENT_GEOMETRY_MANIFEST.json','BROADLEAF_GEOMETRY_MANIFEST.json'):
        manifest=json.loads((SOURCE/filename).read_text(encoding='utf-8'))
        for asset in manifest['assets']:
            mesh=unreal.load_asset(asset['target_package'])
            if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError('Missing native environment mesh '+asset['target_package'])
            if SM.get_lod_count(mesh)!=3:raise RuntimeError('Expected exactly3 authoredLODs '+asset['target_package'])
            item={'mesh':mesh,'path':asset['target_package'],'bounds':bounds(mesh),
                  'materials':material_paths(mesh),'screens':list(SM.get_lod_screen_sizes(mesh)),'lods':[]}
            for lod in range(3):
                desc=mesh.get_static_mesh_description(lod)
                before=snapshot(desc)
                if before['triangles']!=asset['lods'][lod]['triangles']:
                    raise RuntimeError('Native sourceLOD triangle contract mismatch '+asset['asset_name']+' lod'+str(lod))
                item['lods'].append({'desc':desc,'before':before,'plan':plan(desc),'lod':lod})
            HELD.append(item)
    REPORT['preflight_assets']=len(HELD)
    REPORT['preflight_lods']=sum(len(m['lods'])for m in HELD)
    REPORT['preflight_zero_uv_triangles']=sum(len(l['plan']['bad'])for m in HELD for l in m['lods'])
    checkpoint()
    for item in HELD:
        mesh=item['mesh'];descriptions=[];changed=False
        for row in item['lods']:
            d=row['desc'];p=row['plan'];lod=row['lod']
            for vi,uv in p['proposals'].values():d.set_vertex_instance_uv(vi,unreal.Vector2D(*uv),0)
            after=snapshot(d)
            if after!=row['before']:raise RuntimeError('UV repair changed source geometry '+item['path'])
            repaired=plan(d)
            if repaired['bad']:raise RuntimeError('Native UV readback still degenerate '+item['path'])
            build=SM.get_lod_build_settings(mesh,lod)
            # Preserve the narrowly accepted explicit source tangent bases.
            # The failed legacy fallback must not be re-applied on later authoring.
            explicit=(lod==0 and item['path'].rsplit('/',1)[-1] in
                      ('SM_Broadleaf_01','SM_Broadleaf_03') and
                      ASSETS.get_metadata_tag(mesh,'AftershockExplicitTangentsLOD0')==
                      'mikk_uv_derivative_orthogonal_v1')
            if explicit and p['bad']:
                raise RuntimeError('Explicit tangent source UV changed; rerun native repair before finalization '+item['path'])
            # Imported normals and all source positions/colors/topology stay fixed.
            for key,value in (('recompute_normals',False),('recompute_tangents',not explicit),
                              ('use_mikk_t_space',True),('use_full_precision_u_vs',True),
                              ('generate_lightmap_u_vs',False)):
                if build.get_editor_property(key)!=value:
                    changed=True;build.set_editor_property(key,value)
            SM.set_lod_build_settings(mesh,lod,build)
            changed=changed or bool(p['bad'])
            descriptions.append(d)
            REPORT['lods'].append({'asset':item['path'],'lod':lod,
                'zero_uv_triangles_before':len(p['bad']),'zero_uv_triangles_after':0,
                'repaired_vertex_instances':len(p['proposals']),
                'geometry_before':row['before'],'geometry_after_uv_edit':after,
                'min_uv_doublearea_after':repaired['minimum_nonzero_uv_doublearea'],
                'geometry_preserved':True,'vertexcolor_not_modified':True,
                'native_render_tangent_readback_required':True})
        if changed:
            mesh.build_from_static_mesh_descriptions(descriptions,False,False)
            if SM.get_lod_count(mesh)!=3:raise RuntimeError('Rebuild changed sourceLOD count '+item['path'])
            SM.set_lod_screen_sizes(mesh,item['screens'])
            for row in item['lods']:
                final=mesh.get_static_mesh_description(row['lod'])
                if snapshot(final)!=row['before']:raise RuntimeError('Build changed source geometry '+item['path'])
                if plan(final)['bad']:raise RuntimeError('Saved description UV regression '+item['path'])
            if material_paths(mesh)!=item['materials']:raise RuntimeError('Material slots changed '+item['path'])
            if any(abs(a-b)>.05 for a,b in zip(bounds(mesh),item['bounds'])):raise RuntimeError('Native bounds changed '+item['path'])
            ASSETS.set_metadata_tag(mesh,'AftershockUVRepair','dominantFacePlane200cm_v1')
            if not ASSETS.save_loaded_asset(mesh):raise RuntimeError('Native mesh save failed '+item['path'])
        checkpoint()
        unreal.log_warning('AFTERSHOCK_ENVIRONMENT_UV_REPAIR '+json.dumps({'asset':item['path'],
             'zero_before':sum(len(l['plan']['bad'])for l in item['lods']),'zero_after':0,
             'geometry_preserved':True,'render_tangent_readback_pending':True,'saved_if_changed':changed}))
    REPORT['valid']=True
    REPORT['zero_uv_triangles_after']=0
    checkpoint()
    unreal.log_warning('AFTERSHOCK_ENVIRONMENT_UV_REPAIR_COMPLETE '+json.dumps(
        {k:v for k,v in REPORT.items()if k!='lods'}))

try:main()
except Exception as exc:
    REPORT['error']=repr(exc);checkpoint();raise
