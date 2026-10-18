"""Author original globe/region/climate presentation materials in UE 5.8.

Idempotent tagged graph updates. Natural Earth public-domain coastline map, no
shader deletion, terrain mutation, or climate forecast. Runtime UWorldClimateSubsystem owns scenario years.
"""
import json
import os
import unreal

L = unreal.MaterialEditingLibrary
ROOT = "/Game/Environment/Materials/Presentation/"
TAG = "AFTERSHOCK_REGIONAL_GLOBE_V1:"
CANONICAL_WATER_MATCH = {}


def enum_value(cls, text):
    for name in dir(cls):
        if name.replace("_", "").upper() == text:
            return getattr(cls, name)
    raise RuntimeError("Missing reflected enum " + text)


def node(m, key, cls, **properties):
    existing = [n for n in L.get_material_expressions(m)
                if str(n.get_editor_property("desc")) == TAG + key]
    if len(existing) > 1:
        raise RuntimeError("Duplicate owned material node " + key)
    result = existing[0] if existing else L.create_material_expression(m, cls)
    if result.get_class() != cls.static_class():
        raise RuntimeError("Unexpected material node class " + key)
    result.set_editor_property("desc", TAG + key)
    for key, value in properties.items():
        result.set_editor_property(key, value)
    return result


def link(a, b, pin="", output=""):
    if not L.connect_material_expressions(a, output, b, pin):
        raise RuntimeError("Material connection failed: " + pin)


def output(m, n, prop, channel=""):
    if not L.connect_material_property(n, channel, prop):
        raise RuntimeError("Material output failed: " + str(prop))


def scalar(m, name, value):
    return node(m, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(value))


def vector(m, name, rgb):
    return node(m, name, unreal.MaterialExpressionVectorParameter,
                parameter_name=name, default_value=unreal.LinearColor(*rgb, 1.0))


def constant(m, name, value):
    return node(m, name, unreal.MaterialExpressionConstant, r=float(value))


def binary(m, name, cls, a, b):
    n = node(m, name, cls)
    link(a, n, "A"); link(b, n, "B")
    return n


def custom(m, name, code, inputs, width=3):
    n = node(m, name, unreal.MaterialExpressionCustom,
             code=code, output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT" + str(width)))
    fields = []
    for key in inputs:
        item = unreal.CustomInput()
        item.set_editor_property("input_name", key)
        fields.append(item)
    n.set_editor_property("inputs", fields)
    for key, value in inputs.items():
        link(value, n, key)
    return n


def material(name, translucent=False, unlit=False):
    path = ROOT + name
    m = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if m is None:
        unreal.EditorAssetLibrary.make_directory(ROOT.rstrip("/"))
        m = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, ROOT.rstrip("/"), unreal.Material, unreal.MaterialFactoryNew())
    if not isinstance(m, unreal.Material):
        raise RuntimeError("Owned path is not a base material: " + path)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_OPAQUE)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT if unlit else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("two_sided", translucent)
    return m


def common(m):
    return {
        "P": node(m, "WorldPosition", unreal.MaterialExpressionWorldPosition,
                  world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS),
        "Center": vector(m, "PlanetCenter", (12500., 12500., -200000.)),
        "Radius": scalar(m, "PlanetRadius", 200000.),
        "Seed": scalar(m, "WorldSeed", 1.337),
        "GeoEast": vector(m,"GeoEast",(-.4814,.8765,0.)),
        "GeoSouth": vector(m,"GeoSouth",(.5755,.3161,-.7543)),
        "GeoUp": vector(m,"GeoUp",(.6612,.3632,.6567)),
        "Drought": scalar(m, "DroughtPressure", .1333),
        "Ice": scalar(m, "IceCoverage", .804),
        "Heat": scalar(m, "LocalHeatPressure", 0.),
    }


