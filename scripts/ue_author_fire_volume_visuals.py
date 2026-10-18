"""Original shared fire-volume materials for the canonical burning-cell renderer.

Run once inside the UE editor after the project module builds. This authors
exactly four owned materials; it does not create a fire simulation, modify
maps, alter spread/damage, download assets or replace somebody else's graph.
Graph/save acceptance is separate from actual RHI quality/performance checks.
"""
import hashlib
import json
from pathlib import Path

import unreal


ROOT = "/Game/FX/Aftershock/Fire"
TAG = "AFTERSHOCK_FIRE_VOLUME_V1:"
LIB = unreal.MaterialEditingLibrary
ASSETS = unreal.EditorAssetLibrary


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def enum_value(cls, key):
    for name in dir(cls):
        if name.replace("_", "").upper() == key.replace("_", "").upper():
            return getattr(cls, name)
    raise RuntimeError("Missing reflected enum: " + key)


def node(material, label, cls, **properties):
    matches = [entry for entry in LIB.get_material_expressions(material)
               if str(entry.get_editor_property("desc")) == TAG + label]
    require(len(matches) <= 1, "Duplicate owned material node: " + label)
    expression = matches[0] if matches else LIB.create_material_expression(material, cls)
    require(expression is not None and expression.get_class() == cls.static_class(),
            "Owned node class mismatch: " + label)
    expression.set_editor_property("desc", TAG + label)
    for key, value in properties.items():
        expression.set_editor_property(key, value)
    return expression


def link(source, target, pin=""):
    require(LIB.connect_material_expressions(source, "", target, pin), "Material connection failed: " + pin)


def output(material, expression, prop):
    require(LIB.connect_material_property(expression, "", prop), "Material output failed: " + str(prop))


def scalar(material, name, default):
    return node(material, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(default))


