"""Author photorealistic, distance-attenuated river water materials for AFTERSHOCK in UE 5.8.

Diagnoses and resolves:
1. High-frequency normal aliasing and moiré raster interference at God-view distances
   via distance-attenuated multi-octave river currents.
2. Specular zebra striping and mirror flashing by switching to physical PBR roughness (0.20)
   and standard water specular (0.50).
3. Hard polygon edges via smooth DepthFade shoreline opacity.
4. Muddy teal albedo via Beer-Lambert optical depth absorption (shallow clear emerald to deep Siberian navy).
5. Complete elimination of legacy 21.2° planar sinusoidal WPO displacement.
"""

import unreal

LIB = unreal.MaterialEditingLibrary
TAG = "AFTERSHOCK_RIVER_WATER_V2:"


def node(material, label, cls, x=0, y=0, **values):
    matches = [n for n in LIB.get_material_expressions(material)
               if str(n.get_editor_property("desc")) == TAG + label]
    n = matches[0] if matches else LIB.create_material_expression(material, cls, x, y)
    n.set_editor_property("desc", TAG + label)
    for key, value in values.items():
        n.set_editor_property(key, value)
    return n


def build_realistic_water(mat_path):
    material = unreal.load_asset(mat_path)
    if not material or not isinstance(material, unreal.Material):
        unreal.log_error(f"Failed to load material: {mat_path}")
        return False

    # Textures
    tex_normal = unreal.load_asset('/Engine/Functions/Engine_MaterialFunctions02/ExampleContent/Textures/water_n')
    tex_diffuse = unreal.load_asset('/Engine/Functions/Engine_MaterialFunctions02/ExampleContent/Textures/water_d')

    if not tex_normal or not tex_diffuse:
        unreal.log_error("Engine water textures missing!")
        return False

    # Core material configuration
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("tangent_space_normal", True)
    material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    material.set_editor_property("screen_space_reflections", True)

    # 1. World Position (excluding shader offsets) and XY mask
    world_pos = node(material, "WorldPos", unreal.MaterialExpressionWorldPosition, -2200, 0,
                     world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)

    mask_xy = node(material, "MaskXY", unreal.MaterialExpressionComponentMask, -2000, 0,
                   r=True, g=True, b=False, a=False)
    LIB.connect_material_expressions(world_pos, "", mask_xy, "")

    # 2. Multi-Octave River Flow Normals
    # Both octaves flow along a unified natural stream direction to prevent perpendicular wave clashes
    # Octave 1: Primary gentle river swell (Scale 0.00010, ~100m per tile), Speed (0.007, 0.0035)
    scale1 = node(material, "Scale1", unreal.MaterialExpressionMultiply, -1700, -300)
    const_scale1 = node(material, "ConstScale1", unreal.MaterialExpressionConstant, -1850, -280, r=0.00010)
    LIB.connect_material_expressions(mask_xy, "", scale1, "A")
    LIB.connect_material_expressions(const_scale1, "", scale1, "B")

    panner1 = node(material, "Panner1", unreal.MaterialExpressionPanner, -1500, -300,
                   speed_x=0.007, speed_y=0.0035)
    LIB.connect_material_expressions(scale1, "", panner1, "Coordinate")

    sample_n1 = node(material, "SampleN1", unreal.MaterialExpressionTextureSample, -1250, -300,
                     texture=tex_normal, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    LIB.connect_material_expressions(panner1, "", sample_n1, "UVs")

    # Octave 2: Harmonic micro surface ripples (Scale 0.00025, ~40m per tile), Speed (0.009, 0.0045)
    scale2 = node(material, "Scale2", unreal.MaterialExpressionMultiply, -1700, -100)
    const_scale2 = node(material, "ConstScale2", unreal.MaterialExpressionConstant, -1850, -80, r=0.00025)
    LIB.connect_material_expressions(mask_xy, "", scale2, "A")
    LIB.connect_material_expressions(const_scale2, "", scale2, "B")

    panner2 = node(material, "Panner2", unreal.MaterialExpressionPanner, -1500, -100,
                   speed_x=0.009, speed_y=0.0045)
    LIB.connect_material_expressions(scale2, "", panner2, "Coordinate")

    sample_n2 = node(material, "SampleN2", unreal.MaterialExpressionTextureSample, -1250, -100,
                     texture=tex_normal, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    LIB.connect_material_expressions(panner2, "", sample_n2, "UVs")

    # Soften Octave 2 to 35% weight so it gently modulates Octave 1 without interference pattern
    mul_n2 = node(material, "MulN2", unreal.MaterialExpressionMultiply, -1050, -100)
    const_n2_w = node(material, "ConstN2Weight", unreal.MaterialExpressionConstant, -1150, -60, r=0.35)
    LIB.connect_material_expressions(sample_n2, "", mul_n2, "A")
    LIB.connect_material_expressions(const_n2_w, "", mul_n2, "B")

    # Combine Normals: Add and Normalize
    add_n = node(material, "AddNormals", unreal.MaterialExpressionAdd, -900, -200)
    LIB.connect_material_expressions(sample_n1, "", add_n, "A")
    LIB.connect_material_expressions(mul_n2, "", add_n, "B")

    norm_combined = node(material, "NormCombined", unreal.MaterialExpressionNormalize, -750, -200)
    LIB.connect_material_expressions(add_n, "", norm_combined, "")

    # 3. Distance Attenuation for Normals (Complete anti-aliasing against God-View raster aliasing)
    # Beyond 230m (God-View distance), smoothly fades to 100% Flat Normal (0,0,1) to eliminate moiré and zebra stripes
    cam_pos = node(material, "CamPos", unreal.MaterialExpressionCameraPositionWS, -1500, 200)
    cam_dist = node(material, "CamDist", unreal.MaterialExpressionDistance, -1300, 200)
    LIB.connect_material_expressions(cam_pos, "", cam_dist, "A")
    LIB.connect_material_expressions(world_pos, "", cam_dist, "B")

    # Fade calculation: saturate((Dist - 3000.0) * 0.00005) [transitions smoothly from 30m to 230m]
    sub_dist = node(material, "SubDist", unreal.MaterialExpressionSubtract, -1100, 200)
    const_dist_near = node(material, "ConstDistNear", unreal.MaterialExpressionConstant, -1250, 300, r=3000.0)
    LIB.connect_material_expressions(cam_dist, "", sub_dist, "A")
    LIB.connect_material_expressions(const_dist_near, "", sub_dist, "B")

    mul_dist = node(material, "MulDist", unreal.MaterialExpressionMultiply, -950, 200)
    const_dist_inv = node(material, "ConstDistInv", unreal.MaterialExpressionConstant, -1100, 320, r=0.00005)
    LIB.connect_material_expressions(sub_dist, "", mul_dist, "A")
    LIB.connect_material_expressions(const_dist_inv, "", mul_dist, "B")

    sat_dist = node(material, "SatDist", unreal.MaterialExpressionSaturate, -800, 200)
    LIB.connect_material_expressions(mul_dist, "", sat_dist, "")

    mul_fade = node(material, "MulFade", unreal.MaterialExpressionMultiply, -650, 200)
    const_max_fade = node(material, "ConstMaxFade", unreal.MaterialExpressionConstant, -780, 300, r=1.0)
    LIB.connect_material_expressions(sat_dist, "", mul_fade, "A")
    LIB.connect_material_expressions(const_max_fade, "", mul_fade, "B")

    flat_normal = node(material, "FlatNormal", unreal.MaterialExpressionConstant3Vector, -650, -50,
                       constant=unreal.LinearColor(0.0, 0.0, 1.0, 1.0))

    lerp_normal = node(material, "LerpNormal", unreal.MaterialExpressionLinearInterpolate, -450, -150)
    LIB.connect_material_expressions(norm_combined, "", lerp_normal, "A")
    LIB.connect_material_expressions(flat_normal, "", lerp_normal, "B")
    LIB.connect_material_expressions(mul_fade, "", lerp_normal, "Alpha")

    final_normal = node(material, "FinalNormal", unreal.MaterialExpressionNormalize, -250, -150)
    LIB.connect_material_expressions(lerp_normal, "", final_normal, "")
    LIB.connect_material_property(final_normal, "", unreal.MaterialProperty.MP_NORMAL)

    # 4. Beer-Lambert Optical Depth Absorption Base Color
    deep_col = node(material, "DeepColor", unreal.MaterialExpressionVectorParameter, -950, -700,
                    parameter_name="BaseColor",
                    default_value=unreal.LinearColor(0.006, 0.032, 0.075, 1.0))

    shallow_col = node(material, "ShallowColor", unreal.MaterialExpressionVectorParameter, -950, -550,
                       parameter_name="ShallowColor",
                       default_value=unreal.LinearColor(0.035, 0.185, 0.180, 1.0))

    df_depth = node(material, "DFDepth", unreal.MaterialExpressionDepthFade, -950, -400,
                    fade_distance_default=400.0)

    sat_depth = node(material, "SatDepth", unreal.MaterialExpressionSaturate, -750, -400)
    LIB.connect_material_expressions(df_depth, "", sat_depth, "")

    water_col = node(material, "WaterCol", unreal.MaterialExpressionLinearInterpolate, -550, -600)
    LIB.connect_material_expressions(shallow_col, "", water_col, "A")
    LIB.connect_material_expressions(deep_col, "", water_col, "B")
    LIB.connect_material_expressions(sat_depth, "", water_col, "Alpha")

    # 5. Shoreline Micro-Foam (Localized within 30cm of land, not floating in open water)
    df_shore = node(material, "DFShore", unreal.MaterialExpressionDepthFade, -950, 450,
                    fade_distance_default=30.0)

    sat_shore = node(material, "SatShore", unreal.MaterialExpressionSaturate, -750, 450)
    LIB.connect_material_expressions(df_shore, "", sat_shore, "")

    shore_mask = node(material, "ShoreMask", unreal.MaterialExpressionOneMinus, -600, 450)
    LIB.connect_material_expressions(sat_shore, "", shore_mask, "")

    # Foam Texture Panner - flowing along river stream
    scale_foam = node(material, "ScaleFoam", unreal.MaterialExpressionMultiply, -1700, 600)
    const_scale_f = node(material, "ConstScaleF", unreal.MaterialExpressionConstant, -1850, 620, r=0.0004)
    LIB.connect_material_expressions(mask_xy, "", scale_foam, "A")
    LIB.connect_material_expressions(const_scale_f, "", scale_foam, "B")

    panner_foam = node(material, "PannerFoam", unreal.MaterialExpressionPanner, -1500, 600,
                      speed_x=0.007, speed_y=0.0035)
    LIB.connect_material_expressions(scale_foam, "", panner_foam, "Coordinate")

    sample_foam = node(material, "SampleFoam", unreal.MaterialExpressionTextureSample, -1250, 600,
                       texture=tex_diffuse, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    LIB.connect_material_expressions(panner_foam, "", sample_foam, "UVs")

    foam_mask_r = node(material, "FoamMaskR", unreal.MaterialExpressionComponentMask, -1000, 600,
                       r=True, g=False, b=False, a=False)
    LIB.connect_material_expressions(sample_foam, "", foam_mask_r, "")

    mul_foam_inner = node(material, "MulFoamInner", unreal.MaterialExpressionMultiply, -800, 600)
    const_f_weight = node(material, "ConstFWeight", unreal.MaterialExpressionConstant, -950, 700, r=0.25)
    LIB.connect_material_expressions(foam_mask_r, "", mul_foam_inner, "A")
    LIB.connect_material_expressions(const_f_weight, "", mul_foam_inner, "B")

    foam_alpha = node(material, "FoamAlpha", unreal.MaterialExpressionMultiply, -450, 500)
    LIB.connect_material_expressions(shore_mask, "", foam_alpha, "A")
    LIB.connect_material_expressions(mul_foam_inner, "", foam_alpha, "B")

    foam_col = node(material, "FoamCol", unreal.MaterialExpressionConstant3Vector, -450, 650,
                    constant=unreal.LinearColor(0.85, 0.92, 0.96, 1.0))

    final_base_col = node(material, "FinalBaseCol", unreal.MaterialExpressionLinearInterpolate, -250, -500)
    LIB.connect_material_expressions(water_col, "", final_base_col, "A")
    LIB.connect_material_expressions(foam_col, "", final_base_col, "B")
    LIB.connect_material_expressions(foam_alpha, "", final_base_col, "Alpha")
    LIB.connect_material_property(final_base_col, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # 6. Soft Shoreline Opacity via DepthFade (completely dissolves harsh square grid stair-step cell borders)
    df_opacity = node(material, "DFOpacity", unreal.MaterialExpressionDepthFade, -650, 800,
                      fade_distance_default=250.0)

    sat_op_df = node(material, "SatOpDF", unreal.MaterialExpressionSaturate, -450, 800)
    LIB.connect_material_expressions(df_opacity, "", sat_op_df, "")

    lerp_op = node(material, "LerpOp", unreal.MaterialExpressionLinearInterpolate, -250, 800)
    const_op_edge = node(material, "ConstOpEdge", unreal.MaterialExpressionConstant, -450, 900, r=0.0)
    const_op_deep = node(material, "ConstOpDeep", unreal.MaterialExpressionConstant, -450, 950, r=0.90)
    LIB.connect_material_expressions(const_op_edge, "", lerp_op, "A")
    LIB.connect_material_expressions(const_op_deep, "", lerp_op, "B")
    LIB.connect_material_expressions(sat_op_df, "", lerp_op, "Alpha")
    LIB.connect_material_property(lerp_op, "", unreal.MaterialProperty.MP_OPACITY)

    # 7. Physical PBR Optics: Specular (0.50), Roughness (0.28), Metallic (0.0)
    const_zero = node(material, "ConstZero", unreal.MaterialExpressionConstant, -250, 1050, r=0.0)
    LIB.connect_material_property(const_zero, "", unreal.MaterialProperty.MP_METALLIC)

    roughness = node(material, "RoughnessParam", unreal.MaterialExpressionScalarParameter, -250, 1150,
                     parameter_name="Roughness", default_value=0.28)
    LIB.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    specular = node(material, "SpecularParam", unreal.MaterialExpressionScalarParameter, -250, 1250,
                    parameter_name="Specular", default_value=0.50)
    LIB.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)

    # 8. Ambient Subsurface Radiance (Emissive) so shadowed waters do not appear pitch black
    emissive_lift = node(material, "EmissiveLift", unreal.MaterialExpressionMultiply, -250, -350)
    const_em_mult = node(material, "ConstEmMult", unreal.MaterialExpressionConstant, -400, -320, r=0.02)
    LIB.connect_material_expressions(water_col, "", emissive_lift, "A")
    LIB.connect_material_expressions(const_em_mult, "", emissive_lift, "B")
    LIB.connect_material_property(emissive_lift, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # 9. Clean World Position Offset (NO sinusoidal undulating waves)
    zero_wpo = node(material, "ZeroWPO", unreal.MaterialExpressionConstant3Vector, -250, 1350,
                    constant=unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    LIB.connect_material_property(zero_wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    # 10. Enable ISM usage and compile
    LIB.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    LIB.layout_material_expressions(material)
    errors = LIB.recompile_material(material)
    if errors:
        unreal.log_error(f"Material recompile error for {mat_path}: {errors}")
        return False

    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        unreal.log_error(f"Failed to save water material: {mat_path}")
        return False

    unreal.log_warning(f"SUCCESS: Upgraded photorealistic river water material: {mat_path}")
    return True


mats = [
    "/Game/Environment/Materials/Presentation/M_PresentationWater",
    "/Game/Environment/Materials/M_AftershockWater"
]

failed_materials = [m for m in mats if not build_realistic_water(m)]
if failed_materials:
    raise RuntimeError("Realistic water setup incomplete: " + ", ".join(failed_materials))
unreal.log_warning("ALL RIVER WATER MATERIALS SUCCESSFULLY UPGRADED TO PHOTOREALISTIC SHADER")