def read_canonical_water():
    """Read connected city-water nodes; ignore old disconnected parameter copies."""
    path = "/Game/Environment/Materials/Presentation/M_PresentationWater"
    water = unreal.load_asset(path)
    if not isinstance(water, unreal.Material):
        raise RuntimeError("Missing active canonical city water material")

    def connected(prop):
        pending = [L.get_material_property_input_node(water, prop)]
        found = {}
        while pending:
            expression = pending.pop()
            if expression is None or expression.get_path_name() in found:
                continue
            found[expression.get_path_name()] = expression
            pending.extend(L.get_inputs_for_material_expression(water, expression))
        return list(found.values())

    base_nodes = connected(unreal.MaterialProperty.MP_BASE_COLOR)

    def vector_default(name):
        matches = [n for n in base_nodes if isinstance(n, unreal.MaterialExpressionVectorParameter)
                   and str(n.get_editor_property("parameter_name")) == name]
        if len(matches) != 1:
            raise RuntimeError("Canonical connected water vector is ambiguous: " + name)
        value = matches[0].get_editor_property("default_value")
        return (value.r, value.g, value.b)

    def scalar_output(prop):
        expression = L.get_material_property_input_node(water, prop)
        if isinstance(expression, unreal.MaterialExpressionScalarParameter):
            return float(expression.get_editor_property("default_value"))
        if isinstance(expression, unreal.MaterialExpressionConstant):
            return float(expression.get_editor_property("r"))
        raise RuntimeError("Canonical water scalar output is not a direct authored value")

    deep, shallow = vector_default("BaseColor"), vector_default("ShallowColor")
    rough = scalar_output(unreal.MaterialProperty.MP_ROUGHNESS)
    opacity = scalar_output(unreal.MaterialProperty.MP_OPACITY)
    specular = scalar_output(unreal.MaterialProperty.MP_SPECULAR)
    foam = unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions02/ExampleContent/Textures/water_d")
    if not isinstance(foam, unreal.Texture2D) or not any(
            isinstance(n, unreal.MaterialExpressionTextureSample) and n.get_editor_property("texture") == foam
            for n in base_nodes):
        raise RuntimeError("Canonical connected dynamic-water foam texture was not found")
    if not all(0. <= value <= 1. for value in (*deep, *shallow, rough, opacity, specular)):
        raise RuntimeError("Invalid canonical water defaults")
    CANONICAL_WATER_MATCH.update({"material": path, "connected_graph_readback": True,
                                 "deep_rgb": deep, "shallow_rgb": shallow, "roughness": rough,
                                 "opacity_approximation": opacity, "specular": specular,
                                 "foam_texture": foam.get_path_name(), "foam_strength": .45,
                                 "shallow_blend": .35, "world_uv_scale": .0015,
                                 "panner_speed": [.018, .025], "city_material_modified": False,
                                 "exact_translucent_reflection_equivalence_claimed": False})
    return deep, shallow, rough, opacity, specular, foam


NOISE = r"""
struct GlobeNoise {
 float hash(float3 p) { p = frac(p * .1031); p += dot(p, p.yzx + 33.33); return frac((p.x+p.y)*p.z); }
 float noise(float3 p) {
  float3 i=floor(p), f=frac(p); f=f*f*(3.-2.*f);
  return lerp(lerp(lerp(hash(i),hash(i+float3(1,0,0)),f.x),
                   lerp(hash(i+float3(0,1,0)),hash(i+float3(1,1,0)),f.x),f.y),
              lerp(lerp(hash(i+float3(0,0,1)),hash(i+float3(1,0,1)),f.x),
                   lerp(hash(i+float3(0,1,1)),hash(i+float3(1,1,1)),f.x),f.y),f.z);
 }
 float fbm(float3 p) { return noise(p)*.55 + noise(p*2.03+13.7)*.27 + noise(p*4.11+37.1)*.12 + noise(p*8.17+53.2)*.06; }
};
GlobeNoise g;
"""

SURFACE = NOISE + r"""
float3 N=normalize(P-Center);
float3 E=N.x*GeoEast+N.y*GeoSouth+N.z*GeoUp;
float latitude=abs(E.z);
float land=step(EarthMap.b*.85,EarthMap.g);
float detail=g.noise(N*110.);
float3 vegetation=EarthMap*(.94+detail*.12);
// Climate response is artistic; map coastlines are actual public-domain data.
float dryZone=(1.-smoothstep(.45,.75,latitude))*land;
vegetation=lerp(vegetation,float3(.22,.16,.07),Drought*dryZone*.16);
float cityMask=(1.-smoothstep(.035,.085,length(N.xy)))*smoothstep(.97,.995,N.z);
vegetation=lerp(vegetation,float3(.16,.12,.055),Heat*cityMask*.20);
float frozen=smoothstep(lerp(.98,.89,Ice)-.02,lerp(.98,.89,Ice)+.02,latitude);
return lerp(vegetation,float3(.55,.65,.68),frozen*.8);
"""


