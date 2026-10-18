"""Author AFTERSHOCK's own terrain-aware inundation material in UE 5.8.

Run with UnrealEditor-Cmd -run=pythonscript -script=<this absolute path>.
This script authors one new material; no marketplace dependency, texture download,
fluid plugin, graph deletion, map edit, hazard calculation or existing graph rewrite.

CPU contract: gamma-compensated vertex R = rise/FloodEncodingCm,
G = depth/FloodEncodingCm, B = top face (1) or volume front (0).
The legacy encoding remains512. Physical snapshot columns may exceed5m and use
their actual bounded mesh encoding; UV0 then comes only from canonical head
gradient. Zero gradient stays zero flow. Legacy builder's explicit +Y art drift
is preserved. Optional override parameters are bounded unitless LEGACY art
offsets, never a physical velocity or a Model1/Model2 flow authority. Zero
override preserves the input exactly; opposing vectors may safely cancel.
The shader never derives or changes the water/terrain authority.
"""
import unreal

L = unreal.MaterialEditingLibrary
PATH = "/Game/FX/Aftershock/Materials/M_DisasterFloodVolume"
TAG = "AFTERSHOCK_TERRAIN_FLOOD_VOLUME_V1:"


def enum_value(enum, normalized):
    for name in dir(enum):
        if name.replace("_", "").upper() == normalized:
            return getattr(enum, name)
    raise RuntimeError("Missing reflected enum: " + normalized)


def node(material, label, cls, **values):
    matches = [n for n in L.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == TAG + label]
    if len(matches) > 1:
        raise RuntimeError("Duplicate owned material node: " + label)
    n = matches[0] if matches else L.create_material_expression(material, cls)
    if n.get_class() != cls.static_class():
        raise RuntimeError("Owned material node changed class: " + label)
    n.set_editor_property("desc", TAG + label)
    for key, value in values.items():
        n.set_editor_property(key, value)
    return n


def connect(source, target, pin="", output=""):
    if not L.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Cannot connect {}.{} -> {}.{}".format(source.get_name(), output, target.get_name(), pin))


def material_property(material, source, prop, output=""):
    if not L.connect_material_property(source, output, prop):
        raise RuntimeError("Cannot connect material property: " + str(prop))


def parameter(material, name, default):
    return node(material, name, unreal.MaterialExpressionScalarParameter,
                parameter_name=name, default_value=float(default))


def custom(material, label, code, output_size, inputs):
    n = node(material, label, unreal.MaterialExpressionCustom, code=code,
             output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT" + str(output_size)))
    custom_inputs = []
    for name, _, _ in inputs:
        field = unreal.CustomInput()
        field.set_editor_property("input_name", name)
        custom_inputs.append(field)
    n.set_editor_property("inputs", custom_inputs)
    for name, source, output in inputs:
        connect(source, n, name, output)
    return n


material = unreal.load_asset(PATH) if unreal.EditorAssetLibrary.does_asset_exist(PATH) else None
if material is None:
    folder, name = PATH.rsplit("/", 1)
    unreal.EditorAssetLibrary.make_directory(folder)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew())
if not isinstance(material, unreal.Material):
    raise RuntimeError("Expected a project-owned base flood-volume material")

material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
material.set_editor_property("two_sided", True)
material.set_editor_property("tangent_space_normal", True)
material.set_editor_property("translucency_lighting_mode", enum_value(unreal.TranslucencyLightingMode, "TLMSURFACE"))
material.set_editor_property("screen_space_reflections", False)
L.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)

