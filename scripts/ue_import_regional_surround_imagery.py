"""Strict20 EOX2016CCBY4 source import; execute inside Unreal Editor Python.

build_import_plan is ordinary-Python compatible and never starts Unreal or Git.
Explicit save + loaded property/built-size readback; NOT cold-process/RHI proof.
Non-power-of-two source is stretched by UE's import/build setting, never padded,
preserving normalized full-bbox UV coverage and source JPEG bytes.
"""
from __future__ import annotations
from datetime import datetime,timezone
import hashlib,json,math,re,struct,urllib.parse
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=Path('_Imports/Geography/RegionalImagery/Surround')
DEST='/Game/Environment/Textures/Regional/Surround'
ALLOWED_LAYERS={'s2cloudless','s2cloudless-2016'}
LICENSE_URL='https://creativecommons.org/licenses/by/4.0/'
ATTRIBUTION='EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017)'

def require(ok,message):
 if not ok:raise ValueError(message)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def jpeg_dimensions(raw):
 require(raw.startswith(b'\xff\xd8') and raw.endswith(b'\xff\xd9'),'Source is not a complete JPEG')
 p=2
 while p<len(raw):
  require(raw[p]==255,'Malformed JPEG marker');p+=1
  while p<len(raw) and raw[p]==255:p+=1
  require(p<len(raw),'Truncated JPEG marker');marker=raw[p];p+=1
  if marker in (1,*range(208,216)):continue
  require(marker not in (217,218) and p+2<=len(raw),'No JPEG frame before compressed scan')
  size=struct.unpack('>H',raw[p:p+2])[0];require(size>=2 and p+size<=len(raw),'Truncated JPEG segment')
  if marker in (192,193,194,195,197,198,199,201,202,203,205,206,207):
   require(size>=8 and raw[p+2]==8 and raw[p+7]==3,'Expected8bit3component source JPEG')
   h,w=struct.unpack('>HH',raw[p+3:p+7]);return w,h
  p+=size
 raise ValueError('JPEG frame missing')

def expanded(bounds):
 w,s,e,n=bounds;cx=(w+e)*.5;cy=(s+n)*.5;hx=(e-w)*4.;hy=(n-s)*4.
 requested=[cx-hx,cy-hy,cx+hx,cy+hy]
 return requested,[max(-180.,requested[0]),max(-90.,requested[1]),min(180.,requested[2]),min(90.,requested[3])]

