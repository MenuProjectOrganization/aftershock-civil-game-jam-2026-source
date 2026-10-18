"""Import the supplied opening JPG intact. No image edits or recreated logo.

Run inside UE Python. The repository source is byte-identical to the supplied image.
"""
import hashlib
import json
from pathlib import Path
import unreal

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "_Imports/Original/Opening/AftershockOpening.jpg"
DESTINATION = "/Game/UI/Opening"
NAME = "T_AftershockOpening"


def enum_value(cls, normalized):
    for name in dir(cls):
        if name.replace("_", "").upper() == normalized:
            return getattr(cls, name)
    raise RuntimeError("Missing reflected enum " + normalized)


def main():
    source_hash = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    task = unreal.AssetImportTask()
    for name, value in {"filename": str(SOURCE), "destination_path": DESTINATION,
                        "destination_name": NAME, "automated": True,
                        "replace_existing": True, "save": False}.items():
        task.set_editor_property(name, value)
    unreal.EditorAssetLibrary.make_directory(DESTINATION)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(DESTINATION + "/" + NAME)
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError("Opening artwork import failed")
    texture.set_editor_property("lod_group", enum_value(unreal.TextureGroup, "TEXTUREGROUPUI"))
    texture.set_editor_property("compression_settings", enum_value(unreal.TextureCompressionSettings, "TCEDITORICON"))
    texture.set_editor_property("mip_gen_settings", enum_value(unreal.TextureMipGenSettings, "TMGSNOMIPMAPS"))
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("never_stream", True)
    texture.set_editor_property("max_texture_size", 2048)
    if not unreal.EditorAssetLibrary.save_loaded_asset(texture):
        raise RuntimeError("Opening artwork save failed")
    report = {"asset": texture.get_path_name(), "source": str(SOURCE.relative_to(ROOT)),
              "sha256": source_hash, "provenance": "User-supplied project artwork", "user_supplied": True, "image_modified": False,
              "layout": "ScaleToFit, source1280x720, logo preserved, no cropping"}
    folder = ROOT / "Saved/PresentationValidation"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "opening_artwork.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    unreal.log_warning("AFTERSHOCK_OPENING_ARTWORK_READY " + texture.get_path_name() + " sourceSha256=" + source_hash)


if __name__ == "__main__":
    main()
