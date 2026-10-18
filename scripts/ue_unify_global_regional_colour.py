"""Optional V3-only shared EOX2016 BaseColor wrapper for globe and regional views.

Run after the original globe/regional-style recipes and global-image importer.
Before rerunning upstream authoring, run this script once with
-AftershockGlobalColourRestoreOnly to reconnect its preserved Original outputs.
This prevents another recipe from wrapping this wrapper back into itself.
No original expression is deleted or modified; four output wrappers preserve
their exact inactive inputs. Normal/Opacity/OpacityMask/WPO and all geographic
source-mask expressions stay intact.
EOX2016 is historical imagery, not current imagery or a measured PBR albedo.
"""
from pathlib import Path
import json
import re
import unreal

L = unreal.MaterialEditingLibrary
A = unreal.EditorAssetLibrary
ROOT = Path(__file__).resolve().parents[1]
TAG = "AFTERSHOCK_GLOBAL_REGIONAL_COLOUR_V1:"
STYLE_TAG = "AFTERSHOCK_REGIONAL_STYLE_V1:"
GLOBE_TAG = "AFTERSHOCK_REGIONAL_GLOBE_V1:"
GLOBAL_PATH = "/Game/Environment/Textures/Regional/Macro/T_GlobalMacro_EOX2016"
MATERIALS = (
    "/Game/Environment/Materials/Presentation/M_AftershockPlanetSurface",
    "/Game/Environment/Materials/Presentation/M_PresentationGroundPBR",
    "/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend",
)
WRAPPED = {
    "BaseColor": unreal.MaterialProperty.MP_BASE_COLOR,
    "Roughness": unreal.MaterialProperty.MP_ROUGHNESS,
    "Specular": unreal.MaterialProperty.MP_SPECULAR,
    "Emissive": unreal.MaterialProperty.MP_EMISSIVE_COLOR,
}
UNTOUCHED = {
    "Normal": unreal.MaterialProperty.MP_NORMAL,
    "Metallic": unreal.MaterialProperty.MP_METALLIC,
    "Opacity": unreal.MaterialProperty.MP_OPACITY,
    "OpacityMask": unreal.MaterialProperty.MP_OPACITY_MASK,
    "WPO": unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,
}


def enum_value(cls, text):
    text = text.replace("_", "").upper()
    for name in dir(cls):
        if name.replace("_", "").upper() == text:
            return getattr(cls, name)
    raise RuntimeError("Missing UE enum " + text)


def node(material, name, cls, **values):
    found = [expression for expression in L.get_material_expressions(material)
             if str(expression.get_editor_property("desc")) == TAG + name]
    if len(found) > 1:
        raise RuntimeError("Duplicate global-colour node " + name)
    expression = found[0] if found else L.create_material_expression(material, cls)
    if not expression or expression.get_class() != cls.static_class():
        raise RuntimeError("Unexpected global-colour node class " + name)
    expression.set_editor_property("desc", TAG + name)
    for key, value in values.items():
        expression.set_editor_property(key, value)
    return expression


def tagged(material, description):
    found = [expression for expression in L.get_material_expressions(material)
             if str(expression.get_editor_property("desc")) == description]
    if len(found) != 1:
        raise RuntimeError("Upstream material recipe is missing/ambiguous: " + description)
    return found[0]


def link(source, target, pin="", output=""):
    if not L.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Cannot connect global-colour input " + pin)


def connection(material, prop):
    return (L.get_material_property_input_node(material, prop),
            str(L.get_material_property_input_node_output_name(material, prop)))


def original_input(material, expression):
    names = list(L.get_material_expression_input_names(expression))
    inputs = list(L.get_inputs_for_material_expression(material, expression))
    if "Original" not in names or names.index("Original") >= len(inputs):
        raise RuntimeError("Global-colour wrapper lost its Original input")
    source = inputs[names.index("Original")]
    default_emissive = (source is not None and str(source.get_editor_property("desc")) == TAG + "UnconnectedEmissiveDefault"
                        and str(expression.get_editor_property("desc")) == TAG + "WrapEmissive")
    if source is None or (str(source.get_editor_property("desc")).startswith(TAG) and not default_emissive):
        raise RuntimeError("Invalid/cyclic preserved global-colour input")
    result = L.get_input_node_output_name_for_material_expression(expression, source)
    if isinstance(result, (tuple, list)):
        if result and isinstance(result[0], bool) and not result[0]:
            raise RuntimeError("Cannot read preserved output selection")
        outputs = [item for item in result if isinstance(item, str)]
        if len(outputs) != 1:
            raise RuntimeError("Ambiguous preserved output selection")
        output = outputs[0]
    elif isinstance(result, str):
        output = result
    else:
        raise RuntimeError("Cannot read preserved output selection")
    return source, output


