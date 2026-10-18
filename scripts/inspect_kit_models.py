#!/usr/bin/env python3
"""
inspect_kit_models.py
Analyzes all 3D models (.glb, .gltf, .fbx) in the Kenney Starter Kit models directory.
Extracts raw bounding boxes (min/max X, Y, Z), dimensions, vertex count, and polygon (triangle) count.
Uses pure standard library Python (struct, json) to parse GLB/glTF headers directly without third-party dependencies.
"""

import os
import sys
import json
import struct
from pathlib import Path


def parse_glb(file_path):
    """
    Parses a binary glTF (.glb) file and extracts:
    - Bounding Box (min_x, min_y, min_z, max_x, max_y, max_z)
    - Dimensions (dx, dy, dz)
    - Total vertices
    - Total triangles (polygons)
    - Node names / mesh names
    """
    with open(file_path, "rb") as f:
        header = f.read(12)
        if len(header) < 12:
            return None, "File too small"
        
        magic, version, length = struct.unpack("<4sII", header)
        if magic != b"glTF":
            return None, f"Not a valid glTF binary (magic: {magic})"

        # Chunk 0 should be JSON
        chunk_header = f.read(8)
        if len(chunk_header) < 8:
            return None, "Missing chunk header"
        chunk_len, chunk_type = struct.unpack("<I4s", chunk_header)
        if chunk_type != b"JSON":
            return None, f"Expected JSON chunk, got {chunk_type}"

        json_bytes = f.read(chunk_len)
        try:
            gltf = json.loads(json_bytes.decode("utf-8"))
        except Exception as e:
            return None, f"Failed to parse JSON chunk: {e}"

    accessors = gltf.get("accessors", [])
    meshes = gltf.get("meshes", [])
    nodes = gltf.get("nodes", [])

    total_vertices = 0
    total_triangles = 0

    global_min = [float("inf"), float("inf"), float("inf")]
    global_max = [float("-inf"), float("-inf"), float("-inf")]

    mesh_info = []

    for mesh in meshes:
        mesh_name = mesh.get("name", "Unnamed")
        for prim in mesh.get("primitives", []):
            attrs = prim.get("attributes", {})
            pos_acc_idx = attrs.get("POSITION")
            if pos_acc_idx is not None and pos_acc_idx < len(accessors):
                pos_acc = accessors[pos_acc_idx]
                count = pos_acc.get("count", 0)
                total_vertices += count
                
                acc_min = pos_acc.get("min")
                acc_max = pos_acc.get("max")
                if acc_min and acc_max:
                    for i in range(3):
                        if acc_min[i] < global_min[i]:
                            global_min[i] = acc_min[i]
                        if acc_max[i] > global_max[i]:
                            global_max[i] = acc_max[i]

            idx_acc_idx = prim.get("indices")
            if idx_acc_idx is not None and idx_acc_idx < len(accessors):
                idx_acc = accessors[idx_acc_idx]
                idx_count = idx_acc.get("count", 0)
                # Mode 4 is default TRIANGLES
                mode = prim.get("mode", 4)
                if mode == 4:
                    total_triangles += idx_count // 3
                else:
                    total_triangles += idx_count // 3
            elif pos_acc_idx is not None and pos_acc_idx < len(accessors):
                # Non-indexed geometry
                pos_acc = accessors[pos_acc_idx]
                total_triangles += pos_acc.get("count", 0) // 3

        mesh_info.append(mesh_name)

    if global_min[0] == float("inf"):
        dims = [0.0, 0.0, 0.0]
    else:
        dims = [
            round(global_max[0] - global_min[0], 4),
            round(global_max[1] - global_min[1], 4),
            round(global_max[2] - global_min[2], 4),
        ]
        global_min = [round(v, 4) for v in global_min]
        global_max = [round(v, 4) for v in global_max]

    return {
        "file_name": os.path.basename(file_path),
        "file_path": os.path.normpath(file_path).replace("\\", "/"),
        "min_bound": global_min,
        "max_bound": global_max,
        "raw_dimensions": {"x": dims[0], "y": dims[1], "z": dims[2]},
        "vertices": total_vertices,
        "triangles": total_triangles,
        "mesh_names": mesh_info,
        "node_count": len(nodes),
    }, None


def scan_directory(models_dir):
    p = Path(models_dir)
    if not p.exists():
        print(f"Error: Directory not found: {models_dir}", file=sys.stderr)
        return []

    results = []
    # Find all .glb, .gltf, .fbx
    files = sorted(list(p.rglob("*.glb")) + list(p.rglob("*.fbx")) + list(p.rglob("*.gltf")))
    
    print(f"Found {len(files)} model file(s) in {models_dir}\n")

    for f in files:
        ext = f.suffix.lower()
        if ext == ".glb":
            data, err = parse_glb(str(f))
            if err:
                print(f"[WARN] {f.name}: {err}")
            else:
                results.append(data)
        elif ext == ".fbx":
            results.append({
                "file_name": f.name,
                "file_path": str(f).replace("\\", "/"),
                "format": "FBX",
                "note": "Requires FBX SDK or Blender to parse bounds",
            })
        elif ext == ".gltf":
            # Can be added if any
            pass

    return results


def main():
    repo_root = Path(__file__).resolve().parent.parent
    default_models_dir = repo_root / "_refs" / "Starter-Kit-City-Builder" / "models"
    
    models_dir = sys.argv[1] if len(sys.argv) > 1 else str(default_models_dir)
    print(f"Scanning models directory: {models_dir}")

    results = scan_directory(models_dir)

    print("=" * 110)
    print(f"{'Model Name':<28} | {'Raw Dims (X, Y, Z)':<24} | {'Min Bound (X,Y,Z)':<22} | {'Tris':<7} | {'Verts':<7}")
    print("-" * 110)

    for r in results:
        if "raw_dimensions" in r:
            dims = f"{r['raw_dimensions']['x']:.2f} x {r['raw_dimensions']['y']:.2f} x {r['raw_dimensions']['z']:.2f}"
            min_b = f"[{r['min_bound'][0]:.1f}, {r['min_bound'][1]:.1f}, {r['min_bound'][2]:.1f}]"
            print(f"{r['file_name']:<28} | {dims:<24} | {min_b:<22} | {r['triangles']:<7} | {r['vertices']:<7}")
        else:
            print(f"{r['file_name']:<28} | {r.get('note', 'N/A')}")
    print("=" * 110)

    # Save detailed inspection report as JSON in scripts/inspection_report.json
    output_json = repo_root / "scripts" / "inspection_report.json"
    with open(output_json, "w", encoding="utf-8") as out:
        json.dump(results, out, indent=2, ensure_ascii=False)
    print(f"\nSaved detailed analysis report to: {output_json}")


if __name__ == "__main__":
    main()
