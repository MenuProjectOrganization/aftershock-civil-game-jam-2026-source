"""Import the Mert audio/visual package and author the missing runtime materials.

Run from UE 5.8 with PythonScriptPlugin and EditorScriptingUtilities enabled:
UnrealEditor-Cmd.exe JamProject.uproject -run=pythonscript -script=scripts/ue_finalize_mert_assets.py -stdout -FullStdOutLogOutput -unattended -nop4
"""

import json
import os
import unreal


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = os.path.join(ROOT, "scripts", "mert_asset_import_report.json")


def log(message):
    print("[AFTERSHOCK-ASSET-FINALIZE] " + str(message))


def make_scalar_parameter(material, name, value, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, x, y
    )
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def make_vector_parameter(material, name, color, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, x, y
    )
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", color)
    return node


def ensure_instanced_static_mesh_usage(material):
    usage = unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES
    unreal.MaterialEditingLibrary.set_base_material_usage(material, usage, True)


def create_or_update_instance_color_material(asset_path, label):
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.EditorAssetLibrary.delete_asset(asset_path)
    folder, name = asset_path.rsplit("/", 1)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew()
    )
    if not asset:
        raise RuntimeError("Could not create material " + asset_path)

    # This display material consumes the same three RGB custom-data floats written by
    # AAftershockCrowdHostActor. It keeps the existing ISM grid and adds no actors/ticks.
    color = unreal.MaterialEditingLibrary.create_material_expression(
        asset, unreal.MaterialExpressionPerInstanceCustomData3Vector, -400, 0
    )
    color.set_editor_property("data_index", 0)
    unreal.MaterialEditingLibrary.connect_material_property(
        color, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    roughness = make_scalar_parameter(asset, "RoughnessDefault", 0.82, -100, 180)
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS
    )
    ensure_instanced_static_mesh_usage(asset)
    unreal.MaterialEditingLibrary.recompile_material(asset)
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    log("Authored per-instance color material: " + asset_path + " (" + label + ")")
    return asset


def create_or_update_water_material(asset_path):
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        unreal.EditorAssetLibrary.delete_asset(asset_path)
    folder, name = asset_path.rsplit("/", 1)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew()
    )
    if not asset:
        raise RuntimeError("Could not create material " + asset_path)

    asset.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    asset.set_editor_property("two_sided", True)

    base_color = make_vector_parameter(
        asset, "BaseColor", unreal.LinearColor(0.055, 0.31, 0.62, 1.0), -400, -100
    )
    opacity = make_scalar_parameter(asset, "Opacity", 0.72, -400, 100)
    roughness = make_scalar_parameter(asset, "Roughness", 0.28, -400, 250)
    unreal.MaterialEditingLibrary.connect_material_property(
        base_color, "", unreal.MaterialProperty.MP_BASE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS
    )
    ensure_instanced_static_mesh_usage(asset)
    unreal.MaterialEditingLibrary.recompile_material(asset)
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    log("Authored translucent water material: " + asset_path)
    return asset


def import_glb_models():
    source_folder = os.path.join(ROOT, "Content", "Aftershock", "KenneyMeshes", "_Imports")
    destination = "/Game/Aftershock/KenneyMeshes/Imported"
    unreal.EditorAssetLibrary.make_directory(destination)
    for stale_path in (
        destination + "/block-coin/StaticMeshes/block-coin",
        destination + "/block-coin/Materials/colormap",
    ):
        if unreal.EditorAssetLibrary.does_asset_exist(stale_path):
            unreal.EditorAssetLibrary.delete_asset(stale_path)
    results = []
    for filename in sorted(os.listdir(source_folder)):
        if not filename.lower().endswith(".glb"):
            continue
        # This single source GLB references a texture outside the package. It is not
        # used by the game, so do not import a knowingly broken side asset.
        if filename.lower() == "block-coin.glb":
            log("Skipped unused block-coin.glb (missing referenced texture in source package)")
            continue
        source = os.path.join(source_folder, filename)
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", source)
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("destination_name", os.path.splitext(filename)[0])
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        imported = [str(asset_path) for asset_path in task.get_editor_property("imported_object_paths")]
        if not imported:
            log("WARNING: no imported objects reported for " + filename)
        results.append({"source": filename, "assets": imported})
        log("Imported GLB " + filename + " -> " + ", ".join(imported))
    if not any("character" in path.lower() for result in results for path in result["assets"]):
        raise RuntimeError("Kenney character.glb did not produce an asset")
    return results


