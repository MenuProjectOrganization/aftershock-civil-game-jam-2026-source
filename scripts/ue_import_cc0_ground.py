"""Import verified Poly Haven CC0 1k maps and author a four-sample ground.

Run fetch_cc0_ground.py first, then this script inside UE 5.8 Python commandlet.
Creates /Game/Environment/Materials/Presentation/M_PresentationGroundPBR.
The old ground material remains intact. Existing completed PBR graphs are reused
without deleting rooted expressions. Rendering only; no terrain gameplay writes.
"""

import hashlib
import json
import os

import unreal


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "_Imports", "PolyHaven", "leafy_grass")
TEXTURE_ROOT = "/Game/Environment/Textures/PolyHaven"
MATERIAL_PATH = "/Game/Environment/Materials/Presentation/M_PresentationGroundPBR"
MACRO_PATH = "/Game/Environment/Textures/Presentation/T_PresentationBreakup"
LIB = unreal.MaterialEditingLibrary
TEXTURES = {
    "diffuse": ("T_LeafyGrass_Diffuse", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "normal_dx": ("T_LeafyGrass_NormalDX", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    "roughness": ("T_LeafyGrass_Roughness", False, unreal.TextureCompressionSettings.TC_MASKS),
}


def node(material, cls, **properties):
    result = LIB.create_material_expression(material, cls)
    if not result:
        raise RuntimeError("Cannot create " + str(cls))
    for key, value in properties.items():
        result.set_editor_property(key, value)
    return result


def link(source, target, pin="", output=""):
    if not LIB.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Cannot connect material nodes")


def scalar(material, name, value):
    return node(material, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(value))


def const(material, value):
    return node(material, unreal.MaterialExpressionConstant, r=float(value))


def vector(material, values):
    return node(material, unreal.MaterialExpressionConstant3Vector,
                constant=unreal.LinearColor(*values, 1.0))


def binary(material, cls, a, b):
    expression = node(material, cls)
    link(a, expression, "A")
    link(b, expression, "B")
    return expression


def mul(material, a, b):
    return binary(material, unreal.MaterialExpressionMultiply, a, b)


def add(material, a, b):
    return binary(material, unreal.MaterialExpressionAdd, a, b)


def blend(material, a, b, alpha):
    expression = node(material, unreal.MaterialExpressionLinearInterpolate)
    link(a, expression, "A")
    link(b, expression, "B")
    link(alpha, expression, "Alpha")
    return expression


def texture_sample(material, texture, parameter, uv, sampler):
    expression = node(material, unreal.MaterialExpressionTextureSampleParameter2D,
                      parameter_name=parameter, texture=texture, sampler_type=sampler)
    link(uv, expression)
    return expression


def material_output(material, expression, prop, channel=""):
    if not LIB.connect_material_property(expression, channel, prop):
        raise RuntimeError("Cannot connect material output " + str(prop))


def import_textures(manifest):
    assets = {}
    for item in manifest["files"]:
        role = item["role"]
        filename = os.path.join(SOURCE, item["file"])
        with open(filename, "rb") as stream:
            data = stream.read()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise RuntimeError("Verified CC0 source changed: " + filename)
        name, srgb, compression = TEXTURES[role]
        task = unreal.AssetImportTask()
        for key, value in {"filename": filename, "destination_path": TEXTURE_ROOT,
                           "destination_name": name, "automated": True,
                           "replace_existing": True, "save": True}.items():
            task.set_editor_property(key, value)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        texture = unreal.load_asset(TEXTURE_ROOT + "/" + name)
        if not isinstance(texture, unreal.Texture2D):
            raise RuntimeError("Texture import failed: " + name)
        texture.set_editor_property("srgb", srgb)
        texture.set_editor_property("compression_settings", compression)
        texture.set_editor_property("max_texture_size", 1024)
        # WORLD is the stable exposed UE5.8 enum. Normalmap quality is selected
        # by TC_NORMALMAP above; it does not depend on a Python LOD enum alias.
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
        if role == "normal_dx":
            texture.set_editor_property("flip_green_channel", False)
        if not unreal.EditorAssetLibrary.save_loaded_asset(texture):
            raise RuntimeError("Cannot save texture " + name)
        assets[role] = texture
    if set(assets) != set(TEXTURES):
        raise RuntimeError("Diffuse, DirectX normal and roughness are all required")
    return assets


def make_material(textures):
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        material = unreal.load_asset(MATERIAL_PATH)
        parameters = [str(item.get_editor_property("parameter_name"))
                      for item in LIB.get_material_expressions(material)
                      if item.get_class().get_name() == "MaterialExpressionTextureSampleParameter2D"]
        if "GroundDiffuse" not in parameters or "GroundNormalDX" not in parameters:
            raise RuntimeError("Preserve and review incomplete existing PBR graph")
        return material
    folder, name = MATERIAL_PATH.rsplit("/", 1)
    unreal.EditorAssetLibrary.make_directory(folder)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("tangent_space_normal", True)
    position = node(material, unreal.MaterialExpressionWorldPosition,
                    world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    xy = node(material, unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
    link(position, xy)
    uv = mul(material, xy, scalar(material, "WorldTextureScale", 0.005))
    diffuse = texture_sample(material, textures["diffuse"], "GroundDiffuse", uv,
                             unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    normal = texture_sample(material, textures["normal_dx"], "GroundNormalDX", uv,
                            unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    roughness = texture_sample(material, textures["roughness"], "GroundRoughness", uv,
                               unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    vertex = node(material, unreal.MaterialExpressionVertexColor)
    # Biome colors already come from canonical grid samples. Lightly tint the
    # scanned albedo instead of replacing it with the old flat green terrain.
    tint = blend(material, vector(material, (1.0, 1.0, 1.0)),
                 mul(material, vertex, vector(material, (2.4, 2.2, 2.6))),
                 scalar(material, "BiomeTintWeight", 0.16))
    base = mul(material, diffuse, tint)
    macro_texture = unreal.load_asset(MACRO_PATH)
    if not isinstance(macro_texture, unreal.Texture2D):
        raise RuntimeError("Run ue_upgrade_presentation_materials.py before PBR ground import")
    macro_uv = mul(material, xy, scalar(material, "MacroTextureScale", 0.00015))
    macro = texture_sample(material, macro_texture, "GroundMacroBreakup", macro_uv,
                           unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    macro_red = node(material, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
    link(macro, macro_red)
    macro_factor = add(material, const(material, 0.9), mul(material, macro_red, const(material, 0.2)))
    material_output(material, mul(material, base, macro_factor), unreal.MaterialProperty.MP_BASE_COLOR)
    flattened_normal = blend(material, vector(material, (0.0, 0.0, 1.0)), normal,
                             scalar(material, "GroundNormalStrength", 0.45))
    normalized = node(material, unreal.MaterialExpressionNormalize)
    link(flattened_normal, normalized)
    material_output(material, normalized, unreal.MaterialProperty.MP_NORMAL)
    material_output(material, roughness, unreal.MaterialProperty.MP_ROUGHNESS, "R")
    material_output(material, const(material, 0.0), unreal.MaterialProperty.MP_METALLIC)
    material_output(material, const(material, 0.25), unreal.MaterialProperty.MP_SPECULAR)
    LIB.layout_material_expressions(material)
    return material


def main():
    with open(os.path.join(SOURCE, "SOURCE_LICENSE_MANIFEST.json"), encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest["license"] != "CC0-1.0" or manifest["asset"] != "leafy_grass":
        raise RuntimeError("Unexpected third-party source manifest")
    textures = import_textures(manifest)
    material = make_material(textures)
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    errors = LIB.recompile_material(material)
    if errors:
        raise RuntimeError("PBR ground shader compile failed: " + str(list(errors)))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Cannot save PBR ground material")
    report = {"material": MATERIAL_PATH, "compile_errors": [], "samples": 4,
              "texture_paths": {role: texture.get_path_name() for role, texture in textures.items()},
              "source_manifest": os.path.relpath(os.path.join(SOURCE, "SOURCE_LICENSE_MANIFEST.json"), ROOT),
              "license": "CC0-1.0", "world_tile_cm": 200, "normal": "DirectX, green unflipped",
              "max_texture_size": 1024, "displacement": False}
    report_path = os.path.join(ROOT, "Saved", "PresentationValidation", "cc0_ground_import.json")
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    with open(report_path, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    unreal.log_warning("AFTERSHOCK_CC0_GROUND_READY " + json.dumps(report))


main()
