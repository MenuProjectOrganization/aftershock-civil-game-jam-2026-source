#!/usr/bin/env python3
"""
inspect_fbx_geometry.py
Directly imports and measures raw geometry from FBX files in Blender:
- Compares Content/Buildings/Meshes/ vs _staging/fbx/
- Measures exact bounds (X, Y, Z), pivot, skirt depth (Z_min), superstructure height (Z_max)
- Inspects UBX_ simple collision box bounds
- Audits vertex counts, triangle counts, material slots, UV layers, and vertex colors
"""

import sys
import json
from pathlib import Path

try:
    import bpy
    from mathutils import Vector
except ImportError:
    print("Error: Must run inside Blender", file=sys.stderr)
    sys.exit(1)

def measure_mesh_bounds(obj):
    bbox = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    min_x = min(v.x for v in bbox)
    max_x = max(v.x for v in bbox)
    min_y = min(v.y for v in bbox)
    max_y = max(v.y for v in bbox)
    min_z = min(v.z for v in bbox)
    max_z = max(v.z for v in bbox)
    return {
        "min": [round(min_x, 2), round(min_y, 2), round(min_z, 2)],
        "max": [round(max_x, 2), round(max_y, 2), round(max_z, 2)],
        "extent": [round(max_x - min_x, 2), round(max_y - min_y, 2), round(max_z - min_z, 2)],
        "superstructure_height_cm": round(max_z, 2),
        "skirt_depth_cm": round(-min_z, 2)
    }

def inspect_fbx(fbx_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.unit_settings.system = 'METRIC'
    bpy.context.scene.unit_settings.scale_length = 0.01

    bpy.ops.import_scene.fbx(filepath=str(fbx_path))
    
    mesh_objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    
    render_mesh = None
    collision_mesh = None
    
    for o in mesh_objs:
        if o.name.startswith("UBX_"):
            collision_mesh = o
        else:
            render_mesh = o
            
    info = {
        "file_name": fbx_path.name,
        "file_size": fbx_path.stat().st_size,
        "objects_found": [o.name for o in mesh_objs],
    }
    
    if render_mesh:
        mesh = render_mesh.data
        bounds = measure_mesh_bounds(render_mesh)
        vcol_names = [c.name for c in mesh.color_attributes]
        uv_names = [uv.name for uv in mesh.uv_layers]
        mat_slots = [s.name for s in render_mesh.material_slots]
        
        info["render_mesh"] = {
            "name": render_mesh.name,
            "vertex_count": len(mesh.vertices),
            "triangle_count": len(mesh.polygons),
            "bounds": bounds,
            "pivot": [round(render_mesh.location.x, 2), round(render_mesh.location.y, 2), round(render_mesh.location.z, 2)],
            "color_attributes": vcol_names,
            "uv_layers": uv_names,
            "materials": mat_slots
        }
        
    if collision_mesh:
        col_mesh = collision_mesh.data
        col_bounds = measure_mesh_bounds(collision_mesh)
        info["collision_mesh"] = {
            "name": collision_mesh.name,
            "vertex_count": len(col_mesh.vertices),
            "face_count": len(col_mesh.polygons),
            "bounds": col_bounds
        }
        
    return info

def main():
    repo_root = Path(__file__).resolve().parent.parent
    content_dir = repo_root / "Content" / "Buildings" / "Meshes"
    staging_dir = repo_root / "_staging" / "fbx"
    
    all_fbxs = [
        "SM_House_01.fbx",
        "SM_Apartment_01.fbx",
        "SM_Office_01.fbx",
        "SM_Factory_01.fbx",
        "SM_Hospital_01.fbx",
        "SM_School_01.fbx",
        "SM_FireStation_01.fbx",
        "SM_PowerPlant_01.fbx",
        "SM_GreenSpace_01.fbx"
    ]
    
    results = {}
    print("=" * 80)
    print("FBX GEOMETRY AND COLLISION INSPECTION PASS")
    print("=" * 80)
    
    for fbx_name in all_fbxs:
        c_path = content_dir / fbx_name
        s_path = staging_dir / fbx_name
        
        c_info = inspect_fbx(c_path) if c_path.exists() else None
        s_info = inspect_fbx(s_path) if s_path.exists() else None
        
        # Verify geometry match
        geom_match = False
        if c_info and s_info and "render_mesh" in c_info and "render_mesh" in s_info:
            r_c = c_info["render_mesh"]
            r_s = s_info["render_mesh"]
            geom_match = (
                r_c["vertex_count"] == r_s["vertex_count"] and
                r_c["triangle_count"] == r_s["triangle_count"] and
                r_c["bounds"] == r_s["bounds"] and
                r_c["color_attributes"] == r_s["color_attributes"] and
                r_c["uv_layers"] == r_s["uv_layers"]
            )
            
        results[fbx_name] = {
            "canonical_repo_relative_source": f"Content/Buildings/Meshes/{fbx_name}",
            "staging_source": f"_staging/fbx/{fbx_name}",
            "geometry_match_between_locations": geom_match,
            "measured_data": c_info
        }
        
        rm = c_info.get("render_mesh", {})
        cm = c_info.get("collision_mesh", {})
        b = rm.get("bounds", {})
        print(f"\n[{fbx_name}]")
        print(f"  Canonical Source: Content/Buildings/Meshes/{fbx_name}")
        print(f"  Geom Match with Staging: {geom_match}")
        print(f"  Render Mesh: {rm.get('name')} | Verts={rm.get('vertex_count')} | Tris={rm.get('triangle_count')}")
        print(f"  Footprint Extent (X x Y x Z): {b.get('extent', [])} cm")
        print(f"  Superstructure Height: {b.get('superstructure_height_cm')} cm (Z_max)")
        print(f"  Foundation Skirt Depth: -{b.get('skirt_depth_cm')} cm (Z_min = {b.get('min', [0,0,0])[2]} cm)")
        print(f"  Collision Shape: {cm.get('name')} | Extent={cm.get('bounds', {}).get('extent', [])} cm")
        print(f"  Material Slot: {rm.get('materials')} | UVs: {rm.get('uv_layers')} | VCol: {rm.get('color_attributes')}")

    out_file = repo_root / "scripts" / "measured_fbx_inspection.json"
    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)
        
    print(f"\nInspection saved to: {out_file}")

if __name__ == "__main__":
    main()
