"""Author project-owned weather/globe shaders; no external assets or graph deletion.

Run via UnrealEditor-Cmd -run=pythonscript -script=<absolute script path>.
"""
import unreal

L = unreal.MaterialEditingLibrary


def node(m, cls, **values):
    n = L.create_material_expression(m, cls)
    for key, value in values.items():
        n.set_editor_property(key, value)
    return n


def link(a, b, pin="", output=""):
    if not L.connect_material_expressions(a, output, b, pin):
        raise RuntimeError("Cannot connect {} -> {}.{}".format(a.get_name(), b.get_name(), pin))


def prop(m, n, p, output=""):
    if not L.connect_material_property(n, output, p):
        raise RuntimeError("Cannot connect material property " + str(p))


def scalar(m, name, value):
    return node(m, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=value)


def color(m, name, rgb):
    return node(m, unreal.MaterialExpressionVectorParameter, parameter_name=name,
                default_value=unreal.LinearColor(*rgb, 1.0))


def constant(m, value):
    return node(m, unreal.MaterialExpressionConstant, r=value)


def binary(m, cls, a, b):
    n = node(m, cls)
    link(a, n, "A"); link(b, n, "B")
    return n


def mul(m, a, b):
    return binary(m, unreal.MaterialExpressionMultiply, a, b)


def add(m, a, b):
    return binary(m, unreal.MaterialExpressionAdd, a, b)