def custom(material, label, code, inputs, width):
    expression = node(material, label, unreal.MaterialExpressionCustom, code=code,
                      output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT" + str(width)))
    bindings = []
    for name in inputs:
        field = unreal.CustomInput()
        field.set_editor_property("input_name", name)
        bindings.append(field)
    expression.set_editor_property("inputs", bindings)
    for name, source in inputs.items():
        link(source, expression, name)
    return expression


def pixel_data(material, label, index, default):
    instance = node(material, label, unreal.MaterialExpressionPerInstanceCustomData,
                    data_index=index, const_default_value=default)
    result = node(material, label + "ToPixel", unreal.MaterialExpressionVertexInterpolator)
    link(instance, result)
    return result


def create(name, smoke=False):
    path = ROOT + "/" + name
    material = unreal.load_asset(path)
    if material is None:
        require(ASSETS.does_directory_exist(ROOT) or ASSETS.make_directory(ROOT), "Cannot create owned fire folder")
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    require(isinstance(material, unreal.Material), "Owned fire path is not a material: " + path)
    require(material.get_path_name() == path + "." + name, "Redirected fire material: " + path)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT
                                 if smoke else unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", False)
    material.set_editor_property("screen_space_reflections", False)
    if smoke:
        material.set_editor_property("translucency_lighting_mode", enum_value(
            unreal.TranslucencyLightingMode, "TLMVOLUMETRICDIRECTIONAL"))
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    return material


def inputs(material):
    return {
        "P": node(material, "WorldPosition", unreal.MaterialExpressionWorldPosition),
        "UV": node(material, "HullUV", unreal.MaterialExpressionTextureCoordinate, coordinate_index=0),
        "N": node(material, "WorldNormal", unreal.MaterialExpressionPixelNormalWS),
        "V": node(material, "CameraVector", unreal.MaterialExpressionCameraVectorWS),
        "Strength": pixel_data(material, "InstanceStrength", 0, 0.5),
        "Seed": pixel_data(material, "InstancePhase", 1, 0.0),
        "Life": pixel_data(material, "InstanceLifeOpacity", 2, 1.0),
        "Clock": scalar(material, "FireVisualClock", 0.0),
        "Envelope": scalar(material, "EventEnvelope", 0.0),
    }


def connect_alpha(material, rgba, distance):
    alpha = node(material, "Alpha", unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
    link(rgba, alpha)
    fade = node(material, "OpaqueContactFade", unreal.MaterialExpressionDepthFade,
                fade_distance_default=distance, opacity_default=1.0)
    link(alpha, fade, "Opacity")
    output(material, fade, unreal.MaterialProperty.MP_OPACITY)
    return fade


def save(material, expected_outputs, kind):
    for prop, expression in expected_outputs.items():
        require(LIB.get_material_property_input_node(material, prop) == expression,
                "Fire material output readback differs: " + str(prop))
    errors = LIB.recompile_material(material)
    require(not errors, "Fire material compile request failed: " + str(errors))
    require(ASSETS.save_loaded_asset(material, only_if_is_dirty=False), "Cannot save fire material")
    return {"asset": material.get_path_name(), "kind": kind, "explicit_save": True,
            "node_output_readback": True, "actor_per_particle": False,
            "world_position_offset": False, "live_rhi_verified": False}


def flame(name, core):
    material = create(name)
    data = inputs(material)
    data["Core"] = scalar(material, "Core", 1.0 if core else 0.0)
    rgba = custom(material, "AdvectedFlameShell", r"""
// Continuous intensity on a closed tongue hull. The hull/instance trajectory
// carries volume and wind lean; no crossed billboard or screen-space dither.
float phase=Seed*6.2831853;
float h=saturate(UV.y);
float3 q=P*.012+float3(phase,phase*.71,-Clock*3.7);
float n=.54+.21*sin(q.x+sin(q.z*1.1))+.16*sin(q.y*1.6-q.z*1.3)
         +.08*cos(q.x*3.1+q.y*2.7-q.z*2.2);
float tongue=smoothstep(.12,.62,n-h*.22);
float face=smoothstep(.01,.42,abs(dot(normalize(N),normalize(V))));
float foot=smoothstep(0.,.025,h),tip=1.-smoothstep(.88,1.,h);
float alpha=tongue*face*foot*tip*(.45+.30*saturate(Strength))*saturate(Envelope)*saturate(Life);
float heat=saturate(1.-h*1.05+Core*.12+n*.09);
float3 red=float3(3.1,.085,.002),yellow=float3(6.2,1.45,.015),white=float3(7.8,3.7,.30);
float3 color=lerp(red,yellow,smoothstep(.12,.78,heat));
color=lerp(color,white,smoothstep(.87,1.,heat)*(.12+.42*Core));
return float4(color,alpha);
""", data, 4)
    rgb = node(material, "RGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    link(rgba, rgb)
    output(material, rgb, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    alpha = connect_alpha(material, rgba, 24.0)
    return save(material, {unreal.MaterialProperty.MP_EMISSIVE_COLOR: rgb,
                           unreal.MaterialProperty.MP_OPACITY: alpha}, "hot_core_hull" if core else "outer_flame_hull")


def smoke():
    material = create("M_FireSmokeVolume", smoke=True)
    data = inputs(material)
    rgba = custom(material, "AnimatedSmokeDensity", r"""
float phase=Seed*6.2831853;
float3 q=P*.0032+float3(phase,phase*.37,-Clock*.55);
float n=.55+.20*sin(q.x+sin(q.z*.81))+.15*cos(q.y*1.33-q.z*1.7)
         +.08*sin(q.x*2.8+q.y*1.9-q.z*.6);
float face=smoothstep(.008,.34,abs(dot(normalize(N),normalize(V))));
float caps=smoothstep(.0,.045,UV.y)*(1.-smoothstep(.94,1.,UV.y));
// Three overlapping elongated billows form one readable column. Fade actual
// density with per-instance age; the geometry never collapses into smoke beads.
float density=saturate(.74+.23*Strength)*(.58+.42*saturate(n));
float3 thin=float3(.18,.18,.17),thick=float3(.025,.023,.021);
float3 color=lerp(thin,thick,saturate(Strength)*.55+n*.20);
return float4(color,face*caps*density*saturate(Envelope)*saturate(Life));
""", data, 4)
    rgb = node(material, "RGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    link(rgba, rgb)
    output(material, rgb, unreal.MaterialProperty.MP_BASE_COLOR)
    alpha = connect_alpha(material, rgba, 100.0)
    output(material, scalar(material, "SmokeRoughness", 1.0), unreal.MaterialProperty.MP_ROUGHNESS)
    return save(material, {unreal.MaterialProperty.MP_BASE_COLOR: rgb,
                           unreal.MaterialProperty.MP_OPACITY: alpha}, "wind_advected_lit_billow_hull")


def ember():
    material = create("M_FireEmber")
    data = inputs(material)
    rgba = custom(material, "GlowingEmber", r"""
float edge=smoothstep(.04,.45,abs(dot(normalize(N),normalize(V))));
float flicker=.84+.16*sin(Clock*9.+Seed*17.);
float caps=smoothstep(0.,.12,UV.y)*(1.-smoothstep(.75,1.,UV.y));
return float4(float3(8.,1.15,.015)*flicker,edge*caps*saturate(Envelope)*saturate(Life));
""", data, 4)
    rgb = node(material, "RGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
    link(rgba, rgb)
    output(material, rgb, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    alpha = connect_alpha(material, rgba, 8.0)
    return save(material, {unreal.MaterialProperty.MP_EMISSIVE_COLOR: rgb,
                           unreal.MaterialProperty.MP_OPACITY: alpha}, "bounded_visual_ember")


def main():
    source = Path(__file__).resolve()
    target = source.parents[1] / "Saved/PresentationValidation/fire_volume_material_authoring.json"
    report = {"schema_version": 1, "recipe_version": 2, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "per_instance_custom_data": {"strength": 0, "phase": 1, "life_opacity": 2},
              "original_procedural_art": True, "simulation_mutation": False,
              "live_rhi_verified": False, "gpu_performance_verified": False,
              "scope": "Native material graph output readback, compile request and explicit save only.",
              "results": [], "valid": False, "failed": 0}
    try:
        report["results"] = [flame("M_FireFlameVolume", False), flame("M_FireCoreVolume", True), smoke(), ember()]
        report["valid"] = len(report["results"]) == 4
    except Exception as error:
        report["failed"] = 1
        report["error"] = str(error)
        raise
    finally:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    unreal.log("AFTERSHOCK_FIRE_VOLUME_AUTHORING materials=4 explicitSave=1 liveRHIValidated=0 performanceMeasured=0")


if __name__ == "__main__":
    main()
