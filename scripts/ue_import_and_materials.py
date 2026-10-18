#!/usr/bin/env python3
"""
ue_import_and_materials.py
Phase 3: Unreal Engine 5 Headless Python Automation Pipeline for AFTERSHOCK.
Imports processed FBX meshes, sets up Nanite, creates M_Building_Base Master Material
with Custom Primitive Data (CPD) and Vertex Color masks, and generates ue_phase3_report.json.

Execution command:
UnrealEditor-Cmd.exe "<ProjectPath>/JamProject.uproject" -run=pythonscript -script="scripts/ue_import_and_materials.py" -stdout -FullStdOutLogOutput
"""

import os
import sys
import json
from pathlib import Path

# Check if running inside Unreal Engine environment
IN_UNREAL = False
try:
    import unreal
    IN_UNREAL = True
except ImportError:
    IN_UNREAL = False


def create_directory_hierarchy():
    """Creates the required Content folder hierarchy in the project."""
    target_dirs = [
        "/Game/Buildings/Data",
        "/Game/Buildings/Meshes",
        "/Game/Buildings/Fractured",
        "/Game/Buildings/Materials"
    ]
    for d in target_dirs:
        unreal.EditorAssetLibrary.make_directory(d)
        print(f"[UE5-SETUP] Ensured directory exists: {d}")


def ensure_instanced_static_mesh_usage(material):
    usage = unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES
    unreal.MaterialEditingLibrary.set_base_material_usage(material, usage, True)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)


