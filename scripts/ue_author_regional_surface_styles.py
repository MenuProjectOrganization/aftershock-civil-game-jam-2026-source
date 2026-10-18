"""Wrap existing ground outputs with an optional regional CC0 PBR style.

Run AFTER ue_import_cc0_ground.py and ue_upgrade_globe_climate.py. This script
imports the nine already downloaded, manifest-verified Poly Haven 1k textures;
it neither downloads sources nor replaces the original GroundDiffuse parameter.
RegionalStyleEnabled defaults to zero. Existing BaseColor/Normal/Roughness
outputs, including their selected output channels, remain connected upstream.
NASA POWER monthly air climatology + a standard lapse-rate approximation drive
an ARTISTIC snow coating, not measured snow cover, weather or a hazard solver.
No WPO, displacement, height/cell writes or change to water/globe classification.
Root coordinates all UE execution. This file can be syntax checked without UE.
"""
from pathlib import Path
import hashlib
import json
import unreal


ROOT = Path(__file__).resolve().parents[1]
DEST = "/Game/Environment/Textures/Regional"
TAG = "AFTERSHOCK_REGIONAL_STYLE_V1:"
GLOBE_TAG = "AFTERSHOCK_REGIONAL_GLOBE_V1:"
L = unreal.MaterialEditingLibrary
A = unreal.EditorAssetLibrary
MATERIALS = (
    "/Game/Environment/Materials/Presentation/M_PresentationGroundPBR",
    "/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend",
)
SOURCES = {"dry_ground_rocks": "DrySoil", "forrest_ground_01": "ForestFloor",
           "aerial_rocks_01": "Rock"}
ROLES = {"diffuse": ("Diffuse", True, "TC_DEFAULT"),
         "normal_dx": ("NormalDX", False, "TC_NORMALMAP"),
         "roughness": ("Roughness", False, "TC_MASKS")}
WRAPPED = {
    "BaseColor": unreal.MaterialProperty.MP_BASE_COLOR,
    "Normal": unreal.MaterialProperty.MP_NORMAL,
    "Roughness": unreal.MaterialProperty.MP_ROUGHNESS,
}
UNTOUCHED = {
    "Emissive": unreal.MaterialProperty.MP_EMISSIVE_COLOR,
    "WPO": unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,
    "Metallic": unreal.MaterialProperty.MP_METALLIC,
    "Specular": unreal.MaterialProperty.MP_SPECULAR,
    "Opacity": unreal.MaterialProperty.MP_OPACITY,
    "OpacityMask": unreal.MaterialProperty.MP_OPACITY_MASK,
}


def enum_value(cls, key):
    key = key.replace("_", "").upper()
    for name in dir(cls):
        if name.replace("_", "").upper() == key:
            return getattr(cls, name)
    raise RuntimeError("Missing UE enum " + key)


def node(material, key, cls, **props):
    matches = [n for n in L.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == TAG + key]
    if len(matches) > 1:
        raise RuntimeError("Duplicate regional-style node " + key)
    value = matches[0] if matches else L.create_material_expression(material, cls)
    if not value or value.get_class() != cls.static_class():
        raise RuntimeError("Regional-style expression type changed " + key)
    value.set_editor_property("desc", TAG + key)
    for prop, setting in props.items():
        value.set_editor_property(prop, setting)
    return value


def link(source, target, pin="", output=""):
    if not L.connect_material_expressions(source, output, target, pin):
        raise RuntimeError("Regional style material link failed " + pin)


def scalar(material, key, default):
    return node(material, key, unreal.MaterialExpressionScalarParameter,
                parameter_name=key, default_value=float(default))


def vector(material, key, rgb):
    return node(material, key, unreal.MaterialExpressionVectorParameter,
                parameter_name=key, default_value=unreal.LinearColor(*rgb, 1.0))


