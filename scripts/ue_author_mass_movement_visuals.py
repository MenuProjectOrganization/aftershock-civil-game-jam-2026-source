"""Saved-only proposal: author original bounded mass-movement materials/meshes.

No physics, terrain, enum, map, plugin, third-party imagery or asset downloads.
Root executes in real-RHI UE AFTER reviewing/copying this script. Materials are
tagged and updated without expression deletion. Final compiled-resource native
acceptance is separate; this receipt never claims GPU or gameplay acceptance.
"""
import hashlib
import json
import os
import traceback
import uuid
from pathlib import Path

import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
SOURCE = Path(os.environ.get("AFTERSHOCK_MASS_GEOMETRY", str(ROOT / "_Imports/PhysicalHazards/MassMovement")))
OUT = ROOT / "Saved/PresentationValidation/mass_movement_visual_assets.json"
PATH = "/Game/FX/Aftershock/MassMovement"
TAG = "AFTERSHOCK_MASS_VISUAL_V1:"
L = unreal.MaterialEditingLibrary
REPORT = dict(valid=False, in_progress=True, run_id=str(uuid.uuid4()), scope="Native asset authoring; no physics, timing, frame continuity, GPU, game, save or packaged acceptance")


def receipt():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    temporary = OUT.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(REPORT, indent=2) + "\n", encoding="utf-8")
    temporary.replace(OUT)


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def enum(cls, key):
    for name in dir(cls):
        if name.replace("_", "").upper() == key.replace("_", "").upper():
            return getattr(cls, name)
    raise RuntimeError("Missing native enum " + key)


def node(material, key, kind, **properties):
    found = [n for n in L.get_material_expressions(material) if str(n.get_editor_property("desc")) == TAG + key]
    need(len(found) <= 1, "Duplicate owned node " + key)
    n = found[0] if found else L.create_material_expression(material, kind)
    need(n.get_class() == kind.static_class(), "Unexpected node class " + key)
    n.set_editor_property("desc", TAG + key)
    for k, value in properties.items():
        n.set_editor_property(k, value)
    return n


def link(a, b, pin="", channel=""):
    need(L.connect_material_expressions(a, channel, b, pin), "Material link failed " + pin)


def output(m, n, prop):
    need(L.connect_material_property(n, "", prop), "Material property failed " + str(prop))


def scalar(m, name, value):
    return node(m, name, unreal.MaterialExpressionScalarParameter, parameter_name=name, default_value=float(value))


def custom(m, key, code, fields, width=3):
    n = node(m, key, unreal.MaterialExpressionCustom)
    names = list(fields)
    inputs = []
    for name in names:
        item = unreal.CustomInput(); item.set_editor_property("input_name", name); inputs.append(item)
    # Rewire all pins before publishing HLSL to avoid missing-input transient
    # compile warnings. Existing expression graphs are never destroyed.
    notify = unreal.PropertyAccessChangeNotifyMode.NEVER
    n.set_editor_property("inputs", inputs, notify_mode=notify)
    for name, source in fields.items():
        link(source, n, name)
    n.set_editor_property("output_type", enum(unreal.CustomMaterialOutputType, "CMOTFLOAT" + (str(width) if width != 1 else "1")), notify_mode=notify)
    n.set_editor_property("code", code, notify_mode=notify)
    return n


