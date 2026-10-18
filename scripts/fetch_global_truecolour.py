"""Bounded offline EOX2016 world colour, separate from all regional geodata.

Exactly8192x4096 from two4096x4096 WMS1.1.1/EPSG4326 hemispheres: respect the
provider's4096px request cap. Historical2016 CC-BY4 only; no newer NC fallback.
Final source JPEG <=24MiB, cache+new package <=64MiB. No Unreal/Git.
"""
from __future__ import annotations
import argparse,hashlib,html,io,json,math,os,re,sys,time
from datetime import datetime,timezone
from pathlib import Path
import urllib.error,urllib.parse,urllib.request
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'Saved/GeographyPythonPackages'))
from PIL import Image,ImageStat

OUT=ROOT/'_Imports/Geography/GlobalImagery'
CACHE=ROOT/'Saved/GlobalImageryCache'
ENDPOINT='https://tiles.maps.eox.at/wms'
POLICY='https://cloudless.eox.at/license-non-commercial'
LICENSE='https://creativecommons.org/licenses/by/4.0/'
ATTRIBUTION='EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017)'
BOUNDS=[-180,-90,180,90]
WIDTH,HEIGHT=8192,4096
MAX_IMAGE=24*1024*1024
MAX_TOTAL=64*1024*1024
FILENAME='global-eox2016-truecolour.jpg'
DEST='/Game/Environment/Textures/Regional/Macro/T_GlobalMacro_EOX2016'

def need(value,message):
 if not value:raise ValueError(message)
def sha(raw):return hashlib.sha256(raw).hexdigest()
def now():return datetime.now(timezone.utc).isoformat()
def write_json(path,value):
 path.parent.mkdir(parents=True,exist_ok=True);temp=path.with_suffix(path.suffix+'.tmp')
 temp.write_text(json.dumps(value,ensure_ascii=False,allow_nan=False,indent=2)+'\n',encoding='utf-8',newline='\n');os.replace(temp,path)
def total_bytes():return sum(p.stat().st_size for folder in (OUT,CACHE) if folder.exists() for p in folder.rglob('*') if p.is_file())
def protected_inputs():
 receipt=json.loads((ROOT/'_Imports/Geography/RegionalImagery/PROTECTED_GEOGRAPHY_INPUTS.json').read_text(encoding='utf-8'))
 rows=receipt['inventory']+receipt['compiled_sources']
 need(len(rows)==99 and len({row['file'] for row in rows})==99,'Expected99protected geographic inputs')
 for row in rows:need(sha((ROOT/row['file']).read_bytes())==row['sha256'],'Protected geography source hash mismatch')
 folder=ROOT/'_Imports/Geography/RegionalImagery'
 for path in sorted(folder.iterdir()):
  if path.is_file():rows.append({'file':path.relative_to(ROOT).as_posix(),'sha256':sha(path.read_bytes()),'bytes':path.stat().st_size})
 return rows
def check_protected(rows):
 need(all(sha((ROOT/r['file']).read_bytes())==r['sha256'] for r in rows),'Pre-existing geographic/regional imagery sources changed')

