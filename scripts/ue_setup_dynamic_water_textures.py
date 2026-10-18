"""Configure M_PresentationWater and M_AftershockWater with engine dynamic water normal maps and foam textures.
Forwards to ue_upgrade_river_water_visual.py for photorealistic distance-attenuated rendering.
"""

from pathlib import Path
import runpy

script_path = Path(__file__).parent / "ue_upgrade_river_water_visual.py"
runpy.run_path(str(script_path), run_name="__main__")
