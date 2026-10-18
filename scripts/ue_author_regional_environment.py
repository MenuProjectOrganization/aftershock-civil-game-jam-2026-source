"""Serial, offline, rerunnable V3 regional/global material authoring entry point.

Existing base globe/ground graphs must already have been authored. To rebuild
those graphs too, call restore_original_outputs() from the colour library first,
then the existing base recipes, then this entry point. No network or UE process
is launched by this script; root runs it inside the serial editor session.
"""
from pathlib import Path
import runpy
import unreal


def main(source_sha=None):
    directory = Path(__file__).resolve().parent
    colour = runpy.run_path(str(directory / "ue_unify_global_regional_colour.py"), run_name="aftershock_colour_library")
    restored = colour["restore_original_outputs"]()
    importer = runpy.run_path(str(directory / "ue_import_global_truecolour.py"), run_name="aftershock_global_import_library")
    importer["import_global_truecolour"](source_sha=source_sha)
    style = runpy.run_path(str(directory / "ue_author_regional_surface_styles.py"), run_name="aftershock_regional_style_library")
    style["main"]()
    colour["main"]()
    unreal.log_warning("AFTERSHOCK_REGIONAL_ENVIRONMENT_AUTHORED restoreBeforeUpstream=1 globalImport=1 regionalStyle=1 sharedGlobalColour=1 restoredMaterials={} networkRequests=0".format(len(restored)))


if __name__ == "__main__":
    main()