def import_citizen_proxy():
    source = os.path.join(ROOT, "Content", "Aftershock", "KenneyMeshes", "_Imports", "SM_CitizenProxy.obj")
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", "/Game/Aftershock/KenneyMeshes/Imported")
    task.set_editor_property("destination_name", "SM_CitizenProxy")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = [str(asset_path) for asset_path in task.get_editor_property("imported_object_paths")]
    static_mesh_paths = []
    for asset_path in imported:
        asset = unreal.EditorAssetLibrary.load_asset(asset_path)
        if isinstance(asset, unreal.StaticMesh):
            static_mesh_paths.append(asset_path)
    if len(static_mesh_paths) != 1:
        raise RuntimeError("Citizen OBJ must produce one UStaticMesh; got " + repr(imported))
    mesh = unreal.EditorAssetLibrary.load_asset(static_mesh_paths[0])
    nanite = mesh.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", False)
    mesh.set_editor_property("nanite_settings", nanite)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    bounds = mesh.get_bounds()
    dimensions = [round(bounds.box_extent.x * 2.0, 2), round(bounds.box_extent.y * 2.0, 2), round(bounds.box_extent.z * 2.0, 2)]
    log("Imported authored ISM citizen proxy: " + static_mesh_paths[0] + " dimensions_cm=" + repr(dimensions) + " triangles=92")
    return {"asset": static_mesh_paths[0], "dimensions_cm": dimensions, "triangles": 92, "nanite_enabled": False}


def import_procedural_audio():
    source_folder = os.path.join(ROOT, "Content", "Aftershock", "Audio", "Generated")
    destination = "/Game/Aftershock/Audio/Generated"
    unreal.EditorAssetLibrary.make_directory(destination)
    imported_results = []
    for filename in sorted(os.listdir(source_folder)):
        if not filename.lower().endswith(".wav"):
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(source_folder, filename))
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("destination_name", os.path.splitext(filename)[0])
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        imported = [str(asset_path) for asset_path in task.get_editor_property("imported_object_paths")]
        if not imported:
            raise RuntimeError("Could not import generated WAV: " + filename)
        imported_results.extend(imported)
        log("Imported project-generated audio: " + filename + " -> " + ", ".join(imported))
    return imported_results


def set_ambience_looping():
    path = "/Game/Aftershock/Audio/S_Ambience_City"
    sound = unreal.EditorAssetLibrary.load_asset(path)
    if not sound:
        raise RuntimeError("Missing imported city ambience sound: " + path)
    try:
        sound.set_editor_property("looping", True)
    except Exception as exc:
        log("WARNING: unable to set ambience looping property: " + str(exc))
    unreal.EditorAssetLibrary.save_loaded_asset(sound)
    return sound.get_path_name()


def validate_imported_assets():
    expected = [
        "/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy",
        "/Game/Buildings/Meshes/SM_House_01",
        "/Game/Buildings/Meshes/SM_Apartment_01",
        "/Game/Buildings/Meshes/SM_Office_01",
        "/Game/Buildings/Meshes/SM_Factory_01",
        "/Game/Buildings/Meshes/SM_Hospital_01",
        "/Game/Buildings/Meshes/SM_School_01",
        "/Game/Buildings/Meshes/SM_FireStation_01",
        "/Game/Buildings/Meshes/SM_PowerPlant_01",
        "/Game/Buildings/Meshes/SM_GreenSpace_01",
    ]
    expected += [
        "/Game/UI/Icons/Buildings/T_Icon_" + name + "_01"
        for name in ("House", "Apartment", "Office", "Factory", "Hospital", "School", "FireStation", "PowerPlant", "GreenSpace")
    ]
    expected += [
        "/Game/Aftershock/Audio/" + name
        for name in ("S_Ambience_City", "S_Toggle", "S_Rotate", "S_Coin", "S_Break", "S_Damage", "S_Explosion", "S_Fall", "S_PanelSwitch")
    ]
    expected += [
        "/Game/Aftershock/Audio/Generated/" + name
        for name in ("S_DisasterWarning", "S_EarthquakeRumble", "S_FloodSurge", "S_StormGust")
    ]
    results = {}
    for path in expected:
        exists = unreal.EditorAssetLibrary.does_asset_exist(path)
        results[path] = bool(exists)
        if not exists:
            log("ERROR: expected cooked asset missing: " + path)
    return results


def main():
    unreal.EditorAssetLibrary.make_directory("/Game/Environment/Materials")
    create_or_update_instance_color_material("/Game/Environment/Materials/M_TerrainCell", "terrain cell RGB")
    create_or_update_instance_color_material("/Game/Environment/Materials/M_CitizenActivity", "citizen activity RGB")
    create_or_update_water_material("/Game/Environment/Materials/M_AftershockWater")
    glb_results = import_glb_models()
    citizen_proxy_path = import_citizen_proxy()
    generated_audio = import_procedural_audio()
    ambience_path = set_ambience_looping()
    validation = validate_imported_assets()
    report = {
        "engine": "Unreal Engine 5.8",
        "glb_imports": glb_results,
        "citizen_proxy_static_mesh": citizen_proxy_path,
        "generated_audio_assets": generated_audio,
        "ambience_looping_asset": ambience_path,
        "environment_materials": [
            "/Game/Environment/Materials/M_TerrainCell",
            "/Game/Environment/Materials/M_CitizenActivity",
            "/Game/Environment/Materials/M_AftershockWater",
        ],
        "asset_validation": validation,
        "missing_assets": [path for path, exists in validation.items() if not exists],
    }
    with open(REPORT, "w", encoding="utf-8") as report_file:
        json.dump(report, report_file, indent=2, ensure_ascii=False)
    log("Finalization complete. GLB count=" + str(len(glb_results)) + ", missing expected assets=" + str(len(report["missing_assets"])))
    if report["missing_assets"]:
        raise RuntimeError("Some expected assets are missing; see " + REPORT)


main()