def build_import_plan(repo_root=ROOT,source_relative=SOURCE):
 root=Path(repo_root).resolve();folder=root/Path(source_relative);manifest_file=folder/'IMAGERY_MANIFEST.json';manifest=json.loads(manifest_file.read_text(encoding='utf-8'));license_proof=json.loads((folder/'LICENSE_VERIFICATION.json').read_text(encoding='utf-8'))
 matrix_file=root/'_Imports/Geography/RegionalEnvironment/REGION_ENVIRONMENT_MATRIX.json';matrix=json.loads(matrix_file.read_text(encoding='utf-8'));catalog={r['region_id']:r for r in matrix['regions']}
 require(manifest.get('complete') is True and manifest.get('region_count')==20 and len(manifest.get('regions',[]))==20 and len(catalog)==20,'Exactly20complete region images required')
 require(manifest.get('source_matrix_sha256')==digest(matrix_file),'Frozen regional bbox source changed')
 require(manifest.get('expansion_factor')==8 and manifest.get('exact_dimensions')==[2048,2048] and manifest.get('source_water_dem_changed') is False,'Surround8x2048/source scope differs')
 provider=manifest.get('provider',{});require(provider==license_proof,'Licence proof/provider metadata differs')
 for filename,key in [('eox-2016-license-policy.html','official_license_policy'),('eox-wms-1.1.1-capabilities.xml','wms_capabilities')]:
  receipt=folder/'provenance'/filename
  require(receipt.is_file() and not receipt.is_symlink() and digest(receipt)==provider.get(key,{}).get('sha256'),'Raw official provider receipt hash differs')
 require(provider.get('license')=='CC-BY-4.0' and provider.get('license_url')==LICENSE_URL and provider.get('product_year')==2016 and provider.get('source_years')==[2016,2017] and provider.get('required_attribution')==ATTRIBUTION,'Only attributed2016CCBY4product allowed')
 layer=provider.get('layer',{});require(layer.get('name') in ALLOWED_LAYERS and re.search(r'for\s+2016\b',layer.get('title',''),re.I) and 'Creative Commons Attribution 4.0' in layer.get('abstract','') and not re.search(r'NonCommercial|Non-Commercial|BY-NC',layer.get('abstract',''),re.I),'Provider alias/year/licence changed; no NC fallback')
 require(layer.get('wms_version')=='1.1.1' and layer.get('output_crs')=='EPSG:4326','Projection/axis contract differs')
 seen=set();items=[];total=0;used_files=set()
 for row in manifest['regions']:
  rid=row.get('region_id');require(isinstance(rid,str) and re.fullmatch('[a-z][a-z0-9_]*',rid) and rid in catalog and rid not in seen,'Unknown/duplicate region identity');seen.add(rid)
  filename=rid+'-eox2016-surround.jpg';require(row.get('filename')==filename,'Source filename is not the region-specific expected leaf')
  source=folder/filename;require(source.is_file() and not source.is_symlink() and not any(p.is_symlink() or getattr(p,'is_junction',lambda:False)() for p in source.parents),'Missing/linked source file')
  require(row.get('bounds_wgs84')==expanded(catalog[rid]['overview']['bounds_wgs84'])[1] and row.get('context_bounds_wgs84')==catalog[rid]['overview']['bounds_wgs84'] and row.get('requested_unclamped_bounds_wgs84')==expanded(catalog[rid]['overview']['bounds_wgs84'])[0] and row.get('expansion_factor')==8 and len(row['bounds_wgs84'])==4 and all(type(v) in (int,float) and math.isfinite(v) for v in row['bounds_wgs84']),'Exact region bbox differs')
  require(row.get('license')=='CC-BY-4.0' and row.get('layer')==layer['name'] and row.get('source_years')==[2016,2017] and row.get('required_attribution')==ATTRIBUTION and row.get('pbr_albedo') is False and row.get('current_imagery') is False and row.get('cloud_free_guarantee') is False,'Source image licence/date/scope differs')
  raw=source.read_bytes();total+=len(raw);require(len(raw)==row.get('bytes') and hashlib.sha256(raw).hexdigest()==row.get('sha256') and total<=64*1024*1024,'Source hash/bytes/budget mismatch')
  w,h=jpeg_dimensions(raw);require((w,h)==(row.get('width'),row.get('height')) and w==2048 and h==2048,'JPEG actual dimensions differ or exceed2048')
  request=row.get('request',{});require(request.get('bytes')==len(raw) and request.get('sha256')==row['sha256'] and request.get('content_type','').lower().startswith('image/jpeg'),'Source response receipt differs')
  url=urllib.parse.urlparse(request.get('url',''));query=urllib.parse.parse_qs(url.query,keep_blank_values=True)
  require(url.scheme=='https' and url.netloc=='tiles.maps.eox.at' and url.path=='/wms' and query.get('LAYERS')==[layer['name']] and query.get('VERSION')==['1.1.1'] and query.get('SRS')==['EPSG:4326'] and query.get('WIDTH')==[str(w)] and query.get('HEIGHT')==[str(h)],'Source WMS endpoint/layer/projection/size differs')
  requested=list(map(float,query.get('BBOX',[''])[0].split(',')));require(len(requested)==4 and all(abs(a-b)<1e-10 for a,b in zip(requested,row['bounds_wgs84'])),'WMS request bbox differs')
  name='T_RegionalSurround_'+rid;asset=DEST+'/'+name;require(row.get('ue_texture_path')==asset,'Unexpected destination asset path')
  used_files.add(source.resolve());items.append({'region_id':rid,'source_file':source.relative_to(root).as_posix(),'source_sha256':row['sha256'],'source_dimensions':[w,h],'expected_built_dimensions':[1<<(w-1).bit_length(),1<<(h-1).bit_length()],'bounds_wgs84':row['bounds_wgs84'],'asset_path':asset,'object_path':asset+'.'+name,'asset_name':name})
 require(seen==set(catalog) and {p.resolve() for p in folder.glob('*.jpg')}==used_files,'Image coverage missing or extra JPEG source')
 return {'items':items,'manifest_sha256':digest(manifest_file),'license':'CC-BY-4.0','product_year':2016,'source_image_bytes':total,'required_attribution':ATTRIBUTION}

