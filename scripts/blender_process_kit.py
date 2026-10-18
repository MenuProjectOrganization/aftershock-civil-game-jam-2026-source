#!/usr/bin/env python3
"""
blender_process_kit.py
Phase 2: Blender Headless Automation Pipeline for AFTERSHOCK (Civil Game Jam 2026).
Processes 3D building models according to Section #9 specifications:
- Scene unit metric with scale_length=0.01 (UE5 standard 1 unit = 1 cm)
- Imports .glb / .fbx models from Kenney Starter Kit
- Joins multi-part meshes into single object renamed to target Static Mesh name
- Scales footprint to target_footprint_cm and height to floors_min * target_height_per_floor_cm
- Normalizes pivot to base center at Z = 0
- Cleans geometry for Chaos Destruction (merge doubles, fill holes, recalculate normals)
- Extrudes/adds -150 cm Foundation Skirt below Z=0 for procedural terrain stability
- Paints Vertex Colors for M_Building_Base disaster simulation:
    R: Height gradient (0.0 at Z<=0 to 1.0 at roof Z=Z_max for sway / HeightPenalty)
    G: Structural corner/edge mask (1.0 on corners/creases, 0.0 on flat surfaces)
    B: Foundation mask (1.0 for Z < 0 foundation skirt, 0.0 for Z >= 0 superstructure)
    A: 1.0 constant
- Sets single material slot M_Building_Base and generates 2nd UV channel (UVMap_Lightmap)
- Exports FBX to _staging/fbx/{target_static_mesh_name}.fbx with Unreal-compliant settings
- Outputs verification report to _staging/fbx/export_report.json
"""

import os
import sys
import math
import json
from pathlib import Path

try:
    import bpy
    import bmesh
    from mathutils import Vector
except ImportError:
    print("Error: This script must be run inside Blender: blender --background --python scripts/blender_process_kit.py", file=sys.stderr)
    sys.exit(1)


