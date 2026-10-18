"""Restore the Kenney city-kit colormap in the damage-aware building material.

Run with UE 5.8:
UnrealEditor-Cmd.exe JamProject.uproject -run=pythonscript -script=<this file>
"""

import unreal


MASTER_PATH = "/Game/Buildings/Materials/M_Building_Base"
ALBEDO_PATH = "/Game/Aftershock/KenneyMeshes/Imported/building-small-a/Textures/colormap"


def main():
    material = unreal.load_asset(MASTER_PATH)
    albedo = unreal.load_asset(ALBEDO_PATH)
    if not material or not albedo:
        raise RuntimeError("Building material or Kenney colormap asset is missing")

    expressions = unreal.MaterialEditingLibrary.get_material_expressions(material)
    if any(getattr(expression, "parameter_name", None) == "BuildingAlbedo" for expression in expressions):
        unreal.log_warning("Building albedo graph already exists; no duplicate nodes added")
        return

    base_color = next((
        expression for expression in expressions
        if expression.get_class().get_name() == "MaterialExpressionVectorParameter"
        and expression.get_editor_property("parameter_name") == "BaseColorDefault"
    ), None)
    crack_lerp = next((
        expression for expression in expressions
        if expression.get_class().get_name() == "MaterialExpressionLinearInterpolate"
    ), None)
    if not base_color or not crack_lerp:
        raise RuntimeError("Could not find the existing BaseColorDefault and crack blend nodes")

    texture_sample = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionTextureSampleParameter2D, -1000, -350
    )
    texture_sample.set_editor_property("parameter_name", "BuildingAlbedo")
    texture_sample.set_editor_property("texture", albedo)

    tint_multiply = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -750, -350
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(texture_sample, "RGB", tint_multiply, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(base_color, "", tint_multiply, "B")

    tint_amount = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant, -750, -150
    )
    tint_amount.set_editor_property("r", 0.22)

    texture_tint_blend = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -500, -350
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(texture_sample, "RGB", texture_tint_blend, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(tint_multiply, "", texture_tint_blend, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(tint_amount, "", texture_tint_blend, "Alpha")
    # Feed the Kenney material atlas into the existing crack blend. Disaster damage
    # still darkens the G-channel corner mask; this is a visual-only material fix.
    unreal.MaterialEditingLibrary.connect_material_expressions(texture_tint_blend, "", crack_lerp, "A")

    unreal.MaterialEditingLibrary.recompile_material(material)
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Failed to save M_Building_Base")

    unreal.log_warning("BUILDING_MATERIAL_POLISH saved={} albedo={} tint=0.22 expressions={}".format(
        MASTER_PATH, ALBEDO_PATH,
        len(unreal.MaterialEditingLibrary.get_material_expressions(material)),
    ))


main()
