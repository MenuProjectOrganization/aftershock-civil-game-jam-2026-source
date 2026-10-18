"""Strict one-texture historical global EOX2016 import inside Unreal Editor.

build_import_plan is pure stdlib/read-only. Explicit save and same-process
property/built-size readback are not cold-load, actual mip count, cook or RHI
visibility/binding proof. No scene/material changes or engine launch.
"""
from __future__ import annotations
from datetime import datetime,timezone
import hashlib,json,math,re,struct,urllib.parse
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path('_Imports/Geography/GlobalImagery')
FILENAME='global-eox2016-truecolour.jpg'
NAME='T_GlobalMacro_EOX2016'
DEST='/Game/Environment/Textures/Regional/Macro'
LICENSE='https://creativecommons.org/licenses/by/4.0/'
ATTRIBUTION='EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017)'
MAX_IMAGE=24*1024*1024

def require(value,message):
 if not value:raise ValueError(message)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def plain_file(path):
 require(path.is_file() and not path.is_symlink() and not any(p.is_symlink() or getattr(p,'is_junction',lambda:False)() for p in path.parents),'Missing or linked global source input')
 return path
def jpeg_dimensions(raw):
 require(raw.startswith(b'\xff\xd8') and raw.endswith(b'\xff\xd9'),'Incomplete/non-JPEG source');p=2
 while p<len(raw):
  require(raw[p]==255,'Malformed JPEG marker');p+=1
  while p<len(raw) and raw[p]==255:p+=1
  require(p<len(raw),'Truncated JPEG marker');marker=raw[p];p+=1
  if marker in (1,*range(208,216)):continue
  require(marker not in (217,218) and p+2<=len(raw),'JPEG has no frame before compressed scan')
  size=struct.unpack('>H',raw[p:p+2])[0];require(size>=2 and p+size<=len(raw),'Truncated JPEG segment')
  if marker in (192,193,194,195,197,198,199,201,202,203,205,206,207):
   require(size>=8 and raw[p+2]==8 and raw[p+7]==3,'Expected8bit3component JPEG');h,w=struct.unpack('>HH',raw[p+3:p+7]);return w,h
  p+=size
 raise ValueError('JPEG frame missing')