def restore_original(material):
    restored = False
    for label, prop in WRAPPED.items():
        source, output = connection(material, prop)
        if source and str(source.get_editor_property("desc")) == TAG + "Wrap" + label:
            source, output = original_input(material, source)
            unconnected = label == "Emissive" and str(source.get_editor_property("desc")) == TAG + "UnconnectedEmissiveDefault"
            success = L.disconnect_material_property(material, prop) if unconnected else L.connect_material_property(source, output, prop)
            if not success:
                raise RuntimeError("Cannot restore upstream " + label)
            restored = True
    return restored


def scalar(material, name, value):
    return node(material, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(value))


def vector(material, name, value):
    return node(material, name, unreal.MaterialExpressionVectorParameter,
                parameter_name=name, default_value=unreal.LinearColor(*value, 0.))


PLANET = r"""
[branch] if(Ready<.5) return float4(0,0,0,0);
float3 N=normalize(P-Center);
float3 E=N.x*GeoEast+N.y*GeoSouth+N.z*GeoUp;
float2 uv=float2(atan2(E.y,E.x)/6.28318530718+.5,.5-asin(clamp(E.z,-1.,1.))/3.14159265359);
uv.y=clamp(uv.y,.5/4096.,1.-.5/4096.);
// The same modest artistic exposure is applied to both render layers.
float3 photograph=saturate(Texture2DSample(GlobalTexture,GlobalTextureSampler,uv).rgb*1.03);
float longitude=(uv.x-.5)*360.,latitude=(.5-uv.y)*180.;

// One intermediate, source-backed photograph covers the surrounding geography.
// Uniform-disabled by default; no source coastline or gameplay height is inferred.
[branch] if(SurroundReady>.5){
 float2 span=max(SurroundGeoMax.xy-SurroundGeoMin.xy,float2(.00001,.00001));
 float2 surroundUV=float2((longitude-SurroundGeoMin.x)/span.x,(SurroundGeoMax.y-latitude)/span.y);
 float edge=min(min(surroundUV.x,1.-surroundUV.x),min(surroundUV.y,1.-surroundUV.y));
 float blend=smoothstep(0.,.18,edge);
 // Sample outside the varying edge condition so implicit derivatives stay valid.
 float3 surround=saturate(Texture2DSample(SurroundTexture,SurroundTextureSampler,saturate(surroundUV)).rgb*1.03);
 float land=step((OriginalOceanRoughness+OriginalLandRoughness)*.5,PlanetRoughnessAuthority);
 photograph=lerp(photograph,surround,blend*land);
}

return float4(photograph,1.);
"""

REGIONAL = r"""
[branch] if(Ready<.5) return float4(0,0,0,0);
float2 local=(P.xy-WorldMin.xy)/max(WorldMax.xy-WorldMin.xy,float2(1.,1.));
float lon=lerp(GeoMin.x,GeoMax.x,local.x);
float lat=lerp(GeoMax.y,GeoMin.y,local.y);
float2 uv=float2((lon+180.)/360.,(90.-lat)/180.);
uv.y=clamp(uv.y,.5/4096.,1.-.5/4096.);
float3 photograph=saturate(Texture2DSample(GlobalTexture,GlobalTextureSampler,uv).rgb*1.03);
float longitude=lon,latitude=lat;

// One intermediate, source-backed photograph covers the surrounding geography.
// Uniform-disabled by default; no source coastline or gameplay height is inferred.
[branch] if(SurroundReady>.5){
 float2 span=max(SurroundGeoMax.xy-SurroundGeoMin.xy,float2(.00001,.00001));
 float2 surroundUV=float2((longitude-SurroundGeoMin.x)/span.x,(SurroundGeoMax.y-latitude)/span.y);
 float edge=min(min(surroundUV.x,1.-surroundUV.x),min(surroundUV.y,1.-surroundUV.y));
 float blend=smoothstep(0.,.18,edge);
 // Sample outside the varying edge condition so implicit derivatives stay valid.
 float3 surround=saturate(Texture2DSample(SurroundTexture,SurroundTextureSampler,saturate(surroundUV)).rgb*1.03);
 photograph=lerp(photograph,surround,blend*(1.-saturate(Water)));
}

float edge=min(min(local.x,1.-local.x),min(local.y,1.-local.y));
// The wider photographic join and ROI-relative far fade remove a sharp
// high-resolution rectangle on the coarser global image. Geographic water,
// heights, opacity and WPO are not inferred from either photograph.
float interior=smoothstep(0.,.05,edge);
float interiorLand=interior*(1.-saturate(Water));
float photoInterior=smoothstep(0.,.20,edge);
float eyeDistance=length(P-Camera);
float roiSpan=max(max(WorldMax.x-WorldMin.x,WorldMax.y-WorldMin.y),1.);
float macroFarFade=1.-smoothstep(roiSpan*2.0,roiSpan*4.0,eyeDistance);
float farFactor=smoothstep(40000.,200000.,eyeDistance);
[branch] if(MacroReady>.5){
 float3 localPhoto=saturate(Texture2DSample(RegionalPhoto,RegionalPhotoSampler,saturate(local)).rgb*1.03);
 photograph=lerp(photograph,localPhoto,photoInterior*(1.-saturate(Water))*macroFarFade);
}
// Near streets keep the exact original material; far land and all water share
// the photographic optical targets with the same smooth spatial edge blend.
return float4(photograph,lerp(1.,farFactor,interiorLand));
"""


