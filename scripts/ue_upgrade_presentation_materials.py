"""Author AFTERSHOCK's lightweight, lit environment presentation materials.

Run only inside UE 5.8, after the Python/editor scripting plugins are loaded::

    UnrealEditor-Cmd.exe JamProject.uproject -run=pythonscript \
        -script=scripts/ue_upgrade_presentation_materials.py -unattended -nop4

The generated texture is original deterministic periodic value noise. There is
no downloaded art, custom shader HLSL, plugin dependency, runtime generation or
gameplay/terrain simulation in this script. Re-running validates/reuses complete
graphs without deleting any expression or package. UE 5.8 can root expressions
loaded during startup, which makes native expression deletion unsafe here.
Existing imported Kenney palette graphs remain intact; only their ISM usage is
enabled. A JSON manifest with material compile results and mesh bounds is saved
under Saved/PresentationValidation/.
"""

import json
import math
import os
import random
import struct
import zlib

import unreal


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MATERIAL_ROOT = "/Game/Environment/Materials/Presentation"
TEXTURE_ROOT = "/Game/Environment/Textures/Presentation"
TEXTURE_PATH = TEXTURE_ROOT + "/T_PresentationBreakup"
IMPORTED_ROOT = "/Game/Aftershock/KenneyMeshes/Imported"
ATLAS_PATH = IMPORTED_ROOT + "/building-small-a/Textures/colormap"
LIB = unreal.MaterialEditingLibrary
MANIFEST = {"schema": 1, "materials": [], "imported_materials": [], "meshes": []}


def log(message):
    unreal.log_warning("AFTERSHOCK_PRESENTATION_MATERIALS " + message)


def node(material, cls, x=0, y=0, **properties):
    expression = LIB.create_material_expression(material, cls, x, y)
    if not expression:
        raise RuntimeError("Could not create expression " + str(cls))
    for name, value in properties.items():
        expression.set_editor_property(name, value)
    return expression


def link(source, target, pin="", output=""):
    if not LIB.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Could not connect {}.{} -> {}.{}".format(
            source.get_class().get_name(), output, target.get_class().get_name(), pin))


def output(material, expression, prop, pin=""):
    if not LIB.connect_material_property(expression, pin, prop):
        raise RuntimeError("Could not connect material property " + str(prop))


def scalar(material, name, value, x=0, y=0):
    return node(material, unreal.MaterialExpressionScalarParameter, x, y,
                parameter_name=name, default_value=float(value))


def color(material, name, value, x=0, y=0):
    return node(material, unreal.MaterialExpressionVectorParameter, x, y,
                parameter_name=name, default_value=unreal.LinearColor(*value, 1.0))


def constant(material, value):
    return node(material, unreal.MaterialExpressionConstant, r=float(value))


def vector(material, value):
    return node(material, unreal.MaterialExpressionConstant3Vector,
                constant=unreal.LinearColor(*value, 1.0))


def binary(material, cls, left, right):
    expression = node(material, cls)
    link(left, expression, "A")
    link(right, expression, "B")
    return expression


def multiply(material, left, right):
    return binary(material, unreal.MaterialExpressionMultiply, left, right)


def add(material, left, right):
    return binary(material, unreal.MaterialExpressionAdd, left, right)


def lerp(material, left, right, alpha):
    expression = node(material, unreal.MaterialExpressionLinearInterpolate)
    link(left, expression, "A")
    link(right, expression, "B")
    link(alpha, expression, "Alpha")
    return expression


def mask(material, source, r=False, g=False, b=False):
    expression = node(material, unreal.MaterialExpressionComponentMask, r=r, g=g, b=b, a=False)
    link(source, expression)
    return expression


def append(material, left, right):
    return binary(material, unreal.MaterialExpressionAppendVector, left, right)


def begin_material(path):
    material = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if material and not isinstance(material, unreal.Material):
        raise RuntimeError("Expected a base material: " + path)
    if not material:
        folder, name = path.rsplit("/", 1)
        unreal.EditorAssetLibrary.make_directory(folder)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.Material, unreal.MaterialFactoryNew())
    if not material:
        raise RuntimeError("Could not create material: " + path)
    if LIB.get_material_expressions(material):
        raise RuntimeError("Existing graph is incomplete/unrecognized; preserve it for manual review: " + path)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("two_sided", False)
    material.set_editor_property("use_material_attributes", False)
    material.set_editor_property("tangent_space_normal", True)
    return material


