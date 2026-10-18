"""Author four original coastal/dam/tsunami materials, with tagged graph reuse.

No texture downloads, fluid solver, graph deletion, map or gameplay mutation.
Tsunami inputs are event-time vertex data and canonical snapshot display metadata.
"""
import unreal

L = unreal.MaterialEditingLibrary
TAG = "AFTERSHOCK_COASTAL_VISUAL_V1:"


def enum_value(cls, normalized):
    for name in dir(cls):
        if name.replace("_", "").upper() == normalized:
            return getattr(cls, name)
    raise RuntimeError("Missing reflected enum: " + normalized)


def node(m, key, cls, **values):
    found = [n for n in L.get_material_expressions(m) if str(n.get_editor_property("desc")) == TAG + key]
    if len(found) > 1:
        raise RuntimeError("Duplicate coastal expression: " + key)
    n = found[0] if found else L.create_material_expression(m, cls)
    if n.get_class() != cls.static_class():
        raise RuntimeError("Coastal node class mismatch: " + key)
    n.set_editor_property("desc", TAG + key)
    for name, value in values.items():
        n.set_editor_property(name, value)
    return n


def link(a, b, pin="", channel=""):
    if not L.connect_material_expressions(a, channel, b, pin):
        raise RuntimeError("Coastal material link failed: " + pin)


def output(m, n, prop, channel=""):
    if not L.connect_material_property(n, channel, prop):
        raise RuntimeError("Coastal output failed: " + str(prop))


def scalar(m, name, value):
    return node(m, name, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=float(value))


def custom(m, key, code, inputs, width=3):
    n = node(m, key, unreal.MaterialExpressionCustom, code=code,
             output_type=enum_value(unreal.CustomMaterialOutputType, "CMOTFLOAT" + str(width)))
    fields = []
    for name in inputs:
        field = unreal.CustomInput(); field.set_editor_property("input_name", name); fields.append(field)
    n.set_editor_property("inputs", fields)
    for name, source in inputs.items():
        if isinstance(source, tuple):
            link(source[0], n, name, source[1])
        else:
            link(source, n, name)
    return n