def download(url,name,limit,offline,log):
 path=CACHE/name;sidecar=path.with_suffix(path.suffix+'.source.json');CACHE.mkdir(parents=True,exist_ok=True)
 if path.exists():
  need(path.is_file() and not path.is_symlink() and sidecar.is_file(),'Linked/missing source cache')
  raw=path.read_bytes();record=json.loads(sidecar.read_text(encoding='utf-8'))
  need(record.get('url')==url and record.get('sha256')==sha(raw) and record.get('bytes')==len(raw),'Cached source provenance mismatch')
  record={**record,'cached':True}
 else:
  need(not offline,'Missing offline global source cache')
  for attempt in range(1,4):
   try:
    request=urllib.request.Request(url,headers={'User-Agent':'AFTERSHOCK-global-colour/2 (two bounded4096public2016WMSrequests; no runtime traffic)'})
    with urllib.request.urlopen(request,timeout=45) as response:
     need(response.status==200 and response.geturl().startswith('https://'),'Unexpected source response/transport')
     raw=response.read(limit+1);need(len(raw)<=limit,'Source response exceeds hard byte limit')
     record={'url':url,'retrieved_utc':now(),'bytes':len(raw),'sha256':sha(raw),'content_type':response.headers.get('Content-Type',''),'etag':response.headers.get('ETag'),'last_modified':response.headers.get('Last-Modified'),'cached':False}
    need(total_bytes()+len(raw)*2+131072<=MAX_TOTAL,'Global cache+source budget exceeded')
    temp=path.with_suffix(path.suffix+'.tmp');temp.write_bytes(raw);os.replace(temp,path);write_json(sidecar,record);break
   except (urllib.error.HTTPError,urllib.error.URLError,TimeoutError,ConnectionError) as error:
    log.append({'event':'request_failed','utc':now(),'url':url,'attempt':attempt,'error_type':type(error).__name__})
    if attempt==3:raise
    time.sleep(2*attempt)
 need(len(raw)<=limit,'Cached source exceeds hard byte limit');log.append({'event':'source_response',**record});return raw,record

def provider_plan(offline,log):
 policy,policy_record=download(POLICY,'eox-2016-license-policy.html',2*1024*1024,offline,log)
 plain=' '.join(html.unescape(re.sub(r'<[^>]*>',' ',policy.decode('utf-8'))).split())
 need(re.search(r'For the year 2016.{0,200}?Creative Commons Attribution 4\.0',plain,re.I),'Policy no longer confirms2016CCBY4; no NC fallback')
 url=ENDPOINT+'?'+urllib.parse.urlencode({'SERVICE':'WMS','VERSION':'1.1.1','REQUEST':'GetCapabilities'})
 raw,caps_record=download(url,'eox-wms-1.1.1-capabilities.xml',2*1024*1024,offline,log);tree=ET.fromstring(raw)
 need(tree.attrib.get('version')=='1.1.1','WMS axis/version contract changed')
 layers=[layer for layer in tree.iter('Layer') if layer.findtext('Name')=='s2cloudless'];need(len(layers)==1,'Exact2016alias missing/ambiguous')
 layer=layers[0];title=layer.findtext('Title') or '';abstract=layer.findtext('Abstract') or ''
 need(re.search(r'for\s+2016\b',title,re.I) and 'Creative Commons Attribution 4.0' in abstract and not re.search(r'NonCommercial|Non-Commercial|BY-NC',abstract,re.I),'Exact alias no longer explicitly2016CCBY4')
 parents={child:parent for parent in tree.iter() for child in parent};current=layer;srs=set()
 while current is not None:
  for item in current.findall('SRS'):srs.update((item.text or '').split())
  current=parents.get(current)
 need('EPSG:4326' in srs and 'image/jpeg' in [n.text for n in tree.findall('./Capability/Request/GetMap/Format')],'Required4326/JPEG service not advertised')
 return {'provider':'EOX IT Services GmbH','product':'EOxCloudless2016 historical composite','product_year':2016,'source_years':[2016,2017],'license':'CC-BY-4.0','license_url':LICENSE,'required_attribution':ATTRIBUTION,'official_license_policy':policy_record,'wms_capabilities':caps_record,'layer':{'name':'s2cloudless','title':title,'abstract':abstract,'wms_version':'1.1.1','output_crs':'EPSG:4326'},'qualification':'Provider2016CCBY4exception only.2018andnewerNCproducts not used. The historical world composite may include provider background, seams, illumination or residual clouds; no complete native Sentinel coverage/current image/PBR albedo claim.'}