def material(name, powder=False):
    path = PATH + "/" + name
    m = unreal.load_asset(path)
    if m is None:
        m = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, PATH, unreal.Material, unreal.MaterialFactoryNew())
    need(isinstance(m, unreal.Material), "Expected material " + path)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if powder else unreal.BlendMode.BLEND_MASKED)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT if powder else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("two_sided", powder)
    m.set_editor_property("tangent_space_normal", False)
    if powder:
        m.set_editor_property("screen_space_reflections", False)
    L.set_base_material_usage(m, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    return m


def make_surface(name, snow):
    m = material(name)
    depth = node(m, "MetricEndpointDepthsCm", unreal.MaterialExpressionTextureCoordinate, coordinate_index=1)
    phase = scalar(m, "MassFrameAlpha", 0.)
    clast = scalar(m, "MassClastMode", 0.)
    base_world = node(m, "UndisplacedWorld", unreal.MaterialExpressionWorldPosition,
                      world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    displaced_world = node(m, "WorldIncludingActualDepth", unreal.MaterialExpressionWorldPosition)
    native_normal = node(m, "NativeClastNormal", unreal.MaterialExpressionVertexNormalWS)
    fields = dict(Depths=depth, Phase=phase, Clast=clast)
    output(m, custom(m, "ActualMetricDepthWPO", "return float3(0,0,lerp(Depths.x,Depths.y,saturate(Phase))*(1-saturate(Clast)));", fields), unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    output(m, custom(m, "DryCellsAbsent", "return max(saturate(Clast),step(.5,lerp(Depths.x,Depths.y,saturate(Phase))));", fields, 1), unreal.MaterialProperty.MP_OPACITY_MASK)
    # Trigonometric grain is deliberately small and isotropic, no colour stripes
    # or extra displacement. Real flow shape comes only from replay cell volume.
    colour = ("float grain=sin(P.x*.21+sin(P.y*.173))*sin(P.y*.197+sin(P.x*.151));\n"
              + ("return float3(.72,.79,.84)*(1+grain*.035);" if snow else
                 "float broad=sin(P.x*.0031+sin(P.y*.0017))*sin(P.y*.0023+sin(P.x*.0013));return lerp(float3(.085,.059,.035),float3(.16,.115,.068),broad*.35+.5)*(1+grain*.065);"))
    output(m, custom(m, "SnowGrain" if snow else "WetSoilGrain", colour, dict(P=base_world)), unreal.MaterialProperty.MP_BASE_COLOR)
    output(m, scalar(m, "MassRoughness", .81 if snow else .57), unreal.MaterialProperty.MP_ROUGHNESS)
    output(m, scalar(m, "MassSpecular", .26 if snow else .37), unreal.MaterialProperty.MP_SPECULAR)
    output(m, scalar(m, "MassMetallic", 0.), unreal.MaterialProperty.MP_METALLIC)
    output(m, custom(m, "ActualDisplacedSurfaceNormal", "float3 n=cross(ddy(P),ddx(P));float l=dot(n,n);n=l>1e-12?n*rsqrt(l):float3(0,0,1);n*=n.z<0?-1:1;return normalize(lerp(n,N,saturate(Clast)));", dict(P=displaced_world, N=native_normal, Clast=clast)), unreal.MaterialProperty.MP_NORMAL)
    return m


def make_powder(name, snow):
    m = material(name, True)
    uv = node(m, "CardUV", unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
    clock = node(m, "VisualClockOnly", unreal.MaterialExpressionTime)
    mask = custom(m, "SoftBoundedRadialCard", "float2 p=UV*2-1;float r=dot(p,p);float noise=.82+.18*sin(p.x*11+Clock*.6)*sin(p.y*13-Clock*.5);return saturate(1-r)*saturate(1-r)*noise*.2;", dict(UV=uv, Clock=clock), 1)
    fade = node(m, "GroundContactFade", unreal.MaterialExpressionDepthFade, fade_distance_default=80., opacity_default=.2)
    link(mask, fade, "Opacity")
    output(m, fade, unreal.MaterialProperty.MP_OPACITY)
    c = node(m, "PowderColour", unreal.MaterialExpressionVectorParameter, parameter_name="PowderColour",
             default_value=unreal.LinearColor(.56, .64, .7, 1.) if snow else unreal.LinearColor(.11, .075, .04, 1.))
    output(m, c, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return m


def finish(m):
    # get_statistics waits for actual final compilation in native UE5.8. Parent
    # additionally runs the own-resource gate, avoiding a fallback shader pass.
    errors = list(L.recompile_material(m)); stats = L.get_statistics(m)
    row = {k: int(stats.get_editor_property(k)) for k in ("num_vertex_shader_instructions", "num_pixel_shader_instructions", "num_samplers")}
    need(not errors and row["num_vertex_shader_instructions"] > 0 and row["num_pixel_shader_instructions"] > 0,
         "Material resource unavailable " + m.get_path_name() + str(errors))
    need(row["num_samplers"] <= 4, "Unexpected mass visual sampler cost " + str(row))
    need(unreal.EditorAssetLibrary.save_loaded_asset(m), "Material save failed " + m.get_path_name())
    return dict(path=m.get_path_name(), **row)


def import_meshes():
    manifest = json.loads((SOURCE / "MASS_GEOMETRY_MANIFEST.json").read_text(encoding="utf-8"))
    rows = []
    for entry in manifest["meshes"]:
        f = SOURCE / entry["file"]
        need(hashlib.sha256(f.read_bytes()).hexdigest() == entry["sha256"], "Source FBX hash mismatch")
        path = PATH + "/" + entry["name"]
        mesh = unreal.load_asset(path)
        if mesh is None:  # Rerun does not overwrite already accepted native geometry.
            task = unreal.AssetImportTask();task.set_editor_property("filename", str(f));task.set_editor_property("destination_path", PATH)
            task.set_editor_property("destination_name", entry["name"]);task.set_editor_property("automated", True);task.set_editor_property("replace_existing", False);task.set_editor_property("save", False)
            options = unreal.FbxImportUI();options.set_editor_property("import_mesh", True);options.set_editor_property("import_materials", False);options.set_editor_property("import_textures", False)
            options.set_editor_property("import_as_skeletal", False);options.set_editor_property("automated_import_should_detect_type", False);options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)
            data = unreal.FbxStaticMeshImportData();data.set_editor_property("combine_meshes", True);data.set_editor_property("auto_generate_collision", False)
            data.set_editor_property("convert_scene", True);data.set_editor_property("convert_scene_unit", True)
            data.set_editor_property("normal_import_method", unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS);data.set_editor_property("generate_lightmap_u_vs", False)
            options.set_editor_property("static_mesh_import_data", data)
            task.set_editor_property("options", options);unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task]);mesh = unreal.load_asset(path)
        need(isinstance(mesh, unreal.StaticMesh), "Mesh import failed " + path)
        bounds = mesh.get_bounding_box(); size = [float(bounds.max.x - bounds.min.x), float(bounds.max.y - bounds.min.y), float(bounds.max.z - bounds.min.z)]
        expected = entry["bbox_size_cm"]
        need(all(abs(a - b) <= max(.2, abs(b) * .01) for a, b in zip(sorted(size), sorted(expected))), "Mesh cm scale mismatch " + str((size, expected)))
        need(unreal.EditorAssetLibrary.save_loaded_asset(mesh), "Mesh save failed " + path)
        rows.append(dict(path=mesh.get_path_name(), native_bbox_size_cm=size, source_sha256=entry["sha256"]))
    return rows


def main():
    receipt()
    unreal.EditorAssetLibrary.make_directory(PATH)
    REPORT["materials"] = [finish(make_surface("M_AvalancheSnow", True)), finish(make_surface("M_LandslideDebris", False)),
                           finish(make_powder("M_AvalanchePowder", True)), finish(make_powder("M_LandslideDust", False))]
    REPORT["meshes"] = import_meshes()
    REPORT["in_progress"] = False;REPORT["valid"] = True;receipt()
    unreal.log_warning("AFTERSHOCK_MASS_VISUAL_ASSETS_READY " + str(OUT))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        REPORT["error"] = traceback.format_exc();REPORT["in_progress"] = False;REPORT["valid"] = False;receipt()
        raise