def custom(material, key, code, inputs, width):
    expression = node(material, key, unreal.MaterialExpressionCustom, code=code,
                      output_type=enum_value(unreal.CustomMaterialOutputType,
                                             "CMOTFLOAT" + str(width)))
    fields = []
    for name in inputs:
        field = unreal.CustomInput()
        field.set_editor_property("input_name", name)
        fields.append(field)
    expression.set_editor_property("inputs", fields)
    for name, source in inputs.items():
        if isinstance(source, tuple):
            link(source[0], expression, name, source[1])
        else:
            link(source, expression, name)
    return expression


def property_connection(material, prop):
    return (L.get_material_property_input_node(material, prop),
            str(L.get_material_property_input_node_output_name(material, prop)))


def expression_input_connection(material, expression, name):
    names = list(L.get_material_expression_input_names(expression))
    values = list(L.get_inputs_for_material_expression(material, expression))
    if name not in names or names.index(name) >= len(values):
        raise RuntimeError("Regional-style rerun lost original input " + name)
    source = values[names.index(name)]
    if source is None:
        raise RuntimeError("Regional-style original input is disconnected")
    # UE's reflected bool + out FString is returned as a tuple (some Python
    # builds expose the single out value directly). Preserve R/G/B/A selection.
    result = L.get_input_node_output_name_for_material_expression(expression, source)
    if isinstance(result, (tuple, list)):
        if result and isinstance(result[0], bool) and not result[0]:
            raise RuntimeError("Cannot read original material output channel")
        outputs = [item for item in result if isinstance(item, str)]
        if len(outputs) != 1:
            raise RuntimeError("Ambiguous original material output channel")
        output = outputs[0]
    elif isinstance(result, str):
        output = result
    else:
        raise RuntimeError("Cannot read original material output channel")
    return source, output


def original_connection(material, label, prop):
    source, output = property_connection(material, prop)
    if source and str(source.get_editor_property("desc")) == TAG + "Wrap" + label:
        source, output = expression_input_connection(material, source, "Original")
    if source is None or str(source.get_editor_property("desc")).startswith(TAG):
        raise RuntimeError("Cannot safely preserve original " + label)
    return source, output


def tagged_existing(material, key):
    matches = [n for n in L.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == GLOBE_TAG + key]
    if len(matches) != 1:
        raise RuntimeError("Run ue_upgrade_globe_climate.py first: missing " + key)
    return matches[0]


def source_inventory():
    inventory = []
    total = 0
    for asset, slot in SOURCES.items():
        directory = ROOT / "_Imports" / "PolyHaven" / asset
        manifest_path = directory / "SOURCE_LICENSE_MANIFEST.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (manifest.get("asset") != asset or manifest.get("slot") != slot
                or manifest.get("license") != "CC0-1.0"
                or manifest.get("resolution") != "1k"
                or manifest.get("displacement") is not False):
            raise RuntimeError("Unexpected regional source manifest " + asset)
        files = manifest.get("files", [])
        if len(files) != 3 or {item.get("role") for item in files} != set(ROLES):
            raise RuntimeError("Incomplete regional source roles " + asset)
        for item in files:
            name = item["file"]
            if Path(name).name != name or not name.endswith("_1k.jpg"):
                raise RuntimeError("Unexpected regional source filename")
            data = (directory / name).read_bytes()
            total += len(data)
            expected_name = "T_Regional" + slot + "_" + ROLES[item["role"]][0]
            if (len(data) > 4 * 1024 * 1024 or total > 20 * 1024 * 1024
                    or not data.startswith(b"\xff\xd8") or len(data) != item["bytes"]
                    or hashlib.sha256(data).hexdigest() != item["sha256"]
                    or item["texture_name"] != expected_name):
                raise RuntimeError("Regional source checksum/size/name changed " + name)
            inventory.append((directory / name, expected_name, item["role"],
                              str(manifest_path.relative_to(ROOT)), item["sha256"]))
    return inventory


