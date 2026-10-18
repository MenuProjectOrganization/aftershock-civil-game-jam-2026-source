"""Bounded historical EOX2016 regional macro-colour source, no runtime network.

Only the explicitly CC-BY-4.0 2016 WMS layer is allowed. Never substitutes newer
NC layers. Exact20 regional bboxes; <=1024 dimensions, <=50MiB cache+output total.
Images are resampled2016/2017 true-colour composites, not current10m PBR albedo.
Existing96 geography sources,3 compiled inls, APIs and docs remain byte unchanged.
"""
from __future__ import annotations
import argparse,hashlib,html,io,json,math,os,re,sys,time,urllib.error,urllib.parse,urllib.request,xml.etree.ElementTree as ET
from datetime import datetime,timezone
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'Saved/GeographyPythonPackages'))
from PIL import Image,ImageStat
OUT=ROOT/'_Imports/Geography/RegionalImagery';CACHE=ROOT/'Saved/RegionalImageryCache'
MATRIX=ROOT/'_Imports/Geography/RegionalEnvironment/REGION_ENVIRONMENT_MATRIX.json'
FROZEN=OUT/'PROTECTED_GEOGRAPHY_INPUTS.json'
ENDPOINT='https://tiles.maps.eox.at/wms'
POLICY='https://cloudless.eox.at/license-non-commercial'
LICENSE='https://creativecommons.org/licenses/by/4.0/'
ATTRIBUTION='EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017)'
MAX_TOTAL=50*1024*1024;MAX_RESPONSE=2*1024*1024;MAX_DIMENSION=1024
UA='AFTERSHOCK-regional-macro-source/1 (bounded20public2016WMSrequests; no runtime traffic)'

def now():return datetime.now(timezone.utc).isoformat()
def sha(raw):return hashlib.sha256(raw).hexdigest()
def need(value,message):
 if not value:raise ValueError(message)
def write_json(path,value):
 path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n');os.replace(tmp,path)
def actual_bytes():return sum(p.stat().st_size for folder in (CACHE,OUT) if folder.exists() for p in folder.rglob('*') if p.is_file())
def assert_frozen():
 need(FROZEN.is_file(),'Portable protected geography input receipt missing; refusing unverified source mutation scope')
 receipt=json.loads(FROZEN.read_text(encoding='utf-8'))
 need(all(sha((ROOT/r['file']).read_bytes())==r['sha256'] for r in receipt['inventory']+receipt['compiled_sources']),'Frozen geography source was modified')
def download(url,path,limit,offline,log):
 path.parent.mkdir(parents=True,exist_ok=True);sidecar=path.with_suffix(path.suffix+'.source.json')
 if path.exists():
  raw=path.read_bytes();record=json.loads(sidecar.read_text(encoding='utf-8'));need(sha(raw)==record['sha256'] and record['url']==url,'Cached imagery/provenance mismatch');record={**record,'cached':True}
 else:
  need(not offline,'Missing offline imagery cache: '+path.name)
  for attempt in range(1,4):
   try:
    with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':UA}),timeout=45) as response:
     need(response.status==200,'Non200 imagery service response');raw=response.read(limit+1)
     record={'url':url,'retrieved_utc':now(),'bytes':len(raw),'sha256':sha(raw),'content_type':response.headers.get('Content-Type',''),'etag':response.headers.get('ETag'),'last_modified':response.headers.get('Last-Modified'),'cached':False}
    need(len(raw)<=limit,'Response exceeds bounded source cap');need(actual_bytes()+len(raw)*2+65536<=MAX_TOTAL,'50MiB imagery cache+output total cap exceeded')
    tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_bytes(raw);os.replace(tmp,path);write_json(sidecar,record);break
   except (urllib.error.HTTPError,urllib.error.URLError,TimeoutError,ConnectionError) as error:
    log.append({'utc':now(),'event':'request_failed','url':url,'attempt':attempt,'error_type':type(error).__name__})
    if attempt==3:raise
    time.sleep(2*attempt)
 need(len(raw)<=limit,'Cached response exceeds source cap');log.append({'event':'source_response',**record});return raw,record