def finish(m):
    L.set_base_material_usage(m, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    errors = L.recompile_material(m)
    if errors:
        raise RuntimeError("Material compile errors {}: {}".format(m.get_path_name(), errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(m):
        raise RuntimeError("Cannot save " + m.get_path_name())
    unreal.log_warning("AFTERSHOCK_WEATHER_GLOBE_MATERIAL_READY " + m.get_path_name())


def create(path, translucent=False, unlit=False):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.load_asset(path)
        finish(existing)
        return None
    folder, name = path.rsplit("/", 1)
    unreal.EditorAssetLibrary.make_directory(folder)
    m = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("two_sided", translucent)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent else unreal.BlendMode.BLEND_OPAQUE)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT if unlit else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    return m


FLOOD_TAG = "AFTERSHOCK_FLOOD_READABILITY_V2:"


def flood_node(m, label, cls, **values):
    matches = [n for n in L.get_material_expressions(m)
               if str(n.get_editor_property("desc")) == FLOOD_TAG + label]
    if len(matches) > 1:
        raise RuntimeError("Duplicate flood upgrade node: " + label)
    n = matches[0] if matches else node(m, cls)
    if n.get_class() != cls.static_class():
        raise RuntimeError("Unexpected flood node type: " + label)
    n.set_editor_property("desc", FLOOD_TAG + label)
    for key, value in values.items():
        n.set_editor_property(key, value)
    return n


def flood_parameter(m, name, value, vector=False):
    cls = unreal.MaterialExpressionVectorParameter if vector else unreal.MaterialExpressionScalarParameter
    matches = [n for n in L.get_material_expressions(m)
               if n.get_class() == cls.static_class()
               and str(n.get_editor_property("parameter_name")) == name]
    if len(matches) > 1:
        raise RuntimeError("Duplicate flood material parameter: " + name)
    n = matches[0] if matches else flood_node(m, name, cls, parameter_name=name)
    n.set_editor_property("default_value", unreal.LinearColor(*value, 1.0) if vector else float(value))
    return n


def flood_constant(m, label, value):
    return flood_node(m, label, unreal.MaterialExpressionConstant, r=float(value))


def flood_binary(m, label, cls, a, b):
    n = flood_node(m, label, cls)
    link(a, n, "A"); link(b, n, "B")
    return n


def make_flood():
    path = "/Game/FX/Aftershock/Materials/M_DisasterFloodSurface"
    m = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else create(path, True)
    if not isinstance(m, unreal.Material):
        raise RuntimeError("Flood surface must be a base material")
    # Update the SAME asset on every run. Preserve rooted legacy expressions,
    # the artistic 1.8 cm WPO and existing runtime parameter names.
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("tangent_space_normal", True)
    m.set_editor_property("translucency_lighting_mode", enum_value(unreal.TranslucencyLightingMode, "TLMSURFACE"))
    m.set_editor_property("screen_space_reflections", False)
    original_wpo = L.get_material_property_input_node(m, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    water = flood_parameter(m, "WaterTint", (0.07, 0.29, 0.30), True)
    opacity = flood_parameter(m, "Opacity", 0.78)
    foam_tint = flood_parameter(m, "FloodFoamTint", (0.42, 0.49, 0.40), True)
    normal_strength = flood_parameter(m, "FloodNormalStrength", 0.10)
    foam_strength = flood_parameter(m, "FloodFoamStrength", 0.12)
    emissive_lift = flood_parameter(m, "FloodReadabilityLift", 0.22)
    visual_strength = flood_parameter(m, "VisualStrength", 1.0)
    position = flood_node(m, "WorldPosition", unreal.MaterialExpressionWorldPosition,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    time = flood_node(m, "Time", unreal.MaterialExpressionTime)

    def wave(label, direction, speed):
        direction_node = flood_node(m, label + "Direction", unreal.MaterialExpressionConstant3Vector,
                                    constant=unreal.LinearColor(*direction, 1.0))
        phase = flood_binary(m, label + "SpatialPhase", unreal.MaterialExpressionDotProduct, position, direction_node)
        flow = flood_binary(m, label + "FlowPhase", unreal.MaterialExpressionMultiply, time,
                            flood_constant(m, label + "Speed", speed))
        phase = flood_binary(m, label + "Phase", unreal.MaterialExpressionAdd, phase, flow)
        result = flood_node(m, label + "Sine", unreal.MaterialExpressionSine, period=1.0)
        link(phase, result)
        return result

    # Two inexpensive analytic frequencies, continuous across every flood tile.
    a = wave("WaveA", (0.0022, 0.0007, 0.0), 0.13)
    b = wave("WaveB", (-0.0011, 0.0027, 0.0), 0.095)
    nx = flood_binary(m, "NormalX", unreal.MaterialExpressionMultiply, a, normal_strength)
    ny = flood_binary(m, "NormalY", unreal.MaterialExpressionMultiply, b, normal_strength)
    xy = flood_binary(m, "NormalXY", unreal.MaterialExpressionAppendVector, nx, ny)
    xyz = flood_binary(m, "NormalXYZ", unreal.MaterialExpressionAppendVector, xy, flood_constant(m, "NormalZ", 1.0))
    normal = flood_node(m, "NormalNormalized", unreal.MaterialExpressionNormalize); link(xyz, normal)
    prop(m, normal, unreal.MaterialProperty.MP_NORMAL)
    combined = flood_binary(m, "WaveSum", unreal.MaterialExpressionAdd, a, b)
    combined = flood_binary(m, "WaveMean", unreal.MaterialExpressionMultiply, combined, flood_constant(m, "Half", 0.5))
    crest = flood_binary(m, "CrestThreshold", unreal.MaterialExpressionAdd, combined, flood_constant(m, "CrestCutoff", -0.63))
    crest = flood_binary(m, "CrestContrast", unreal.MaterialExpressionMultiply, crest, flood_constant(m, "CrestGain", 3.0))
    saturated = flood_node(m, "CrestSaturated", unreal.MaterialExpressionSaturate); link(crest, saturated)
    foam = flood_binary(m, "CrestFoamStrength", unreal.MaterialExpressionMultiply, saturated, foam_strength)
    foam = flood_binary(m, "CrestEventStrength", unreal.MaterialExpressionMultiply, foam, visual_strength)
    base = flood_node(m, "WaterAndFoam", unreal.MaterialExpressionLinearInterpolate)
    link(water, base, "A"); link(foam_tint, base, "B"); link(foam, base, "Alpha")
    prop(m, base, unreal.MaterialProperty.MP_BASE_COLOR)
    # A restrained low-energy colour lift makes shallow inundation readable in
    # distant/poorly sampled translucency lighting volumes; it is not an unlit
    # neon surface. No screen-depth lookup or per-light forward shading is used.
    emission = flood_binary(m, "ReadabilityEmission", unreal.MaterialExpressionMultiply, base, emissive_lift)
    prop(m, emission, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    prop(m, opacity, unreal.MaterialProperty.MP_OPACITY)
    prop(m, flood_parameter(m, "FloodRoughness", 0.26), unreal.MaterialProperty.MP_ROUGHNESS)
    if not L.get_material_property_input_node(m, unreal.MaterialProperty.MP_SPECULAR):
        prop(m, flood_constant(m, "Specular", 0.45), unreal.MaterialProperty.MP_SPECULAR)
    if original_wpo is None:
        amplitude = flood_parameter(m, "WaveAmplitudeCm", 1.8)
        height = flood_binary(m, "InitialWPOHeight", unreal.MaterialExpressionMultiply, combined, amplitude)
        up = flood_node(m, "InitialWPOUp", unreal.MaterialExpressionConstant3Vector,
                        constant=unreal.LinearColor(0, 0, 1, 1))
        prop(m, flood_binary(m, "InitialWPO", unreal.MaterialExpressionMultiply, height, up),
             unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    elif L.get_material_property_input_node(m, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET) != original_wpo:
        raise RuntimeError("Flood upgrade unexpectedly changed existing WPO")
    finish(m)
    unreal.log_warning("AFTERSHOCK_FLOOD_READABILITY_V2_READY path={} lighting=SurfaceTranslucencyVolume opacity=.78 normal=.10 foam=.12 emissiveLift=.22 oldWPOPreserved={}".format(
        path, original_wpo is not None))


def add_continuous_flood_lift():
    """Lift the snapshot mesh from encoded canonical heights, preserving waves.

    CPU snapshot vertices start at canonical ground - 26 cm. Their red channel
    holds (18 + 1.8 * bilinearSeverity + 26) / 256. Only the shared envelope is
    animated at runtime; no fluid calculation or per-tile transform is added.
    """
    path = "/Game/FX/Aftershock/Materials/M_DisasterFloodSurface"
    m = unreal.load_asset(path)
    if not isinstance(m, unreal.Material):
        raise RuntimeError("Create the flood material before adding canonical surface lift")
    current_wpo = L.get_material_property_input_node(m, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    if not current_wpo:
        raise RuntimeError("Preserve the existing flood wave graph before adding lift")
    root_tag = FLOOD_TAG + "ContinuousCanonicalLiftRoot"
    roots = [n for n in L.get_material_expressions(m)
             if str(n.get_editor_property("desc")) == root_tag]
    if len(roots) > 1:
        raise RuntimeError("Duplicate continuous flood lift roots; preserve for review")
    existing_root = roots[0] if roots else None
    if existing_root and current_wpo == existing_root:
        connected = list(L.get_inputs_for_material_expression(m, existing_root))
        if not connected or not connected[0] or connected[0] == existing_root:
            raise RuntimeError("Tagged flood lift root has no valid preserved legacy A input")
        legacy_wave = connected[0]
    else:
        legacy_wave = current_wpo
        # An external wrapper could already contain our lift root. Reusing that
        # wrapper as A would introduce a cycle, so fail before rewiring anything.
        pending = [legacy_wave]
        visited = set()
        while pending:
            expression = pending.pop()
            if not expression or expression.get_path_name() in visited:
                continue
            visited.add(expression.get_path_name())
            if existing_root and expression == existing_root:
                raise RuntimeError("Existing WPO wraps the tagged lift root; preserve for manual review")
            pending.extend(L.get_inputs_for_material_expression(m, expression))
    vertices = flood_node(m, "ContinuousLiftVertexColor", unreal.MaterialExpressionVertexColor)
    decode = flood_node(m, "ContinuousLiftDecodeCm", unreal.MaterialExpressionMultiply)
    link(vertices, decode, "A", "R")
    link(flood_constant(m, "ContinuousLiftEncodingScale", 256.0), decode, "B")
    envelope = flood_parameter(m, "FloodEnvelope", 0.0)
    height = flood_binary(m, "ContinuousLiftEnvelope", unreal.MaterialExpressionMultiply, decode, envelope)
    up = flood_node(m, "ContinuousLiftUp", unreal.MaterialExpressionConstant3Vector,
                    constant=unreal.LinearColor(0, 0, 1, 1))
    lift = flood_binary(m, "ContinuousLiftOffset", unreal.MaterialExpressionMultiply, height, up)
    combined = flood_binary(m, "ContinuousCanonicalLiftRoot", unreal.MaterialExpressionAdd, legacy_wave, lift)
    prop(m, combined, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    inputs = list(L.get_inputs_for_material_expression(m, combined))
    if len(inputs) < 2 or inputs[0] != legacy_wave or inputs[1] != lift:
        raise RuntimeError("Continuous flood lift did not retain legacy waves in A and decoded height in B")
    finish(m)
    unreal.log_warning("AFTERSHOCK_FLOOD_CONTINUOUS_LIFT_READY path={} encodedChannel=VertexColor.R decodeCm=256 runtimeParameter=FloodEnvelope default=0 oldWaveA={} liftB={} graphDeletion=0".format(
        path, legacy_wave.get_path_name(), lift.get_path_name()))


def make_streak(path, tint, opacity):
    m = create(path, True, True)
    if not m: return
    prop(m, color(m, "Tint", tint), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    uv = node(m, unreal.MaterialExpressionTextureCoordinate)
    mask = node(m, unreal.MaterialExpressionComponentMask, r=True, g=False, b=False, a=False); link(uv, mask)
    centered = add(m, mask, constant(m, -0.5))
    absolute = node(m, unreal.MaterialExpressionAbs); link(centered, absolute)
    fade = node(m, unreal.MaterialExpressionOneMinus); link(mul(m, absolute, constant(m, 2.0)), fade)
    prop(m, mul(m, mul(m, fade, fade), scalar(m, "Opacity", opacity)), unreal.MaterialProperty.MP_OPACITY)
    finish(m)


def make_globe():
    m = create("/Game/Environment/Materials/Presentation/M_AftershockGlobe")
    if m:
        vertex = node(m, unreal.MaterialExpressionVertexColor)
        prop(m, vertex, unreal.MaterialProperty.MP_BASE_COLOR)
        prop(m, vertex, unreal.MaterialProperty.MP_ROUGHNESS, "A")
        prop(m, constant(m, 0.45), unreal.MaterialProperty.MP_SPECULAR)
        finish(m)
    m = create("/Game/Environment/Materials/Presentation/M_AftershockGlobeAtmosphere", True, True)
    if m:
        fresnel = node(m, unreal.MaterialExpressionFresnel)
        prop(m, color(m, "AtmosphereTint", (0.09, 0.40, 0.90)), unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        prop(m, mul(m, fresnel, constant(m, 0.15)), unreal.MaterialProperty.MP_OPACITY)
        finish(m)


def enum_value(enum, normalized_name):
    for name in dir(enum):
        if name.replace("_", "").upper() == normalized_name:
            return getattr(enum, name)
    raise RuntimeError("Missing reflected enum " + normalized_name)


def add_foliage_sway():
    m = unreal.load_asset("/Game/Environment/Materials/Presentation/M_PresentationKenneyAtlas")
    if not m: raise RuntimeError("Kenney atlas must exist before adding storm sway")
    if any(n.get_class().get_name() == "MaterialExpressionScalarParameter" and
           str(n.get_editor_property("parameter_name")) == "StormStrength" for n in L.get_material_expressions(m)):
        finish(m); return
    position = node(m, unreal.MaterialExpressionWorldPosition,
                    world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    local = node(m, unreal.MaterialExpressionTransformPosition,
                 transform_source_type=enum_value(unreal.MaterialPositionTransformSource, "TRANSFORMPOSSOURCEWORLD"),
                 transform_type=enum_value(unreal.MaterialPositionTransformSource, "TRANSFORMPOSSOURCEINSTANCE"))
    link(position, local)
    z = node(m, unreal.MaterialExpressionComponentMask, r=False, g=False, b=True, a=False); link(local, z)
    height = mul(m, z, constant(m, 0.01))
    clamp = node(m, unreal.MaterialExpressionSaturate); link(height, clamp)
    phase = binary(m, unreal.MaterialExpressionDotProduct, position,
                   node(m, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0.003, 0.002, 0)))
    phase = add(m, phase, mul(m, node(m, unreal.MaterialExpressionTime), constant(m, 0.45)))
    wave = node(m, unreal.MaterialExpressionSine); link(phase, wave)
    intensity = node(m, unreal.MaterialExpressionPerInstanceCustomData, data_index=0, const_default_value=1.0)
    amount = mul(m, mul(m, wave, clamp), mul(m, intensity, scalar(m, "StormStrength", 0.0)))
    amount = mul(m, amount, scalar(m, "StormAmplitudeCm", 22.0))
    prop(m, mul(m, amount, color(m, "StormDirection", (1.0, 0.35, 0.0))), unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    finish(m)


make_flood()
add_continuous_flood_lift()
make_streak("/Game/FX/Aftershock/Materials/M_DisasterWindStreak", (0.72, 0.80, 0.78), 0.40)
make_streak("/Game/FX/Aftershock/Materials/M_DisasterRainStreak", (0.36, 0.52, 0.64), 0.35)
make_globe()
add_foliage_sway()