def build_import_plan(repo_root=ROOT):
 root=Path(repo_root).resolve();folder=root/SOURCE
 manifest_path=plain_file(folder/'IMAGERY_MANIFEST.json');manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
 proof=json.loads(plain_file(folder/'LICENSE_VERIFICATION.json').read_text(encoding='utf-8'))
 require(manifest.get('schema_version')==2 and manifest.get('source_revision')=='8192x4096-two-4096-hemispheres-v2' and manifest.get('complete') is True and manifest.get('image_count')==1,'Exactly one completed8192source revision2required')
 provider=manifest.get('provider',{});require(provider==proof,'Global provider/licence receipt differs')
 require(provider.get('license')=='CC-BY-4.0' and provider.get('license_url')==LICENSE and provider.get('product_year')==2016 and provider.get('source_years')==[2016,2017] and provider.get('required_attribution')==ATTRIBUTION,'Only attributed2016CCBY4global product allowed')
 layer=provider.get('layer',{})
 require(layer.get('name')=='s2cloudless' and re.search(r'for\s+2016\b',layer.get('title',''),re.I) and 'Creative Commons Attribution 4.0' in layer.get('abstract','') and not re.search(r'NonCommercial|Non-Commercial|BY-NC',layer.get('abstract',''),re.I),'Alias/year/licence changed; no NC fallback')
 require(layer.get('wms_version')=='1.1.1' and layer.get('output_crs')=='EPSG:4326','Global projection/axis contract differs')
 row=manifest.get('image',{});source=plain_file(folder/FILENAME);raw=source.read_bytes()
 expected_files={FILENAME,'west-eox2016-source4096.jpg','east-eox2016-source4096.jpg'}
 require(row.get('filename')==FILENAME and {p.name for p in folder.glob('*.jpg')}==expected_files,'Missing/extra/unexpected global JPEG source')
 require(row.get('bounds_wgs84')==[-180,-90,180,90] and (row.get('width'),row.get('height'))==(8192,4096) and jpeg_dimensions(raw)==(8192,4096),'Global dimensions/bbox must be exact8192x4096WGS84')
 require(0<len(raw)<=MAX_IMAGE and len(raw)==row.get('bytes') and hashlib.sha256(raw).hexdigest()==row.get('sha256'),'Global source byte/hash cap differs')
 require(row.get('layer')=='s2cloudless' and row.get('license')=='CC-BY-4.0' and row.get('license_url')==LICENSE and row.get('source_years')==[2016,2017] and row.get('required_attribution')==ATTRIBUTION,'Global image licence/date differs')
 require(all(row.get(flag) is False for flag in ('current_imagery','cloud_free_guarantee','pbr_albedo','source_native_resolution_claim')),'Global historical coarse-image scope differs')
 composition=row.get('composition',{});tiles=row.get('source_tiles')
 require(composition.get('jpeg_quality')==95 and composition.get('jpeg_subsampling')==0 and composition.get('jpeg_optimize') is True and composition.get('source_tile_count')==2 and composition.get('maximum_wms_request_dimension')==4096,'Global composition recipe differs')
 require(isinstance(tiles,list) and len(tiles)==2,'Two original hemisphere source receipts required')
 for index,(tile,bounds,name) in enumerate(zip(tiles,([-180,-90,0,90],[0,-90,180,90]),('west-eox2016-source4096.jpg','east-eox2016-source4096.jpg'))):
  require(isinstance(tile,dict) and tile.get('filename')==name and tile.get('bounds_wgs84')==bounds and tile.get('destination_pixel_offset')==[index*4096,0],'Hemisphere identity/order/bbox differs')
  tile_raw=plain_file(folder/name).read_bytes();require(0<len(tile_raw)<=MAX_IMAGE and len(tile_raw)==tile.get('bytes') and hashlib.sha256(tile_raw).hexdigest()==tile.get('sha256') and jpeg_dimensions(tile_raw)==(4096,4096),'Original hemisphere dimensions/bytes/hash differ')
  url=urllib.parse.urlparse(tile.get('request',{}).get('url',''));query=urllib.parse.parse_qs(url.query,keep_blank_values=True)
  expected={'SERVICE':'WMS','VERSION':'1.1.1','REQUEST':'GetMap','LAYERS':'s2cloudless','STYLES':'','SRS':'EPSG:4326','BBOX':','.join(map(str,bounds)),'WIDTH':'4096','HEIGHT':'4096','FORMAT':'image/jpeg','TRANSPARENT':'FALSE'}
  require(url.scheme=='https' and url.netloc=='tiles.maps.eox.at' and url.path=='/wms' and query=={k:[v] for k,v in expected.items()},'Hemisphere source WMS endpoint/request differs')
  require(tile['request'].get('bytes')==len(tile_raw) and tile['request'].get('sha256')==tile['sha256'],'Hemisphere HTTP receipt differs from original source bytes')
 asset=DEST+'/'+NAME;require(row.get('ue_texture_path')==asset,'Unexpected global destination')
 return {'source_file':source.relative_to(root).as_posix(),'source_sha256':row['sha256'],'source_bytes':len(raw),'source_dimensions':[8192,4096],'source_tile_count':2,'bounds_wgs84':row['bounds_wgs84'],'manifest_sha256':digest(manifest_path),'asset_name':NAME,'asset_path':asset,'object_path':asset+'.'+NAME,'license':'CC-BY-4.0','required_attribution':ATTRIBUTION}

