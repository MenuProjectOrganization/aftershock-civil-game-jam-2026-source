"""Enable the required ISM shader permutation on project-owned materials.

Run in UE 5.8 Editor with PythonScriptPlugin and EditorScriptingUtilities enabled:
UnrealEditor-Cmd.exe JamProject.uproject -run=pythonscript -script=<absolute path>
"""

import json
import os
import unreal


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = os.path.join(ROOT, "scripts", "ism_material_usage_report.json")
USAGE = unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES
MASTER_MATERIALS = (
    "/Game/Buildings/Materials/M_Building_Base",
    "/Game/Environment/Materials/M_TerrainCell",
    "/Game/Environment/Materials/M_CitizenActivity",
    "/Game/Environment/Materials/M_AftershockWater",
)


def set_ism_usage(asset_path):
    material = unreal.EditorAssetLibrary.load_asset(asset_path)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Expected a UMaterial at " + asset_path)

    before = unreal.MaterialEditingLibrary.has_material_usage(material, USAGE)
    unreal.MaterialEditingLibrary.set_base_material_usage(material, USAGE, True)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    after = unreal.MaterialEditingLibrary.has_material_usage(material, USAGE)
    if not after:
        raise RuntimeError("InstancedStaticMeshes usage did not persist for " + asset_path)
    print("[AFTERSHOCK-ISM-MATERIAL] " + asset_path + " before=" + str(before) + " after=" + str(after))
    return {"asset": asset_path, "before": bool(before), "after": bool(after)}


results = [set_ism_usage(path) for path in MASTER_MATERIALS]
for asset_path in unreal.EditorAssetLibrary.list_assets("/Game/Buildings/Materials", recursive=False, include_folder=False):
    if "/MI_" not in asset_path:
        continue
    instance = unreal.EditorAssetLibrary.load_asset(asset_path)
    if isinstance(instance, unreal.MaterialInstanceConstant):
        has_usage = unreal.MaterialEditingLibrary.has_material_usage(instance, USAGE)
        print("[AFTERSHOCK-ISM-MATERIAL] " + asset_path + " inherited_usage=" + str(has_usage))
        if not has_usage:
            unreal.MaterialEditingLibrary.set_material_usage_override(instance, USAGE, True, True)
            unreal.EditorAssetLibrary.save_loaded_asset(instance)
            has_usage = unreal.MaterialEditingLibrary.has_material_usage(instance, USAGE)
        if not has_usage:
            raise RuntimeError("Building material instance cannot use ISM: " + asset_path)
        results.append({"asset": asset_path, "after": bool(has_usage)})

with open(REPORT, "w", encoding="utf-8") as report_file:
    json.dump({"engine": "Unreal Engine 5.8", "usage": "InstancedStaticMeshes", "materials": results}, report_file, indent=2)
print("[AFTERSHOCK-ISM-MATERIAL] Saved " + REPORT)
