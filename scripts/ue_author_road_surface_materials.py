"""Original shared road shaders over verified CC0 asphalt and owned concrete."""
from pathlib import Path
import hashlib,json,runpy
import unreal
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'_Imports/GlobalEnvironment/RoadSurface'
H=runpy.run_path(str(ROOT/'scripts/ue_author_global_environment.py'),run_name='aftershock_material_helpers')
node=H['node'];link=H['link'];prop=H['prop'];c=H['c'];vec=H['vec'];sc=H['sc'];mul=H['mul'];add=H['add'];lerp=H['lerp'];mask=H['mask'];tex=H['tex'];import_file=H['import_file']
L=unreal.MaterialEditingLibrary;A=unreal.EditorAssetLibrary;TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
REPORT={'valid':False,'materials':[],'textures':[],'canonical_road_geometry_modified':False}
manifest=json.loads((SOURCE/'SOURCE_MANIFEST.json').read_text(encoding='utf-8'));textures={}
for row in manifest['files']:
 p=SOURCE/row['relative_path'];assert hashlib.sha256(p.read_bytes()).hexdigest()==row['sha256']
 dest='/Game/Environment/Textures/GlobalPBR/T_asphalt_02_'+{'diffuse':'C','normal_dx':'N','roughness':'R'}[row['role']]
 t=import_file(p,dest);normal=row['role']=='normal_dx';srgb=row['role']=='diffuse'
 t.set_editor_property('srgb',srgb);t.set_editor_property('never_stream',False);t.set_editor_property('max_texture_size',2048)
 t.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_NORMALMAP if normal else unreal.TextureCompressionSettings.TC_DEFAULT if srgb else unreal.TextureCompressionSettings.TC_MASKS)
 t.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if normal else unreal.TextureGroup.TEXTUREGROUP_WORLD)
 assert A.save_loaded_asset(t);textures[row['role']]=dest;REPORT['textures'].append({'path':dest,'sha256':row['sha256'],'source_license':'CC0-1.0'})
for name in ('Asphalt','LaneMarkings','Sidewalk'):
 path='/Game/Environment/Materials/Realistic/M_Environment'+name
 m=unreal.load_asset(path) if A.does_asset_exist(path) else TOOLS.create_asset(path.rsplit('/',1)[1],path.rsplit('/',1)[0],unreal.Material,unreal.MaterialFactoryNew())
 existing=L.get_material_expressions(m)
 if existing:
  assert any(str(n.get_editor_property('desc'))=='AS_ROAD_PBR_V1:Complete' for n in existing),'Preserve unknown/incomplete road graph'
 else:
  world=node(m,'WorldPosition',world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
  uv=mul(m,mask(m,world,r=True,g=True),c(m,1./300.))
  if name=='LaneMarkings':
   base=vec(m,(.77,.78,.73));rough=c(m,.86);normal=vec(m,(0,0,1))
  else:
   sources=textures if name=='Asphalt' else {'diffuse':'/Game/Aftershock/CitySample/Textures/T_Concrete_C','normal_dx':'/Game/Aftershock/CitySample/Textures/T_Concrete_N'}
   base=tex(m,'SurfaceAlbedo',sources['diffuse'],uv)
   rough=mask(m,tex(m,'SurfaceRoughness',sources['roughness'],uv,linear=True),r=True) if 'roughness' in sources else c(m,.82)
   photo=tex(m,'SurfaceNormal',sources['normal_dx'],uv,normal=True)
   normal=node(m,'Normalize');link(lerp(m,vec(m,(0,0,1)),photo,sc(m,'GlobalGroundNormalQuality',.85)),normal)
  prop(m,base,'BASE_COLOR');prop(m,rough,'ROUGHNESS');prop(m,normal,'NORMAL');prop(m,c(m,.25),'SPECULAR')
  m.set_editor_property('two_sided',False)
  L.set_base_material_usage(m,unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,True)
  L.layout_material_expressions(m)
 errors=L.recompile_material(m)
 if errors:raise RuntimeError('Road shader '+name+str(list(errors)))
 stats=L.get_statistics(m)
 if stats.num_pixel_shader_instructions<=0:raise RuntimeError('Road actual shader resource missing '+name)
 if not existing:node(m,'Constant',r=1).set_editor_property('desc','AS_ROAD_PBR_V1:Complete')
 assert A.save_loaded_asset(m)
 REPORT['materials'].append({'path':path,'compiled':True,'pixel_instructions':stats.num_pixel_shader_instructions,'world_uv_tile_cm':300,'geometry_or_collision_authority':False})
REPORT['valid']=True
out=ROOT/'Saved/PresentationValidation/road_pbr_native.json';out.write_text(json.dumps(REPORT,indent=2)+'\n',encoding='utf-8')