def import_textures(inventory):
    textures, report = {}, []
    A.make_directory(DEST)
    for filename, name, role, manifest, digest in inventory:
        task = unreal.AssetImportTask()
        for prop, value in {"filename": str(filename), "destination_path": DEST,
                            "destination_name": name, "automated": True,
                            "replace_existing": True, "save": False}.items():
            task.set_editor_property(prop, value)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        texture = unreal.load_asset(DEST + "/" + name)
        if not isinstance(texture, unreal.Texture2D):
            raise RuntimeError("Cannot import regional texture " + name)
        _, srgb, compression = ROLES[role]
        texture.set_editor_property("srgb", srgb)
        texture.set_editor_property("compression_settings", getattr(unreal.TextureCompressionSettings, compression))
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
        texture.set_editor_property("max_texture_size", 1024)
        texture.set_editor_property("address_x", unreal.TextureAddress.TA_WRAP)
        texture.set_editor_property("address_y", unreal.TextureAddress.TA_WRAP)
        if role == "normal_dx":
            texture.set_editor_property("flip_green_channel", False)
        if not A.save_loaded_asset(texture):
            raise RuntimeError("Cannot save regional texture " + name)
        textures[name] = texture
        report.append({"texture": texture.get_path_name(), "role": role,
                       "source_manifest": manifest, "sha256": digest,
                       "srgb": srgb, "max_size": 1024})
    return textures, report


# Geometric normal is independent of MP_NORMAL; no pixel-normal dependency cycle.
# Standard-atmosphere lapse rate is visual-only. Continuous snow shading and
# opaque PBR avoid dither/temporal masks and introduce no geometry displacement.
COMMON = r"""
// Uniform branch: legacy/disabled surfaces exit BEFORE all new texture reads.
// Coverage remains a separate varying guard so its per-pixel derivative path
// is not the predicate of this explicit uniform branch.
[branch] if(Enabled<.5 || Ready<.5) return Original;
if(Coverage<=0.) return Original;
float3 ng=normalize(N); float3 weights=pow(abs(ng),4.);
weights/=max(dot(weights,float3(1.,1.,1.)),.0001);
float3 qb=P/max(BaseTile,50.); float3 qr=P/max(RockTile,50.);
// Continuous, stateless domain warp. Unlike independently rotated square
// tiles this never inserts an artificial UV border. All PBR roles share it;
// no new texture lookup, cell classification, normal displacement or WPO.
struct RegionalDomain {
 float hash(float2 p){float3 q=frac(float3(p.x,p.y,p.x)*float3(.1031,.11369,.13787));q+=dot(q,q.yzx+19.19);return frac((q.x+q.y)*q.z);}
 float noise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3.-2.*f);return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+float2(1,1)),f.x),f.y);}
};
RegionalDomain domain;
qb+=(float3(domain.noise(qb.xy*.23+11.7),domain.noise(qb.yz*.19+37.3),domain.noise(qb.zx*.27+71.9))-.5)*1.35;
qr+=(float3(domain.noise(qr.xy*.23+11.7),domain.noise(qr.yz*.19+37.3),domain.noise(qr.zx*.27+71.9))-.5)*1.65;
float macroVariation=domain.noise(P.xy/13000.+float2(17.3,53.7))*.67+domain.noise(P.xy/87000.+float2(73.1,11.9))*.33;
float forest=saturate(LandUse.g)*saturate(LandUseEnabled);
float agriculture=saturate(LandUse.b)*saturate(LandUseEnabled);
float rock=smoothstep(.22,.58,1.-saturate(abs(ng.z)));
// Recover source MSL from the broad context's presentation-only Earth sag.
float2 outside=max(max(DetailMin.xy-P.xy,P.xy-DetailMax.xy),float2(0.,0.));
float2 spans=float2(P.x<DetailMin.x?DetailMin.x-OuterMin.x:OuterMax.x-DetailMax.x,
                    P.y<DetailMin.y?DetailMin.y-OuterMin.y:OuterMax.y-DetailMax.y);
float2 normalized=outside/max(spans,float2(1.,1.));
float t=saturate(max(normalized.x,normalized.y));float blend=t*t*(3.-2.*t);
float2 local=P.xy-TangentCenter.xy;float r2=dot(local,local);float radius=637100000.;
float sag=-r2/(sqrt(max(1.,radius*radius-r2))+radius)*blend*saturate(CurvatureEnabled);
float elevation=(P.z-OriginZ-sag)*.01;
float localTemp=MeanTemp-.00649*(elevation-ClimateElevation);
float snow=(1.-smoothstep(-3.,2.,localTemp))*smoothstep(.35,.82,abs(ng.z));
float amount=saturate(Coverage)*saturate(Enabled)*saturate(Ready);
"""