def provider_plan(offline,log):
 policy,policy_meta=download(POLICY,CACHE/'eox-2016-license-policy.html',MAX_RESPONSE,offline,log)
 plain=' '.join(html.unescape(re.sub(r'<[^>]*>',' ',policy.decode('utf-8'))).split())
 need(re.search(r'For the year 2016.{0,200}?Creative Commons Attribution 4\.0',plain,re.I),'Official policy no longer confirms2016CCBY4; stop, noNC fallback')
 caps_url=ENDPOINT+'?'+urllib.parse.urlencode({'SERVICE':'WMS','VERSION':'1.1.1','REQUEST':'GetCapabilities'})
 raw,caps_meta=download(caps_url,CACHE/'eox-wms-1.1.1-capabilities.xml',MAX_RESPONSE,offline,log)
 tree=ET.fromstring(raw);need(tree.attrib.get('version')=='1.1.1','WMS version/axis-order contract changed')
 parent={child:item for item in tree.iter() for child in item};selected=None
 for preferred in ('s2cloudless-2016','s2cloudless','s2cloudless-2016_3857','s2cloudless_3857'):
  for layer in tree.iter('Layer'):
   if layer.findtext('Name')!=preferred:continue
   abstract=layer.findtext('Abstract') or '';normal=' '.join(abstract.split())
   need(re.search(r'for\s+2016\b',layer.findtext('Title') or '',re.I),'EOX alias no longer advertises2016, no silent futureyear fallback')
   need('2016' in normal and re.search(r'Creative Commons Attribution\s+4\.0|CC\s*BY\s*4\.0',normal,re.I) and not re.search(r'NonCommercial|Non-Commercial|BY-NC',normal,re.I),'2016 advertised layer licence is notCCBY4')
   srs=set();current=layer
   while current is not None:
    for node in current.findall('SRS'):srs.update((node.text or '').split())
    current=parent.get(current)
   if 'EPSG:4326' in srs:selected={'name':preferred,'title':layer.findtext('Title'),'abstract':abstract,'wms_version':'1.1.1','output_crs':'EPSG:4326','source_advertised_srs':sorted(srs)};break
  if selected:break
 need(selected is not None,'No explicitly2016CCBY4layer with4326 support; no futureyear substitution')
 need('image/jpeg' in [n.text for n in tree.findall('./Capability/Request/GetMap/Format')],'WMS JPEG output not advertised')
 return {'provider':'EOX IT Services GmbH','product':'EOxCloudless2016 historical composite','product_year':2016,'source_years':[2016,2017],'license':'CC-BY-4.0','license_url':LICENSE,'required_attribution':ATTRIBUTION,
         'official_license_policy':policy_meta,'wms_capabilities':caps_meta,'layer':selected,'qualification':'2016explicitCCBY4exception.2018andnewerNCcommerciallicences are not used. Historical visually balanced true-colour composite, not current imagery, measured PBR albedo or guaranteed cloud-free pixels.'}

def metres_axes(lat):
 phi=math.radians(lat);e2=.0066943799901413165;a=6378137.;d=1-e2*math.sin(phi)**2
 return a/math.sqrt(d)*math.cos(phi)*math.pi/180.,a*(1-e2)/d**1.5*math.pi/180.
def dimensions(bounds):
 w,s,e,n=bounds;mx,my=metres_axes((s+n)*.5);width=(e-w)*mx;height=(n-s)*my
 need(width>0 and height>0,'Invalid regional source bbox')
 if width>=height:return 1024,max(1,round(1024*height/width))
 return max(1,round(1024*width/height)),1024