def import_regional_surround_imagery(repo_root=ROOT,unreal_module=None,report_path=None,source_sha=None,source_relative=SOURCE):
 if unreal_module is None:import unreal as unreal_module
 ue=unreal_module;root=Path(repo_root).resolve();report_path=Path(report_path) if report_path else root/'Saved/PresentationValidation/regional_surround_imagery_import.json'
 report={'schema_version':1,'utc':datetime.now(timezone.utc).isoformat(),'source_sha':source_sha,'scope':'Editor20textureimport/save+loadedproperty/built-size readback only; not cold-process, actualRHI mipchain, binding, cook or visible result.','expected':20,'planned':0,'imported':0,'saved':0,'verified':0,'failed':0,'valid':False,'source_images_unchanged':None,'actual_mip_count_verified':False,'cold_process_readback_verified':False,'runtime_binding_verified':False,'results':[],'failures':[]}
 def write():
  report_path.parent.mkdir(parents=True,exist_ok=True);report_path.write_text(json.dumps(report,ensure_ascii=False,allow_nan=False,indent=2)+'\n',encoding='utf-8',newline='\n')
 try:
  require(source_sha is None or re.fullmatch('[0-9a-fA-F]{40}',source_sha),'Explicit source SHA must be40hex, or null')
  plan=build_import_plan(root,source_relative);report['planned']=len(plan['items']);report['manifest_sha256']=plan['manifest_sha256'];A=ue.EditorAssetLibrary
  for item in plan['items']:
   if A.does_asset_exist(item['asset_path']):
    existing=ue.load_asset(item['object_path']);require(isinstance(existing,ue.Texture2D) and existing.get_path_name()==item['object_path'],'Foreign/redirected object occupies '+item['asset_path'])
  require(A.does_directory_exist(DEST) or A.make_directory(DEST),'Cannot create owned macro texture folder')
  asset_tools=ue.AssetToolsHelpers.get_asset_tools()
 except Exception as error:
  report['failed']=1;report['not_attempted']=20;report['failures'].append({'stage':'preflight','error':str(error).replace(str(root),'<repo>')});write();raise RuntimeError('Macro imagery preflight failed; see Saved report') from error
 settings={'srgb':True,'never_stream':False,'compression_settings':ue.TextureCompressionSettings.TC_DEFAULT,'lod_group':ue.TextureGroup.TEXTUREGROUP_WORLD,'mip_gen_settings':ue.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP,'power_of_two_mode':ue.TexturePowerOfTwoSetting.STRETCH_TO_POWER_OF_TWO,'max_texture_size':2048,'lod_bias':0,'virtual_texture_streaming':False,'address_x':ue.TextureAddress.TA_CLAMP,'address_y':ue.TextureAddress.TA_CLAMP}
 for item in plan['items']:
  result={**item,'valid':False}
  try:
   require(digest(root/item['source_file'])==item['source_sha256'],'Source image changed after preflight')
   task=ue.AssetImportTask()
   for key,value in {'filename':str(root/item['source_file']),'destination_path':DEST,'destination_name':item['asset_name'],'automated':True,'replace_existing':True,'replace_existing_settings':True,'save':False}.items():task.set_editor_property(key,value)
   asset_tools.import_asset_tasks([task]);asset=ue.load_asset(item['object_path']);require(isinstance(asset,ue.Texture2D) and asset.get_path_name()==item['object_path'],'Imported result is not the exact Texture2D');report['imported']+=1
   for key,value in settings.items():asset.set_editor_property(key,value)
   for key,value in {'Aftershock.SourceSHA256':item['source_sha256'],'Aftershock.License':'CC-BY-4.0','Aftershock.ProductYear':'2016','Aftershock.Attribution':ATTRIBUTION,'Aftershock.BoundsWGS84':json.dumps(item['bounds_wgs84']),'Aftershock.Scope':'Historical intermediate surround colour; sourceWater/DEM unchanged, not measured PBR albedo/current imagery'}.items():A.set_metadata_tag(asset,key,value)
   require(A.save_loaded_asset(asset,only_if_is_dirty=False),'Explicit texture package save failed');report['saved']+=1
   loaded=ue.load_asset(item['object_path']);require(isinstance(loaded,ue.Texture2D),'Saved Texture2D readback missing')
   for key,value in settings.items():require(loaded.get_editor_property(key)==value,'Texture property readback mismatch: '+key)
   built=loaded.blueprint_get_built_texture_size();size=[int(built.x),int(built.y)];require(size==item['expected_built_dimensions'] and max(size)<=2048,'Built normalized-UV size differs from PoT/import cap')
   require(A.get_metadata_tag(loaded,'Aftershock.SourceSHA256')==item['source_sha256'] and A.get_metadata_tag(loaded,'Aftershock.License')=='CC-BY-4.0','Saved source/licence metadata differs')
   package=root/'Content/Environment/Textures/Regional/Surround'/(item['asset_name']+'.uasset');require(package.is_file(),'Saved package file missing')
   result.update(valid=True,built_dimensions=size,expected_full_mip_count=max(size).bit_length(),actual_mip_count_verified=False,package_file=package.relative_to(root).as_posix(),package_sha256=digest(package),srgb=True,never_stream=False,world_colour_default_compression=True,mip_generation_from_group=True,pot_mode='StretchToPowerOfTwo; no padding/fullbboxUV preserved');report['verified']+=1
  except Exception as error:
   report['failed']+=1;result['error']=str(error).replace(str(root),'<repo>');report['failures'].append({'region_id':item['region_id'],'error':result['error']})
  report['results'].append(result);write()
 report['source_images_unchanged']=all(digest(root/i['source_file'])==i['source_sha256'] for i in plan['items']) and digest(root/Path(source_relative)/'IMAGERY_MANIFEST.json')==plan['manifest_sha256']
 report['valid']=report['source_images_unchanged'] and report['failed']==0 and all(report[k]==20 for k in ('planned','imported','saved','verified'));write()
 if not report['valid']:raise RuntimeError('Macro imagery20asset import/readback incomplete; see Saved report')
 ue.log('AFTERSHOCK_REGIONAL_SURROUND_IMPORT expected=20 imported=20 saved=20 verified=20 failed=0 srgb=1 neverStream=0 currentImagery=0 actualMipCountVerified=0 runtimeBindingVerified=0')
 return report

if __name__=='__main__':import_regional_surround_imagery()