def import_global_truecolour(repo_root=ROOT,unreal_module=None,report_path=None,source_sha=None):
 if unreal_module is None:import unreal as unreal_module
 ue=unreal_module;root=Path(repo_root).resolve();target=Path(report_path) if report_path else root/'Saved/PresentationValidation/global_truecolour_import.json'
 report={'schema_version':1,'utc':datetime.now(timezone.utc).isoformat(),'source_sha':source_sha,'scope':'One Editor texture import/save and same-process property/built-size readback only. Not cold load, actual RHI mipchain, scene binding, cook or visible-result acceptance.','expected':1,'planned':0,'imported':0,'saved':0,'verified':0,'failed':0,'valid':False,'source_unchanged':False,'actual_mip_count_verified':False,'cold_process_readback_verified':False,'runtime_binding_verified':False}
 def write():
  target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(report,ensure_ascii=False,allow_nan=False,indent=2)+'\n',encoding='utf-8',newline='\n')
 try:
  require(source_sha is None or re.fullmatch(r'[a-fA-F0-9]{40}',source_sha),'Explicit source SHA must be40hex or null')
  plan=build_import_plan(root);report['planned']=1;report['manifest_sha256']=plan['manifest_sha256'];A=ue.EditorAssetLibrary
  if A.does_asset_exist(plan['asset_path']):
   existing=ue.load_asset(plan['object_path']);require(isinstance(existing,ue.Texture2D) and existing.get_path_name()==plan['object_path'],'Foreign/redirected object occupies global texture path')
  require(A.does_directory_exist(DEST) or A.make_directory(DEST),'Cannot create owned global texture folder')
  task=ue.AssetImportTask()
  for key,value in {'filename':str(root/plan['source_file']),'destination_path':DEST,'destination_name':NAME,'automated':True,'replace_existing':True,'replace_existing_settings':True,'save':False}.items():task.set_editor_property(key,value)
  ue.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task]);asset=ue.load_asset(plan['object_path'])
  require(isinstance(asset,ue.Texture2D) and asset.get_path_name()==plan['object_path'],'Imported object is not exact Texture2D');report['imported']=1
  settings={'srgb':True,'never_stream':False,'compression_settings':ue.TextureCompressionSettings.TC_DEFAULT,'lod_group':ue.TextureGroup.TEXTUREGROUP_WORLD,'mip_gen_settings':ue.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP,'power_of_two_mode':ue.TexturePowerOfTwoSetting.STRETCH_TO_POWER_OF_TWO,'max_texture_size':8192,'lod_bias':0,'virtual_texture_streaming':False,'address_x':ue.TextureAddress.TA_WRAP,'address_y':ue.TextureAddress.TA_CLAMP}
  for key,value in settings.items():asset.set_editor_property(key,value)
  for key,value in {'Aftershock.SourceSHA256':plan['source_sha256'],'Aftershock.License':'CC-BY-4.0','Aftershock.ProductYear':'2016','Aftershock.Attribution':ATTRIBUTION,'Aftershock.BoundsWGS84':'[-180,-90,180,90]','Aftershock.Scope':'Historical8192x4096two-hemisphere global colour; provider background/composite, not current imagery or regional accuracy'}.items():A.set_metadata_tag(asset,key,value)
  require(A.save_loaded_asset(asset,only_if_is_dirty=False),'Global texture explicit save failed');report['saved']=1
  loaded=ue.load_asset(plan['object_path']);require(isinstance(loaded,ue.Texture2D) and loaded.get_path_name()==plan['object_path'],'Saved exact global texture missing')
  for key,value in settings.items():require(loaded.get_editor_property(key)==value,'Global texture property readback mismatch: '+key)
  built=loaded.blueprint_get_built_texture_size();require([int(built.x),int(built.y)]==[8192,4096],'Built global texture dimensions differ')
  require(A.get_metadata_tag(loaded,'Aftershock.SourceSHA256')==plan['source_sha256'] and A.get_metadata_tag(loaded,'Aftershock.License')=='CC-BY-4.0','Global saved metadata differs')
  package=root/'Content/Environment/Textures/Regional/Macro'/(NAME+'.uasset');require(package.is_file(),'Global texture package missing')
  report.update(result={**plan,'built_dimensions':[8192,4096],'expected_full_mip_count':14,'package_file':package.relative_to(root).as_posix(),'package_sha256':digest(package),'srgb':True,'never_stream':False,'longitude_wrap':True,'latitude_clamp':True},verified=1)
  report['source_unchanged']=digest(root/plan['source_file'])==plan['source_sha256'] and digest(root/SOURCE/'IMAGERY_MANIFEST.json')==plan['manifest_sha256']
  require(report['source_unchanged'],'Global source changed during import');report['valid']=True;write()
 except Exception as error:
  report.update(failed=1,valid=False,error=str(error).replace(str(root),'<repo>'));write();raise RuntimeError('Global texture import/readback failed; see Saved report') from error
 ue.log('AFTERSHOCK_GLOBAL_TRUECOLOUR_IMPORT expected=1 imported=1 saved=1 verified=1 failed=0 srgb=1 neverStream=0 actualMipCountVerified=0 runtimeBindingVerified=0')
 return report

if __name__=='__main__':import_global_truecolour()