def source_image(region,provider,offline,log):
 bounds=region['overview']['bounds_wgs84'];width,height=dimensions(bounds);rid=region['region_id'];need(max(width,height)<=1024,'Dimension cap exceeded')
 query={'SERVICE':'WMS','VERSION':'1.1.1','REQUEST':'GetMap','LAYERS':provider['layer']['name'],'STYLES':'','SRS':'EPSG:4326','BBOX':','.join(format(v,'.15g') for v in bounds),'WIDTH':width,'HEIGHT':height,'FORMAT':'image/jpeg','TRANSPARENT':'FALSE'}
 url=ENDPOINT+'?'+urllib.parse.urlencode(query);cache=CACHE/(rid+'-'+sha(url.encode())[:12]+'.jpg');raw,record=download(url,cache,MAX_RESPONSE,offline,log)
 image=Image.open(io.BytesIO(raw));need(image.format=='JPEG' and image.mode=='RGB' and image.size==(width,height),'Service response is not exact RGB JPEG requested dimensions')
 image.load();variance=ImageStat.Stat(image).var;need(max(variance)>4,'Blank/uninformative imagery tile must not be accepted')
 filename=rid+'-eox2016-truecolour.jpg';target=OUT/filename
 if target.exists():need(target.read_bytes()==raw,'Existing source imagery differs; explicit source upgrade required')
 else:
  need(actual_bytes()+len(raw)+65536<=MAX_TOTAL,'50MiB cap before source image publication');tmp=target.with_suffix('.jpg.tmp');tmp.write_bytes(raw);os.replace(tmp,target)
 w,s,e,n=bounds;mx,my=metres_axes((s+n)*.5)
 return {'region_id':rid,'label':region['label'],'bounds_wgs84':bounds,'filename':filename,'sha256':sha(raw),'bytes':len(raw),'width':width,'height':height,'format':'JPEG','color_space':'sRGB','layer':provider['layer']['name'],'license':'CC-BY-4.0','license_url':LICENSE,'required_attribution':ATTRIBUTION,'source_years':[2016,2017],
         'request':record,'query':query,'pixel_mapping':'EPSG4326 equirectangular; u=(lon-west)/(east-west),v=(north-lat)/(north-south); source pixel centres are half a sample inside bbox',
         'output_ground_metres_per_pixel_at_bbox_centre':[(e-w)*mx/width,(n-s)*my/height],'source_native_resolution_claim':False,'cloud_free_guarantee':False,'current_imagery':False,'pbr_albedo':False,
         'ue_texture_path':'/Game/Environment/Textures/Regional/Macro/T_RegionalMacro_'+rid,'qualification':'Server-clipped/resampled2016/2017 Sentinel composite. Spatially registered macro-colour/pattern reference only; may contain residual cloud, illumination, baked shadows, seasonal seams and historical buildings. Output metres/pixel is not native sensor accuracy. No hazard/landcover/geology derivation.'}

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--offline',action='store_true');parser.add_argument('--plan-only',action='store_true');args=parser.parse_args();log=[];completed=False
 assert_frozen();OUT.mkdir(parents=True,exist_ok=True)
 try:
  source=provider_plan(args.offline,log);write_json(OUT/'LICENSE_VERIFICATION.json',source);print(json.dumps({'licensed_layer':source['layer']['name'],'license':source['license'],'source_years':source['source_years'],'commercial_free_with_attribution':True,'output_dimension_cap':1024,'global_cache_output_cap_bytes':MAX_TOTAL}),flush=True)
  if args.plan_only:completed=True;return
  matrix=json.loads(MATRIX.read_text(encoding='utf-8'));regions=matrix['regions'];need(len(regions)==20 and len({r['region_id'] for r in regions})==20,'Expected20unique region identities')
  accepted={};order=sorted(regions,key=lambda r:(0 if r['region_id']=='etna' else 1 if r['region_id']=='cyprus' else 2 if r['region_id']=='miami_keys' else 3,regions.index(r)))
  for region in order:
   row=source_image(region,source,args.offline,log);accepted[row['region_id']]=row
   manifest={'schema_version':1,'complete':len(accepted)==20,'regions':[accepted[r['region_id']] for r in regions if r['region_id'] in accepted],
             'provider':source,'source_matrix_sha256':sha(MATRIX.read_bytes()),'region_count':len(accepted),'cache_plus_output_bytes':actual_bytes(),'max_cache_plus_output_bytes':MAX_TOTAL,'engine_import_verified':False,'render_visibility_verified':False}
   write_json(OUT/'IMAGERY_MANIFEST.json',manifest);need(actual_bytes()<=MAX_TOTAL,'Global imagery cap exceeded after metadata')
   print(json.dumps({'region':row['region_id'],'ready_source_images':len(accepted),'width':row['width'],'height':row['height'],'bytes':row['bytes'],'sha256':row['sha256'],'output_metres_per_pixel':row['output_ground_metres_per_pixel_at_bbox_centre']}),flush=True)
  completed=True
 finally:
  write_json(OUT/'DOWNLOAD_LOG.json',{'utc':now(),'completed':completed,'scope':'offline2016CCBY4regionalmacroimageryonly; notUErender','requests':log,'cache_plus_output_bytes':actual_bytes(),'max_bytes':MAX_TOTAL});assert_frozen()

if __name__=='__main__':main()
