"""Repeatable disaster material authoring inside Unreal Editor.

Quake fissures use tagged/reused nodes and per-instance custom data index0.
Flood/tsunami delegate to canonical recipes without fixed water panners, extra
engine textures, depth/arrival/WPO changes or per-pixel lighting/SSR upgrades.
Graph authoring/readback/compile/save is separate from live RHI, cold loading,
event binding and performance acceptance. No downloads, map or physics edits.
"""
from datetime import datetime, timezone
import json
from pathlib import Path
import runpy
import unreal

LIB = unreal.MaterialEditingLibrary
ASSETS = unreal.EditorAssetLibrary
REPO = Path(__file__).resolve().parents[1]
ROOT = "/Game/FX/Aftershock/Materials"
TAG = "AFTERSHOCK_QUAKE_FISSURE_V1:"
FLOOD_TAG = "AFTERSHOCK_TERRAIN_FLOOD_VOLUME_V1:"
COASTAL_TAG = "AFTERSHOCK_COASTAL_VISUAL_V1:"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def enum_value(enum, normalized):
    for name in dir(enum):
        if name.replace("_", "").upper() == normalized:
            return getattr(enum, name)
    raise RuntimeError("Missing reflected enum: " + normalized)


def tagged_node(material, tag, label):
    matches = [n for n in LIB.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == tag + label]
    require(len(matches) == 1, "Missing/duplicate owned node: " + tag + label)
    return matches[0]


def node(material, label, cls, **properties):
    matches = [n for n in LIB.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == TAG + label]
    require(len(matches) <= 1, "Duplicate fissure node: " + label)
    expression = matches[0] if matches else LIB.create_material_expression(material, cls)
    require(expression is not None and expression.get_class() == cls.static_class(),
            "Fissure node class mismatch: " + label)
    expression.set_editor_property("desc", TAG + label)
    for key, value in properties.items():
        expression.set_editor_property(key, value)
    return expression


def link(source, target, pin=""):
    require(LIB.connect_material_expressions(source, "", target, pin),
            "Disaster material link failed: " + pin)


def output(material, expression, prop):
    require(LIB.connect_material_property(expression, "", prop),
            "Disaster material output failed: " + str(prop))


def load_exact_material(path):
    material = unreal.load_asset(path)
    expected = path + "." + path.rsplit("/", 1)[1]
    require(isinstance(material, unreal.Material) and material.get_path_name() == expected,
            "Missing, foreign-class or redirected material: " + path)
    return material


def compile_and_save(material):
    errors = LIB.recompile_material(material)
    require(not errors, "Material compile request reported errors: " + str(errors))
    require(ASSETS.save_loaded_asset(material, only_if_is_dirty=False),
            "Explicit material save failed: " + material.get_path_name())


def validate_outputs(material, tag, properties):
    for prop, label in properties.items():
        require(LIB.get_material_property_input_node(material, prop) == tagged_node(material, tag, label),
                "Canonical material output differs: " + label)


def validate_surface_water(material):
    require(material.get_editor_property("screen_space_reflections") is False,
            "Canonical water unexpectedly enables screen-space reflections")
    require(material.get_editor_property("translucency_lighting_mode") ==
            enum_value(unreal.TranslucencyLightingMode, "TLMSURFACE"),
            "Canonical water lighting mode differs")


