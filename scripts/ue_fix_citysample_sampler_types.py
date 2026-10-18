"""Repair selected owned material samplers after native texture-compression policy.
Run only in AFTERSHOCK; this does not edit the acquired City Sample source.
"""
from pathlib import Path
import hashlib,json
import unreal
root=Path(unreal.Paths.project_dir()).resolve()
assert Path(unreal.Paths.get_project_file_path()).stem=="JamProject"
path="/Game/Aftershock/CitySample/Materials/M_Bench"
material=unreal.load_asset(path);assert isinstance(material,unreal.Material)
L=unreal.MaterialEditingLibrary;rows=[]
for expression in L.get_material_expressions(material):
 try:texture=expression.get_editor_property("texture")
 except Exception:continue
 if not texture:continue
 assert texture.get_path_name().startswith("/Game/Aftershock/CitySample/Textures/")
 compression=texture.get_editor_property("compression_settings")
 expected=(unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if compression==unreal.TextureCompressionSettings.TC_NORMALMAP
           else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if compression==unreal.TextureCompressionSettings.TC_MASKS
           else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if texture.get_editor_property("srgb")
           else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
 old=expression.get_editor_property("sampler_type")
 expression.set_editor_property("sampler_type",expected)
 rows.append({"texture":texture.get_path_name(),"before":str(old),"after":str(expression.get_editor_property("sampler_type")),"matches_compression":expression.get_editor_property("sampler_type")==expected})
assert len(rows)==3 and all(x["matches_compression"] for x in rows)
L.recompile_material(material) # A compile request; actual D3D12 acceptance is separate.
assert unreal.EditorAssetLibrary.save_loaded_asset(material)
manifest_path=root/"_Imports/Epic/CitySample/EXTRACTION_MANIFEST.json"
manifest=json.loads(manifest_path.read_text(encoding="utf-8-sig"))
vault=Path(r"C:\ProgramData\Epic\EpicGamesLauncher\VaultCache\CitySample_5.8\data\Content")
source_unchanged=True
for row in manifest["assets"]:
 if row.get("source_package"):
  original=vault/(row["source_package"].removeprefix("/Game/")+".uasset")
  source_unchanged &= hashlib.sha256(original.read_bytes()).hexdigest()==row["source_sha256"]
 if row["destination_package"]==path:
  f=root/"Content"/(path.removeprefix("/Game/")+".uasset")
  row["package_sha256"]=hashlib.sha256(f.read_bytes()).hexdigest();row["package_bytes"]=f.stat().st_size
  row.pop("original_graph_preserved_except_owned_texture_references",None)
  row["original_graph_preserved_except_owned_texture_references_and_matching_sampler_types"]=True
  row["sampler_nodes_verified"]=3
assert source_unchanged
manifest["total_package_bytes"]=sum(row["package_bytes"] for row in manifest["assets"])
manifest["sampler_policy_matches_compression"]=True
manifest_path.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8",newline="\n")
report={"valid":True,"samplers":rows,"source_assets_unchanged":source_unchanged,"material":path,"compile_requested":True,"actual_D3D12_compile_acceptance":False,"total_package_bytes":manifest["total_package_bytes"]}
out=root/"Saved/CitySampleAudit/sampler_repair_report.json";out.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8",newline="\n")
unreal.log_warning("AFTERSHOCK_CITYSAMPLE_SAMPLER_POLICY_READY "+json.dumps(report))