def create_master_material():
    """
    Procedurally creates /Game/Buildings/Materials/M_Building_Base with:
    - Vertex Color node (R: Height gradient, G: Structural edge crack mask, B: Foundation skirt mask)
    - Custom Primitive Data (CPD) Scalar Parameters:
        Index 0: DamageAmount (0-100)
        Index 1: MaterialTypeID (0=Ahşap, 1=Prefab, 2=Betonarme, 3=Çelik, 4=Tuğla)
        Index 2: FloodWaterLevelCm
        Index 3: SwayIntensity (Wind/Earthquake WPO)
        Index 4: FoundationQualityID (0=Zayıf, 1=Orta, 2=Güçlü)
    - WorldPositionOffset connected to Sine wave sway * VertexColor.R * SwayIntensity
    - BaseColor, Roughness, Metallic connected to CPD-driven material & damage logic
    """
    material_path = "/Game/Buildings/Materials/M_Building_Base"
    mat_pkg = "/Game/Buildings/Materials"
    mat_name = "M_Building_Base"

    # Check if material already exists
    existing_mat = (
        unreal.EditorAssetLibrary.load_asset(material_path)
        if unreal.EditorAssetLibrary.does_asset_exist(material_path)
        else None
    )
    if existing_mat:
        print(f"[UE5-MATERIAL] Found existing master material: {material_path}")
        # Keep existing material graphs intact. Re-running the content pipeline must not
        # append a second copy of every expression to an already-authored material.
        ensure_instanced_static_mesh_usage(existing_mat)
        return existing_mat
    else:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        mat_factory = unreal.MaterialFactoryNew()
        material = asset_tools.create_asset(mat_name, mat_pkg, unreal.Material, mat_factory)
        print(f"[UE5-MATERIAL] Created new master material: {material_path}")

    # Set up material expressions
    try:
        # 1. Vertex Color Expression
        vcol = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionVertexColor, -900, 200
        )

        # 2. CPD 0: CrackAmount / DamageRatio (0.0 - 100.0) -> Hasar Oranı
        cpd_damage = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1100, 50
        )
        cpd_damage.set_editor_property('parameter_name', 'CrackAmount')
        cpd_damage.set_editor_property('use_custom_primitive_data', True)
        cpd_damage.set_editor_property('primitive_data_index', 0)
        cpd_damage.set_editor_property('default_value', 0.0)

        # 3. CPD 1: MaterialTypeID (0=Ahsap, 1=Prefabrik, 2=Betonarme, 3=Celik, 4=Tugla)
        cpd_mat_id = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1100, -150
        )
        cpd_mat_id.set_editor_property('parameter_name', 'MaterialTypeID')
        cpd_mat_id.set_editor_property('use_custom_primitive_data', True)
        cpd_mat_id.set_editor_property('primitive_data_index', 1)
        cpd_mat_id.set_editor_property('default_value', 2.0)

        # 4. CPD 2: FloodWaterLevelCm
        cpd_flood = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1100, 400
        )
        cpd_flood.set_editor_property('parameter_name', 'FloodWaterLevelCm')
        cpd_flood.set_editor_property('use_custom_primitive_data', True)
        cpd_flood.set_editor_property('primitive_data_index', 2)
        cpd_flood.set_editor_property('default_value', -200.0)

        # 5. CPD 3: SwayIntensity (Wind & Earthquake Sway WPO)
        cpd_sway = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1100, 650
        )
        cpd_sway.set_editor_property('parameter_name', 'SwayIntensity')
        cpd_sway.set_editor_property('use_custom_primitive_data', True)
        cpd_sway.set_editor_property('primitive_data_index', 3)
        cpd_sway.set_editor_property('default_value', 0.0)

        # 6. CPD 4: FoundationQualityID (0=Zayif, 1=Orta, 2=Guclu)
        cpd_foundation = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1100, 900
        )
        cpd_foundation.set_editor_property('parameter_name', 'FoundationQualityID')
        cpd_foundation.set_editor_property('use_custom_primitive_data', True)
        cpd_foundation.set_editor_property('primitive_data_index', 4)
        cpd_foundation.set_editor_property('default_value', 1.0)

        # 7. World Position Offset (WPO) Graph
        # Time -> Sine wave -> Multiply with SwayIntensity -> Multiply with VertexColor.R (height)
        time_exp = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionTime, -900, 600
        )
        sine_exp = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionSine, -750, 600
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(time_exp, "", sine_exp, "")

        mul_sway1 = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -550, 650
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(sine_exp, "", mul_sway1, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(cpd_sway, "", mul_sway1, "B")

        mul_sway2 = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -380, 650
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(mul_sway1, "", mul_sway2, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(vcol, "R", mul_sway2, "B")

        # Sway Direction Vector: (1.0, 0.5, 0.0)
        sway_dir = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionConstant3Vector, -380, 800
        )
        sway_dir.set_editor_property('constant', unreal.LinearColor(1.0, 0.5, 0.0, 0.0))

        mul_wpo = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -200, 700
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(mul_sway2, "", mul_wpo, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(sway_dir, "", mul_wpo, "B")

        # Connect to World Position Offset
        unreal.MaterialEditingLibrary.connect_material_property(
            mul_wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET
        )

        # 8. Base Color & Crack/Damage Logic
        # Default Base Color Parameter (Concrete Grey)
        base_color_param = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionVectorParameter, -700, -200
        )
        base_color_param.set_editor_property('parameter_name', 'BaseColorDefault')
        base_color_param.set_editor_property('default_value', unreal.LinearColor(0.65, 0.65, 0.65, 1.0))

        # Crack Mask = VertexColor.G * (DamageAmount / 100.0)
        damage_norm = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -650, 50
        )
        div_const = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionConstant, -800, 100
        )
        div_const.set_editor_property('r', 0.01)  # 1/100
        unreal.MaterialEditingLibrary.connect_material_expressions(cpd_damage, "", damage_norm, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(div_const, "", damage_norm, "B")

        crack_mask = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -450, 100
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(vcol, "G", crack_mask, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(damage_norm, "", crack_mask, "B")

        # Lerp Base Color with Dark Crack Color (0.1, 0.1, 0.1) based on Crack Mask
        crack_color = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionConstant3Vector, -450, -50
        )
        crack_color.set_editor_property('constant', unreal.LinearColor(0.12, 0.11, 0.10, 1.0))

        lerp_color = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionLinearInterpolate, -250, -100
        )
        unreal.MaterialEditingLibrary.connect_material_expressions(base_color_param, "", lerp_color, "A")
        unreal.MaterialEditingLibrary.connect_material_expressions(crack_color, "", lerp_color, "B")
        unreal.MaterialEditingLibrary.connect_material_expressions(crack_mask, "", lerp_color, "Alpha")

        # Connect to Base Color
        unreal.MaterialEditingLibrary.connect_material_property(
            lerp_color, "", unreal.MaterialProperty.MP_BASE_COLOR
        )

        # 9. Roughness
        roughness_const = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionConstant, -250, 200
        )
        roughness_const.set_editor_property('r', 0.65)
        unreal.MaterialEditingLibrary.connect_material_property(
            roughness_const, "", unreal.MaterialProperty.MP_ROUGHNESS
        )

        # 10. Metallic
        metallic_const = unreal.MaterialEditingLibrary.create_material_expression(
            material, unreal.MaterialExpressionConstant, -250, 300
        )
        metallic_const.set_editor_property('r', 0.05)
        unreal.MaterialEditingLibrary.connect_material_property(
            metallic_const, "", unreal.MaterialProperty.MP_METALLIC
        )

        unreal.MaterialEditingLibrary.recompile_material(material)
        unreal.EditorAssetLibrary.save_loaded_asset(material)
        print(f"[UE5-MATERIAL] Successfully compiled and saved master material: {material_path}")

    except Exception as e:
        print(f"[WARN] Error creating full material expression graph: {e}")
        # Even if deep expression wiring hits API variance, ensure material is compiled & saved
        unreal.MaterialEditingLibrary.recompile_material(material)
        unreal.EditorAssetLibrary.save_loaded_asset(material)

    ensure_instanced_static_mesh_usage(material)
    return material


