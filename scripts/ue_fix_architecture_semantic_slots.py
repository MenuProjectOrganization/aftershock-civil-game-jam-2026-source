import unreal,json
from pathlib import Path
A=unreal.EditorAssetLibrary
report=[]
for p in A.list_assets('/Game/Buildings/Architecture/Meshes',True,False):
 m=unreal.load_asset(p)
 if not isinstance(m,unreal.StaticMesh):continue
 for i,slot in enumerate(m.get_editor_property('static_materials')):
  role=str(slot.get_editor_property('material_slot_name'));kind='Metal' if 'Metal' in role else 'Glass' if 'Glass' in role else 'Roof' if 'Roof' in role else 'Plaster'
  material=unreal.load_asset('/Game/Buildings/Architecture/Materials/M_Arch_'+kind);assert material
  m.set_material(i,material);report.append({'mesh':p,'slot':i,'source_role':role,'material':kind})
 assert A.save_loaded_asset(m)
root=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
(root/'Saved/PresentationValidation/architecture_semantic_slots.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