def reuse_material(path, required_parameter):
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        return False
    material = unreal.load_asset(path)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Expected a base material: " + path)
    for expression in LIB.get_material_expressions(material):
        if expression.get_class().get_name() in (
                "MaterialExpressionScalarParameter", "MaterialExpressionTextureSampleParameter2D"):
            if str(expression.get_editor_property("parameter_name")) == required_parameter:
                finish_material(material)
                log("REUSED " + path)
                return True
    if LIB.get_material_expressions(material):
        raise RuntimeError("Existing graph missing {}: {}".format(required_parameter, path))
    return False


def finish_material(material):
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    LIB.layout_material_expressions(material)
    errors = LIB.recompile_material(material)
    if errors:
        raise RuntimeError("Material failed to compile: {}: {}".format(material.get_path_name(), list(errors)))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Could not save material " + material.get_path_name())
    MANIFEST["materials"].append({"path": material.get_path_name(), "compile_errors": [],
                                  "ism_usage": True, "nodes": len(LIB.get_material_expressions(material))})
    log("COMPILED " + material.get_path_name())


def periodic_noise(x, y, size, lattice):
    cell_x = x * size
    cell_y = y * size
    ix, iy = math.floor(cell_x), math.floor(cell_y)
    tx, ty = cell_x - ix, cell_y - iy
    tx, ty = tx * tx * (3.0 - 2.0 * tx), ty * ty * (3.0 - 2.0 * ty)
    a = lattice[(iy % size) * size + (ix % size)]
    b = lattice[(iy % size) * size + ((ix + 1) % size)]
    c = lattice[((iy + 1) % size) * size + (ix % size)]
    d = lattice[((iy + 1) % size) * size + ((ix + 1) % size)]
    return (a + (b - a) * tx) * (1.0 - ty) + (c + (d - c) * tx) * ty


def write_noise_png(path):
    """Produce seamless authored data with no imaging library/runtime dependency."""
    size = 256
    rng = random.Random(20261017)
    octaves = [(frequency, [rng.random() for _ in range(frequency * frequency)])
               for frequency in (4, 8, 16, 32, 64)]
    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter None, RGBA8.
        for x in range(size):
            values = [periodic_noise(x / size, y / size, frequency, lattice)
                      for frequency, lattice in octaves]
            broad = sum(value * weight for value, weight in zip(values, (0.45, 0.27, 0.16, 0.08, 0.04)))
            medium = values[2] * 0.55 + values[3] * 0.30 + values[4] * 0.15
            fine = values[3] * 0.35 + values[4] * 0.65
            row.extend((round(broad * 255), round(medium * 255), round(fine * 255), 255))
        rows.append(bytes(row))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)

    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as stream:
        stream.write(data)


def import_breakup_texture():
    source = os.path.join(ROOT, "Content", "Environment", "Textures", "Presentation", "T_PresentationBreakup.png")
    write_noise_png(source)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", TEXTURE_ROOT)
    task.set_editor_property("destination_name", "T_PresentationBreakup")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(TEXTURE_PATH)
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError("Could not import presentation breakup texture")
    texture.set_editor_property("srgb", False)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
    if not unreal.EditorAssetLibrary.save_loaded_asset(texture):
        raise RuntimeError("Could not save breakup texture")
    MANIFEST["texture"] = {"path": TEXTURE_PATH, "source": os.path.relpath(source, ROOT),
                            "srgb": False, "size": [256, 256], "seed": 20261017,
                            "license": "Project-authored deterministic value noise"}
    return texture


