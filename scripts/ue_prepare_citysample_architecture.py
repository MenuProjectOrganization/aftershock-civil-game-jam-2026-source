"""Native CitySample module inventory/extraction proposal. Parent runs UE only.

Default PROBE changes no assets. EXTRACT requires explicit per-candidate approval
in MODULE_CANDIDATES.json, an exact isolated scratch project, and native bounded
geometry/material/dependency gates. BIND is a separate actual-project pass.
No source saves, whole-project copying, actor/BP extraction or geometry resizing.
"""
import hashlib
import itertools
import json
import math
import os
import traceback
import uuid
from pathlib import Path

import unreal

HERE = Path(__file__).resolve().parent
REPO = next(p for p in (HERE, *HERE.parents) if (p / "Source/JamProject").is_dir() and (p / "Content").is_dir())
PROJECT = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())).resolve()
SOURCE = Path(os.environ.get("AFTERSHOCK_CITY_SAMPLE_CONTENT", "C:/ProgramData/Epic/EpicGamesLauncher/VaultCache/CitySample_5.8/data/Content")).resolve()
MANIFEST_PATH = REPO / "_Imports/Epic/CitySample/Architecture/MODULE_CANDIDATES.json"
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
PREFIX = MANIFEST["native_output_prefix"]
PHASE = os.environ.get("AFTERSHOCK_CITY_MODULE_PHASE", "PROBE").upper()
RECEIPTS = Path(os.environ.get("AFTERSHOCK_CITY_ARCH_RECEIPTS", str(REPO / "Saved/PresentationValidation/CitySampleArchitecture"))).resolve()
if not RECEIPTS.is_relative_to(REPO / "Saved"):
    raise RuntimeError("Native receipts must remain inside this project Saved directory")
RECEIPTS.mkdir(parents=True, exist_ok=True)
OUT = RECEIPTS / ("native_modules_" + PHASE.lower() + ".json")
A = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
SM = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
REG = unreal.AssetRegistryHelpers.get_asset_registry()
DEPENDENCIES = unreal.AssetRegistryDependencyOptions(include_soft_package_references=True,
    include_hard_package_references=True, include_searchable_names=False,
    include_soft_management_references=False, include_hard_management_references=False)
REPORT = dict(schema=1, valid=False, in_progress=True, phase=PHASE, run_id=str(uuid.uuid4()),
              source_assets_cc0=False, source=MANIFEST["source_project"], candidates=[],
              licence_urls=MANIFEST["license_urls"], source_geometry_mutated=False, candidate_manifest_sha256=hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest(),
              source_hash_scope="Nine allowlisted geometry packages and their sidecars, not all104GiB Vault files",
              scope="Native module inventory/owned derivative preparation; no live scene, regional art, GPU or canonical envelope acceptance")


def need(ok, message):
    if not ok:
        raise RuntimeError(message)


def receipt():
    temporary = OUT.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(REPORT, indent=2) + "\n", encoding="utf-8")
    temporary.replace(OUT)


def xyz(p):
    return [float(p.x), float(p.y), float(p.z)]


def finite(values):
    return all(math.isfinite(x) for x in values)


def source_file(package):
    need(package.startswith("/Game/"), "Source package must be Game namespace")
    p = (SOURCE / (package[6:] + ".uasset")).resolve()
    need(p.is_relative_to(SOURCE) and p.is_file(), "Source asset absent/outside Vault: " + package)
    return p


def hashes(package):
    p = source_file(package)
    return {q.suffix: hashlib.sha256(q.read_bytes()).hexdigest() for suffix in (".uasset", ".uexp", ".ubulk", ".uptnl") if (q := p.with_suffix(suffix)).is_file()}


def bbox(mesh):
    b = mesh.get_bounding_box()
    low, high = xyz(b.min), xyz(b.max)
    need(finite(low + high) and all(a < z for a, z in zip(low, high)), "Invalid native mesh bounds")
    return dict(min_cm=low, max_cm=high, size_cm=[z - a for a, z in zip(low, high)])