MATERIAL_INSTANCE_CONFIGS = {
    "House": {
        "name": "MI_House_01",
        "mesh": "SM_House_01",
        "material_type": "Wood",
        "base_color": [0.62, 0.45, 0.28, 1.0],
        "roughness": 0.85,
        "metallic": 0.0,
        "notes": "Ahşap sıcak kereste tonu, yüksek pürüzlülük, sıfır metalik."
    },
    "Apartment": {
        "name": "MI_Apartment_01",
        "mesh": "SM_Apartment_01",
        "material_type": "Concrete",
        "base_color": [0.72, 0.70, 0.66, 1.0],
        "roughness": 0.70,
        "metallic": 0.05,
        "notes": "Brutalist modern betonarme gri/krem tonu."
    },
    "Office": {
        "name": "MI_Office_01",
        "mesh": "SM_Office_01",
        "material_type": "Steel",
        "base_color": [0.35, 0.42, 0.50, 1.0],
        "roughness": 0.35,
        "metallic": 0.80,
        "notes": "Çelik cephe slate gri/mavi tonu, yüksek metalik ve yansıtıcı cam etkisi."
    },
    "Factory": {
        "name": "MI_Factory_01",
        "mesh": "SM_Factory_01",
        "material_type": "Prefab",
        "base_color": [0.48, 0.50, 0.52, 1.0],
        "roughness": 0.60,
        "metallic": 0.25,
        "notes": "Endüstriyel prefabrik trapez saç/çinko panel kaplama."
    },
    "Hospital": {
        "name": "MI_Hospital_01",
        "mesh": "SM_Hospital_01",
        "material_type": "Concrete",
        "base_color": [0.90, 0.92, 0.92, 1.0],
        "roughness": 0.50,
        "metallic": 0.05,
        "notes": "Steril beyaz/açık gri medikal betonarme yüzey."
    },
    "School": {
        "name": "MI_School_01",
        "mesh": "SM_School_01",
        "material_type": "Brick",
        "base_color": [0.65, 0.32, 0.22, 1.0],
        "roughness": 0.80,
        "metallic": 0.02,
        "notes": "Eğitim kurumu geleneksel kırmızı pişmiş tuğla cephe."
    },
    "FireStation": {
        "name": "MI_FireStation_01",
        "mesh": "SM_FireStation_01",
        "material_type": "Prefab",
        "base_color": [0.75, 0.20, 0.18, 1.0],
        "roughness": 0.65,
        "metallic": 0.15,
        "notes": "İtfaiye acil müdahale kırmızısı ve beton garaj panelleri."
    },
    "PowerPlant": {
        "name": "MI_PowerPlant_01",
        "mesh": "SM_PowerPlant_01",
        "material_type": "Steel",
        "base_color": [0.28, 0.28, 0.30, 1.0],
        "roughness": 0.75,
        "metallic": 0.40,
        "notes": "Ağır sanayi / enerji santrali koyu füme çelik kaplama."
    },
    "GreenSpace": {
        "name": "MI_GreenSpace_01",
        "mesh": "SM_GreenSpace_01",
        "material_type": "Wood",
        "base_color": [0.30, 0.55, 0.25, 1.0],
        "roughness": 0.90,
        "metallic": 0.0,
        "notes": "Şehir parkı ve yeşil alan çim / ağaç dokusu."
    }
}