def read_shared_optics():
    """Read existing planet targets, instead of inventing another water tone."""
    planet = unreal.load_asset(MATERIALS[0])
    if not isinstance(planet, unreal.Material):
        raise RuntimeError("Original planet material missing")
    rough = tagged(planet, GLOBE_TAG + "LandOceanRoughness")
    match = re.fullmatch(r"\s*return\s+lerp\(\s*([.0-9]+)\s*,\s*([.0-9]+)\s*,\s*step\(EarthMap\.b\*\.85,EarthMap\.g\)\);\s*",
                         str(rough.get_editor_property("code")))
    if not match:
        raise RuntimeError("Original planet roughness contract changed; inspect its targets")
    ocean, land = map(float, match.groups())
    specular = float(tagged(planet, GLOBE_TAG + "Specular").get_editor_property("r"))
    night = float(tagged(planet, GLOBE_TAG + "AmbientReadability").get_editor_property("default_value"))
    if not all(0. <= value <= 1. for value in (ocean, land, specular, night)):
        raise RuntimeError("Invalid original planet optical targets")
    return {"ocean_roughness": ocean, "land_roughness": land, "specular": specular, "night": night}


def make_custom(material, label, code, fields, width):
    expression = node(material, label, unreal.MaterialExpressionCustom, code=code,
                      output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT" + str(width)))
    custom_inputs = []
    for name in fields:
        field = unreal.CustomInput(); field.set_editor_property("input_name", name); custom_inputs.append(field)
    expression.set_editor_property("inputs", custom_inputs)
    for name, source in fields.items():
        link(source[0], expression, name, source[1]) if isinstance(source, tuple) else link(source, expression, name)
    return expression


def author(path, texture, optics):
    material = unreal.load_asset(path)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Required existing material missing " + path)
    before = {name: connection(material, prop) for name, prop in UNTOUCHED.items()}
    originals = {}
    for label, prop in WRAPPED.items():
        source, selected = connection(material, prop)
        if source and str(source.get_editor_property("desc")) == TAG + "Wrap" + label:
            source, selected = original_input(material, source)
        if source is None and label == "Emissive":
            # A disconnected material emissive has the exact zero default.
            # Its tagged sentinel lets restore remove the temporary connection.
            source = node(material, "UnconnectedEmissiveDefault", unreal.MaterialExpressionConstant3Vector,
                          constant=unreal.LinearColor(0., 0., 0., 0.))
            selected = ""
        own_default = label == "Emissive" and source is not None and str(source.get_editor_property("desc")) == TAG + "UnconnectedEmissiveDefault"
        if source is None or (str(source.get_editor_property("desc")).startswith(TAG) and not own_default):
            raise RuntimeError("Cannot preserve original optical output " + label)
        originals[label] = source, selected
    original, selected = originals["BaseColor"]
    planet = path.endswith("M_AftershockPlanetSurface")
    expected = GLOBE_TAG + "ContinuousBiome" if planet else STYLE_TAG + "WrapBaseColor"
    if original is None or str(original.get_editor_property("desc")) != expected:
        raise RuntimeError("Run the original globe/style recipe first; unexpected BaseColor " + path)
    fields = {
        "P": node(material, "WorldPosition", unreal.MaterialExpressionWorldPosition,
                  world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets, "WPTEXCLUDEALLSHADEROFFSETS")),
        "Ready": scalar(material, "GlobalImageryReady", 0.),
        "GlobalTexture": node(material, "GlobalImageryTexture", unreal.MaterialExpressionTextureObjectParameter,
                              parameter_name="GlobalImageryTexture", texture=texture,
                              sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
    }
    fields.update({
        "SurroundReady": scalar(material, "RegionalSurroundReady", 0.),
        "SurroundTexture": node(material, "RegionalSurroundTexture", unreal.MaterialExpressionTextureObjectParameter,
                               parameter_name="RegionalSurroundTexture", texture=texture,
                               sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
        "SurroundGeoMin": vector(material, "RegionalSurroundGeoMin", (-180., -90., 0.)),
        "SurroundGeoMax": vector(material, "RegionalSurroundGeoMax", (180., 90., 0.)),
    })
    if planet:
        fields.update({"Center": tagged(material, GLOBE_TAG + "PlanetCenter"),
                       "GeoEast": tagged(material, GLOBE_TAG + "GeoEast"),
                       "GeoSouth": tagged(material, GLOBE_TAG + "GeoSouth"),
                       "GeoUp": tagged(material, GLOBE_TAG + "GeoUp"),
                       "PlanetRoughnessAuthority": tagged(material, GLOBE_TAG + "LandOceanRoughness"),
                       "OriginalOceanRoughness": scalar(material, "GlobalOceanRoughness", optics["ocean_roughness"]),
                       "OriginalLandRoughness": scalar(material, "GlobalLandRoughness", optics["land_roughness"])})
    else:
        if path.endswith("M_AftershockRegionalBlend"):
            fields["Water"] = tagged(material, GLOBE_TAG + "RegionalWaterCoverage")
        else:
            water = node(material, "ExistingGroundWaterR", unreal.MaterialExpressionComponentMask,
                         r=True, g=False, b=False, a=False)
            link(tagged(material, STYLE_TAG + "GroundExistingSourceLandUse"), water)
            fields["Water"] = water
        fields.update({"WorldMin": vector(material, "GlobalRegionWorldMin", (0., 0., 0.)),
                       "WorldMax": vector(material, "GlobalRegionWorldMax", (1., 1., 0.)),
                       "GeoMin": vector(material, "GlobalRegionWGS84Min", (-180., -90., 0.)),
                       "GeoMax": vector(material, "GlobalRegionWGS84Max", (180., 90., 0.)),
                       "Camera": node(material, "CameraPosition", unreal.MaterialExpressionCameraPositionWS),
                       "MacroReady": tagged(material, STYLE_TAG + "RegionalMacroReady"),
                       "RegionalPhoto": tagged(material, STYLE_TAG + "RegionalMacroTexture")})
        photo_node = fields["RegionalPhoto"]
        if (photo_node.get_class() != unreal.MaterialExpressionTextureObjectParameter.static_class()
                or str(photo_node.get_editor_property("parameter_name")) != "RegionalMacroTexture"
                or not isinstance(photo_node.get_editor_property("texture"), unreal.Texture2D)):
            raise RuntimeError("Existing regional macro texture parameter is invalid")
    shared = make_custom(material, "SharedPhotoAndOpticBlend", PLANET if planet else REGIONAL, fields, 4)
    common = {"Ready": fields["Ready"], "PhotoBlend": shared}
    # Optical classification comes from the existing geographic authority.
    # A blue/hazy forest photograph is not evidence that the ground is water.
    if planet:
        roughness_code = "return Original;"
        roughness_fields = {"Original": originals["Roughness"]}
    else:
        roughness_code = r"""
[branch] if(Ready<.5) return Original;
float target=lerp(LandRoughness,OceanRoughness,saturate(SourceWater));
return lerp(Original,target,saturate(PhotoBlend.a));
"""
        roughness_fields = dict(common, Original=originals["Roughness"], SourceWater=fields["Water"],
                               OceanRoughness=scalar(material, "GlobalOceanRoughness", optics["ocean_roughness"]),
                               LandRoughness=scalar(material, "GlobalLandRoughness", optics["land_roughness"]))
    wrappers = {
        "BaseColor": make_custom(material, "WrapBaseColor", r"""
[branch] if(Ready<.5) return Original;
return lerp(Original,PhotoBlend.rgb,saturate(PhotoBlend.a));
""", dict(common, Original=originals["BaseColor"]), 3),
        "Roughness": make_custom(material, "WrapRoughness", roughness_code, roughness_fields, 1),
        "Specular": make_custom(material, "WrapSpecular", r"""
[branch] if(Ready<.5) return Original;
return lerp(Original,SharedSpecular,saturate(PhotoBlend.a));
""", dict(common, Original=originals["Specular"], SharedSpecular=scalar(material, "GlobalSharedSpecular", optics["specular"])), 1),
        "Emissive": make_custom(material, "WrapEmissive", r"""
[branch] if(Ready<.5) return Original;
return lerp(Original,PhotoBlend.rgb*NightReadability,saturate(PhotoBlend.a));
""", dict(common, Original=originals["Emissive"], NightReadability=scalar(material, "GlobalNightReadability", optics["night"])), 3),
    }
    for label, expression in wrappers.items():
        if not L.connect_material_property(expression, "", WRAPPED[label]):
            raise RuntimeError("Cannot connect shared global optical output " + label)
        if original_input(material, expression) != originals[label]:
            raise RuntimeError("Original optical output/channel was not preserved " + label)
    after = {name: connection(material, prop) for name, prop in UNTOUCHED.items()}
    if before != after:
        raise RuntimeError("Global-colour wrapper changed an unrelated material output")
    errors = L.recompile_material(material)
    if errors:
        raise RuntimeError("Global-colour material compile failed: " + str(errors))
    if not A.save_loaded_asset(material):
        raise RuntimeError("Cannot save shared global colour " + path)
    return {"material": path, "original_outputs": {label: {"node": value[0].get_path_name(), "output": value[1]}
                                                   for label, value in originals.items()},
            "global_texture": texture.get_path_name(), "ready_default": 0., "new_active_shared_texture_samples_max": 2 if planet else 3,
            "optional_source_backed_surround": True, "surround_photo_edge_fraction": .18,
            "surround_ready_default": 0.,
            "far_regional_photo_transition_cm": None if planet else [40000, 200000],
            "regional_photo_edge_fraction": None if planet else .20,
            "regional_macro_far_fade_roi_span_ratios": None if planet else [2.0, 4.0],
            "near_pbr_optic_alpha_edge_fraction": None if planet else .05,
            "shared_float4_photo_and_optic_blend": True, "shared_optical_targets_from_original_planet": optics,
            "roughness_water_authority": "original_natural_earth_land_ocean" if planet else "existing_geographic_source_water_mask",
            "photo_rgb_water_inference": False,
            "untouched_outputs": list(UNTOUCHED), "historical_imagery_not_current_or_measured_albedo": True}


def restore_original_outputs():
    """Public pre-authoring hook; safe through runpy without command-line modes."""
    restored = []
    for path in MATERIALS:
        material = unreal.load_asset(path)
        if not isinstance(material, unreal.Material):
            raise RuntimeError("Cannot restore missing material " + path)
        if restore_original(material):
            if not A.save_loaded_asset(material):
                raise RuntimeError("Cannot save restored upstream material " + path)
            restored.append(path)
    unreal.log_warning("AFTERSHOCK_GLOBAL_COLOUR_ORIGINALS_RESTORED " + json.dumps(restored))
    return restored


def main():
    if "-AftershockGlobalColourRestoreOnly" in unreal.SystemLibrary.get_command_line().split():
        restore_original_outputs()
        return
    texture = unreal.load_asset(GLOBAL_PATH)
    if not isinstance(texture, unreal.Texture2D) or not texture.get_editor_property("srgb"):
        raise RuntimeError("Import the verified EOX2016 global image first")
    if texture.blueprint_get_size_x() != 8192 or texture.blueprint_get_size_y() != 4096:
        raise RuntimeError("Global texture dimensions must be8192x4096")
    built = texture.blueprint_get_built_texture_size()
    if (int(built.x), int(built.y)) != (8192, 4096) or texture.get_editor_property("max_texture_size") != 8192:
        raise RuntimeError("Global built texture must retain8192x4096 with max-size8192")
    optics = read_shared_optics()
    report = {"materials": [author(path, texture, optics) for path in MATERIALS], "wrapper_version": 2,
              "source": "EOX2016 historical cloudless Sentinel composite", "license": "CC-BY-4.0",
              "runtime_v3_only": True, "opacity_wpo_coast_classification_changed": False,
              "global_built_dimensions": [8192, 4096], "expected_max_full_mip_count": 14,
              "actual_rhi_mip_count_verified_by_this_recipe": False,
              "zero_flicker_or_calibrated_albedo_claim": False,
              "rerun_order": ["ue_unify_global_regional_colour.py -AftershockGlobalColourRestoreOnly",
                              "original globe/regional style recipes", "ue_import_global_truecolour.py",
                              "ue_unify_global_regional_colour.py"]}
    output = ROOT / "Saved" / "PresentationValidation" / "global_regional_colour_authoring.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    unreal.log_warning("AFTERSHOCK_GLOBAL_REGIONAL_COLOUR_READY " + json.dumps(report))


if __name__ == "__main__":
    main()