def make_surface(regional=False):
    m = material("M_AftershockRegionalBlend" if regional else "M_AftershockPlanetSurface")
    data = common(m)
    geo_uv = custom(m,"GeographicUV",r"""
float3 N=normalize(P-Center);
float3 E=N.x*GeoEast+N.y*GeoSouth+N.z*GeoUp;
return float2(atan2(E.y,E.x)/6.28318530718+.5,.5-asin(clamp(E.z,-1.,1.))/3.14159265359);
""",{key:data[key] for key in ("P","Center","GeoEast","GeoSouth","GeoUp")},2)
    earth=unreal.load_asset("/Game/Environment/Textures/Geography/T_NaturalEarthLand")
    if not isinstance(earth,unreal.Texture2D): raise RuntimeError("Missing offline Natural Earth cartographic texture")
    sample=node(m,"NaturalEarthMap",unreal.MaterialExpressionTextureSampleParameter2D,
                parameter_name="NaturalEarthLand",texture=earth,sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    link(geo_uv,sample)
    data["EarthMap"]=sample
    globe = custom(m, "ContinuousBiome", SURFACE, data)
    if regional:
        deep, shallow, water_roughness, water_opacity, water_specular, foam_texture = read_canonical_water()
        # Same four samples/UVs/tint as the actual canonical city ground. Alpha
        # belongs to the broad annulus and smoothly fades into the SAME globe code.
        xy = node(m, "WorldXY", unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
        link(data["P"], xy)
        uv = binary(m, "GroundUV", unreal.MaterialExpressionMultiply, xy, scalar(m, "WorldTextureScale", .005))
        samples = {}
        for name, path, sampler in (
            ("Diffuse", "/Game/Environment/Textures/PolyHaven/T_LeafyGrass_Diffuse", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
            ("Normal", "/Game/Environment/Textures/PolyHaven/T_LeafyGrass_NormalDX", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),
            ("Roughness", "/Game/Environment/Textures/PolyHaven/T_LeafyGrass_Roughness", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS),
        ):
            texture = unreal.load_asset(path)
            if not isinstance(texture, unreal.Texture2D):
                raise RuntimeError("Required existing CC0 ground texture missing: " + path)
            samples[name] = node(m, name, unreal.MaterialExpressionTextureSampleParameter2D,
                                 parameter_name="Ground" + name, texture=texture, sampler_type=sampler)
            link(uv, samples[name])
        vc = node(m, "CanonicalBiome", unreal.MaterialExpressionVertexColor)
        alpha = node(m, "RegionalAlpha", unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False)
        # VertexColor's default output is RGB. Select its explicit A output first.
        link(vc, alpha, output="A")
        macro_uv = binary(m, "MacroUV", unreal.MaterialExpressionMultiply, xy, scalar(m, "MacroTextureScale", .00015))
        macro_texture = unreal.load_asset("/Game/Environment/Textures/Presentation/T_PresentationBreakup")
        if not isinstance(macro_texture, unreal.Texture2D):
            raise RuntimeError("Existing CC0 ground macro texture missing")
        macro = node(m, "Macro", unreal.MaterialExpressionTextureSampleParameter2D,
                     parameter_name="GroundMacroBreakup", texture=macro_texture,
                     sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        link(macro_uv, macro)
        source_enable = scalar(m, "RegionalSourceMaskEnabled", 0.)
        source_uv = custom(m, "RegionalSourceUV", r"""
float2 extent=max(BoundsMax.xy-BoundsMin.xy,float2(1.,1.));
// The source raster contains257 nodes including both WGS84 bounds. Address
// their texel centres exactly, rather than shifting the coastline half a pixel.
return (saturate((P.xy-BoundsMin.xy)/extent)*256.+.5)/257.;
""", {"P": data["P"], "BoundsMin": vector(m, "RegionalSourceBoundsMin", (0.,0.,0.)),
       "BoundsMax": vector(m, "RegionalSourceBoundsMax", (1.,1.,0.))}, 2)
        source_mask = node(m, "RegionalSourceMask", unreal.MaterialExpressionTextureSampleParameter2D,
                           parameter_name="RegionalSourceMask", texture=macro_texture,
                           sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        link(source_uv, source_mask)
        water_mask = custom(m, "RegionalWaterCoverage", r"""
// V2 is exclusively the linear source R channel. VertexColor is gamma packed;
// its blue biome/snow tint must never classify a highland as an ocean.
return lerp(smoothstep(.65,.85,VC.b),saturate(Mask.r),step(.5,Enable));
""", {"VC": vc, "Mask": source_mask, "Enable": source_enable}, 1)
        water_uv = custom(m, "CanonicalWaterFoamUV", "return P.xy*.0015+TimeValue*float2(.018,.025);",
                          {"P": data["P"], "TimeValue": node(m, "CanonicalWaterTime", unreal.MaterialExpressionTime)}, 2)
        water_foam = node(m, "CanonicalWaterFoam", unreal.MaterialExpressionTextureSampleParameter2D,
                          parameter_name="CanonicalWaterFoam", texture=foam_texture,
                          sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        link(water_uv, water_foam)
        water_albedo = custom(m, "CanonicalWaterAlbedo", "return lerp(lerp(Deep,Shallow,.35),float3(.75,.90,.95),saturate(Foam.r)*.45);",
                              {"Deep": vector(m, "CanonicalWaterDeep", deep), "Shallow": vector(m, "CanonicalWaterShallow", shallow),
                               "Foam": water_foam})
        # Canonical city RGB remains EXACT at Alpha=0: same four texture
        # samples, world UVs, vertex tint and macro multiplier. The old radial
        # lerp kept dry PBR albedo across almost the full 2.8x annulus, producing
        # a conspicuous yellow disc at globe distance. Transition sooner and
        # break its front with world-space low-frequency noise, without a new
        # texture sample, opacity mask, geometry displacement or cloud pass.
        regional_alpha = custom(m, "IrregularRegionalBlend", NOISE + r"""
float a=saturate(Alpha);
float front=pow(saturate(a*Reach),.62);
float coarse=g.noise(float3((P.xy-Center.xy)/9000.,Seed+19.7));
float breakup=coarse*.78+Macro.r*.22;
float varied=front+(breakup-.5)*Irregularity*4.*front*(1.-front);
// Exact endpoint locks preserve the inner canonical join and outer globe join.
return a<=0.00001 ? 0. : (a>=.99999 ? 1. : saturate(varied));
""", {"Alpha": alpha, "Macro": macro, "P": data["P"], "Center": data["Center"], "Seed": data["Seed"],
       "Reach": scalar(m, "RegionalBlendReach", 2.05),
       "Irregularity": scalar(m, "RegionalBlendIrregularity", .65)}, 1)
        base = custom(m, "GroundToBiome", r"""
float3 tint=lerp(float3(1,1,1), VC*float3(2.4,2.2,2.6),.16);
float3 ground=Diffuse*tint*(.9+Macro.r*.2);
float mapped=step(.5,SourceEnabled);
ground=lerp(ground,ground*.66+float3(.018,.038,.010),saturate(SourceMask.g)*mapped*.75);
ground=lerp(ground,ground*float3(1.02,1.06,.78),saturate(SourceMask.b)*mapped*.45);
// The city uses translucent water. Approximate its underlying-ground blend in
// this opaque context, with the SAME connected albedo/foam and world-space UV.
float3 canonicalWater=lerp(ground,WaterAlbedo,saturate(WaterOpacity));
float3 waterTint=lerp(float3(.015,.095,.14)*(.94+Macro.r*.10),canonicalWater,mapped);
ground=lerp(ground,waterTint,WaterMask);
return lerp(ground,Globe,Alpha);
""", {"VC": vc, "Diffuse": samples["Diffuse"], "Macro": macro, "Globe": globe, "Alpha": regional_alpha,
       "SourceMask": source_mask, "SourceEnabled": source_enable, "WaterMask": water_mask,
       "WaterAlbedo": water_albedo, "WaterOpacity": scalar(m, "CanonicalWaterOpacity", water_opacity)})
        normal = custom(m, "FadingGroundNormal", "return normalize(lerp(float3(0,0,1),GroundNormal,.45*(1.-Alpha)*(1.-WaterMask)));",
                        {"GroundNormal": samples["Normal"], "Alpha": regional_alpha, "WaterMask": water_mask})
        output(m, normal, unreal.MaterialProperty.MP_NORMAL)
        rough = custom(m, "RegionalRoughness", "return lerp(lerp(GroundRoughness.r,lerp(.23,WaterRoughness,step(.5,Enable)),WaterMask),.84,Alpha);",
                       {"GroundRoughness": samples["Roughness"], "Alpha": regional_alpha, "WaterMask": water_mask,
                        "WaterRoughness": scalar(m, "CanonicalWaterRoughness", water_roughness), "Enable": source_enable}, 1)
        output(m, rough, unreal.MaterialProperty.MP_ROUGHNESS)
        specular = custom(m, "RegionalWaterSpecular", "return lerp(.25,WaterSpecular,saturate(WaterMask)*step(.5,Enable));",
                          {"WaterSpecular": scalar(m, "CanonicalWaterSpecular", water_specular), "WaterMask": water_mask, "Enable": source_enable}, 1)
    else:
        base = globe
        # The global sea-level sphere must not cover real below-MSL land in
        # the bounded regional DEM. Runtime enables this mask only while the
        # actual source context is visible; V1 and full-globe defaults stay whole.
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        m.set_editor_property("opacity_mask_clip_value", .5)
        regional_clip = custom(m, "SourceContextPlanetClip", r"""
float inside=step(BoundsMin.x,P.x)*step(P.x,BoundsMax.x)*step(BoundsMin.y,P.y)*step(P.y,BoundsMax.y);
float nearHemisphere=step(Center.z,P.z);
return 1.-step(.5,Enable)*inside*nearHemisphere;
""", {"P":data["P"], "Center":data["Center"],
       "BoundsMin":vector(m,"RegionalContextBoundsMin",(0.,0.,0.)),
       "BoundsMax":vector(m,"RegionalContextBoundsMax",(0.,0.,0.)),
       "Enable":scalar(m,"RegionalContextClipEnabled",0.)}, 1)
        output(m, regional_clip, unreal.MaterialProperty.MP_OPACITY_MASK)
        rough = custom(m,"LandOceanRoughness","return lerp(.26,.86,step(EarthMap.b*.85,EarthMap.g));",{"EarthMap":sample},1)
        output(m, rough, unreal.MaterialProperty.MP_ROUGHNESS)
    output(m, base, unreal.MaterialProperty.MP_BASE_COLOR)
    output(m, specular if regional else constant(m, "Specular", .25), unreal.MaterialProperty.MP_SPECULAR)
    output(m, constant(m, "Metallic", 0.), unreal.MaterialProperty.MP_METALLIC)
    # A very small common ambient floor preserves continents on the night limb.
    lift = binary(m, "NightReadability", unreal.MaterialExpressionMultiply, base, scalar(m, "AmbientReadability", .025))
    output(m, lift, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return m


def make_clouds():
    m = material("M_AftershockClimateClouds", True)
    m.set_editor_property("translucency_lighting_mode", enum_value(unreal.TranslucencyLightingMode, "TLMSURFACE"))
    data = common(m)
    data["TimeValue"] = node(m, "Time", unreal.MaterialExpressionTime)
    data["Storm"] = scalar(m, "StormPressure", .1)
    data["UseRealGeographicScale"] = scalar(m, "UseRealGeographicScale", 0.)
    opacity = custom(m, "ScenarioCloudPattern", NOISE + r"""
float3 N=normalize(P-Center);
float3 q=N*9.+float3(Seed+TimeValue*.0008,2.1,5.7);
float latitude=abs(dot(N,normalize(float3(.20,.93,.30))));
float bands=.05*sin(latitude*24.+g.noise(q)*2.);
float density=smoothstep(.51-Storm*.035,.69,g.fbm(q)+bands);
// The legacy compressed globe cleared its small city cap. On a physical Earth
// that angular cap would erase clouds across hundreds of kilometres.
float cityExclusion=lerp(1.-smoothstep(.965,.995,N.z),1.,saturate(UseRealGeographicScale));
return density*(.30+Storm*.12)*cityExclusion;
""", data, 1)
    tint = vector(m, "CloudColor", (.62, .66, .68))
    output(m, tint, unreal.MaterialProperty.MP_BASE_COLOR)
    output(m, opacity, unreal.MaterialProperty.MP_OPACITY)
    output(m, constant(m, "CloudRoughness", 1.), unreal.MaterialProperty.MP_ROUGHNESS)
    return m


def make_atmosphere():
    m = material("M_AftershockPlanetAtmosphere", True, True)
    fresnel = node(m, "LimbFresnel", unreal.MaterialExpressionFresnel, exponent=5., base_reflect_fraction=0.)
    opacity = binary(m, "LimbOpacity", unreal.MaterialExpressionMultiply, fresnel, scalar(m, "HaloOpacity", .12))
    output(m, opacity, unreal.MaterialProperty.MP_OPACITY)
    output(m, vector(m, "AtmosphereTint", (.055, .16, .30)), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return m


def main():
    source=os.path.abspath(os.path.join(unreal.Paths.project_dir(),"_Imports/Geography/T_NaturalEarthLand.png"))
    target="/Game/Environment/Textures/Geography/T_NaturalEarthLand"
    if not unreal.EditorAssetLibrary.does_asset_exist(target):
        if not os.path.isfile(source): raise RuntimeError("Run scripts/fetch_offline_geography.py first")
        task=unreal.AssetImportTask()
        task.set_editor_property("filename",source)
        task.set_editor_property("destination_path","/Game/Environment/Textures/Geography")
        task.set_editor_property("destination_name","T_NaturalEarthLand")
        task.set_editor_property("automated",True);task.set_editor_property("save",True)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    if not isinstance(unreal.load_asset(target),unreal.Texture2D): raise RuntimeError("Natural Earth texture import failed")
    materials = [make_surface(), make_surface(True), make_clouds(), make_atmosphere()]
    report = {"scenario_model": True, "original_shaders": True, "real_geographic_coastlines":True, "map_source":"Natural Earth110m public domain", "map_not_satellite":True, "materials": [],
              "regional_blend": {"canonical_inner_rgb_preserved": True, "world_space_breakup_cm": 9000,
                                 "reach": 2.05, "irregularity": .65, "new_texture_samples": 2,
                                 "cloud_graph_unchanged": False, "atmosphere_graph_unchanged": True},
              "regional_canonical_water_match": dict(CANONICAL_WATER_MATCH),
              "regional_source_mask": {"texture_parameter": "RegionalSourceMask", "enabled_parameter": "RegionalSourceMaskEnabled",
                                       "default_enabled": 0., "runtime_v2_enabled": 1., "source_side": 257, "mip_levels": 9,
                                       "format": "PF_B8G8R8A8", "srgb": False, "sample_type": "linear_color",
                                       "channels": "R water; G mapped forest; B mapped agriculture; A ocean",
                                       "bounds_parameters": ["RegionalSourceBoundsMin", "RegionalSourceBoundsMax"],
                                       "v2_water_exclusively_source_r": True, "legacy_vertex_water_preserved": True,
                                       "geometry_resolution_increased": False, "dem_elevation_modified": False},
              "cloud_geographic_scale": {"parameter": "UseRealGeographicScale", "legacy_default": 0.,
                                         "real_earth_value": 1., "real_earth_city_exclusion_disabled": True},
              "regional_planet_clip": {"enabled_parameter": "RegionalContextClipEnabled", "default": 0.,
                                       "material": "M_AftershockPlanetSurface", "blend_mode": "masked",
                                       "bounds": "actual_source_context_world_xy", "near_hemisphere_only": True,
                                       "full_globe_and_legacy_clip_disabled": True, "dem_elevation_modified": False}}
    for m in materials:
        L.set_base_material_usage(m, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
        L.layout_material_expressions(m)
        errors = L.recompile_material(m)
        if errors:
            raise RuntimeError("Material compile failed {}: {}".format(m.get_path_name(), errors))
        if not unreal.EditorAssetLibrary.save_loaded_asset(m):
            raise RuntimeError("Cannot save " + m.get_path_name())
        report["materials"].append(m.get_path_name())
        unreal.log_warning("AFTERSHOCK_GLOBE_CLIMATE_MATERIAL_READY " + m.get_path_name())
    folder = os.path.join(unreal.Paths.project_saved_dir(), "PresentationValidation")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "globe_climate_materials.json"), "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)


if __name__ == "__main__":
    main()
