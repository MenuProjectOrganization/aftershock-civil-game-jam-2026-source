"""CPU-only provenance receipt for the accepted two modules and sixteen bakes.

Does not run Unreal, copy source assets, modify geometry or assert live quality.
"""
from pathlib import Path
import argparse, hashlib, json, math
from datetime import datetime

REPO = next(p for p in Path(__file__).resolve().parents if (p / "Source/JamProject").is_dir())
SOURCE_MANIFEST = REPO / "_Imports/Epic/CitySample/Architecture/MODULE_CANDIDATES.json"
OUT = SOURCE_MANIFEST.with_name("DERIVATION_MANIFEST.json")

def require(ok, text):
    if not ok:
        raise RuntimeError(text)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read_report(root, name):
    path = root / name
    obj = json.loads(path.read_text(encoding="utf-8-sig"))
    require(obj.get("valid") is True and obj.get("in_progress") is False and obj.get("run_id"), "Rejected or incomplete native receipt: " + str(path))
    return obj, dict(path=path.relative_to(REPO).as_posix(), sha256=digest(path), run_id=obj["run_id"])

def asset_record(package):
    require(package.startswith("/Game/Aftershock/CitySample/Architecture/"), "Outside bounded architecture namespace")
    base = REPO / "Content" / (package[6:] + ".uasset")
    require(base.is_file(), "Actual native package missing: " + package)
    files = [base.with_suffix(s) for s in (".uasset", ".uexp", ".ubulk", ".uptnl") if base.with_suffix(s).is_file()]
    return dict(package=package, files=[dict(path=p.relative_to(REPO).as_posix(), bytes=p.stat().st_size, sha256=digest(p)) for p in files],
                package_bytes=sum(p.stat().st_size for p in files))

def receipt_file(path):
    path = path.resolve()
    require(path.is_relative_to(REPO / "Saved"), "Repair/frame/backup receipt must remain under project Saved")
    obj = json.loads(path.read_text(encoding="utf-8-sig"))
    require(obj.get("valid") is True and obj.get("in_progress") is False and obj.get("run_id"), "Rejected/incomplete receipt: " + str(path))
    return obj, dict(path=path.relative_to(REPO).as_posix(), sha256=digest(path), run_id=obj["run_id"])

def utc_stamp(text):
    value = datetime.fromisoformat(text.replace("Z", "+00:00"))
    require(value.tzinfo is not None, "Native receipt requires an explicit UTC time")
    return value.timestamp()