def create_quake_fissure_material():
    path = ROOT + "/M_DisasterQuakeFissure"
    if not ASSETS.does_asset_exist(path):
        require(ASSETS.does_directory_exist(ROOT) or ASSETS.make_directory(ROOT),
                "Cannot create owned disaster material folder")
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            "M_DisasterQuakeFissure", ROOT, unreal.Material, unreal.MaterialFactoryNew())
        require(asset is not None, "Cannot create fissure material")
    material = load_exact_material(path)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("screen_space_reflections", False)
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)

    # Retain old untagged nodes disconnected on the first migration. Later
    # runs reuse these same eight owned nodes instead of growing the graph.
    uv = node(material, "UV", unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
    intensity = node(material, "CrackIntensity", unreal.MaterialExpressionPerInstanceCustomData,
                     data_index=0, const_default_value=0.0)
    interpolator = node(material, "IntensityInterpolator", unreal.MaterialExpressionVertexInterpolator)
    link(intensity, interpolator)
    fissure = node(material, "FissureRGBA", unreal.MaterialExpressionCustom,
                   code=r"""
float intensity=saturate(Intensity);
if(intensity<=.0001) return float4(0,0,0,0);
float2 u=UV;
float crackLine=(u.y-.5)+sin(u.x*14.+sin(u.x*31.)*.4)*.12+sin(u.x*47.)*.035;
float halfWidth=.13*intensity+.02;
float dist=abs(crackLine);
if(dist>halfWidth) return float4(0,0,0,0);
float edge=smoothstep(halfWidth*.42,halfWidth,dist);
float3 abyss=float3(.015,.012,.010);
float3 crumb=float3(.16,.155,.145)+float3(.04,.035,.03)*sin(u.x*65.);
return float4(lerp(abyss,crumb,edge),1.);
""", output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT4"))
    fields = []
    for name in ("UV", "Intensity"):
        field = unreal.CustomInput()
        field.set_editor_property("input_name", name)
        fields.append(field)
    fissure.set_editor_property("inputs", fields)
    link(uv, fissure, "UV")
    link(interpolator, fissure, "Intensity")
    rgb = node(material, "RGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    alpha = node(material, "Alpha", unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
    link(fissure, rgb)
    link(fissure, alpha)
    roughness = node(material, "Roughness", unreal.MaterialExpressionConstant, r=.94)
    specular = node(material, "Specular", unreal.MaterialExpressionConstant, r=.12)
    outputs = {unreal.MaterialProperty.MP_BASE_COLOR: (rgb, "RGB"),
               unreal.MaterialProperty.MP_OPACITY_MASK: (alpha, "Alpha"),
               unreal.MaterialProperty.MP_ROUGHNESS: (roughness, "Roughness"),
               unreal.MaterialProperty.MP_SPECULAR: (specular, "Specular")}
    for prop, (expression, _) in outputs.items():
        output(material, expression, prop)
    validate_outputs(material, TAG, {prop: label for prop, (_, label) in outputs.items()})
    require(intensity.get_editor_property("data_index") == 0,
            "Fissure per-instance custom-data index differs")
    require(sum(str(n.get_editor_property("desc")).startswith(TAG)
                for n in LIB.get_material_expressions(material)) == 8,
            "Unexpected fissure owned-node count")
    compile_and_save(material)
    unreal.log("AFTERSHOCK_QUAKE_FISSURE_AUTHORED taggedNodes=8 customDataIndex=0 liveRHIValidated=0")
    return {"material": path, "owned_nodes": 8, "tagged_reuse": True,
            "per_instance_custom_data_index": 0, "explicit_save": True}


def canonical_recipe(name):
    path = REPO / "scripts" / name
    require(path.is_file(), "Canonical disaster recipe missing: " + name)
    # Check both delegates before the first asset mutation in main().
    compile(path.read_text(encoding="utf-8"), str(path), "exec")
    return path


def upgrade_flood_volume_material():
    recipe = canonical_recipe("ue_upgrade_flood_volume.py")
    # The canonical flood recipe authors/compiles/saves at module scope.
    runpy.run_path(str(recipe), run_name="aftershock_canonical_flood_authoring")
    path = ROOT + "/M_DisasterFloodVolume"
    material = load_exact_material(path)
    validate_surface_water(material)
    validate_outputs(material, FLOOD_TAG, {
        unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET: "BoundedColumnRiseAndRipples",
        unreal.MaterialProperty.MP_BASE_COLOR: "OpticsRGB",
        unreal.MaterialProperty.MP_OPACITY: "OpaqueTerrainAndBuildingContactFade",
        unreal.MaterialProperty.MP_NORMAL: "FlowingAnalyticNormal"})
    for label, channel in (("ReconstructedSurfaceFlow", 0), ("ExactHydraulicCentimetres", 1),
                           ("RiverCoverageAndExactArrival", 2)):
        require(tagged_node(material, FLOOD_TAG, label).get_editor_property("coordinate_index") == channel,
                "Canonical flood UV route differs: " + label)
    for label in ("FloodEncodingCm", "FloodPhysicalArrivalEnabled", "FloodArrivalProgress", "FloodPhysicalDataEnabled"):
        tagged_node(material, FLOOD_TAG, label)
    return {"material": path, "canonical_recipe": recipe.relative_to(REPO).as_posix(),
            "canonical_outputs_uv_and_arrival_readback": True, "explicit_save": True}


def upgrade_tsunami_material():
    recipe = canonical_recipe("ue_author_coastal_visuals.py")
    namespace = runpy.run_path(str(recipe), run_name="aftershock_canonical_coastal_library")
    author = namespace.get("make_tsunami")
    require(callable(author), "Canonical tsunami author function missing")
    # Invoke only tsunami, not the sea or either dam material.
    material = author()
    path = ROOT + "/M_DisasterTsunamiWave"
    require(material == load_exact_material(path), "Canonical tsunami returned a different material")
    validate_surface_water(material)
    validate_outputs(material, COASTAL_TAG, {
        unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET: "AdvancingMarineCrest",
        unreal.MaterialProperty.MP_BASE_COLOR: "WaveColor",
        unreal.MaterialProperty.MP_OPACITY: "LandContact",
        unreal.MaterialProperty.MP_NORMAL: "CrestAnalyticNormal"})
    for label in ("CanonicalWaveData", "PropagationDirection", "TsunamiProgress", "TsunamiDrain", "CrestHeightCm"):
        tagged_node(material, COASTAL_TAG, label)
    compile_and_save(material)
    return {"material": path, "canonical_recipe": recipe.relative_to(REPO).as_posix(),
            "canonical_outputs_flow_and_arrival_readback": True, "explicit_save": True}


def main():
    report = {"schema_version": 1, "utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Editor graph authoring/readback, compile request and explicit saves only; not live RHI, cold-process, event binding or performance acceptance.",
              "results": [], "failed": 0, "valid": False,
              "live_rhi_verified": False, "gpu_performance_verified": False}
    target = REPO / "Saved/PresentationValidation/disaster_fx_material_authoring.json"
    try:
        canonical_recipe("ue_upgrade_flood_volume.py")
        canonical_recipe("ue_author_coastal_visuals.py")
        for action in (create_quake_fissure_material, upgrade_flood_volume_material, upgrade_tsunami_material):
            report["results"].append(action())
        report["valid"] = len(report["results"]) == 3
    except Exception as error:
        report["failed"] = 1
        report["error"] = str(error).replace(str(REPO), "<repo>")
        raise
    finally:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    unreal.log("AFTERSHOCK_DISASTER_MATERIAL_AUTHORING count=3 failed=0 scope=EditorGraphAndSave liveRHIValidated=0 performanceMeasured=0")
    return report


if __name__ == "__main__":
    main()