def transform_record(t):
    # Preserve actual quaternion, translation and nonuniform/signed scale. A
    # Rotator or actor bounds approximation is not an accepted replacement.
    p, q, s = t.translation, t.rotation, t.scale3d
    values = xyz(p) + [float(q.x), float(q.y), float(q.z), float(q.w)] + xyz(s)
    need(finite(values), "Nonfinite native transform")
    return dict(translation_cm=values[:3], quaternion_xyzw=values[3:7], scale_xyz=values[7:])


def transformed_bounds(mesh, transform):
    box = mesh.get_bounding_box()
    points = [transform.transform_location(unreal.Vector(x, y, z))
              for x, y, z in itertools.product((box.min.x, box.max.x), (box.min.y, box.max.y), (box.min.z, box.max.z))]
    low = [min(xyz(p)[axis] for p in points) for axis in range(3)]
    high = [max(xyz(p)[axis] for p in points) for axis in range(3)]
    need(finite(low + high), "Invalid transformed8-corner mesh bounds")
    return dict(min_cm=low, max_cm=high, size_cm=[z - a for a, z in zip(low, high)])


def packed_recipe():
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    need(actors is not None, "Complete packed transforms require initialized EditorActorSubsystem; use full -ExecutePythonScript lifecycle, not commandlet")
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    name = "BPP_QBA_Ref_N1"
    package = "/Game/Building/Library/Kit_Ref_Bldg/" + name
    klass = unreal.load_class(None, package + "." + name + "_C")
    need(klass is not None, "Native packed QBA class unavailable")
    actor = actors.spawn_actor_from_class(klass, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0), transient=True)
    need(actor is not None and isinstance(actor, unreal.PackedLevelActor), "Root must be native PackedLevelActor, no CitySample game module")
    result = dict(package=package, components=[], total_instances=0,
                  actor_bounds_used=False, geometry_resized=False)
    mins, maxs = [], []
    try:
        for c in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = c.get_editor_property("static_mesh")
            if mesh is None:
                continue
            count = c.get_instance_count() if isinstance(c, unreal.InstancedStaticMeshComponent) else 1
            need(count >= 0 and result["total_instances"] + count <= 4096, "Packed recipe instance staging cap exceeded")
            row = dict(component_name=c.get_name(), mesh=mesh.get_path_name(), native_bbox=bbox(mesh),
                       component_world_transform=transform_record(c.get_world_transform()), instances=[],
                       material_overrides=[x.get_path_name() if x else None for x in c.get_materials()])
            for index in range(count):
                if isinstance(c, unreal.InstancedStaticMeshComponent):
                    local = c.get_instance_transform(index, world_space=False)
                    world = c.get_instance_transform(index, world_space=True)
                    need(isinstance(local, unreal.Transform) and isinstance(world, unreal.Transform), "ISM transform getter failed; never substitute component transform")
                else:
                    local = unreal.Transform()
                    world = c.get_world_transform()
                box = transformed_bounds(mesh, world)
                row["instances"].append(dict(index=index, local_transform=transform_record(local), world_transform=transform_record(world), world_bbox=box))
                mins.append(box["min_cm"]);maxs.append(box["max_cm"])
            result["components"].append(row);result["total_instances"] += count
        need(mins, "Packed actor contains no actual instance geometry")
        low = [min(p[a] for p in mins) for a in range(3)];high = [max(p[a] for p in maxs) for a in range(3)]
        result["actual_world_bbox"] = dict(min_cm=low, max_cm=high, size_cm=[z - a for a, z in zip(low, high)])
        return result
    finally:
        actors.destroy_actor(actor)