def archive_previous():
 manifest_path=OUT/'IMAGERY_MANIFEST.json'
 if not manifest_path.exists():return None
 old=json.loads(manifest_path.read_text(encoding='utf-8'))
 if old.get('schema_version')==2:return None
 need(old.get('schema_version')==1 and old.get('complete') is True and old.get('image',{}).get('width')==2048 and old['image'].get('height')==1024,'Only the explicitly authorised2048to8192source upgrade is allowed')
 source=OUT/FILENAME;need(sha(source.read_bytes())==old['image']['sha256'],'Previous2048source differs from receipt')
 folder=OUT/'archive'/('2048x1024-'+old['image']['sha256'][:16]);folder.mkdir(parents=True,exist_ok=True);files=[]
 for name in (FILENAME,'IMAGERY_MANIFEST.json','LICENSE_VERIFICATION.json','DOWNLOAD_LOG.json','PROTECTED_INPUTS.json','README.md'):
  path=OUT/name
  if not path.is_file():continue
  raw=path.read_bytes();target=folder/name
  if target.exists():need(target.read_bytes()==raw,'Existing historical archive is immutable')
  else:target.write_bytes(raw)
  files.append({'file':target.relative_to(ROOT).as_posix(),'bytes':len(raw),'sha256':sha(raw)})
 return {'scope':'Explicit authorised2048to8192upgrade; previous source/receipts preserved without relabelling as8192 proof','previous_manifest_sha256':sha(manifest_path.read_bytes()),'files':files}

