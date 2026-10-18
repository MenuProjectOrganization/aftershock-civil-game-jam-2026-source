from pathlib import Path
import os,runpy
HERE=Path(__file__).resolve().parent
os.environ['AFTERSHOCK_CITY_MODULE_PHASE']='BIND'
runpy.run_path(str(HERE/'ue_prepare_citysample_architecture.py'),run_name='__main__')
runpy.run_path(str(HERE/'ue_bake_citysample_facades.py'),run_name='__main__')