def world_sample(material, texture, world_scale):
    position = node(material, unreal.MaterialExpressionWorldPosition, -1200, 0)
    position.set_editor_property("world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    uv = mask(material, position, r=True, g=True)
    uv = multiply(material, uv, scalar(material, "WorldTextureScale", world_scale, -1200, 180))
    sample = node(material, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0,
                  parameter_name="SurfaceBreakup", texture=texture,
                  sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(uv, sample)
    return sample


def sample_channel(material, sample, channel):
    # A named output avoids a redundant mask node and is valid for TextureSample.
    expression = node(material, unreal.MaterialExpressionMultiply, const_b=1.0)
    link(sample, expression, "A", channel)
    return expression


def make_surface(path, texture, base, roughness, scale, breakup=0.18,
                 vertex_weight=0.0, instance_weight=0.0, normal_strength=0.035):
    if reuse_material(path, "SurfaceBreakup"):
        return
    material = begin_material(path)
    base_color = color(material, "BaseColor", base, -800, -380)
    vertex = node(material, unreal.MaterialExpressionVertexColor, -1050, -400)
    base_color = lerp(material, base_color, vertex,
                      scalar(material, "VertexTintWeight", vertex_weight, -1000, -260))
    per_instance = node(material, unreal.MaterialExpressionPerInstanceCustomData3Vector, -1050, -100,
                        data_index=0, const_default_value=unreal.LinearColor(*base, 1.0))
    base_color = lerp(material, base_color, per_instance,
                      scalar(material, "InstanceTintWeight", instance_weight, -1000, -160))
    sample = world_sample(material, texture, scale)
    red = sample_channel(material, sample, "R")
    green = sample_channel(material, sample, "G")
    blue = sample_channel(material, sample, "B")
    centered = add(material, red, constant(material, -0.5))
    variation = add(material, constant(material, 1.0), multiply(
        material, centered, scalar(material, "ColorBreakup", breakup, -600, 220)))
    output(material, multiply(material, base_color, variation), unreal.MaterialProperty.MP_BASE_COLOR)
    rough = add(material, scalar(material, "Roughness", roughness, -600, 340),
                multiply(material, add(material, blue, constant(material, -0.5)), constant(material, 0.08)))
    saturated = node(material, unreal.MaterialExpressionSaturate)
    link(rough, saturated)
    output(material, saturated, unreal.MaterialProperty.MP_ROUGHNESS)
    output(material, constant(material, 0.0), unreal.MaterialProperty.MP_METALLIC)
    output(material, constant(material, 0.25), unreal.MaterialProperty.MP_SPECULAR)
    normal_x = multiply(material, add(material, green, constant(material, -0.5)),
                        scalar(material, "MicroNormalStrength", normal_strength))
    normal_y = multiply(material, add(material, blue, constant(material, -0.5)), constant(material, normal_strength))
    normal = node(material, unreal.MaterialExpressionNormalize)
    link(append(material, append(material, normal_x, normal_y), constant(material, 1.0)), normal)
    output(material, normal, unreal.MaterialProperty.MP_NORMAL)
    finish_material(material)


def wave(material, position, time, direction, speed):
    dot = binary(material, unreal.MaterialExpressionDotProduct, position, vector(material, direction))
    phase = add(material, dot, multiply(material, time, constant(material, speed)))
    sine = node(material, unreal.MaterialExpressionSine, period=1.0)
    link(phase, sine)
    return sine


def make_water(path):
    if reuse_material(path, "WaveAmplitudeCm"):
        return
    material = begin_material(path)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    material.set_editor_property("screen_space_reflections", True)
    position = node(material, unreal.MaterialExpressionWorldPosition, -1200, 0)
    position.set_editor_property("world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    time = node(material, unreal.MaterialExpressionTime, -1200, 200)
    a = wave(material, position, time, (0.0018, 0.0007, 0.0), 0.12)
    b = wave(material, position, time, (-0.0011, 0.0024, 0.0), 0.085)
    wave_sum = multiply(material, add(material, a, b), constant(material, 0.5))
    wave_01 = add(material, multiply(material, wave_sum, constant(material, 0.5)), constant(material, 0.5))
    base = color(material, "BaseColor", (0.018, 0.12, 0.15), -800, -400)
    glint = color(material, "ShallowColor", (0.035, 0.23, 0.22), -800, -260)
    # Low-amplitude moving color variations plus lit micro-normal reflection.
    output(material, lerp(material, base, glint,
                          multiply(material, wave_01, constant(material, 0.18))), unreal.MaterialProperty.MP_BASE_COLOR)
    output(material, scalar(material, "Opacity", 0.82, -500, 400), unreal.MaterialProperty.MP_OPACITY)
    output(material, scalar(material, "Roughness", 0.22, -500, 500), unreal.MaterialProperty.MP_ROUGHNESS)
    output(material, constant(material, 0.5), unreal.MaterialProperty.MP_SPECULAR)
    output(material, constant(material, 0.0), unreal.MaterialProperty.MP_METALLIC)
    nx = multiply(material, a, scalar(material, "WaveNormalStrength", 0.065))
    ny = multiply(material, b, constant(material, 0.05))
    normal = node(material, unreal.MaterialExpressionNormalize)
    link(append(material, append(material, nx, ny), constant(material, 1.0)), normal)
    output(material, normal, unreal.MaterialProperty.MP_NORMAL)
    # 0.35 cm artistic surface ripple; it does not change water level/flow/flood math.
    offset = multiply(material, wave_sum, scalar(material, "WaveAmplitudeCm", 0.35))
    output(material, multiply(material, offset, vector(material, (0.0, 0.0, 1.0))),
           unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    finish_material(material)


def make_atlas():
    path = MATERIAL_ROOT + "/M_PresentationKenneyAtlas"
    if reuse_material(path, "KenneyPalette"):
        return
    texture = unreal.load_asset(ATLAS_PATH)
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError("Existing Kenney palette is missing: " + ATLAS_PATH)
    material = begin_material(path)
    sample = node(material, unreal.MaterialExpressionTextureSampleParameter2D, -400, -100,
                  parameter_name="KenneyPalette", texture=texture)
    output(material, sample, unreal.MaterialProperty.MP_BASE_COLOR, "RGB")
    output(material, scalar(material, "Roughness", 0.78, -400, 160), unreal.MaterialProperty.MP_ROUGHNESS)
    output(material, constant(material, 0.0), unreal.MaterialProperty.MP_METALLIC)
    output(material, constant(material, 0.25), unreal.MaterialProperty.MP_SPECULAR)
    finish_material(material)


def imported_mesh_manifest_and_usage():
    parents = {}
    for path in sorted(unreal.EditorAssetLibrary.list_assets(IMPORTED_ROOT, recursive=True, include_folder=False)):
        if "/StaticMeshes/" not in path:
            continue
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        bounds = mesh.get_bounds()
        materials = []
        for slot in mesh.get_editor_property("static_materials"):
            material = slot.get_editor_property("material_interface")
            if not material:
                continue
            materials.append(material.get_path_name())
            parent = material
            while not isinstance(parent, unreal.Material):
                if not isinstance(parent, unreal.MaterialInstance):
                    raise RuntimeError("Unexpected material type " + parent.get_path_name())
                parent = parent.get_editor_property("parent")
                if not parent:
                    raise RuntimeError("Imported material instance has no parent: " + material.get_path_name())
            parents[parent.get_path_name()] = parent
        MANIFEST["meshes"].append({"path": path,
            "bounds_origin_cm": [bounds.origin.x, bounds.origin.y, bounds.origin.z],
            "bounds_size_cm": [bounds.box_extent.x * 2, bounds.box_extent.y * 2, bounds.box_extent.z * 2],
            "materials": materials})
    for path, material in sorted(parents.items()):
        # Preserve imported palette graph: just allow its native mesh to be instanced.
        # Never save an Engine/plugin parent as a side effect of a project audit.
        if not path.startswith("/Game/"):
            if not LIB.has_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES):
                raise RuntimeError("External material parent needs project-side ISM override: " + path)
            MANIFEST["imported_materials"].append({"path": path, "ism_usage": True,
                                                  "compile_errors": [], "external_parent_read_only": True})
            continue
        LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
        errors = LIB.recompile_material(material)
        if errors:
            raise RuntimeError("Imported material compile failed: {} {}".format(path, list(errors)))
        if not unreal.EditorAssetLibrary.save_loaded_asset(material):
            raise RuntimeError("Could not save ISM usage for " + path)
        MANIFEST["imported_materials"].append({"path": path, "ism_usage": True, "compile_errors": []})


def main():
    texture = import_breakup_texture()
    make_surface(MATERIAL_ROOT + "/M_PresentationGround", texture,
                 (0.12, 0.19, 0.075), 0.92, 0.0002, 0.20, vertex_weight=1.0)
    make_surface(MATERIAL_ROOT + "/M_PresentationRiverbank", texture,
                 (0.20, 0.17, 0.105), 0.95, 0.00065, 0.30, normal_strength=0.08)
    make_surface(MATERIAL_ROOT + "/M_PresentationRock", texture,
                 (0.24, 0.255, 0.23), 0.91, 0.00045, 0.36, normal_strength=0.16)
    make_surface(MATERIAL_ROOT + "/M_PresentationAsphalt", texture,
                 (0.035, 0.045, 0.052), 0.88, 0.003, 0.18, normal_strength=0.045)
    make_surface(MATERIAL_ROOT + "/M_PresentationSidewalk", texture,
                 (0.33, 0.34, 0.31), 0.86, 0.002, 0.14, normal_strength=0.025)
    make_water(MATERIAL_ROOT + "/M_PresentationWater")
    make_atlas()
    # Canonical legacy paths stay intact for the old ISM fallback. Their nodes
    # can be rooted by startup material references in UE 5.8; never delete them.
    # The continuous runtime surface hard-references the new presentation paths.
    MANIFEST["preserved_legacy_materials"] = [
        "/Game/Environment/Materials/M_TerrainCell",
        "/Game/Environment/Materials/M_AftershockWater",
    ]
    imported_mesh_manifest_and_usage()
    report = os.path.join(ROOT, "Saved", "PresentationValidation", "material_manifest.json")
    os.makedirs(os.path.dirname(report), exist_ok=True)
    with open(report, "w", encoding="utf-8") as stream:
        json.dump(MANIFEST, stream, ensure_ascii=False, indent=2)
    log("COMPLETE authored={} imported_ism_materials={} mesh_bounds={} report={}".format(
        len(MANIFEST["materials"]), len(MANIFEST["imported_materials"]), len(MANIFEST["meshes"]), report))


main()