def reset_scene():
    """Completely resets Blender scene and sets metric scale to 0.01 (1 unit = 1 cm)."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = 'METRIC'
    scene.unit_settings.scale_length = 0.01  # UE 1 unit = 1 cm


def import_model(source_path):
    """Imports GLB, glTF or FBX into the current scene."""
    ext = Path(source_path).suffix.lower()
    if ext in ('.glb', '.gltf'):
        bpy.ops.import_scene.gltf(filepath=str(source_path))
    elif ext == '.fbx':
        bpy.ops.import_scene.fbx(filepath=str(source_path))
    else:
        raise ValueError(f"Unsupported file format: {ext}")


def clean_and_join_meshes(target_name):
    """Removes non-mesh objects, joins remaining meshes, and renames active object."""
    # Remove non-mesh objects (lights, cameras, empties, armatures)
    for obj in list(bpy.context.scene.objects):
        if obj.type != 'MESH':
            bpy.data.objects.remove(obj, do_unlink=True)

    mesh_objs = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    if not mesh_objs:
        raise RuntimeError("No mesh objects found after import")

    # Select all and join
    bpy.ops.object.select_all(action='DESELECT')
    for obj in mesh_objs:
        obj.select_set(True)
    
    bpy.context.view_layer.objects.active = mesh_objs[0]
    if len(mesh_objs) > 1:
        bpy.ops.object.join()

    main_obj = bpy.context.view_layer.objects.active
    main_obj.name = target_name
    main_obj.data.name = target_name + "_Mesh"
    return main_obj


def normalize_scale_and_pivot(obj, target_x, target_y, target_z):
    """
    Scales model to target footprint (X, Y) and height (Z),
    moves base center to (0, 0, 0), and applies transforms.
    """
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    # Calculate raw bounding box
    bbox = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    min_x = min(v.x for v in bbox)
    max_x = max(v.x for v in bbox)
    min_y = min(v.y for v in bbox)
    max_y = max(v.y for v in bbox)
    min_z = min(v.z for v in bbox)
    max_z = max(v.z for v in bbox)

    raw_dx = max_x - min_x
    raw_dy = max_y - min_y
    raw_dz = max_z - min_z

    # Scale factors
    scale_x = (target_x / raw_dx) if raw_dx > 0 else 1.0
    scale_y = (target_y / raw_dy) if raw_dy > 0 else 1.0
    scale_z = (target_z / raw_dz) if raw_dz > 0 else 1.0

    obj.scale = (scale_x, scale_y, scale_z)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    # Recalculate bounds after scale
    bbox_scaled = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
    s_min_x = min(v.x for v in bbox_scaled)
    s_max_x = max(v.x for v in bbox_scaled)
    s_min_y = min(v.y for v in bbox_scaled)
    s_max_y = max(v.y for v in bbox_scaled)
    s_min_z = min(v.z for v in bbox_scaled)

    # Move base center to (0, 0, 0)
    center_x = (s_min_x + s_max_x) / 2.0
    center_y = (s_min_y + s_max_y) / 2.0
    base_z = s_min_z

    obj.location = Vector((-center_x, -center_y, -base_z))
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=False)


def prepare_geometry_for_chaos(obj):
    """
    Cleans geometry: merges duplicate vertices, fills holes,
    and recalculates outward face normals for clean Chaos Destruction fractures.
    """
    bm = bmesh.new()
    bm.from_mesh(obj.data)

    # Merge duplicate vertices (remove doubles)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.01)

    # Fill open boundary holes
    boundary_edges = [e for e in bm.edges if e.is_boundary]
    if boundary_edges:
        try:
            bmesh.ops.holes_fill(bm, edges=boundary_edges, sides=0)
        except Exception:
            pass

    # Triangulate any n-gons (faces with >4 vertices) so tangent space can be computed cleanly without warnings
    ngons = [f for f in bm.faces if len(f.verts) > 4]
    if ngons:
        bmesh.ops.triangulate(bm, faces=ngons)

    # Recalculate face normals outward
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)

    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def add_foundation_skirt(obj, skirt_depth=150.0):
    """
    Adds a closed foundation box (skirt) extending from Z = 0 down to Z = -skirt_depth (-150 cm).
    The pivot stays at Z = 0.
    """
    bm = bmesh.new()
    bm.from_mesh(obj.data)

    # Determine footprint dimensions at base
    base_verts = [v for v in bm.verts if abs(v.co.z) <= 5.0]
    if not base_verts:
        base_verts = bm.verts

    min_x = min(v.co.x for v in base_verts)
    max_x = max(v.co.x for v in base_verts)
    min_y = min(v.co.y for v in base_verts)
    max_y = max(v.co.y for v in base_verts)

    z_top = 0.0
    z_bottom = -skirt_depth

    # Create 8 skirt vertices
    v0 = bm.verts.new(Vector((min_x, min_y, z_top)))
    v1 = bm.verts.new(Vector((max_x, min_y, z_top)))
    v2 = bm.verts.new(Vector((max_x, max_y, z_top)))
    v3 = bm.verts.new(Vector((min_x, max_y, z_top)))

    v4 = bm.verts.new(Vector((min_x, min_y, z_bottom)))
    v5 = bm.verts.new(Vector((max_x, min_y, z_bottom)))
    v6 = bm.verts.new(Vector((max_x, max_y, z_bottom)))
    v7 = bm.verts.new(Vector((min_x, max_y, z_bottom)))

    # Create skirt faces (outward facing normals)
    # Bottom face: v4, v7, v6, v5
    bm.faces.new([v4, v7, v6, v5])
    # Front face: v0, v1, v5, v4
    bm.faces.new([v0, v1, v5, v4])
    # Right face: v1, v2, v6, v5
    bm.faces.new([v1, v2, v6, v5])
    # Back face: v2, v3, v7, v6
    bm.faces.new([v2, v3, v7, v6])
    # Left face: v3, v0, v4, v7
    bm.faces.new([v3, v0, v4, v7])

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)

    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def paint_simulation_vertex_colors(obj):
    """
    Paints Vertex Colors (Color Attribute 'Col') for M_Building_Base:
    - R: Height gradient (0.0 at Z<=0, 1.0 at Z=Z_max) for wind/earthquake sway
    - G: Structural corner/edge mask (1.0 on feature edges, 0.0 on flat faces) for cracks
    - B: Foundation mask (1.0 for Z < 0 skirt, 0.0 for Z >= 0 superstructure) for flood & foundation quality
    - A: 1.0 constant
    """
    mesh = obj.data

    # Find Z bounds
    z_coords = [v.co.z for v in mesh.vertices]
    z_max = max(z_coords) if z_coords else 300.0

    # Calculate sharp/feature edges for G channel mask
    sharp_vert_indices = set()
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.edges.ensure_lookup_table()
    for edge in bm.edges:
        if edge.is_boundary:
            sharp_vert_indices.add(edge.verts[0].index)
            sharp_vert_indices.add(edge.verts[1].index)
        elif len(edge.link_faces) == 2:
            angle = edge.calc_face_angle(0.0)
            if angle > math.radians(30.0):
                sharp_vert_indices.add(edge.verts[0].index)
                sharp_vert_indices.add(edge.verts[1].index)
    bm.free()

    # Precalculate per-vertex colors
    vert_colors = []
    for idx, v in enumerate(mesh.vertices):
        z = v.co.z
        
        # R Channel: Height gradient (0.0 at Z<=0, 1.0 at Z_max)
        if z <= 0.0 or z_max <= 0.0:
            r = 0.0
        else:
            r = min(1.0, max(0.0, z / z_max))

        # G Channel: Structural corner / feature edge mask
        g = 1.0 if idx in sharp_vert_indices else 0.0

        # B Channel: Foundation skirt mask (Z < -0.1 -> 1.0, else 0.0)
        b = 1.0 if z < -0.1 else 0.0

        # A Channel: 1.0
        a = 1.0
        vert_colors.append((r, g, b, a))

    # Add Color Attribute
    # Remove existing color attributes
    while mesh.color_attributes:
        mesh.color_attributes.remove(mesh.color_attributes[0])

    # In modern Blender, create BYTE_COLOR or FLOAT_COLOR with domain 'CORNER'
    color_layer = mesh.color_attributes.new(name="Col", type='BYTE_COLOR', domain='CORNER')

    # Assign colors to loops
    for loop_idx, loop in enumerate(mesh.loops):
        v_idx = loop.vertex_index
        color_layer.data[loop_idx].color = vert_colors[v_idx]

    mesh.update()


def setup_materials_and_uvs(obj):
    """
    Cleans multiple material slots, assigns single M_Building_Base slot,
    and generates 2nd UV channel (UVMap_Lightmap) via Smart UV Project.
    """
    mesh = obj.data

    # 1. Single material M_Building_Base
    mat_name = "M_Building_Base"
    mat = bpy.data.materials.get(mat_name)
    if not mat:
        mat = bpy.data.materials.new(name=mat_name)
        mat.use_nodes = True

    mesh.materials.clear()
    mesh.materials.append(mat)

    # 2. UV Channels
    if len(mesh.uv_layers) == 0:
        mesh.uv_layers.new(name="UVMap")

    # Generate 2nd UV channel for Lightmap / Damage Projection
    if len(mesh.uv_layers) < 2:
        uv2 = mesh.uv_layers.new(name="UVMap_Lightmap")
        mesh.uv_layers.active = uv2

        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(angle_limit=66.0, island_margin=0.02)
        bpy.ops.object.mode_set(mode='OBJECT')

        # Reset active UV to first channel
        mesh.uv_layers.active = mesh.uv_layers[0]


def create_simple_box_collision(target_name, min_x, max_x, min_y, max_y, min_z, max_z):
    """
    Creates an optimized Simple Box Collision mesh named 'UBX_{target_name}_01'.
    Unreal Engine's FBX importer automatically detects the 'UBX_' prefix and converts
    this box into a native simple box collision shape (FBox in BodySetup), completely
    avoiding expensive complex mesh collision.
    """
    collision_mesh = bpy.data.meshes.new(f"UBX_{target_name}_01_Mesh")
    collision_obj = bpy.data.objects.new(f"UBX_{target_name}_01", collision_mesh)
    bpy.context.scene.collection.objects.link(collision_obj)

    bm = bmesh.new()
    # 8 vertices for the bounding box spanning from min_z (0.0 ground) to max_z (roof)
    v0 = bm.verts.new(Vector((min_x, min_y, min_z)))
    v1 = bm.verts.new(Vector((max_x, min_y, min_z)))
    v2 = bm.verts.new(Vector((max_x, max_y, min_z)))
    v3 = bm.verts.new(Vector((min_x, max_y, min_z)))

    v4 = bm.verts.new(Vector((min_x, min_y, max_z)))
    v5 = bm.verts.new(Vector((max_x, min_y, max_z)))
    v6 = bm.verts.new(Vector((max_x, max_y, max_z)))
    v7 = bm.verts.new(Vector((min_x, max_y, max_z)))

    # 6 quad faces with outward-facing normals
    bm.faces.new([v0, v3, v2, v1])  # Bottom
    bm.faces.new([v4, v5, v6, v7])  # Top
    bm.faces.new([v0, v1, v5, v4])  # Front
    bm.faces.new([v1, v2, v6, v5])  # Right
    bm.faces.new([v2, v3, v7, v6])  # Back
    bm.faces.new([v3, v0, v4, v7])  # Left

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(collision_mesh)
    bm.free()
    collision_mesh.update()

    return collision_obj


def export_ue5_fbx(objects_to_export, output_path):
    """Exports model(s) to FBX using Unreal Engine standard settings."""
    if not isinstance(objects_to_export, (list, tuple)):
        objects_to_export = [objects_to_export]

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects_to_export:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects_to_export[0]

    bpy.ops.export_scene.fbx(
        filepath=str(output_path),
        use_selection=True,
        global_scale=1.0,
        apply_unit_scale=True,
        apply_scale_options='FBX_SCALE_NONE',
        axis_forward='-Y',
        axis_up='Z',
        mesh_smooth_type='FACE',
        use_tspace=True,
        colors_type='SRGB',
    )


def process_building(entry, repo_root, staging_fbx_dir, content_meshes_dir):
    """Processes a single building asset entry from building_asset_map.json."""
    building_type = entry["building_type"]
    target_mesh_name = entry["target_static_mesh_name"]
    source_model_rel = entry["source_model_path"]
    source_model_path = repo_root / source_model_rel

    if not source_model_path.exists():
        raise FileNotFoundError(f"Source model not found: {source_model_path}")

    # Footprint and height targets in cm
    footprint = entry.get("target_footprint_cm", {"x": 400.0, "y": 400.0})
    target_x = float(footprint.get("x", 400.0))
    target_y = float(footprint.get("y", 400.0))
    floors_min = int(entry.get("floors_min", 1))
    height_per_floor = float(entry.get("target_height_per_floor_cm", 300.0))
    target_z = floors_min * height_per_floor

    print(f"\n>>> Processing [{building_type}] -> {target_mesh_name}")
    print(f"    Source: {source_model_path}")
    print(f"    Target Dims: {target_x:.1f} x {target_y:.1f} x {target_z:.1f} cm (Skirt: -150 cm)")

    # a. Reset scene & set metric scale
    reset_scene()

    # b. Import model
    import_model(source_model_path)

    # c. Delete non-mesh & join
    main_obj = clean_and_join_meshes(target_mesh_name)

    # d. Normalize scale and pivot
    normalize_scale_and_pivot(main_obj, target_x, target_y, target_z)

    # e. Chaos geometry cleanup
    prepare_geometry_for_chaos(main_obj)

    # f. Foundation Skirt (-150 cm)
    add_foundation_skirt(main_obj, skirt_depth=150.0)

    # g. Paint Vertex Colors
    paint_simulation_vertex_colors(main_obj)

    # h. Material and UV setup
    setup_materials_and_uvs(main_obj)

    # Final mesh metrics
    bbox = [main_obj.matrix_world @ Vector(c) for c in main_obj.bound_box]
    final_dims = {
        "x": round(max(v.x for v in bbox) - min(v.x for v in bbox), 2),
        "y": round(max(v.y for v in bbox) - min(v.y for v in bbox), 2),
        "z": round(max(v.z for v in bbox) - min(v.z for v in bbox), 2),
    }
    z_min = round(min(v.z for v in bbox), 2)
    z_max = round(max(v.z for v in bbox), 2)
    vertex_count = len(main_obj.data.vertices)
    poly_count = len(main_obj.data.polygons)
    has_vcol = len(main_obj.data.color_attributes) > 0
    uv_channels = len(main_obj.data.uv_layers)

    # i. Create Simple Box Collision (UBX_{target_mesh_name}_01)
    # The collision box spans the above-ground superstructure: Z=0 to Z=z_max
    super_verts = [v for v in main_obj.data.vertices if v.co.z >= -0.1]
    if super_verts:
        c_min_x = min(v.co.x for v in super_verts)
        c_max_x = max(v.co.x for v in super_verts)
        c_min_y = min(v.co.y for v in super_verts)
        c_max_y = max(v.co.y for v in super_verts)
    else:
        c_min_x = min(v.x for v in bbox)
        c_max_x = max(v.x for v in bbox)
        c_min_y = min(v.y for v in bbox)
        c_max_y = max(v.y for v in bbox)

    collision_obj = create_simple_box_collision(
        target_name=target_mesh_name,
        min_x=c_min_x,
        max_x=c_max_x,
        min_y=c_min_y,
        max_y=c_max_y,
        min_z=0.0,
        max_z=z_max
    )

    # j. FBX Export to BOTH _staging/fbx/ and Content/Buildings/Meshes/
    output_fbx_staging = staging_fbx_dir / f"{target_mesh_name}.fbx"
    output_fbx_content = content_meshes_dir / f"{target_mesh_name}.fbx"

    export_ue5_fbx([main_obj, collision_obj], output_fbx_staging)
    export_ue5_fbx([main_obj, collision_obj], output_fbx_content)

    report_entry = {
        "building_type": building_type,
        "display_name": entry.get("display_name", ""),
        "target_static_mesh_name": target_mesh_name,
        "fbx_file_path": str(output_fbx_content.relative_to(repo_root)).replace("\\", "/"),
        "staging_fbx_path": str(output_fbx_staging.relative_to(repo_root)).replace("\\", "/"),
        "file_size_bytes": output_fbx_content.stat().st_size if output_fbx_content.exists() else 0,
        "dimensions_cm": final_dims,
        "z_min_cm": z_min,
        "z_max_cm": z_max,
        "foundation_skirt_ok": (z_min <= -149.0),
        "vertex_count": vertex_count,
        "triangle_count": poly_count,
        "has_vertex_colors": has_vcol,
        "uv_channels": uv_channels,
        "material_slot": "M_Building_Base",
        "collision_type": "Simple Box Collision",
        "collision_mesh_name": f"UBX_{target_mesh_name}_01",
        "nanite_policy": "Disabled (Sub-1000 LowPoly Optimization)",
        "lod_chain": {
            "LOD0": {"reduction": "100%", "tris": poly_count, "screen_size": 1.0},
            "LOD1": {"reduction": "60%", "tris": int(poly_count * 0.6), "screen_size": 0.3},
            "LOD2": {"reduction": "30%", "tris": int(poly_count * 0.3), "screen_size": 0.1}
        }
    }
    print(f"    Exported: {output_fbx_content.name} ({report_entry['file_size_bytes']} bytes)")
    print(f"    Bounds: Z_min={z_min} cm, Z_max={z_max} cm, Verts={vertex_count}, Polys={poly_count}, VCol={has_vcol}")
    print(f"    Collision: UBX_{target_mesh_name}_01 (Simple Box Collision)")
    return report_entry


def main():
    repo_root = Path(__file__).resolve().parent.parent
    map_json_path = repo_root / "scripts" / "building_asset_map.json"
    staging_fbx_dir = repo_root / "_staging" / "fbx"
    staging_fbx_dir.mkdir(parents=True, exist_ok=True)
    content_meshes_dir = repo_root / "Content" / "Buildings" / "Meshes"
    content_meshes_dir.mkdir(parents=True, exist_ok=True)

    if not map_json_path.exists():
        print(f"Error: Asset map not found: {map_json_path}", file=sys.stderr)
        sys.exit(1)

    with open(map_json_path, "r", encoding="utf-8") as f:
        entries = json.load(f)

    print(f"Loaded {len(entries)} building mapping entries.")
    results = []

    for entry in entries:
        try:
            report = process_building(entry, repo_root, staging_fbx_dir, content_meshes_dir)
            results.append(report)
        except Exception as e:
            print(f"[ERROR] Failed processing {entry.get('building_type')}: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()

    # Save export report to both locations
    report_path_staging = staging_fbx_dir / "export_report.json"
    with open(report_path_staging, "w", encoding="utf-8") as out:
        json.dump(results, out, indent=2, ensure_ascii=False)

    report_path_content = content_meshes_dir.parent / "asset_pipeline_report.json"
    with open(report_path_content, "w", encoding="utf-8") as out:
        json.dump(results, out, indent=2, ensure_ascii=False)

    print("\n" + "=" * 90)
    print(f"Successfully processed {len(results)} of {len(entries)} building models.")
    print(f"Export report saved to:")
    print(f"  Staging: {report_path_staging}")
    print(f"  Content: {report_path_content}")
    print("=" * 90)


if __name__ == "__main__":
    main()