vertex = node(material, "DisplayDepthData", unreal.MaterialExpressionVertexColor)
position = node(material, "WorldPositionWithoutWPO", unreal.MaterialExpressionWorldPosition,
                world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
time = node(material, "WaveTime", unreal.MaterialExpressionTime)
flow = node(material, "ReconstructedSurfaceFlow", unreal.MaterialExpressionTextureCoordinate,
            coordinate_index=0)
envelope = parameter(material, "FloodEnvelope", 0.0)
opacity = parameter(material, "Opacity", 0.88)
strength = parameter(material, "VisualStrength", 0.5)
encoding = parameter(material, "FloodEncodingCm", 512.0)
physical_arrival = parameter(material, "FloodPhysicalArrivalEnabled", 0.0)
arrival_progress = parameter(material, "FloodArrivalProgress", 1.0)
physical_data = parameter(material, "FloodPhysicalDataEnabled", 0.0)
hydraulic = node(material, "ExactHydraulicCentimetres", unreal.MaterialExpressionTextureCoordinate,
                 coordinate_index=1)
surface_data = node(material, "RiverCoverageAndExactArrival", unreal.MaterialExpressionTextureCoordinate,
                    coordinate_index=2)

flow_x = parameter(material, "FloodFlowOverrideX", 0.0)
flow_y = parameter(material, "FloodFlowOverrideY", 0.0)

world_offset = custom(material, "BoundedColumnRiseAndRipples", r"""
float exactArrival=lerp(Arrival,SurfaceData.y,step(.5,PhysicalData));
float exactDepth=lerp(Depth*max(Encoding,512.0),HydraulicData.y,step(.5,PhysicalData));
float exactRise=lerp(Rise*max(Encoding,512.0),HydraulicData.x,step(.5,PhysicalData));
float arrivalWeight=lerp(1.,Progress>=1.?1.:smoothstep(max(0.,exactArrival-.05),min(1.,exactArrival+.05),Progress),step(.5,PhysicalArrival));
float localEnvelope=Envelope*arrivalWeight;
float2 overrideFlow = float2(FlowOverrideX, FlowOverrideY);
// UV0 already contains the canonical/legacy direction. Preserve its magnitude
// and zero; no normalize(0), invented +Y or global physical-velocity override.
float2 f = all(isfinite(Flow.xy)) ? Flow.xy : float2(0.0, 0.0);
if (PhysicalData < 0.5 && all(isfinite(overrideFlow)))
    f += clamp(overrideFlow, float2(-1.0, -1.0), float2(1.0, 1.0));
float2 p = Position.xy;
float phase = dot(p,f)*0.012 - Time*1.7;
float crossPhase = dot(p,float2(-f.y,f.x))*0.009 + Time*1.15;
float depthCm = max(0.0,(exactDepth+4.0)*localEnvelope-4.0);
float ripple = (sin(phase)*0.65+sin(crossPhase)*0.35)*1.35;
ripple *= Top*saturate(depthCm/22.0)*localEnvelope;
return float3(0.0,0.0,exactRise*localEnvelope+ripple);
""", 3, [("Rise", vertex, "R"), ("Depth", vertex, "G"), ("Top", vertex, "B"),
          ("Flow", flow, ""), ("Position", position, ""), ("Time", time, ""), ("Envelope", envelope, ""), ("Encoding", encoding, ""),
          ("Arrival", vertex, "A"), ("PhysicalArrival", physical_arrival, ""), ("Progress", arrival_progress, ""),
          ("PhysicalData", physical_data, ""), ("HydraulicData", hydraulic, ""), ("SurfaceData", surface_data, ""),
          ("FlowOverrideX", flow_x, ""), ("FlowOverrideY", flow_y, "")])
material_property(material, world_offset, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

optics = custom(material, "DepthAbsorptionShoreFoam", r"""
float exactArrival = lerp(Arrival, SurfaceData.y, step(0.5, PhysicalData));
float exactDepth   = lerp(Depth * max(Encoding, 512.0), HydraulicData.y, step(0.5, PhysicalData));
float arrivalWeight = lerp(1., Progress >= 1. ? 1. :
    smoothstep(max(0., exactArrival - 0.05), min(1., exactArrival + 0.05), Progress),
    step(0.5, PhysicalArrival));
float localEnvelope = Envelope * arrivalWeight;
float depthCm = max(0.0, (exactDepth + 4.0) * localEnvelope - 4.0);

float2 overrideFlow = float2(FlowOverrideX, FlowOverrideY);
// Optional offsets are unitless art for the legacy builder only. Exact zero
// and cancellation remain zero, while physical snapshot flow is untouched.
float2 f = all(isfinite(Flow.xy)) ? Flow.xy : float2(0.0, 0.0);
if (PhysicalData < 0.5 && all(isfinite(overrideFlow)))
    f += clamp(overrideFlow, float2(-1.0, -1.0), float2(1.0, 1.0));

float phase      = dot(Position.xy, f) * 0.012 - Time * 1.7;
float crossPhase = dot(Position.xy, float2(-f.y, f.x)) * 0.009 + Time * 1.15;
float wave   = sin(phase) * 0.65 + sin(crossPhase) * 0.35;

float turb = 0.5 + 0.5 * sin(Position.x * 0.017 + Position.y * 0.013 + Time * 0.23);
float detail = 0.5 + 0.5 * sin(Position.x * 0.031 + sin(Position.y * 0.024) + Time * 0.75);

float absorption = 1.0 - exp(-depthCm / 85.0);

float3 shallow = float3(0.07, 0.31, 0.23);
float3 deep    = float3(0.010, 0.055, 0.110);
float3 water   = lerp(shallow, deep, absorption);
water *= (0.86 + 0.14 * turb);

float shore = (1.0 - smoothstep(5.0, 24.0, depthCm)) * smoothstep(1.5, 6.0, depthCm);
float crest = pow(saturate(wave * 0.5 + 0.5), 10.0);
float foam  = (shore * (0.08 + 0.22 * detail) + crest * 0.055 * Strength) * Top;
float3 colour = lerp(water, float3(0.55, 0.57, 0.50), saturate(foam));

float alpha = Opacity * smoothstep(0.5, 6.0, depthCm)
            * lerp(0.18, 0.88, saturate(depthCm / 120.0));
alpha *= lerp(0.78, 1.0, Top);
alpha *= lerp(1.0, smoothstep(9.0, 12.0, depthCm),
              step(0.5, PhysicalData) * step(0.5, SurfaceData.x));
return float4(colour, alpha);
""", 4, [("Depth", vertex, "G"), ("Top", vertex, "B"), ("Flow", flow, ""),
          ("Position", position, ""), ("Time", time, ""), ("Envelope", envelope, ""),
          ("Opacity", opacity, ""), ("Strength", strength, ""), ("Encoding", encoding, ""),
          ("Arrival", vertex, "A"), ("PhysicalArrival", physical_arrival, ""), ("Progress", arrival_progress, ""),
          ("PhysicalData", physical_data, ""), ("HydraulicData", hydraulic, ""), ("SurfaceData", surface_data, ""),
          ("FlowOverrideX", flow_x, ""), ("FlowOverrideY", flow_y, "")])

rgb = node(material, "OpticsRGB", unreal.MaterialExpressionComponentMask, r=True, g=True, b=True, a=False)
alpha = node(material, "OpticsOpacity", unreal.MaterialExpressionComponentMask, r=False, g=False, b=False, a=True)
connect(optics, rgb); connect(optics, alpha)
material_property(material, rgb, unreal.MaterialProperty.MP_BASE_COLOR)
depth_fade = node(material, "OpaqueTerrainAndBuildingContactFade", unreal.MaterialExpressionDepthFade,
                  fade_distance_default=36.0, opacity_default=1.0)
connect(alpha, depth_fade, "Opacity")
material_property(material, depth_fade, unreal.MaterialProperty.MP_OPACITY)

normal = custom(material, "FlowingAnalyticNormal", r"""
float exactArrival=lerp(Arrival,SurfaceData.y,step(.5,PhysicalData));
float exactDepth=lerp(Depth*max(Encoding,512.0),HydraulicData.y,step(.5,PhysicalData));
float arrivalWeight=lerp(1.,Progress>=1.?1.:smoothstep(max(0.,exactArrival-.05),min(1.,exactArrival+.05),Progress),step(.5,PhysicalArrival));
float localEnvelope=Envelope*arrivalWeight;
float2 overrideFlow = float2(FlowOverrideX, FlowOverrideY);
float2 f = all(isfinite(Flow.xy)) ? Flow.xy : float2(0.0, 0.0);
if (PhysicalData < 0.5 && all(isfinite(overrideFlow)))
    f += clamp(overrideFlow, float2(-1.0, -1.0), float2(1.0, 1.0));
float a=dot(Position.xy,f)*0.012-Time*1.7;
float b=dot(Position.xy,float2(-f.y,f.x))*0.009+Time*1.15;
float2 slope=f*cos(a)*0.10+float2(-f.y,f.x)*cos(b)*0.065;
float depthCm=max(0.0,(exactDepth+4.0)*localEnvelope-4.0);
return normalize(float3(slope*Top*saturate(depthCm/16.0),1.0));
""", 3, [("Flow", flow, ""), ("Position", position, ""), ("Time", time, ""),
          ("Depth", vertex, "G"), ("Top", vertex, "B"), ("Envelope", envelope, ""), ("Encoding", encoding, ""),
          ("Arrival", vertex, "A"), ("PhysicalArrival", physical_arrival, ""), ("Progress", arrival_progress, ""),
          ("PhysicalData", physical_data, ""), ("HydraulicData", hydraulic, ""), ("SurfaceData", surface_data, ""),
          ("FlowOverrideX", flow_x, ""), ("FlowOverrideY", flow_y, "")])
material_property(material, normal, unreal.MaterialProperty.MP_NORMAL)
material_property(material, parameter(material, "WaterRoughness", 0.19), unreal.MaterialProperty.MP_ROUGHNESS)
material_property(material, parameter(material, "WaterSpecular", 0.52), unreal.MaterialProperty.MP_SPECULAR)
lift = node(material, "RestrainedAmbientReadability", unreal.MaterialExpressionMultiply)
connect(rgb, lift, "A"); connect(parameter(material, "AmbientReadability", 0.025), lift, "B")
material_property(material, lift, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

errors = L.recompile_material(material)
if errors:
    raise RuntimeError("Flood-volume material compile errors: " + str(errors))
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError("Cannot save " + PATH)
unreal.log_warning("AFTERSHOCK_FLOOD_VOLUME_MATERIAL_READY path={} encodingParameter=FloodEncodingCm defaultEncodingCm=512 depthAbsorption=1 shoreClip=1 opaqueDepthFadeCm=36 normalRipples=1 boundedWaveCm=1.35 flow=CanonicalOrLegacySurfaceSlope zeroGradientFlow=0 zeroOverridePreservesInput=1 finiteLegacyUnitlessOverrideOnly=1 cancellationSafe=1 legacyExplicitArtPositiveYPreserved=1 noTextureDownloads=1 shaderChangesWaterAuthority=0 graphDeletion=0".format(PATH))