# Display-only join between the original257-node detail and overview masks.
# The runtime shares the actual detail texture; it is never resampled into the
# much wider overview raster. A uniform-disabled join performs no extra read.
# Avoid a varying sample branch: implicit derivatives remain defined across
# the bounded edge band and its outside boundary.
SOURCE_MASK_JOIN = r"""
[branch] if(JoinEnabled<.5 || SourceEnabled<.5) return Original;
float2 extent=max(DetailMax.xy-DetailMin.xy,float2(1.,1.));
float2 uv=(saturate((P.xy-DetailMin.xy)/extent)*256.+.5)/257.;
float2 outside=max(max(DetailMin.xy-P.xy,P.xy-DetailMax.xy),float2(0.,0.));
float t=saturate(max(outside.x/max(BlendCm.x,1.),outside.y/max(BlendCm.y,1.)));
float weight=1.-t*t*(3.-2.*t);
float3 detail=Texture2DSample(DetailMask,DetailMaskSampler,uv).rgb;
return lerp(Original,detail,weight);
"""


def texture_object(material, name, texture, sampler):
    return node(material, name, unreal.MaterialExpressionTextureObjectParameter,
                parameter_name=name, texture=texture, sampler_type=sampler)


def author_material(path, textures):
    material = unreal.load_asset(path)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Required existing ground material missing " + path)
    if not material.get_editor_property("tangent_space_normal"):
        raise RuntimeError("Existing ground normal convention changed " + path)
    before = {name: property_connection(material, prop) for name, prop in UNTOUCHED.items()}
    originals = {name: original_connection(material, name, prop) for name, prop in WRAPPED.items()}
    ground_parameters = [(n, n.get_editor_property("texture"))
                         for n in L.get_material_expressions(material)
                         if n.get_class().get_name() == "MaterialExpressionTextureSampleParameter2D"
                         and str(n.get_editor_property("parameter_name")) == "GroundDiffuse"]
    if len(ground_parameters) != 1:
        raise RuntimeError("Expected exact original GroundDiffuse parameter " + path)
    inputs = {
        "P": node(material, "Position", unreal.MaterialExpressionWorldPosition,
                  world_position_shader_offset=enum_value(unreal.WorldPositionIncludedOffsets,
                                                          "WPTEXCLUDEALLSHADEROFFSETS")),
        "N": node(material, "GeometricNormal", unreal.MaterialExpressionVertexNormalWS),
        "Enabled": scalar(material, "RegionalStyleEnabled", 0.),
        "Ready": scalar(material, "RegionalStyleTexturesReady", 0.),
        "BaseTile": scalar(material, "RegionalBaseTileCm", 400.),
        "RockTile": scalar(material, "RegionalRockTileCm", 8000.),
        "MeanTemp": scalar(material, "RegionalMeanTemperatureC", 15.),
        "ClimateElevation": scalar(material, "RegionalClimateElevationM", 0.),
        "OriginZ": scalar(material, "RegionalWorldOriginZ", 0.),
        "Wetness": scalar(material, "RegionalWetness", .25),
        "DryTint": vector(material, "RegionalDrySoilTint", (.4, .34, .25)),
        "RockTint": vector(material, "RegionalRockTint", (.4, .39, .35)),
        "Volcanic": scalar(material, "RegionalVolcanic", 0.),
        "VolcanicActualStyle": scalar(material, "RegionalVolcanicActualStyle", 0.),
        "PixelDepthCm": node(material, "ViewDepth", unreal.MaterialExpressionPixelDepth),
    }
    # Runtime sets this informational slot/month too. Climate temperature already
    # embodies the actual month; no northern-hemisphere seasonal height shortcut.
    scalar(material, "RegionalVisualMonth", 10.)
    scalar(material, "RegionalBiomeSlot", 0.)
    if path.endswith("M_AftershockRegionalBlend"):
        fallback_mask = unreal.load_asset("/Game/Environment/Textures/Presentation/T_PresentationBreakup")
        if not isinstance(fallback_mask, unreal.Texture2D):
            raise RuntimeError("Existing presentation fallback texture is unavailable")
        joined_mask = custom(material, "RegionalSourceMaskJoin", SOURCE_MASK_JOIN,
            {"Original": tagged_existing(material, "RegionalSourceMask"),
             "SourceEnabled": tagged_existing(material, "RegionalSourceMaskEnabled"),
             "JoinEnabled": scalar(material, "RegionalDetailSourceJoinEnabled", 0.),
             "P": inputs["P"],
             "DetailMin": vector(material,"RegionalDetailSourceBoundsMin",(0.,0.,0.)),
             "DetailMax": vector(material,"RegionalDetailSourceBoundsMax",(1.,1.,0.)),
             "BlendCm": vector(material,"RegionalDetailSourceBlendCm",(1.,1.,0.)),
             "DetailMask": texture_object(material,"RegionalDetailSourceMask",fallback_mask,
                                          unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)}, 3)
        # Reconnect every existing raw-mask consumer as well as the new regional
        # land-use wrapper. Water/land and forest/agriculture share one join.
        link(joined_mask, tagged_existing(material, "RegionalWaterCoverage"), "Mask")
        link(joined_mask, tagged_existing(material, "GroundToBiome"), "SourceMask")
        coverage = custom(material, "LandCoverage", "return (1.-saturate(Water))*(1.-saturate(Alpha));",
                          {"Water": tagged_existing(material, "RegionalWaterCoverage"),
                           "Alpha": tagged_existing(material, "IrregularRegionalBlend")}, 1)
        inputs["LandUse"] = joined_mask
        inputs["LandUseEnabled"] = tagged_existing(material, "RegionalSourceMaskEnabled")
    else:
        coverage = node(material, "LandCoverage", unreal.MaterialExpressionConstant, r=1.)
        # Root may bind the EXISTING canonical context mask to this parameter.
        # No new mask is generated here. Missing/disabled masks classify nothing.
        fallback_mask = unreal.load_asset("/Game/Environment/Textures/Presentation/T_PresentationBreakup")
        if not isinstance(fallback_mask, unreal.Texture2D):
            raise RuntimeError("Existing presentation fallback texture is unavailable")
        mask = texture_object(material, "RegionalSourceMask", fallback_mask, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        inputs["LandUseEnabled"] = scalar(material, "RegionalSourceMaskEnabled", 0.)
        inputs["LandUse"] = custom(material, "GroundExistingSourceLandUse", r"""
[branch] if(Enabled<.5 || MaskEnabled<.5) return float3(0,0,0);
float2 extent=max(BoundsMax.xy-BoundsMin.xy,float2(1,1));
float2 uv=(saturate((P.xy-BoundsMin.xy)/extent)*256.+.5)/257.;
return Texture2DSample(SourceMask,SourceMaskSampler,uv).rgb;
""", {"Enabled": inputs["Enabled"], "MaskEnabled": inputs["LandUseEnabled"], "P": inputs["P"],
       "BoundsMin": vector(material,"RegionalSourceBoundsMin",(0.,0.,0.)),
       "BoundsMax": vector(material,"RegionalSourceBoundsMax",(1.,1.,0.)), "SourceMask": mask}, 3)
    inputs["Coverage"] = coverage
    inputs.update({"CurvatureEnabled": scalar(material,"RegionalCurvatureEnabled",0.),
                   "DetailMin": vector(material,"RegionalCurvatureDetailMin",(0.,0.,0.)),
                   "DetailMax": vector(material,"RegionalCurvatureDetailMax",(1.,1.,0.)),
                   "OuterMin": vector(material,"RegionalCurvatureOuterMin",(-1.,-1.,0.)),
                   "OuterMax": vector(material,"RegionalCurvatureOuterMax",(2.,2.,0.)),
                   "TangentCenter": vector(material,"RegionalTangentCenter",(0.,0.,0.))})
    objects = {}
    for prefix, slot in (("RegionalBase", "DrySoil"), ("RegionalRock", "Rock")):
        for role, (suffix, _, _) in ROLES.items():
            sampler = (unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if role == "normal_dx"
                       else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if role == "diffuse"
                       else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
            objects[prefix + suffix] = texture_object(material, prefix + suffix,
                textures["T_Regional" + slot + "_" + suffix], sampler)

    color_inputs = dict(inputs, Original=originals["BaseColor"],
                        Base=objects["RegionalBaseDiffuse"], Rock=objects["RegionalRockDiffuse"])
    # The real-image importer owns these optional historical assets. A proven
    # existing texture is only the shader's disabled fallback; it is NEVER
    # presented as regional imagery while Ready=0 or outside the actual ROI.
    color_inputs.update({"MacroTexture": texture_object(material,"RegionalMacroTexture",textures["T_RegionalDrySoil_Diffuse"],unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
                         "MacroReady": scalar(material,"RegionalMacroReady",0.),
                         "MacroMin": vector(material,"RegionalMacroBoundsMin",(0.,0.,0.)),
                         "MacroMax": vector(material,"RegionalMacroBoundsMax",(1.,1.,0.))})
    color = custom(material, "WrapBaseColor", COMMON + r"""
float3 base=Texture2DSample(Base,BaseSampler,qb.xy).rgb;
float3 cliff=Texture2DSample(Rock,RockSampler,qr.yz).rgb*weights.x
            +Texture2DSample(Rock,RockSampler,qr.xz).rgb*weights.y
            +Texture2DSample(Rock,RockSampler,qr.xy).rgb*weights.z;
// Source albedo remains dominant; regional colors are subtle art interpretation,
// never a claim that every pixel is mapped or sampled geological bedrock.
float3 bt=DryTint/max(max(DryTint.r,DryTint.g),max(DryTint.b,.25));
float3 rt=RockTint/max(max(RockTint.r,RockTint.g),max(RockTint.b,.25));
float3 regional=lerp(base*lerp(float3(1,1,1),bt,.12),cliff*lerp(float3(1,1,1),rt,.14),rock);
regional*=lerp(.82,1.16,macroVariation);
// Volcanic terrain cannot inherit the scan's bright green moss everywhere.
// Absolute profile reflectance controls exposed basalt; actual mapped forest
// and farmland remain distinct rather than being recoloured as fresh lava.
float exposed=saturate(Volcanic)*saturate(VolcanicActualStyle)*(1.-saturate(max(forest,agriculture)));
float grain=dot(lerp(base,cliff,rock),float3(.2126,.7152,.0722));
float3 basalt=RockTint*clamp(.38+grain*.85,.38,1.05)*lerp(.82,1.12,macroVariation);
regional=lerp(regional,basalt,exposed*.96);
regional=lerp(regional,regional*.46+float3(.018,.043,.012),forest*.72);
regional=lerp(regional,lerp(DryTint*.47,DryTint*.82,macroVariation),agriculture*.48);
[branch] if(MacroReady>.5){
 float2 extent=max(MacroMax.xy-MacroMin.xy,float2(1.,1.));float2 uv=(P.xy-MacroMin.xy)/extent;
 float inside=step(0.,uv.x)*step(uv.x,1.)*step(0.,uv.y)*step(uv.y,1.);
 float3 photograph=Texture2DSample(MacroTexture,MacroTextureSampler,saturate(uv)).rgb;
 // Historical2016/17 Sentinel composite supplies broad land pattern, not
 // current weather, a surveyed parcel or a physically exact PBR albedo.
 float mix=lerp(.20,.78,smoothstep(25000.,250000.,PixelDepthCm))*inside;
 float fine=lerp(.94,1.06,saturate(grain*1.8));regional=lerp(regional,photograph*fine,mix);
}
regional*=lerp(1.,.78,saturate(Wetness));
regional=lerp(regional,float3(.72,.78,.81),snow);
return lerp(Original,regional,amount);
""", color_inputs, 3)
    rough_inputs = dict(inputs, Original=originals["Roughness"],
                        Base=objects["RegionalBaseRoughness"], Rock=objects["RegionalRockRoughness"])
    rough = custom(material, "WrapRoughness", COMMON + r"""
float base=Texture2DSample(Base,BaseSampler,qb.xy).r;
float cliff=Texture2DSample(Rock,RockSampler,qr.yz).r*weights.x
           +Texture2DSample(Rock,RockSampler,qr.xz).r*weights.y
           +Texture2DSample(Rock,RockSampler,qr.xy).r*weights.z;
float regional=lerp(base,cliff,rock)*lerp(1.,.68,saturate(Wetness));
regional=lerp(regional,.90,forest*.6);regional=lerp(regional,.84,agriculture*.4);
return lerp(Original,lerp(clamp(regional,.18,.99),.94,snow),amount);
""", rough_inputs, 1)
    normal_inputs = dict(inputs, Original=originals["Normal"],
                         Base=objects["RegionalBaseNormalDX"], Rock=objects["RegionalRockNormalDX"])
    normal = custom(material, "WrapNormal", COMMON + r"""
// Custom texture objects are raw BC5, so reconstruct Z explicitly. Base follows
// world XY; rock uses three projections, avoiding cliff texture stretching.
float2 b=Texture2DSample(Base,BaseSampler,qb.xy).rg*2.-1.;
float3 baseWorld=normalize(ng+float3(b*.38,0.));
float2 x=Texture2DSample(Rock,RockSampler,qr.yz).rg*2.-1.;
float2 y=Texture2DSample(Rock,RockSampler,qr.xz).rg*2.-1.;
float2 z=Texture2DSample(Rock,RockSampler,qr.xy).rg*2.-1.;
float3 nx=float3(sqrt(saturate(1.-dot(x,x)))*sign(ng.x),x.x,x.y);
float3 ny=float3(y.x,sqrt(saturate(1.-dot(y,y)))*sign(ng.y),y.y);
float3 nz=float3(z.x,z.y,sqrt(saturate(1.-dot(z,z)))*sign(ng.z));
float3 world=normalize(lerp(baseWorld,normalize(nx*weights.x+ny*weights.y+nz*weights.z),rock));
world=normalize(lerp(world,ng,max(forest,agriculture)*.35));
world=normalize(lerp(world,ng,snow*.8));
float3 tangent=TransformWorldVectorToTangent(Parameters.TangentToWorld,world);
return normalize(lerp(Original,tangent,amount));
""", normal_inputs, 3)
    outputs = {"BaseColor": color, "Roughness": rough, "Normal": normal}
    for name, expression in outputs.items():
        if not L.connect_material_property(expression, "", WRAPPED[name]):
            raise RuntimeError("Cannot attach regional wrapper " + name)
        if expression_input_connection(material, expression, "Original") != originals[name]:
            raise RuntimeError("Original material output/channel was not preserved " + name)
    after = {name: property_connection(material, prop) for name, prop in UNTOUCHED.items()}
    if after != before:
        raise RuntimeError("Regional style changed an unowned material output")
    if ground_parameters[0][0].get_editor_property("texture") != ground_parameters[0][1]:
        raise RuntimeError("Original GroundDiffuse texture was modified")
    L.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    L.layout_material_expressions(material)
    errors = L.recompile_material(material)
    if errors:
        raise RuntimeError("Regional style shader compile failed " + path + ": " + str(list(errors)))
    if not A.save_loaded_asset(material):
        raise RuntimeError("Cannot save regional ground style " + path)
    return {"material": path, "default_enabled": 0., "default_textures_ready": 0.,
            "original_connections": {name: {"node": value[0].get_path_name(), "output": value[1]}
                                     for name, value in originals.items()},
            "original_ground_diffuse": ground_parameters[0][1].get_path_name(),
            "untouched_outputs": list(UNTOUCHED), "compile_errors": [],
            "legacy_water_and_globe_coverage_preserved": True,
            "regional_source_water_landuse_join": path.endswith("M_AftershockRegionalBlend"),
            "detail_mask_shared_without_overview_resampling": path.endswith("M_AftershockRegionalBlend"),
            "additional_exact_detail_mask_sample_when_join_enabled": 1 if path.endswith("M_AftershockRegionalBlend") else 0,
            "new_unique_pbr_texture_parameters": 6, "new_active_pbr_samples": 12,
            "additional_ground_existing_mask_sample_when_enabled": not path.endswith("M_AftershockRegionalBlend"),
            "additional_optional_macro_sample_when_ready": 1, "macro_ready_default": 0.,
            "domain_warp_additional_texture_samples": 0, "macro_variation_additional_texture_samples": 0,
            "disabled_branch_returns_original_before_new_samples": True}


def main():
    inventory = source_inventory()  # Verify ALL nine sources before touching assets.
    textures, imported = import_textures(inventory)
    materials = [author_material(path, textures) for path in MATERIALS]
    report = {"textures": imported, "materials": materials,
              "dependency_order": ["ue_upgrade_presentation_materials.py", "ue_import_cc0_ground.py",
                                   "ue_upgrade_globe_climate.py", "ue_author_regional_surface_styles.py"],
              "license": "CC0-1.0", "license_scope": "Only the nine imported PBR maps; optional EOX imagery has a separate CC-BY-4.0 importer/manifest/attribution",
              "optional_macro_license": "CC-BY-4.0", "optional_macro_scope": "Historical EOX2016 cloudless Sentinel composite, not current imagery or exact measured PBR albedo",
              "original_shader_wrapper": True,
              "continuous_uv_domain_warp": True, "macro_colour_noise": True,
              "nonperiodic_style_additional_texture_samples": 0,
              "volcanic_absolute_profile_reflectance": True, "mapped_forest_agriculture_preserved": True,
              "climate_source": "NASA POWER/MERRA2 monthly climatology supplied by canonical regional profile",
              "snow_scope": "artistic air-temperature/lapse-rate coating; not observed snow/weather/forecast",
              "lapse_rate_c_per_m": .00649, "slope_source": "VertexNormalWS",
              "base_tiles_cm": {"LeafyGrass": 200, "DrySoil": 400, "ForestFloor": 200, "Rock": 8000},
              "wpo": False, "height_writes": 0, "terrain_authority_changed": False,
              "legacy_version_1_2_runtime_style_disabled": True,
              "proof_scope": "UE import, graph readback/compile/save only; live visual and GPU profile proof separate"}
    target = ROOT / "Saved" / "PresentationValidation" / "regional_surface_styles.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    unreal.log_warning("AFTERSHOCK_REGIONAL_SURFACE_STYLES_READY " + json.dumps(report))


if __name__ == "__main__":
    main()
