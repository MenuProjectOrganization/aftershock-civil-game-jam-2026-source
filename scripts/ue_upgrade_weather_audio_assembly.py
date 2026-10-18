"""Import verified 30s weather beds and author the original assembly pad material.

Run build_weather_audio.py offline first. Execute this script in UE Python.
Only five exact SoundWave paths and one project-owned tagged material are written.
Existing expressions are reused, never deleted; no map/plugin/gameplay changes.
"""
import hashlib
import json
from pathlib import Path

import unreal

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "Content" / "Aftershock" / "Audio" / "Generated"
AUDIO_PATH = "/Game/Aftershock/Audio/Generated"
ASSEMBLY_PATH = "/Game/Environment/Materials/Presentation/M_EmergencyAssembly"
TAG = "AFTERSHOCK_EMERGENCY_ASSEMBLY_V1:"
L = unreal.MaterialEditingLibrary
SOUNDS = ("S_Flood_RainBed", "S_Flood_WaterBed", "S_Storm_WindBed", "S_Earthquake_RumbleBed", "S_Tsunami_SurgeBed")


def enum_value(cls, normalized):
    for key in dir(cls):
        if key.replace("_", "").upper() == normalized:
            return getattr(cls, key)
    raise RuntimeError("Missing reflected enum: " + normalized)