def validate_repair_chain(args, bake, expected, modules, sources):
    """Accept only this explicit geometry-preserving repair, never a hash bypass."""
    require(args.frame_receipt and args.repair_backup, "--repair-receipt requires --frame-receipt and --repair-backup")
    repair, rr = receipt_file(args.repair_receipt)
    frames, fr = receipt_file(args.frame_receipt)
    require(repair.get("policy") == "metric_uv_seam_mikk_v1" and repair.get("preflight_meshes") == 16
            and repair.get("expected_meshes") == 16 and repair.get("expected_render_lods") == 48 and repair.get("error") == "", "Unsupported/failed native repair policy")
    repaired = repair.get("results", [])
    require(len(repaired) == 16 and {r.get("asset") for r in repaired} == expected, "Exact16 owned repair assets required")
    repaired_by_package = {r["asset"]: r for r in repaired}
    require(len(repaired_by_package) == 16, "Duplicate repair asset")
    backup_path = args.repair_backup.resolve()
    require(backup_path.is_relative_to(REPO / "Saved"), "Backup receipt must remain under Saved")
    backup = json.loads(backup_path.read_text(encoding="utf-8-sig"))
    items = backup.get("assets", [])
    all_packages = expected | {m["package"] for m in modules}
    wanted_paths = {"Content/" + p[6:] + ".uasset" for p in all_packages}
    require(len(items) == 18 and {r.get("asset") for r in items} == wanted_paths, "Exact original18package pre-repair backup required")
    backed = {r["asset"]: r for r in items}
    require(len(backed) == 18, "Duplicate backup asset")
    for row in items:
        p = Path(row["backup"]).resolve()
        require(p.is_relative_to(backup_path.parent) and p.is_file(), "Preserve only named bounded backup files")
        require(p.stat().st_size == row["bytes"] and digest(p) == row["sha256"], "Backup byte/hash drift")
    require(sum(r["bytes"] for r in items) == backup["total_bytes"], "Backup aggregate byte drift")
    # Original two geometry derivatives must remain byte-identical to the
    # pre-repair bound inputs; the owned repair was only allowed on16bakes.
    for row in modules:
        require(len(row["files"]) == 1, "Unexpected module sidecars need explicit provenance extension")
        f = row["files"][0]; old = backed[f["path"]]
        require(f["bytes"] == old["bytes"] and f["sha256"] == old["sha256"], "Bound source module changed during repair")
    native_rows = frames.get("results", [])
    keys = [(r.get("asset", "").split(".", 1)[0], r.get("lod")) for r in native_rows]
    require(len(keys) == 48 and len(set(keys)) == 48 and set(keys) == {(p,l) for p in expected for l in range(3)}, "Exact16asset48LOD fresh frame records required")
    require(frames.get("meshes") == 16 and frames.get("lods") == 48 and frames.get("missing") == [], "Fresh frame aggregate incomplete")
    by_lod = dict(zip(keys, native_rows))
    bad = ("nonfinite_frames_or_positions", "zero_frames", "parallel_frames", "nonorthogonal_frames", "invalid_handedness")
    require(all(r.get("valid") is True and r.get("cpu_render_data_available") is True and r.get("retained_native_glass_section") is True
                and r.get("vertices", 0) > 0 and all(r.get(k) == 0 for k in bad) for r in native_rows), "Fresh actual frame/glass gate failed")
    require(all(r.get("source_lod0_description_present") is True and r.get("source_lod0_zero_uv_triangles") == 0
                and r.get("source_lod0_nonfinite_uv_triangles") == 0 and r.get("source_lod0_geometrically_degenerate_triangles") == 0
                for r in native_rows if r["lod"] == 0), "LOD0 UV/source geometry gate failed")
    started = utc_stamp(frames["started_utc"])
    require(started >= utc_stamp(repair["completed_utc"]) >= utc_stamp(repair["started_utc"]), "Frame process must start after repair completes")
    chain_rows = []
    for baked in bake["meshes"]:
        p = baked["package"]; fixed = repaired_by_package[p]
        native = asset_record(p)
        require(len(native["files"]) == 1, "Unexpected repaired sidecars need explicit provenance extension")
        current = REPO / native["files"][0]["path"]; original = backed[native["files"][0]["path"]]
        require(baked.get("sha256") == original["sha256"] and baked["package_bytes"] == original["bytes"], "Original R4 native bake no longer matches original backup")
        protected = ("protected_geometry_normal_color_other_uv_materials", "rebuilt_source_preserved", "rebuilt48_policy_frames_valid", "bounds_lod0_triangle_count_preserved", "saved")
        require(all(fixed.get(k) is True for k in protected) and fixed.get("rebuilt_invalid_frames") == 0, "Repair source/bounds/frame preservation failed")
        require(fixed.get("invalid_uv_polygons") == 0 and fixed.get("new_projection_seam_corners") == 0, "This receipt chain only accepts the actual UV-preserving tangent repair")
        require(len(fixed.get("protected_corner_sha1", "")) == 40, "Missing protected-corner fingerprint")
        require(hashlib.sha1(current.read_bytes()).hexdigest().lower() == fixed.get("saved_package_sha1", "").lower(), "Actual repaired native bytes do not match saved repair SHA1")
        require(current.stat().st_mtime <= started + 1.e-3, "Native package changed after fresh frame audit began")
        tris = baked["triangles_per_lod"]
        require(fixed.get("triangles_per_lod") == tris and [by_lod[p,l]["triangles"] for l in range(3)] == tris, "Original bake / repair / fresh native LOD counts differ")
        require(all(0 < t <= cap for t,cap in zip(tris,(50000,18000,6000))) and len(tris) == 3, "Classic geometry cap violated")
        chain_rows.append(dict(package=p, original_bake_sha256=original["sha256"], current_sha256=native["files"][0]["sha256"],
            repaired_saved_package_sha1=fixed["saved_package_sha1"], protected_corner_sha1=fixed["protected_corner_sha1"],
            original_native_bounds_preserved=True, triangles_per_lod=tris, actual_fresh_render_frames=True))
    # Read only the two allowlisted original geometry packages, not the Vault.
    source_content = args.source_content.resolve()
    for source in sources:
        for suffix, checksum in source["source_hashes"].items():
            require(suffix in (".uasset", ".uexp", ".ubulk", ".uptnl"), "Unexpected original source sidecar")
            file = (source_content / source["source_package"][6:]).with_suffix(suffix)
            require(file.is_file() and digest(file) == checksum, "Allowlisted original Vault geometry changed/missing")
    rr.update(backup_manifest=dict(path=backup_path.relative_to(REPO).as_posix(), sha256=digest(backup_path)),
        original_bake_receipt_preserved=True, no_new_bake_process_claimed=True, no_original_geometry_or_bound_module_changes=True,
        rows=chain_rows, fresh_frame_receipt=fr)
    return rr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipts", type=Path, default=REPO / "Saved/PresentationValidation/CitySampleArchitecture")
    parser.add_argument("--repair-receipt", type=Path, default=None)
    parser.add_argument("--frame-receipt", type=Path, default=None)
    parser.add_argument("--repair-backup", type=Path, default=None)
    parser.add_argument("--source-content", type=Path, default=Path("C:/ProgramData/Epic/EpicGamesLauncher/VaultCache/CitySample_5.8/data/Content"))
    parser.add_argument("--output", type=Path, default=OUT)
    args = parser.parse_args()
    output = args.output.resolve()
    require(output == OUT or output.is_relative_to(REPO / "Saved"), "Output must be canonical manifest or a Saved preview")
    require(not args.frame_receipt or args.repair_receipt, "Frame receipt requires an explicit repair chain in this writer")
    root = args.receipts.resolve()
    require(root.is_relative_to(REPO / "Saved"), "Receipts must be inside project Saved")
    manifest = json.loads(SOURCE_MANIFEST.read_text(encoding="utf-8"))
    extract, er = read_report(root, "native_modules_extract.json")
    bind, br = read_report(root, "native_modules_bind.json")
    bake, kr = read_report(root, "native_facade_bake.json")
    require(extract.get("phase") == "EXTRACT" and extract.get("source_assets_unchanged") is True, "Original source unchanged proof missing")
    require(bind.get("phase") == "BIND" and len(bind.get("bound_modules", [])) == 2, "Exactly two accepted module bindings required")
    approved = {r["key"]: r for r in manifest["candidates"] if r["allow_extract"]}
    rows = [r for r in extract["candidates"] if "extraction" in r]
    require({r["key"] for r in rows} == set(approved) and len(rows) == 2, "Extraction allowlist drift")
    sources, modules = [], []
    for row in rows:
        candidate = approved[row["key"]]
        require(row["source_package"] == candidate["source_package"] and row["source_hashes"][".uasset"] == candidate["source_sha256"], "Source provenance mismatch")
        sources.append(dict(source_package=row["source_package"], source_hashes=row["source_hashes"], native_bounds=row["native_bbox"],
                            semantic_slots=row["semantic_slots"], attribution="City Sample, Epic Games / applicable licensors; adapted for AFTERSHOCK"))
        modules.append(asset_record(row["extraction"]["destination_package"]))
    require(sum(r["package_bytes"] for r in modules) <= 16 * 1024 * 1024, "Two module derivative budget exceeded")
    expected = {f"/Game/Aftershock/CitySample/Architecture/Baked/SM_CS_Facade_B{b}_F{f:02d}" for f in range(1, 5) for b in range(1, 5)}
    require(len(bake.get("meshes", [])) == 16 and {r["package"] for r in bake["meshes"]} == expected, "All sixteen exact native bakes required")
    repair_chain = validate_repair_chain(args,bake,expected,modules,sources) if args.repair_receipt else None
    bakes = []
    for row in bake["meshes"]:
        native = asset_record(row["package"])
        tris = row["triangles_per_lod"]
        require(len(tris) == 3 and all(0 < t <= cap for t, cap in zip(tris, (50000, 18000, 6000))), "Classic LOD budget mismatch")
        require(set(row["semantic_slots"]) == {"Wall", "Glass", "Metal"}, "Native role mismatch")
        require(all(math.isfinite(x) for key in ("min_cm", "max_cm", "size_cm") for x in row["bounds"][key]), "Nonfinite native bounds")
        if row.get("sha256") and repair_chain is None:
            require(native["files"][0]["sha256"] == row["sha256"], "Native package changed after bake receipt")
        native.update(native_bounds=row["bounds"], triangles_per_lod=tris, semantic_slots=row["semantic_slots"],
                      source_classic_lod=row.get("source_classic_lod"), native_dependencies=row["native_dependencies"])
        bakes.append(native)
    require(sum(r["package_bytes"] for r in bakes) <= 64 * 1024 * 1024, "Sixteen bake package budget exceeded")
    report = dict(schema=1, source_product="User-acquired City Sample 5.8, Epic Games", source_assets_cc0=False, new_purchase_tl=0,
                  previous_acquisition_price_not_inferred=True, license_urls=manifest["license_urls"],
                  sharing="Private same-project authorized collaborators under applicable acquisition terms; no standalone source asset distribution",
                  source_hash_scope="Only the allowlisted original geometry packages and sidecars; no claim about the full Vault",
                  source_packages_unchanged_at_native_extraction=True, sources=sources, module_derivatives=modules, baked_derivatives=bakes,
                  total_geometry_package_bytes=sum(r["package_bytes"] for r in modules+bakes), new_textures=0,
                  material_policy="Existing shared project PBR masters only; native sections Wall/Glass/Metal retained",
                  geometry_policy=dict(native_floor_pitch_cm=300, native_panel_depth_cm=75, source_native_front="+Y", output_front="-Y",
                                       repeated_native_apertures=True, no_whole_building_compression=True, no_canonical_envelope_writes=True),
                  native_receipts=[er,br,kr], native_repair_chain=repair_chain, production_scripts=["scripts/prepare_citysample_architecture.ps1",
                    "scripts/ue_prepare_citysample_architecture.py", "scripts/ue_bind_and_bake_citysample_facades.py", "scripts/ue_bake_citysample_facades.py"],
                  acceptance=dict(native_geometry_lod_material_roles=True, source_and_render_tangent_frames=repair_chain is not None, actual_live_scene=False, cook=False, performance=False),
                  note="Native geometry generation is not studio graphics acceptance; remaining gates must be recorded separately.")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    temporary.replace(output)
    print(f"Accepted bounded native geometry provenance only: {output} ({report['total_geometry_package_bytes']} bytes)")
if __name__ == "__main__":
    main()
