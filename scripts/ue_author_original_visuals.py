"""Targeted mesh/material authoring, intentionally never loads a level or game."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import unreal
import ue_author_citizen_visuals
import ue_author_transport_visuals
import ue_author_building_detail

ue_author_citizen_visuals.main()
ue_author_transport_visuals.main()
ue_author_building_detail.main()
unreal.log_warning('AFTERSHOCK_ORIGINAL_VISUALS_AUTHORING_COMPLETE')
