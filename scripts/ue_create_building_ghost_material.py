"""Create a dedicated, translucent green/red placement-preview material (UE 5.8)."""

import unreal


ASSET_PATH = "/Game/Buildings/Materials/M_BuildingGhost"


def main():
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        material = unreal.load_asset(ASSET_PATH)
        if not material:
            raise RuntimeError("Could not load existing placement ghost material")
        unreal.log_warning("BUILDING_GHOST_MATERIAL_EXISTS path={}".format(ASSET_PATH))
        return

    folder, name = ASSET_PATH.rsplit("/", 1)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew()
    )
    if not material:
        raise RuntimeError("Could not create placement ghost material")

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)

    tint = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -400, -100
    )
    tint.set_editor_property("parameter_name", "GhostTint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.10, 0.95, 0.25, 1.0))

    opacity = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -400, 100
    )
    opacity.set_editor_property("parameter_name", "GhostOpacity")
    opacity.set_editor_property("default_value", 0.46)

    unreal.MaterialEditingLibrary.connect_material_property(
        tint, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY
    )
    unreal.MaterialEditingLibrary.set_base_material_usage(
        material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True
    )
    unreal.MaterialEditingLibrary.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Could not save placement ghost material")
    unreal.log_warning("BUILDING_GHOST_MATERIAL_CREATED path={} opacity=0.46".format(ASSET_PATH))


main()