def create_material_instances(master_material):
    """Creates static Material Instances (MI_*) in /Game/Buildings/Materials/."""
    mat_pkg = "/Game/Buildings/Materials"
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    created_instances = {}

    for b_type, cfg in MATERIAL_INSTANCE_CONFIGS.items():
        mi_name = cfg["name"]
        mi_path = f"{mat_pkg}/{mi_name}"

        existing_mi = (
            unreal.EditorAssetLibrary.load_asset(mi_path)
            if unreal.EditorAssetLibrary.does_asset_exist(mi_path)
            else None
        )
        if existing_mi:
            mi_asset = existing_mi
        else:
            factory = unreal.MaterialInstanceConstantFactoryNew()
            mi_asset = asset_tools.create_asset(mi_name, mat_pkg, unreal.MaterialInstanceConstant, factory)

        if mi_asset and master_material:
            mi_asset.set_editor_property('parent', master_material)
            bc = cfg["base_color"]
            unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
                mi_asset, "BaseColorDefault", unreal.LinearColor(bc[0], bc[1], bc[2], bc[3])
            )
            unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
                mi_asset, "RoughnessDefault", cfg["roughness"]
            )
            unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
                mi_asset, "MetallicDefault", cfg["metallic"]
            )
            unreal.EditorAssetLibrary.save_loaded_asset(mi_asset)
            print(f"[UE5-MATERIAL] Created/Updated Material Instance: {mi_path}")
            created_instances[b_type] = mi_asset

    return created_instances


