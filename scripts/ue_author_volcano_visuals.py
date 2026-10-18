"""Author original, shared volcano display materials inside UE5.8.

No terrain/hazard data, downloaded art, per-citizen materials, fluid solver or
historical-eruption reconstruction is generated. Re-running updates only tagged
nodes in these four NEW owned materials; existing disaster materials are intact.
Run after the C++ build, with the editor Python commandlet. Root coordinates UE.
"""
import json
import os
import unreal


ROOT = "/Game/FX/Aftershock/Volcano"
TAG = "AFTERSHOCK_VOLCANO_V1:"
L = unreal.MaterialEditingLibrary


def enum_value(cls, key):
    key = key.replace("_", "").upper()
    for name in dir(cls):
        if name.replace("_", "").upper() == key:
            return getattr(cls, name)
    raise RuntimeError("Missing enum {}.{}".format(cls, key))


def node(material, key, cls, **properties):
    matches = [n for n in L.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == TAG + key]
    if len(matches) > 1:
        raise RuntimeError("Duplicate owned node " + key)
    value = matches[0] if matches else L.create_material_expression(material, cls)
    if not value or value.get_class() != cls.static_class():
        raise RuntimeError("Owned node type mismatch " + key)
    value.set_editor_property("desc", TAG + key)
    for name, setting in properties.items():
        value.set_editor_property(name, setting)
    return value


def link(source, target, pin="", output=""):
    if not L.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Volcano material connection " + pin)


def output(material, value, prop):
    if not L.connect_material_property(value, "", prop):
        raise RuntimeError("Volcano material output " + str(prop))


def scalar(material, name, default):
    return node(material, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(default))


def custom(material, name, code, inputs, width=3):
    value = node(material, name, unreal.MaterialExpressionCustom, code=code,
                 output_type=enum_value(unreal.CustomMaterialOutputType,
                                        "CMOTFLOAT" + str(width)))
    bindings = []
    for name in inputs:
        binding = unreal.CustomInput()
        binding.set_editor_property("input_name", name)
        bindings.append(binding)
    value.set_editor_property("inputs", bindings)
    for name, source in inputs.items():
        link(source, value, name)
    return value


def begin(name, blend):
    path = ROOT + "/" + name
    material = unreal.load_asset(path)
    if material is None:
        unreal.EditorAssetLibrary.make_directory(ROOT)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Owned path is not a material " + path)
    material.set_editor_property("blend_mode", blend)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("two_sided", False)
    if blend == unreal.BlendMode.BLEND_TRANSLUCENT:
        material.set_editor_property("translucency_lighting_mode", enum_value(
            unreal.TranslucencyLightingMode, "TLMVOLUMETRICDIRECTIONAL"))
        material.set_editor_property("screen_space_reflections", False)
    L.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    return material


def inputs(material):
    return {
        "P": node(material, "WorldPosition", unreal.MaterialExpressionWorldPosition),
        "N": node(material, "WorldNormal", unreal.MaterialExpressionPixelNormalWS),
        "V": node(material, "CameraVector", unreal.MaterialExpressionCameraVectorWS),
        "C": node(material, "VertexData", unreal.MaterialExpressionVertexColor),
        "UV": node(material, "UV", unreal.MaterialExpressionTextureCoordinate),
        "TimeValue": node(material, "Time", unreal.MaterialExpressionTime),
        "EventEnvelope": scalar(material, "EventEnvelope", 0.0),
    }


