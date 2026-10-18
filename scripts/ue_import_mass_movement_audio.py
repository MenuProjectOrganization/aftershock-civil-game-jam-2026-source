"""Import only the two project-original, checksummed mass movement sound beds."""
import hashlib, json
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "_Imports/Original/MassMovementAudio/SOURCE_MANIFEST.json"
DESTINATION = "/Game/Aftershock/Audio/Generated"
report = {"valid": False, "original_sounds": [], "field_recordings": False}
for row in json.loads(SOURCE.read_text(encoding="utf-8"))["files"]:
    path = (ROOT / row["file"]).resolve()
    if not path.is_relative_to(ROOT / "_Imports/Original/MassMovementAudio"):
        raise RuntimeError("Sound source outside owned directory")
    if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
        raise RuntimeError("Sound checksum differs")
    task = unreal.AssetImportTask()
    for key, value in dict(filename=str(path), destination_path=DESTINATION,
        destination_name=row["name"], automated=True, replace_existing=True, save=False).items():
        task.set_editor_property(key, value)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    sound = unreal.load_asset(DESTINATION + "/" + row["name"])
    if not isinstance(sound, unreal.SoundWave):
        raise RuntimeError("Native SoundWave missing")
    sound.set_editor_property("looping", True)
    sound.set_editor_property("virtualization_mode", unreal.VirtualizationMode.PLAY_WHEN_SILENT)
    sound.set_editor_property("compression_quality", 80)
    if not unreal.EditorAssetLibrary.save_loaded_asset(sound):
        raise RuntimeError("Native SoundWave save failed")
    report["original_sounds"].append(sound.get_path_name())
report["valid"] = len(report["original_sounds"]) == 2
out = ROOT / "Saved/PresentationValidation/mass_movement_audio.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
