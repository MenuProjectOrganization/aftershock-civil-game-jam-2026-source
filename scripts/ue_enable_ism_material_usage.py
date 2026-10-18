"""Compile the project's instance materials with ISM usage enabled (UE 5.8)."""

import unreal


ASSET_PATHS = (
    "/Game/Buildings/Materials/M_Building_Base",
    "/Game/Environment/Materials/M_TerrainCell",
    "/Game/Environment/Materials/M_CitizenActivity",
    "/Game/Environment/Materials/M_AftershockWater",
)


def main():
    usage = unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES
    for path in ASSET_PATHS:
        material = unreal.load_asset(path)
        if not material:
            raise RuntimeError("Missing master material: " + path)
        result = unreal.MaterialEditingLibrary.set_base_material_usage(material, usage, True)
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not unreal.EditorAssetLibrary.save_loaded_asset(material):
            raise RuntimeError("Failed to save material: " + path)
        unreal.log_warning("ISM_MATERIAL_USAGE path={} setter_result={}".format(path, result))


main()