def import_weather():
    manifest = json.loads((GENERATED / "WEATHER_AUDIO_MANIFEST.json").read_text(encoding="utf-8-sig"))
    outputs = {entry["name"]: entry for entry in manifest["outputs"]}
    result = []
    unreal.EditorAssetLibrary.make_directory(AUDIO_PATH)
    for name in SOUNDS:
        entry = outputs[name]
        path = ROOT / entry["file"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError("Processed WAV checksum mismatch: " + name)
        if entry["clipped_samples"] or entry["peak_linear"] > 0.8 or entry["duration_seconds"] != 30.0:
            raise RuntimeError("Processed WAV quality contract failed: " + name)
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", str(path))
        task.set_editor_property("destination_path", AUDIO_PATH)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", False)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        asset = unreal.load_asset(AUDIO_PATH + "/" + name)
        if not isinstance(asset, unreal.SoundWave):
            raise RuntimeError("SoundWave import failed: " + name)
        asset.set_editor_property("looping", True)
        asset.set_editor_property("virtualization_mode", enum_value(unreal.VirtualizationMode, "PLAYWHENSILENT"))
        asset.set_editor_property("volume", 1.0)
        asset.set_editor_property("pitch", 1.0)
        asset.set_editor_property("compression_quality", 80)
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
            raise RuntimeError("SoundWave save failed: " + name)
        result.append(asset.get_path_name())
        unreal.log_warning("AFTERSHOCK_WEATHER_AUDIO_READY {} duration=30s sampleRate=44100 channels=2 pcmBits=16 looping=1 virtualization=PlayWhenSilent rmsDbfs={} peak={}".format(
            asset.get_path_name(), entry["rms_dbfs"], entry["peak_linear"]))
    return result


def node(material, key, cls, **properties):
    found = [n for n in L.get_material_expressions(material) if str(n.get_editor_property("desc")) == TAG + key]
    if len(found) > 1:
        raise RuntimeError("Duplicate owned assembly expression: " + key)
    result = found[0] if found else L.create_material_expression(material, cls)
    if result.get_class() != cls.static_class():
        raise RuntimeError("Owned expression class mismatch: " + key)
    result.set_editor_property("desc", TAG + key)
    for name, value in properties.items():
        result.set_editor_property(name, value)
    return result


def connect(a, b, pin="", output=""):
    if not L.connect_material_expressions(a, output, b, pin):
        raise RuntimeError("Assembly material link failed: " + pin)


def output(material, source, prop):
    if not L.connect_material_property(source, "", prop):
        raise RuntimeError("Assembly material output failed: " + str(prop))


def custom_float_type():
    for key in dir(unreal.CustomMaterialOutputType):
        if key.replace("_", "").upper() == "CMOTFLOAT1":
            return getattr(unreal.CustomMaterialOutputType, key)
    raise RuntimeError("Missing reflected custom float output type")


def author_assembly():
    material = unreal.load_asset(ASSEMBLY_PATH) if unreal.EditorAssetLibrary.does_asset_exist(ASSEMBLY_PATH) else None
    if material is None:
        folder, name = ASSEMBLY_PATH.rsplit("/", 1)
        unreal.EditorAssetLibrary.make_directory(folder)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    if not isinstance(material, unreal.Material):
        raise RuntimeError("Assembly path must contain a base material")
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("two_sided", False)
    L.set_base_material_usage(material, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES, True)
    uv = node(material, "UV", unreal.MaterialExpressionTextureCoordinate)
    icon = node(material, "AssemblyIcon", unreal.MaterialExpressionCustom, output_type=custom_float_type(), code=r"""
float2 p = (UV - 0.5) * 2.0;
float aa = max(max(fwidth(p.x), fwidth(p.y)), 0.002);
float edge = max(abs(p.x), abs(p.y));
float border = smoothstep(0.82-aa, 0.82+aa, edge) * (1.0-smoothstep(0.88-aa, 0.88+aa, edge));
// Four inward diagonal arrows surround three original procedural people.
float2 q = abs(p);
float along = (q.x+q.y) * 0.70710678;
float across = abs(q.x-q.y) * 0.70710678;
float shaft = (1.0-smoothstep(0.035-aa,0.035+aa,across)) * smoothstep(0.67-aa,0.67+aa,along) * (1.0-smoothstep(0.99-aa,0.99+aa,along));
float arrowWidth = max(0.0,(along-0.40)*0.60);
float head = (1.0-smoothstep(arrowWidth-aa,arrowWidth+aa,across)) * smoothstep(0.40-aa,0.40+aa,along) * (1.0-smoothstep(0.69-aa,0.69+aa,along));
float people = 0.0;
[unroll] for (int i=0;i<3;++i)
{
    float2 r = p-float2((i-1)*0.17,0.0);
    float circle = 1.0-smoothstep(0.047-aa,0.047+aa,length(r-float2(0.0,-0.14)));
    float body = (1.0-smoothstep(0.040-aa,0.040+aa,abs(r.x))) * (1.0-smoothstep(0.075-aa,0.075+aa,abs(r.y+0.02)));
    float legs = (1.0-smoothstep(0.016-aa,0.016+aa,abs(abs(r.x)-0.023))) * (1.0-smoothstep(0.060-aa,0.060+aa,abs(r.y-0.105)));
    people = max(people,max(circle,max(body,legs)));
}
return saturate(max(border,max(shaft,max(head,people))));
""")
    field = unreal.CustomInput()
    field.set_editor_property("input_name", "UV")
    icon.set_editor_property("inputs", [field])
    connect(uv, icon, "UV")
    green = node(material, "PadGreen", unreal.MaterialExpressionVectorParameter, parameter_name="PadGreen", default_value=unreal.LinearColor(0.025, 0.29, 0.14, 1.0))
    white = node(material, "MarkWhite", unreal.MaterialExpressionVectorParameter, parameter_name="MarkWhite", default_value=unreal.LinearColor(0.88, 0.94, 0.88, 1.0))
    color = node(material, "IconColor", unreal.MaterialExpressionLinearInterpolate)
    connect(green, color, "A"); connect(white, color, "B"); connect(icon, color, "Alpha")
    output(material, color, unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = node(material, "Roughness", unreal.MaterialExpressionConstant, r=0.89)
    specular = node(material, "Specular", unreal.MaterialExpressionConstant, r=0.15)
    output(material, roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    output(material, specular, unreal.MaterialProperty.MP_SPECULAR)
    lift = node(material, "NightReadability", unreal.MaterialExpressionMultiply, const_b=0.035)
    connect(color, lift, "A")
    output(material, lift, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    errors = L.recompile_material(material)
    if errors:
        raise RuntimeError("Assembly material compile errors: " + str(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError("Assembly material save failed")
    unreal.log_warning("AFTERSHOCK_EMERGENCY_ASSEMBLY_MATERIAL_READY " + material.get_path_name())
    return material.get_path_name()


if __name__ == "__main__":
    sounds = import_weather()
    material = author_assembly()
    unreal.log_warning("AFTERSHOCK_WEATHER_AUDIO_ASSEMBLY_COMPLETE sounds={} material={}".format(len(sounds), material))
