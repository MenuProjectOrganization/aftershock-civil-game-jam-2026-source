"""Final isolated import slot: vehicle winding correction + building albedo.

No new C++ DLL, game/world/map load, or gameplay mutation. Execute with
-run=pythonscript -AllowCommandletRendering -d3d12. Ends in native asset readback
and synchronous real shader resource validation, not a visual-quality claim.
"""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import unreal
import ue_author_transport_visuals
import ue_author_building_albedo_detail

ue_author_transport_visuals.main()
ue_author_building_albedo_detail.main()
unreal.log_warning('AFTERSHOCK_FINAL_ORIGINAL_SURFACE_IMPORT_COMPLETE')