def slots(mesh, primary):
    result = []
    for index, slot in enumerate(mesh.get_editor_property("static_materials")):
        original = slot.get_editor_property("material_interface")
        name = str(slot.get_editor_property("material_slot_name"))
        path = original.get_path_name() if original else None
        semantic = "Glass" if "glass" in (name + " " + (path or "")).lower() else primary
        need(semantic in ("Plaster", "Wood", "Roof", "Concrete", "Brick", "Metal", "Glass"), "Unmapped semantic material role")
        result.append(dict(index=index, slot_name=name, original_material=path, semantic=semantic,
                           target_material="/Game/Buildings/Architecture/Materials/M_Arch_" + semantic,
                           semantic_evidence="Explicit source Glass label" if semantic == "Glass" else "Candidate author role; native preview approval required"))
    need(result, "Static mesh has no material slots")
    return result


def owned_save(asset):
    need(PHASE in ("EXTRACT", "BIND"), "Any asset save is forbidden in read-only PROBE")
    need(asset.get_path_name().startswith(PREFIX + "/"), "Source/Vault save blocked")
    need(A.save_loaded_asset(asset), "Owned derivative save failed")


def dependencies(package):
    return sorted(str(p) for p in REG.get_dependencies(unreal.Name(package), DEPENDENCIES))


def extract(candidate, source, row):
    need(candidate["allow_extract"] is True, "Extraction requires explicit per-module native preview approval")
    need(PROJECT == (REPO / "Saved/CitySampleExtract").resolve(), "EXTRACT allowed only in isolated scratch project")
    target_dir = (PROJECT / "Content" / PREFIX[6:] / "Meshes").resolve()
    need(not target_dir.is_relative_to(SOURCE), "Owned output resolves into Vault")
    destination = PREFIX + "/Meshes/" + candidate["destination_name"]
    existing = unreal.load_asset(destination)
    if existing is not None:
        need(isinstance(existing, unreal.StaticMesh) and A.get_metadata_tag(existing, "AftershockCityModuleSourceHash") == candidate["source_sha256"]
             and A.get_metadata_tag(existing, "AftershockCityModuleSourcePackage") == candidate["source_package"],
             "Preserve unowned or incompatible existing derivative")
    # Geometry/source materials may load together, but nothing under the source
    # Game folders is ever saved. Only the duplicate's exact package is saved.
    mesh = existing or TOOLS.duplicate_asset(candidate["destination_name"], PREFIX + "/Meshes", source)
    need(isinstance(mesh, unreal.StaticMesh), "Native duplicate failed")
    A.set_metadata_tag(mesh, "AftershockCityModuleSourceHash", candidate["source_sha256"])
    A.set_metadata_tag(mesh, "AftershockCityModuleSourcePackage", candidate["source_package"])
    before = row["native_bbox"]
    placeholder = unreal.load_asset("/Engine/EngineMaterials/DefaultMaterial")
    need(placeholder is not None, "Engine placeholder absent")
    for s in row["semantic_slots"]:
        mesh.set_material(s["index"], placeholder)
    settings = SM.get_nanite_settings(mesh);settings.set_editor_property("enabled", False);SM.set_nanite_settings(mesh, settings, False)
    # Clear imported high-resolution source only on the owned duplicate.
    if SM.get_nanite_source_filename(mesh):
        need(SM.update_nanite_source_filename(mesh, ""), "Cannot detach duplicate high-res source")
    reduction = unreal.StaticMeshReductionOptions();reduction.set_editor_property("auto_compute_lod_screen_size", False)
    levels = []
    for percent, screen in ((1., 1.), (.35, .16), (.12, .045)):
        setting = unreal.StaticMeshReductionSettings();setting.set_editor_property("percent_triangles", percent);setting.set_editor_property("screen_size", screen);levels.append(setting)
    reduction.set_editor_property("reduction_settings", levels)
    need(SM.set_lods(mesh, reduction) == 3, "Owned classic3LOD construction failed")
    SM.remove_collisions(mesh);mesh.set_editor_property("allow_cpu_access", False)
    need(not SM.get_nanite_settings(mesh).get_editor_property("enabled"), "Classic derived mesh still Nanite-only")
    after = bbox(mesh)
    need(all(abs(a - b) <= max(.1, abs(b) * .0001) for a, b in zip(after["size_cm"], before["size_cm"])), "Native extraction changed module dimensions")
    triangles = [mesh.get_num_triangles(i) for i in range(3)]
    need(all(t > 0 for t in triangles) and triangles[0] <= 30000 and triangles[1] <= 12000 and triangles[2] <= 4000,
         "Derived mesh3LOD triangle budget failed " + str(triangles))
    owned_save(mesh)
    asset = target_dir / (candidate["destination_name"] + ".uasset")
    REG.scan_files_synchronous([str(asset)], True)
    closure = dependencies(destination)
    need(not [p for p in closure if p.startswith("/Game/") and not p.startswith(PREFIX + "/")], "Unbounded original Game dependency survived slot replacement: " + str(closure))
    size = sum(p.stat().st_size for s in (".uasset", ".uexp", ".ubulk", ".uptnl") if (p := asset.with_suffix(s)).is_file())
    need(size <= 8 * 1024 * 1024, "Single derived geometry package exceeds8MiB; inspect native hi-res payload")
    return dict(destination_package=destination, package_bytes=size, package_sha256=hashlib.sha256(asset.read_bytes()).hexdigest(),
                triangles_per_lod=triangles, native_bbox=after, native_dependencies=closure,
                temporary_engine_materials=True, final_material_binding_pending=True)