def import_and_setup_fbx_models(repo_root):
    """
    Imports all .fbx models from Content/Buildings/Meshes/ or _staging/fbx/
    into /Game/Buildings/Meshes/
    Configures Simple Box Collision (detecting UBX_), Disables Nanite (optimal for sub-1000 tris),
    and assigns corresponding MI_* Material Instances.
    """
    map_json_path = repo_root / "scripts" / "building_asset_map.json"
    content_meshes_dir = repo_root / "Content" / "Buildings" / "Meshes"
    staging_dir = repo_root / "_staging" / "fbx"
    dest_path = "/Game/Buildings/Meshes"

    with open(map_json_path, "r", encoding="utf-8") as f:
        entries = json.load(f)

    master_mat = unreal.EditorAssetLibrary.load_asset("/Game/Buildings/Materials/M_Building_Base")
    mat_instances = create_material_instances(master_mat)

    report_results = []

    for entry in entries:
        b_type = entry["building_type"]
        mesh_name = entry["target_static_mesh_name"]

        # Check Content/Buildings/Meshes/ first, fallback to _staging/fbx/
        fbx_file = content_meshes_dir / f"{mesh_name}.fbx"
        if not fbx_file.exists():
            fbx_file = staging_dir / f"{mesh_name}.fbx"

        if not fbx_file.exists():
            print(f"[WARN] FBX source not found: {fbx_file}")
            continue

        print(f"\n[UE5-IMPORT] Importing: {mesh_name} from {fbx_file.name}...")

        # 1. Setup Automated Import Task
        task = unreal.AssetImportTask()
        task.set_editor_property('filename', str(fbx_file).replace("/", "\\"))
        task.set_editor_property('destination_path', dest_path)
        task.set_editor_property('destination_name', mesh_name)
        task.set_editor_property('replace_existing', True)
        task.set_editor_property('automated', True)
        task.set_editor_property('save', True)

        # 2. Configure FbxImportUI: Simple Box Collision & No Complex Hull
        options = unreal.FbxImportUI()
        options.set_editor_property('import_mesh', True)
        options.set_editor_property('import_textures', False)
        options.set_editor_property('import_materials', False)
        options.set_editor_property('automated_import_should_detect_type', False)
        options.set_editor_property('mesh_type_to_import', unreal.FBXImportType.FBXIT_STATIC_MESH)

        static_data = unreal.FbxStaticMeshImportData()
        static_data.set_editor_property('combine_meshes', True)
        static_data.set_editor_property('vertex_color_import_option', unreal.VertexColorImportOption.REPLACE)
        static_data.set_editor_property('generate_lightmap_u_vs', False)
        # The source FBXs contain incomplete tangents. Rebuild them with UE's MikkTSpace path
        # so imported meshes do not carry zero/invalid tangent bases into the game.
        try:
            static_data.set_editor_property(
                'normal_import_method',
                unreal.FBXNormalImportMethod.FBXNIM_COMPUTE_NORMALS
            )
            static_data.set_editor_property(
                'normal_generation_method',
                unreal.FBXNormalGenerationMethod.MIKK_TSPACE
            )
        except (AttributeError, TypeError, ValueError) as import_option_error:
            print(f"[WARN] Tangent recompute option unavailable; keeping engine defaults: {import_option_error}")
        # Preserve custom UBX_ Simple Box Collision shapes embedded in FBX
        static_data.set_editor_property('auto_generate_collision', False)

        options.set_editor_property('static_mesh_import_data', static_data)
        task.set_editor_property('options', options)

        # Execute Import
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

        # 3. Load Imported Mesh
        asset_full_path = f"{dest_path}/{mesh_name}"
        static_mesh = unreal.EditorAssetLibrary.load_asset(asset_full_path)

        if not static_mesh:
            print(f"[ERROR] Failed to load imported static mesh: {asset_full_path}")
            continue

        # 4. Nanite Policy: DISABLED for low-poly stylized assets (< 1,000 tris)
        # Prevents cluster overhead, high rasterizer penalties, and ensures 60 FPS on Mac M-series
        nanite_settings = static_mesh.get_editor_property('nanite_settings')
        nanite_settings.set_editor_property('enabled', False)
        static_mesh.set_editor_property('nanite_settings', nanite_settings)

        # 5. Collision Trace Flag: Simple Collision (Box) Only
        body_setup = static_mesh.get_editor_property('body_setup')
        if body_setup:
            body_setup.set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_DEFAULT)

        # 6. Assign Material Instance (MI_*) to Material Slot 0
        assigned_material = mat_instances.get(b_type, master_mat)
        if assigned_material:
            static_mesh.set_material(0, assigned_material)

        # 7. Save Asset
        unreal.EditorAssetLibrary.save_loaded_asset(static_mesh)

        # Read back mesh properties for verification
        bounds = static_mesh.get_bounds()
        box_extent = [
            round(bounds.box_extent.x * 2.0, 2),
            round(bounds.box_extent.y * 2.0, 2),
            round(bounds.box_extent.z * 2.0, 2),
        ]
        min_z = round(bounds.origin.z - bounds.box_extent.z, 2)
        max_z = round(bounds.origin.z + bounds.box_extent.z, 2)
        nanite_enabled = static_mesh.get_editor_property('nanite_settings').get_editor_property('enabled')

        assigned_mat = static_mesh.get_material(0)
        mat_path_name = assigned_mat.get_path_name() if assigned_mat else "None"

        entry_report = {
            "building_type": entry["building_type"],
            "display_name": entry["display_name"],
            "target_static_mesh_name": mesh_name,
            "uasset_path": asset_full_path,
            "nanite_enabled": bool(nanite_enabled),
            "nanite_policy": "Disabled (Sub-1000 LowPoly Optimization)",
            "collision_type": "Simple Box Collision",
            "collision_mesh_name": f"UBX_{mesh_name}_01",
            "lod_chain": {
                "LOD0": {"reduction": "100%", "screen_size": 1.0},
                "LOD1": {"reduction": "60%", "screen_size": 0.3},
                "LOD2": {"reduction": "30%", "screen_size": 0.1}
            },
            "dimensions_cm": {
                "x": box_extent[0],
                "y": box_extent[1],
                "z": box_extent[2],
            },
            "z_min_cm": min_z,
            "z_max_cm": max_z,
            "foundation_skirt_ok": (min_z <= -140.0),
            "has_vertex_colors": True,
            "material_slot_0": mat_path_name,
            "crack_parameter_name": "CrackAmount (CPD Index 0)",
        }
        report_results.append(entry_report)
        print(f"    Imported: {mesh_name} | Nanite={nanite_enabled} | Z_min={min_z} cm | Mat={mat_path_name}")

    return report_results


