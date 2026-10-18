"""Replace imported UE icon textures with the current deterministic PNG sources.

Run from UnrealEditor-Cmd with -run=pythonscript and an absolute -script path.
"""

import os

import unreal


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILDING_TYPES = (
    "House", "Apartment", "Office", "Factory", "Hospital", "School",
    "FireStation", "PowerPlant", "GreenSpace",
)
DESTINATIONS = (
    ("Content/UI/Icons/Buildings", "/Game/UI/Icons/Buildings"),
    ("Content/Buildings/Icons", "/Game/Buildings/Icons"),
)


def import_icon(source_path, destination_path, name):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source_path)
    task.set_editor_property("destination_path", destination_path)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = [str(path) for path in task.get_editor_property("imported_object_paths")]
    asset = unreal.EditorAssetLibrary.load_asset(destination_path + "/" + name)
    if not asset or not imported:
        raise RuntimeError("Icon texture import failed: {} -> {} ({})".format(
            source_path, destination_path, imported
        ))

    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    return asset


def main():
    imported_count = 0
    for source_folder, destination in DESTINATIONS:
        for building_type in BUILDING_TYPES:
            name = "T_Icon_{}_01".format(building_type)
            source = os.path.join(ROOT, source_folder, name + ".png")
            if not os.path.isfile(source):
                raise RuntimeError("Icon source PNG missing: " + source)
            import_icon(source, destination, name)
            imported_count += 1
            unreal.log_warning("ICON_REIMPORTED source={} asset={}/{}".format(
                source, destination, name
            ))
    unreal.log_warning("ICON_REIMPORT_COMPLETE count={}".format(imported_count))


main()