def bind():
    need(PROJECT == REPO, "BIND must run in actual project, never Vault wrapper")
    report = json.loads((RECEIPTS / "native_modules_extract.json").read_text())
    need(report.get("valid") and not report.get("in_progress")
         and report.get("candidate_manifest_sha256") == hashlib.sha256(MANIFEST_PATH.read_bytes()).hexdigest(),
         "Accepted extraction for this exact source/approval manifest required")
    rows = []
    for row in report["candidates"]:
        if "extraction" not in row:
            continue
        package = row["extraction"]["destination_package"]
        mesh = unreal.load_asset(package);need(isinstance(mesh, unreal.StaticMesh), "Staged native derivative missing")
        need(A.get_metadata_tag(mesh,"AftershockCityModuleSourceHash")==row["source_hashes"][".uasset"]
             and A.get_metadata_tag(mesh,"AftershockCityModuleSourcePackage")==row["source_package"]
             and mesh.get_num_lods()==3,"Preserve incompatible/unowned migrated module")
        need(bbox(mesh) == row["extraction"]["native_bbox"], "Staged geometry bounds changed")
        for slot in row["semantic_slots"]:
            material = unreal.load_asset(slot["target_material"])
            need(isinstance(material, unreal.MaterialInterface), "Canonical PBR master missing " + slot["target_material"])
            mesh.set_material(slot["index"], material)
        owned_save(mesh)
        rows.append(dict(package=package, slots=row["semantic_slots"], geometry_resized=False))
    need(rows, "No approved extracted modules were bound")
    REPORT["bound_modules"] = rows
    REPORT["canonical_renderer_integration_pending"] = True


def migrate_owned(rows):
    selected=[r['extraction']['destination_package'] for r in rows if 'extraction' in r]
    need(len(selected)==2,"Exactly the two selected native module derivatives may migrate")
    options=unreal.MigrationOptions()
    options.set_editor_property('prompt',False)
    options.set_editor_property('ignore_dependencies',False)
    options.set_editor_property('asset_conflict',unreal.AssetMigrationConflict.SKIP)
    existed={p: (REPO/'Content'/(p[6:]+'.uasset')).is_file() for p in selected}
    TOOLS.migrate_packages([unreal.Name(p) for p in selected],str(REPO/'Content'),options)
    migrated=[]
    for package in selected:
        file=REPO/'Content'/(package[6:]+'.uasset')
        need(file.is_file(),"Native migration destination missing "+package)
        migrated.append(dict(package=package,destination_existed_and_preserved=existed[package],
            source_metadata_checked_by_following_bind=True,destination_bytes=file.stat().st_size,sha256=hashlib.sha256(file.read_bytes()).hexdigest()))
    REPORT['native_migration_to_private_project']=migrated