def material(path, translucent=False):
    m = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if m is None:
        folder, name = path.rsplit("/", 1); unreal.EditorAssetLibrary.make_directory(folder)
        m = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    if not isinstance(m, unreal.Material):
        raise RuntimeError("Expected coastal base material: " + path)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_OPAQUE)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("two_sided", translucent)
    if translucent:
        m.set_editor_property("translucency_lighting_mode", enum_value(unreal.TranslucencyLightingMode, "TLMSURFACE"))
        m.set_editor_property("screen_space_reflections", False)
    L.set_base_material_usage(m, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    return m


def common(m):
    return {"P": node(m, "WorldPosition", unreal.MaterialExpressionWorldPosition,
                      world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS),
            "TimeValue": node(m, "Time", unreal.MaterialExpressionTime)}


def make_sea():
    m = material("/Game/Environment/Materials/Presentation/M_CoastalSea", True)
    data = common(m)
    color = custom(m, "BlueGreenWater", r"""
float ripple=sin(P.x*.009+TimeValue*.8)*sin(P.y*.006-TimeValue*.65);
return lerp(float3(.015,.095,.14),float3(.035,.19,.22),ripple*.18+.45);
""", data)
    output(m, color, unreal.MaterialProperty.MP_BASE_COLOR)
    output(m, custom(m, "SeaNormal", "return normalize(float3(cos(P.x*.012+TimeValue)*.12,sin(P.y*.009-TimeValue*.8)*.09,1.0));", data), unreal.MaterialProperty.MP_NORMAL)
    output(m, custom(m, "SeaSwell", "return float3(0,0,(sin(P.x*.004+TimeValue*.7)+sin(P.y*.003-TimeValue*.55))*.9);", data), unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    fade = node(m, "ShoreContact", unreal.MaterialExpressionDepthFade, fade_distance_default=100.0, opacity_default=.93)
    output(m, fade, unreal.MaterialProperty.MP_OPACITY)
    output(m, scalar(m, "Roughness", .23), unreal.MaterialProperty.MP_ROUGHNESS)
    output(m, scalar(m, "Specular", .52), unreal.MaterialProperty.MP_SPECULAR)
    lift = node(m, "AmbientFloor", unreal.MaterialExpressionMultiply, const_b=.035); link(color,lift,"A")
    output(m,lift,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return m


def make_dam(steel=False):
    name = "M_DamSteel" if steel else "M_DamConcrete"
    m = material("/Game/Environment/Materials/Presentation/" + name)
    p = common(m)["P"]
    code = "return float3(.055,.075,.085);" if steel else r"""
float grain=frac(sin(dot(floor(P*.12),float3(12.9898,78.233,37.719)))*43758.5453);
float joints=smoothstep(.03,.065,abs(frac(P.z/90.)-.5));
return float3(.37,.39,.36)*(.92+grain*.10)*lerp(.88,1.,joints);
"""
    output(m, custom(m,"SurfaceColor",code,{"P":p}), unreal.MaterialProperty.MP_BASE_COLOR)
    output(m,scalar(m,"Roughness",.36 if steel else .86),unreal.MaterialProperty.MP_ROUGHNESS)
    output(m,scalar(m,"Metallic",.72 if steel else 0),unreal.MaterialProperty.MP_METALLIC)
    return m


def make_tsunami():
    m = material("/Game/FX/Aftershock/Materials/M_DisasterTsunamiWave", True)
    data = common(m)
    vertex = node(m,"CanonicalWaveData",unreal.MaterialExpressionVertexColor)
    flow = node(m,"PropagationDirection",unreal.MaterialExpressionTextureCoordinate)
    data.update({"Depth":(vertex,"R"),"Arrival":(vertex,"G"),"Ocean":(vertex,"B"),"Flow":flow,
                 "Progress":scalar(m,"TsunamiProgress",0),"Drain":scalar(m,"TsunamiDrain",0),
                 "CrestHeight":scalar(m,"CrestHeightCm",300)})
    wave = r"""
float delta=Progress-Arrival;
float wet=smoothstep(-.055,.065,delta)*Drain;
float crest=exp(-pow(delta/.043,2.0))*Drain;
"""
    offset = custom(m,"AdvancingMarineCrest",wave+r"""
float column=(Depth*2048.+4.)*wet*lerp(1.,.40,Ocean);
float ripple=sin(dot(P.xy,Flow.xy)*.014-TimeValue*1.7)*1.6*wet;
float localCrest=min(CrestHeight,Depth*2048.*.70);
return float3(0,0,column+localCrest*crest+ripple);
""",data)
    output(m,offset,unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    optics = custom(m,"MarineDepthAndCrestFoam",wave+r"""
float2 f=normalize(Flow.xy+float2(.00001,0));
float broken=.5+.5*sin(dot(P.xy,float2(-f.y,f.x))*.022+TimeValue*2.3);
float foam=pow(crest,.6)*(.34+.23*broken);
float3 water=lerp(float3(.035,.13,.14),float3(.015,.09,.16),Ocean);
float3 colour=lerp(water,float3(.66,.74,.70),saturate(foam));
float alpha=.86*saturate(wet+crest)*smoothstep(.0,.06,Drain);
return float4(colour,alpha);
""",data,4)
    rgb=node(m,"WaveColor",unreal.MaterialExpressionComponentMask,r=True,g=True,b=True,a=False)
    alpha=node(m,"WaveAlpha",unreal.MaterialExpressionComponentMask,r=False,g=False,b=False,a=True)
    link(optics,rgb);link(optics,alpha)
    output(m,rgb,unreal.MaterialProperty.MP_BASE_COLOR)
    fade=node(m,"LandContact",unreal.MaterialExpressionDepthFade,fade_distance_default=28.,opacity_default=1.)
    link(alpha,fade,"Opacity");output(m,fade,unreal.MaterialProperty.MP_OPACITY)
    normal=custom(m,"CrestAnalyticNormal",wave+r"""
float slope=clamp(-delta/.043*crest,-.7,.7);
float2 f=normalize(Flow.xy+float2(.00001,0));
return normalize(float3(f*slope+float2(sin(P.x*.014),cos(P.y*.011))*.04*wet,1));
""",data)
    output(m,normal,unreal.MaterialProperty.MP_NORMAL)
    output(m,scalar(m,"Roughness",.21),unreal.MaterialProperty.MP_ROUGHNESS)
    output(m,scalar(m,"Specular",.55),unreal.MaterialProperty.MP_SPECULAR)
    lift=node(m,"AmbientReadability",unreal.MaterialExpressionMultiply,const_b=.04);link(rgb,lift,"A")
    output(m,lift,unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return m


def main():
    for m in (make_sea(),make_dam(),make_dam(True),make_tsunami()):
        errors=L.recompile_material(m)
        if errors: raise RuntimeError("Coastal shader compile failed: "+str(errors))
        if not unreal.EditorAssetLibrary.save_loaded_asset(m): raise RuntimeError("Coastal save failed")
        unreal.log_warning("AFTERSHOCK_COASTAL_MATERIAL_READY "+m.get_path_name())


if __name__ == "__main__":
    main()