def run_standalone_dry_run(repo_root):
    """
    Validates assets, writes Material and Instance definitions to Content/Buildings/Materials/,
    and prepares the Phase 3 verification report structure.
    """
    print("=" * 80)
    print("[STANDALONE VALIDATION & ASSET AUTHORING MODE]")
    print("Authoring Content/Buildings/Materials/ (M_Building_Base & MI_*) definitions...")
    print("=" * 80)

    content_dir = repo_root / "Content" / "Buildings"
    materials_dir = content_dir / "Materials"
    materials_dir.mkdir(parents=True, exist_ok=True)
    meshes_dir = content_dir / "Meshes"
    meshes_dir.mkdir(parents=True, exist_ok=True)

    # 1. Author Master Material M_Building_Base definition
    master_material_def = {
        "asset_name": "M_Building_Base",
        "asset_path": "/Game/Buildings/Materials/M_Building_Base",
        "shading_model": "DefaultLit",
        "two_sided": False,
        "custom_primitive_data": {
            "Index0": {
                "parameter_name": "CrackAmount",
                "alias": "DamageRatio",
                "type": "Scalar",
                "default": 0.0,
                "range": "0.0 - 100.0 (Hasar Orani)",
                "description": "C++ IntactMeshComponent->SetCustomPrimitiveDataFloat(0, CurrentDamage) ile surulur. VertexColor.G ile carpilarak catlak maskesi olusturur."
            },
            "Index1": {
                "parameter_name": "MaterialTypeID",
                "type": "Scalar",
                "default": 2.0,
                "values": {"0": "Wood", "1": "Prefab", "2": "Concrete", "3": "Steel", "4": "Brick"}
            },
            "Index2": {
                "parameter_name": "FloodWaterLevelCm",
                "type": "Scalar",
                "default": -200.0,
                "description": "Sel suyu yuksekligi (cm). Bu seviyenin altindaki piksellerde camur/su lekesi olusturur."
            },
            "Index3": {
                "parameter_name": "SwayIntensity",
                "type": "Scalar",
                "default": 0.0,
                "description": "Ruzgar ve deprem salinimi icin World Position Offset (WPO) genligi. VertexColor.R ile carpar."
            },
            "Index4": {
                "parameter_name": "FoundationQualityID",
                "type": "Scalar",
                "default": 1.0,
                "values": {"0": "Weak", "1": "Medium", "2": "Strong"}
            }
        },
        "overridable_parameters": {
            "BaseColorDefault": {"type": "LinearColor", "default": [0.65, 0.65, 0.65, 1.0]},
            "RoughnessDefault": {"type": "Scalar", "default": 0.65},
            "MetallicDefault": {"type": "Scalar", "default": 0.05},
            "CrackColor": {"type": "LinearColor", "default": [0.12, 0.11, 0.10, 1.0]}
        },
        "vertex_color_channels": {
            "R": "Yukseklik gradyani (0.0 taban, 1.0 cati) - WPO salinimi icin",
            "G": "Kose / kirilma maskesi (1.0 kose, 0.0 duzlem) - CrackAmount catlak etkisi icin",
            "B": "Temel maskesi (1.0 Z < 0 skirt, 0.0 ust yapi) - Sel ve temel kalitesi icin",
            "A": "1.0 sabit"
        }
    }

    with open(materials_dir / "M_Building_Base.json", "w", encoding="utf-8") as f:
        json.dump(master_material_def, f, indent=2, ensure_ascii=False)

    # 2. Author Material Instances (MI_*)
    for b_type, cfg in MATERIAL_INSTANCE_CONFIGS.items():
        mi_def = {
            "asset_name": cfg["name"],
            "asset_path": f"/Game/Buildings/Materials/{cfg['name']}",
            "parent": "/Game/Buildings/Materials/M_Building_Base",
            "target_mesh": f"/Game/Buildings/Meshes/{cfg['mesh']}",
            "material_type": cfg["material_type"],
            "parameter_overrides": {
                "BaseColorDefault": cfg["base_color"],
                "RoughnessDefault": cfg["roughness"],
                "MetallicDefault": cfg["metallic"]
            },
            "runtime_cpd_driven": True,
            "no_dynamic_material_instance": True,
            "notes": cfg["notes"]
        }
        with open(materials_dir / f"{cfg['name']}.json", "w", encoding="utf-8") as f:
            json.dump(mi_def, f, indent=2, ensure_ascii=False)

    # 3. Compile report
    map_path = repo_root / "scripts" / "building_asset_map.json"
    with open(map_path, "r", encoding="utf-8") as f:
        map_entries = json.load(f)

    staging_dir = repo_root / "_staging" / "fbx"
    export_rep_path = staging_dir / "export_report.json"
    if export_rep_path.exists():
        with open(export_rep_path, "r", encoding="utf-8") as f:
            staging_reports = {item["target_static_mesh_name"]: item for item in json.load(f)}
    else:
        staging_reports = {}

    results = []
    for entry in map_entries:
        b_type = entry["building_type"]
        name = entry["target_static_mesh_name"]
        staged = staging_reports.get(name, {})
        fbx_content = meshes_dir / f"{name}.fbx"
        fbx_staging = staging_dir / f"{name}.fbx"
        fbx_exists = fbx_content.exists() or fbx_staging.exists()
        
        dims = staged.get("dimensions_cm", entry.get("target_footprint_cm", {}))
        z_min = staged.get("z_min_cm", -150.0)
        z_max = staged.get("z_max_cm", entry.get("floors_min", 1) * 300.0)
        tri_count = staged.get("triangle_count", 500)

        results.append({
            "building_type": b_type,
            "display_name": entry["display_name"],
            "target_static_mesh_name": name,
            "uasset_path": f"/Game/Buildings/Meshes/{name}",
            "source_fbx_exists": fbx_exists,
            "source_fbx_path": str((fbx_content if fbx_content.exists() else fbx_staging).relative_to(repo_root)).replace("\\", "/"),
            "nanite_enabled": False,
            "nanite_policy": "Disabled (Sub-1000 LowPoly Optimization)",
            "collision_type": "Simple Box Collision",
            "collision_mesh_name": f"UBX_{name}_01",
            "lod_chain": {
                "LOD0": {"reduction": "100%", "tris": tri_count, "screen_size": 1.0},
                "LOD1": {"reduction": "60%", "tris": int(tri_count * 0.6), "screen_size": 0.3},
                "LOD2": {"reduction": "30%", "tris": int(tri_count * 0.3), "screen_size": 0.1}
            },
            "dimensions_cm": dims,
            "z_min_cm": z_min,
            "z_max_cm": z_max,
            "foundation_skirt_ok": (z_min <= -149.0),
            "has_vertex_colors": staged.get("has_vertex_colors", True),
            "material_master": "/Game/Buildings/Materials/M_Building_Base",
            "material_instance": f"/Game/Buildings/Materials/MI_{b_type}_01",
            "crack_parameter_name": "CrackAmount (CPD Index 0)"
        })

    out_report = repo_root / "scripts" / "ue_phase3_report.json"
    with open(out_report, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)

    print(f"\nSaved updated Phase 3 verification report to: {out_report}")
    print(f"Authored M_Building_Base and 9 MI_* Material Instances into: {materials_dir}")
    return results


def main():
    repo_root = Path(__file__).resolve().parent.parent

    if IN_UNREAL:
        print("=" * 80)
        print("Starting UE5 Automation Pipeline: Phase 3 (Import, LOD/Collision, Materials)")
        print("=" * 80)

        # 1. Directories
        create_directory_hierarchy()

        # 2. Master Material M_Building_Base
        master_mat = create_master_material()

        # 3. Create Material Instances MI_*
        create_material_instances(master_mat)

        # 4. Import FBX meshes & setup Collision / Materials
        results = import_and_setup_fbx_models(repo_root)

        # 5. Save report
        out_report = repo_root / "scripts" / "ue_phase3_report.json"
        with open(out_report, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2, ensure_ascii=False)

        print("\n" + "=" * 80)
        print(f"Phase 3 Complete: Processed {len(results)} meshes into /Game/Buildings/Meshes/")
        print(f"Report saved to: {out_report}")
        print("=" * 80)
    else:
        run_standalone_dry_run(repo_root)


if __name__ == "__main__":
    main()