def main():
    receipt();need(PHASE in ("PROBE", "EXTRACT", "BIND"), "Choose PROBE, EXTRACT or BIND explicitly")
    REPORT["static_mesh_editor_subsystem_available"] = SM is not None
    REPORT["lod_query_api"] = "UStaticMesh.GetNumLODs/GetNumTriangles/GetNumVertices actual render resources"
    if PHASE == "EXTRACT":
        need(SM is not None, "Native extraction requires initialized StaticMeshEditorSubsystem; use full -ExecutePythonScript editor lifecycle")
        need({c['key'] for c in MANIFEST['candidates'] if c['allow_extract']}=={'SM_CS_02_2nd_Floor_Kit','SM_CS_06_1st_Floor_Window_3x3_M'},
             "This bounded batch permits only the two parent-approved02/06 modules")
    if PHASE == "BIND":
        bind()
    else:
        need(PROJECT == (REPO / "Saved/CitySampleExtract").resolve(), "Source probes require the isolated existing wrapper")
        source_hashes = {}
        for candidate in MANIFEST["candidates"]:
            package = candidate["source_package"];source_hashes[package] = hashes(package)
            need(source_hashes[package][".uasset"] == candidate["source_sha256"], "Original source package changed since preparation")
            mesh = unreal.load_asset(package);need(isinstance(mesh, unreal.StaticMesh), "Not a native StaticMesh " + package)
            # These runtime reflected methods are available without an editor
            # subsystem. They read actual classic render LODs, not source-model
            # counts or filename guesses. Keep source/Nanite counts separate.
            lods = mesh.get_num_lods()
            need(1 <= lods <= 8, "Source has no bounded actual classic render LODs")
            lod_triangles = [mesh.get_num_triangles(i) for i in range(lods)]
            lod_vertices = [mesh.get_num_vertices(i) for i in range(lods)]
            need(all(t > 0 for t in lod_triangles) and all(v >= 3 for v in lod_vertices),
                 "An actual classic render LOD has empty triangles/vertices")
            row = dict(key=candidate["key"], source_package=package, source_hashes=source_hashes[package], native_bbox=bbox(mesh),
                       source_lod_count=lods, lod_count_semantics="Actual classic render-resource LODs; not source models",
                       source_model_lod_count=SM.get_lod_count(mesh) if SM is not None else None,
                       source_lod0_triangles=lod_triangles[0], triangles_per_classic_lod=lod_triangles,
                       vertices_per_classic_lod=lod_vertices, native_nanite_triangles=mesh.get_num_nanite_triangles(),
                       semantic_slots=slots(mesh, candidate["primary_semantic"]))
            need(row["source_lod0_triangles"] > 0, "Source has no usable classic geometry; do not accept a zero fallback")
            if PHASE == "EXTRACT" and candidate["allow_extract"]:
                row["extraction"] = extract(candidate, mesh, row)
            REPORT["candidates"].append(row);receipt()
        if PHASE == "PROBE":
            REPORT["packed_recipe"] = packed_recipe()
        total = sum(row.get("extraction", {}).get("package_bytes", 0) for row in REPORT["candidates"])
        need(total <= 64 * 1024 * 1024, "Derived geometry closure exceeds64MiB batch budget")
        if PHASE == "EXTRACT":
            need(any("extraction" in row for row in REPORT["candidates"]), "No explicit approved module extracted")
            migrate_owned(REPORT['candidates'])
        REPORT["derived_package_bytes"] = total
        REPORT["source_assets_unchanged"] = all(hashes(package) == h for package, h in source_hashes.items())
        need(REPORT["source_assets_unchanged"], "A source file changed; stop staging")
    REPORT["valid"] = True;REPORT["in_progress"] = False;receipt()
    unreal.log_warning("AFTERSHOCK_CITY_MODULE_PREPARATION_COMPLETE " + str(OUT))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        REPORT["error"] = traceback.format_exc();REPORT["valid"] = False;REPORT["in_progress"] = False;receipt()
        raise