def source_image(provider,offline,log):
 canvas=Image.new('RGB',(WIDTH,HEIGHT));tiles=[]
 for index,bounds in enumerate(([-180,-90,0,90],[0,-90,180,90])):
  query={'SERVICE':'WMS','VERSION':'1.1.1','REQUEST':'GetMap','LAYERS':'s2cloudless','STYLES':'','SRS':'EPSG:4326','BBOX':','.join(map(str,bounds)),'WIDTH':4096,'HEIGHT':4096,'FORMAT':'image/jpeg','TRANSPARENT':'FALSE'}
  url=ENDPOINT+'?'+urllib.parse.urlencode(query);raw,record=download(url,'hemisphere-'+sha(url.encode())[:12]+'.jpg',MAX_IMAGE,offline,log)
  image=Image.open(io.BytesIO(raw));need(image.format=='JPEG' and image.mode=='RGB' and image.size==(4096,4096),'Hemisphere response is not exact4096x4096RGB JPEG');image.load();need(max(ImageStat.Stat(image).var)>4,'Blank hemisphere source')
  canvas.paste(image,(index*4096,0));image.close();name=('west' if index==0 else 'east')+'-eox2016-source4096.jpg';target=OUT/name
  if target.exists():need(target.read_bytes()==raw,'Existing raw hemisphere source differs')
  else:need(total_bytes()+len(raw)+131072<=MAX_TOTAL,'Hemisphere publication cap exceeded');target.write_bytes(raw)
  tiles.append({'filename':name,'sha256':sha(raw),'bytes':len(raw),'bounds_wgs84':bounds,'width':4096,'height':4096,'destination_pixel_offset':[index*4096,0],'request':record,'query':query})
  print(json.dumps({'hemisphere':index,'bytes':len(raw),'source_sha256':sha(raw),'dimensions':[4096,4096]}),flush=True)
 encoded=io.BytesIO();canvas.save(encoded,format='JPEG',quality=95,subsampling=0,optimize=True);canvas.close();raw=encoded.getvalue()
 need(0<len(raw)<=MAX_IMAGE,'Final8192composite exceeds24MiB');return raw,tiles

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--offline',action='store_true');parser.add_argument('--plan-only',action='store_true');args=parser.parse_args()
 protected=protected_inputs();log=[];completed=False;OUT.mkdir(parents=True,exist_ok=True)
 try:
  provider=provider_plan(args.offline,log)
  if args.plan_only:completed=True;print(json.dumps({'licensed_layer':'s2cloudless','product_year':2016,'license':'CC-BY-4.0','planned_dimensions':[WIDTH,HEIGHT],'max_source_bytes':MAX_IMAGE}));return
  raw,tiles=source_image(provider,args.offline,log)
  image=Image.open(io.BytesIO(raw));need(image.format=='JPEG' and image.mode=='RGB' and image.size==(WIDTH,HEIGHT),'Composite is not exact8192x4096RGB JPEG');image.load()
  need(max(ImageStat.Stat(image).var)>4,'Blank global imagery response')
  historical=archive_previous();target=OUT/FILENAME
  if target.exists() and historical is None:need(target.read_bytes()==raw,'Existing8192source differs; another explicit upgrade is required')
  else:
   need(total_bytes()+len(raw)+131072<=MAX_TOTAL,'Source publication budget exceeded');temp=target.with_suffix('.jpg.tmp');temp.write_bytes(raw);os.replace(temp,target)
  row={'filename':FILENAME,'sha256':sha(raw),'bytes':len(raw),'width':WIDTH,'height':HEIGHT,'bounds_wgs84':BOUNDS,'format':'JPEG','color_space':'sRGB','layer':'s2cloudless','source_years':[2016,2017],'license':'CC-BY-4.0','license_url':LICENSE,'required_attribution':ATTRIBUTION,'source_tiles':tiles,'composition':{'method':'Horizontal integer-pixel paste, west then east, no resampling; JPEG decoded then output re-encoded','jpeg_quality':95,'jpeg_subsampling':0,'jpeg_optimize':True,'source_tile_count':2,'maximum_wms_request_dimension':4096},'pixel_mapping':'EPSG4326 equirectangular: u=(longitude+180)/360; v=(90-latitude)/180; first row north, pixel centres half a sample inside bbox','output_degrees_per_pixel':[360/WIDTH,180/HEIGHT],'output_equator_metres_per_pixel_approx':[40075016.6856/WIDTH,20003931.4586/HEIGHT],'source_native_resolution_claim':False,'current_imagery':False,'cloud_free_guarantee':False,'pbr_albedo':False,'ue_texture_path':DEST,'qualification':'Historical world colour only, approximately4.9km/pixel at equator. Not regional detail, terrain/shore geometry, actual streets, live weather, complete Sentinel coverage or a satellite-accuracy guarantee.'}
  previous=historical
  if previous is None and (OUT/'IMAGERY_MANIFEST.json').is_file():previous=json.loads((OUT/'IMAGERY_MANIFEST.json').read_text()).get('previous_source_archive')
  write_json(OUT/'LICENSE_VERIFICATION.json',provider)
  write_json(OUT/'IMAGERY_MANIFEST.json',{'schema_version':2,'source_revision':'8192x4096-two-4096-hemispheres-v2','complete':True,'image_count':1,'provider':provider,'image':row,'previous_source_archive':previous,'max_source_image_bytes':MAX_IMAGE,'cache_plus_output_bytes':total_bytes(),'max_cache_plus_output_bytes':MAX_TOTAL,'engine_import_verified':False,'render_visibility_verified':False})
  write_json(OUT/'PROTECTED_INPUTS.json',{'scope':'Pre-existing99geography plus regional imagery source files; unchanged, not global runtime acceptance','files':protected})
  need(total_bytes()<=MAX_TOTAL,'Final cache/source budget exceeded');completed=True
  print(json.dumps({'source_ready':True,'width':WIDTH,'height':HEIGHT,'bytes':len(raw),'sha256':sha(raw),'license':'CC-BY-4.0','cache_plus_output_bytes':total_bytes(),'unreal_verified':False}),flush=True)
 finally:
  write_json(OUT/'DOWNLOAD_LOG.json',{'utc':now(),'completed':completed,'scope':'Bounded offline historical global source only; no Unreal/GPU verification','requests':log,'cache_plus_output_bytes':total_bytes(),'max_bytes':MAX_TOTAL});check_protected(protected)

if __name__=='__main__':main()