def finish(material, metadata):
    errors = L.recompile_material(material)
    if errors:
        raise RuntimeError("Volcano material compilation failed: " + str(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Cannot save " + material.get_path_name())
    metadata["asset"] = material.get_path_name()
    metadata["compile_errors"] = 0
    unreal.log_warning("AFTERSHOCK_VOLCANO_MATERIAL_READY " + material.get_path_name())
    return metadata


manifest = {"schema_version": 1, "materials": [], "original_procedural_art": True,
            "terrain_writes": 0, "hazard_solver_added": False,
            "runtime_geometry": "canonical/versioned ground,1500positive lava cells,96spatter maximum",
            "geometry_note": "Small vent-spatter rim is art detail; the actual mountain comes from the versioned DEM.",
            "scientific_style_references": [
                "https://www.ingv.it/en/etna",
                "https://vo-tcs.ct.ingv.it/gateway/EtnaSupersite/about_etna.html",
                "https://www.usgs.gov/news/volcano-watch-pahoehoe-and-aa-lava-flows"]}

lava = begin("M_VolcanoLavaVolume", unreal.BlendMode.BLEND_MASKED)
data = inputs(lava)
data["Cooling"] = scalar(lava, "Cooling", 0.0)
data["FlowProgress"] = scalar(lava, "FlowProgress", 0.0)
output(lava, custom(lava, "BasaltCrust", r"""
float2 q=P.xy*.008;
float n=.5+.24*sin(q.x*1.31+sin(q.y*.83))+.16*cos(q.y*1.79-q.x*.37);
return float3(.018,.014,.012)+saturate(n)*float3(.022,.018,.015);
""", data), unreal.MaterialProperty.MP_BASE_COLOR)
output(lava, custom(lava, "MoltenCracks", r"""
// Advected texture follows the local ground downhill normal; the simulation's
// arrival map alone controls WHICH already-affected cells are revealed.
float2 downhill=N.xy/max(length(N.xy),.05);
float2 q=P.xy*.013-downhill*TimeValue*.20;
float f=sin(q.x*1.8+sin(q.y*.81))+sin(q.y*1.37+sin(q.x*.63));
float crack=pow(saturate(1.-abs(f)*2.8),2.);
float hot=crack*saturate(C.r)*saturate(EventEnvelope)*(1.-.96*saturate(Cooling));
return float3(8.,1.15,.025)*hot;
""", data), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
output(lava, custom(lava, "CanonicalArrivalReveal",
       "return step(saturate(C.g),saturate(FlowProgress)+.004);", data, 1),
       unreal.MaterialProperty.MP_OPACITY_MASK)
output(lava, scalar(lava, "CrustRoughness", .91), unreal.MaterialProperty.MP_ROUGHNESS)
manifest["materials"].append(finish(lava, {"kind": "closed_lava_volume", "blend": "masked",
                            "world_position_offset": False, "temporal_dither": False,
                            "arrival_source": "canonicalLavaArrivalFraction; legacyempty explicit fallback"}))

vent = begin("M_VolcanoVentRock", unreal.BlendMode.BLEND_OPAQUE)
data = inputs(vent)
output(vent, custom(vent, "PorousBasalt", r"""
float n=.5+.25*sin(P.x*.022+sin(P.y*.031))+.18*cos(P.y*.056-P.x*.019);
return lerp(float3(.018,.016,.014),float3(.095,.078,.065),saturate(n));
""", data), unreal.MaterialProperty.MP_BASE_COLOR)
output(vent, custom(vent, "InnerVentGlow",
       "return float3(3.2,.16,.006)*C.b*saturate(EventEnvelope);", data),
       unreal.MaterialProperty.MP_EMISSIVE_COLOR)
output(vent, scalar(vent, "RockRoughness", .96), unreal.MaterialProperty.MP_ROUGHNESS)
manifest["materials"].append(finish(vent, {"kind": "local_spatter_rim", "blend": "opaque",
                            "world_position_offset": False, "historical_crater_reconstruction": False}))

plume = begin("M_VolcanoPlume", unreal.BlendMode.BLEND_TRANSLUCENT)
data = inputs(plume)
data["AshDensity"] = scalar(plume, "AshDensity", .75)
output(plume, custom(plume, "ScatteringColor", r"""
return lerp(float3(.48,.47,.45),float3(.075,.069,.061),saturate(AshDensity));
""", data), unreal.MaterialProperty.MP_BASE_COLOR)
alpha = custom(plume, "ContinuousBillows", r"""
float2 q=float2(P.x+P.y*.37,P.z)*.0018-float2(TimeValue*.07,TimeValue*.22);
float n=.62+.18*sin(q.x*1.8+sin(q.y*1.3))+.12*cos(q.y*2.7+q.x*.8);
float edge=pow(saturate(abs(dot(normalize(N),normalize(V)))),.8);
float cap=smoothstep(0.,.08,UV.y)*(1.-smoothstep(.8,1.,UV.y));
return saturate(n*edge*cap*(.18+.36*saturate(AshDensity))*EventEnvelope);
""", data, 1)
fade = node(plume, "GroundContactFade", unreal.MaterialExpressionDepthFade,
            fade_distance_default=180.0, opacity_default=1.0)
link(alpha, fade, "Opacity")
output(plume, fade, unreal.MaterialProperty.MP_OPACITY)
output(plume, scalar(plume, "PlumeRoughness", 1.0), unreal.MaterialProperty.MP_ROUGHNESS)
manifest["materials"].append(finish(plume, {"kind": "single_wind_bent_plume_hull", "blend": "translucent",
                            "world_position_offset": False, "crossed_coplanar_cards": False}))

spatter = begin("M_VolcanoSpatter", unreal.BlendMode.BLEND_OPAQUE)
data = inputs(spatter)
instance = node(spatter, "InstanceHeat", unreal.MaterialExpressionPerInstanceCustomData,
                data_index=0, const_default_value=.5)
interpolated = node(spatter, "PixelHeat", unreal.MaterialExpressionVertexInterpolator)
link(instance, interpolated)
data["Heat"] = interpolated
output(spatter, custom(spatter, "SpatterCrust",
       "return float3(.042,.025,.016);", data), unreal.MaterialProperty.MP_BASE_COLOR)
output(spatter, custom(spatter, "CoolingSpatterEmission",
       "return float3(7.,.72,.012)*saturate(Heat)*saturate(EventEnvelope);", data),
       unreal.MaterialProperty.MP_EMISSIVE_COLOR)
output(spatter, scalar(spatter, "SpatterRoughness", .86), unreal.MaterialProperty.MP_ROUGHNESS)
manifest["materials"].append(finish(spatter, {"kind": "bounded_visual_ballistic_spatter",
                            "blend": "opaque", "world_position_offset": False,
                            "particles_maximum": 96, "actor_per_particle": False}))

directory = os.path.abspath(os.path.join(unreal.Paths.project_saved_dir(), "PresentationValidation"))
os.makedirs(directory, exist_ok=True)
with open(os.path.join(directory, "volcano_authoring.json"), "w", encoding="utf-8", newline="\n") as handle:
    json.dump(manifest, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
unreal.log_warning("AFTERSHOCK_VOLCANO_AUTHORING_COMPLETE materials=4 originalArt=1 terrainWrites=0")
