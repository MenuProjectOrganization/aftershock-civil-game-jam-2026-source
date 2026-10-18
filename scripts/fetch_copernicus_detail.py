"""Replace selected detail DEMs with bounded public Copernicus GLO30 COG windows.

Never edits legacy base65. HTTP Range blocks have a hard global byte budget,
ETag/length checks and independent SHA256 receipts; raw COG blocks stay in Saved.
The resampled result is an edited DSM, NOT a bare-earth DTM or parcel survey.
"""
from __future__ import annotations
import argparse,base64,io,json,math,sys,urllib.request,zlib
from contextlib import ExitStack
from pathlib import Path
import fetch_regional_environment_detail as detail
import fetch_real_geographic_tiles as base

sys.path.insert(0,str(detail.ROOT/'Saved/GeospatialRuntime'))
import rasterio
from rasterio.windows import Window
from rasterio.merge import merge

NOTICE='produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved'
LICENSE='https://dataspace.copernicus.eu/explore-data/data-collections/copernicus-contributing-missions/collections-description/COP-DEM'
BLOCK=262144;MAX_BYTES=30*1024*1024;MAX_REQUESTS=200

class Ranges:
 def __init__(self,offline=False):self.offline=offline;self.blocks={};self.bytes=0;self.requests=0;self.records={};self.tile_list=None;self.tile_list_source=None
 def has_tile(self,url):
  if self.tile_list is None:
   raw,meta=base.Sources(self.offline).request('https://copernicus-dem-30m.s3.amazonaws.com/tileList.txt',detail.CACHE/'copernicus/tileList.txt',4*1024*1024,'copernicus-public-tile-index')
   self.tile_list={line.strip().strip('/') for line in raw.decode('utf-8').splitlines() if line.strip()};self.tile_list_source=meta
  return url.split('/')[-2] in self.tile_list
 def metadata(self,url):
  key=detail.sha(url.encode());folder=detail.CACHE/'copernicus'/key;path=folder/'source.json'
  if path.exists():meta=json.loads(path.read_text(encoding='utf-8'));detail.require(meta['url']==url,'COG cache URL mismatch')
  else:
   detail.require(not self.offline,'Missing COG metadata offline')
   request=urllib.request.Request(url,method='HEAD',headers={'User-Agent':base.USER_AGENT})
   with urllib.request.urlopen(request,timeout=30) as response:
    meta={'url':url,'content_length':int(response.headers['Content-Length']),'etag':response.headers['ETag'],'last_modified':response.headers.get('Last-Modified'),'retrieved_utc':base.now()}
   detail.require(0<meta['content_length']<=100*1024*1024,'Unbounded COG source object');detail.write_json(path,meta)
  self.records.setdefault(url,{**meta,'range_blocks':[]});return folder,meta
 def block(self,url,index):
  key=(url,index)
  if key in self.blocks:return self.blocks[key]
  folder,meta=self.metadata(url);start=index*BLOCK;end=min(start+BLOCK,meta['content_length'])-1
  detail.require(0<=start<=end,'Invalid COG range');size=end-start+1
  detail.require(self.bytes+size<=MAX_BYTES,'COG global30MiB block budget exceeded')
  path=folder/f'{start}-{end}.bin';side=path.with_suffix('.json')
  if path.exists():
   raw=path.read_bytes();receipt=json.loads(side.read_text(encoding='utf-8'));detail.require(detail.sha(raw)==receipt['sha256'] and receipt['etag']==meta['etag'],'COG range cache receipt mismatch')
  else:
   detail.require(not self.offline,'Missing COG range offline');self.requests+=1;detail.require(self.requests<=MAX_REQUESTS,'COG200request budget exceeded')
   request=urllib.request.Request(url,headers={'User-Agent':base.USER_AGENT,'Range':f'bytes={start}-{end}','If-Match':meta['etag']})
   with urllib.request.urlopen(request,timeout=45) as response:
    detail.require(response.status==206 and response.headers.get('Content-Range')==f'bytes {start}-{end}/{meta["content_length"]}' and response.headers.get('ETag')==meta['etag'],'Server did not honour immutable bounded range')
    raw=response.read(size+1)
   detail.require(len(raw)==size,'Incomplete COG range')
   receipt={'start':start,'end':end,'bytes':size,'sha256':detail.sha(raw),'etag':meta['etag'],'retrieved_utc':base.now()}
   path.write_bytes(raw);detail.write_json(side,receipt)
  detail.require(len(raw)==size,'Invalid cached range size');self.bytes+=size;self.blocks[key]=raw;self.records[url]['range_blocks'].append(receipt);return raw
 def opener(self,url):
  self.metadata(url)
  def open_range(name,mode='rb'):
   if name!=url or mode not in ('r','rb'):raise FileNotFoundError(name)
   return RangeFile(self,url)
  return open_range

class RangeFile(io.RawIOBase):
 def __init__(self,owner,url):self.owner=owner;self.url=url;self.position=0;self.length=owner.metadata(url)[1]['content_length']
 def readable(self):return True
 def seekable(self):return True
 def tell(self):return self.position
 def seek(self,offset,whence=0):
  position=offset if whence==0 else self.position+offset if whence==1 else self.length+offset if whence==2 else -1
  if position<0:raise ValueError('Negative COG seek')
  self.position=position;return position
 def read(self,size=-1):
  if size<0:size=self.length-self.position
  if size==0 or self.position>=self.length:return b''
  detail.require(size<=8*1024*1024,'Unbounded COG single read requested');end=min(self.length,self.position+size);chunks=[]
  while self.position<end:
   index=self.position//BLOCK;data=self.owner.block(self.url,index);offset=self.position%BLOCK;take=min(end-self.position,len(data)-offset)
   detail.require(take>0,'COG range reader made no progress');chunks.append(data[offset:offset+take]);self.position+=take
  return b''.join(chunks)
 def readinto(self,buffer):
  raw=self.read(len(buffer));buffer[:len(raw)]=raw;return len(raw)

def tile_url(latitude,longitude):
 ns=('N' if latitude>=0 else 'S')+f'{abs(latitude):02d}_00';ew=('E' if longitude>=0 else 'W')+f'{abs(longitude):03d}_00'
 stem=f'Copernicus_DSM_COG_10_{ns}_{ew}_DEM';return f'https://copernicus-dem-30m.s3.amazonaws.com/{stem}/{stem}.tif'

def sample(owner,bounds,overview_level=None,surface_flags=None):
 np=base.np;w,s,e,n=bounds;lon,lat=np.meshgrid(np.linspace(w,e,257),np.linspace(n,s,257));result=np.full((257,257),np.nan,dtype=np.float64);sources=[]
 if overview_level is not None:
  # Source-built average overview pixels straddle integer-degree tile seams.
  # Merge the adjoining SOURCE grids before interpolation; never patch a gap
  # with an invented/clamped border elevation.
  datasets=[];ocean_absent=[]
  with ExitStack() as stack:
   stack.enter_context(rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN='EMPTY_DIR'))
   for south in range(math.floor(s),math.floor(n)+1):
    for west in range(math.floor(w),math.floor(e)+1):
     url=tile_url(south,west)
     if not owner.has_tile(url):
      target=(lat>=south)&(lat<south+1)&(lon>=west)&(lon<west+1)
      detail.require(surface_flags is not None and np.all((surface_flags.reshape(257,257)[target]&1)!=0),'Absent public COG tile includes mapped land; no invented terrain fallback')
      ocean_absent.append(target);sources.append({'url':url,'absent_from_public_tile_list':True,'mapped_ocean_target_samples':int(np.count_nonzero(target)),'official_ocean_zero_rule':'https://registry.opendata.aws/copernicus-dem/','tile_list_source':owner.tile_list_source});continue
     ds=stack.enter_context(rasterio.open(url,opener=owner.opener(url),OVERVIEW_LEVEL=overview_level))
     detail.require(ds.crs.to_epsg()==4326 and ds.dtypes[0]=='float32' and ds.width*ds.height<=1024*1024,'Unexpected/unbounded COG overview grid')
     datasets.append(ds);sources.append({'url':url,'source_bounds':list(ds.bounds),'transform':list(ds.transform)[:6],'source_width':ds.width,'source_height':ds.height,'source_cog_overview_level':overview_level,'read_window_col_row_width_height':[0,0,ds.width,ds.height],'nodata':ds.nodata})
   detail.require(datasets,'Overview contains no available land source COG');raster,t=merge(datasets);detail.require(raster.nbytes<=16*1024*1024,'Merged source context window exceeds16MiB')
   a=raster[0];x=(lon-t.c)/t.a-.5;y=(lat-t.f)/t.e-.5;ix=np.floor(x).astype(int);iy=np.floor(y).astype(int);fx=x-ix;fy=y-iy
   enclosed=(ix>=0)&(iy>=0)&(ix+1<a.shape[1])&(iy+1<a.shape[0]);ocean=(surface_flags.reshape(257,257)&1)!=0 if surface_flags is not None else np.zeros((257,257),dtype=bool)
   for missing in ocean_absent:enclosed&=~missing
   detail.require(np.all(enclosed|ocean),'Source COG grid does not enclose a land target; no height clamp permitted')
   xx=ix[enclosed];yy=iy[enclosed];u=fx[enclosed];v=fy[enclosed]
   result[enclosed]=a[yy,xx]*(1-u)*(1-v)+a[yy,xx+1]*u*(1-v)+a[yy+1,xx]*(1-u)*v+a[yy+1,xx+1]*u*v
   result[~enclosed]=0.0
   if np.any(~enclosed):sources.append({'mapped_ocean_edge_or_absent_target_samples':int(np.count_nonzero(~enclosed)),'elevation_m':0,'official_ocean_zero_rule':'https://registry.opendata.aws/copernicus-dem/','qualification':'Only explicit NaturalEarth-ocean targets outside averaged COG pixel-centre coverage; never applied to land.'})
   detail.require(np.all(np.isfinite(result)) and result.min()>-12000 and result.max()<9000,'Invalid context source mosaic')
   return result.astype('<f4'),sources
 for south in range(math.floor(s),math.floor(n)+1):
  for west in range(math.floor(w),math.floor(e)+1):
   url=tile_url(south,west)
   with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN='EMPTY_DIR'):
    with rasterio.open(url,opener=owner.opener(url),**({'OVERVIEW_LEVEL':overview_level} if overview_level is not None else {})) as dataset:
     detail.require(dataset.crs.to_epsg()==4326 and dataset.count==1 and dataset.dtypes[0]=='float32','Unexpected GLO30 COG geometry/type')
     t=dataset.transform;detail.require(t.b==0 and t.d==0 and t.a>0 and t.e<0,'Rotated/unexpected COG')
     col=(lon-t.c)/t.a-.5;row=(lat-t.f)/t.e-.5
     eligible=(col>=0)&(row>=0)&(col<=dataset.width-1)&(row<=dataset.height-1)&~np.isfinite(result)
     if not np.any(eligible):continue
     c0=max(0,int(np.floor(col[eligible].min())));r0=max(0,int(np.floor(row[eligible].min())))
     c1=min(dataset.width,int(np.floor(col[eligible].max()))+2);r1=min(dataset.height,int(np.floor(row[eligible].max()))+2)
     values=dataset.read(1,window=Window(c0,r0,c1-c0,r1-r0),masked=True)
     detail.require(not np.any(np.ma.getmaskarray(values)) and np.all(np.isfinite(values)),'NoData in requested Copernicus land window')
     array=np.asarray(values,dtype=np.float64);x=col[eligible]-c0;y=row[eligible]-r0;ix=np.floor(x).astype(int);iy=np.floor(y).astype(int);fx=x-ix;fy=y-iy
     detail.require(np.all(ix+1<array.shape[1]) and np.all(iy+1<array.shape[0]),'Source boundary lacks bilinear neighbour; require adjacent source window')
     result[eligible]=(array[iy,ix]*(1-fx)*(1-fy)+array[iy,ix+1]*fx*(1-fy)+array[iy+1,ix]*(1-fx)*fy+array[iy+1,ix+1]*fx*fy)
     sources.append({'url':url,'source_bounds':list(dataset.bounds),'transform':list(t)[:6],'source_width':dataset.width,'source_height':dataset.height,'source_cog_overview_level':overview_level,'read_window_col_row_width_height':[c0,r0,c1-c0,r1-r0],'nodata':dataset.nodata})
 detail.require(np.all(np.isfinite(result)) and result.min()>-12000 and result.max()<9000,'Incomplete/invalid Copernicus source coverage')
 return result.astype('<f4'),sources

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--regions',nargs='*',default=['valdivia','sendai','jakarta','rotterdam']);parser.add_argument('--overview-regions',nargs='*',default=[]);parser.add_argument('--offline',action='store_true');args=parser.parse_args()
 catalog=json.loads(base.CATALOG.read_text(encoding='utf-8'));flat=[(r,f) for r in catalog['regions'] for f in r['focuses']]
 detail.require(set(args.regions+args.overview_regions)<={r['id'] for r in catalog['regions']},'Unknown source region');old_hashes={p.name:detail.sha(p.read_bytes()) for p in base.OUT.glob('*') if p.is_file()};owner=Ranges(args.offline);completed=False;failure=None
 try:
  for region,focus in flat:
   if region['id'] not in args.regions:continue
   key=region['id']+'_'+focus['id'];path=detail.OUT/(key+'-detail.json');p=json.loads(path.read_text(encoding='utf-8'))
   if 'preferred_copernicus_source' in p:print(json.dumps({'focus':key,'already_upgraded':True}),flush=True);continue
   raw=zlib.decompress(base64.b64decode(p['zlib_payload_base64']));detail.require(detail.sha(raw)==p['uncompressed_sha256'],'Invalid pre-upgrade packet')
   history=detail.CACHE/'detail-history'/(key+'-'+detail.sha(path.read_bytes())+'.json');history.parent.mkdir(parents=True,exist_ok=True);history.write_bytes(path.read_bytes())
   heights,sources=sample(owner,p['bounds_wgs84']);newraw=heights.tobytes()+raw[257*257*4:];packed=zlib.compress(newraw,9)
   p['source_upgrade']={'previous_packet_sha256':detail.sha(path.read_bytes()),'previous_elevation_min_m':p['dem_min_m'],'previous_elevation_max_m':p['dem_max_m'],'reason':'Use independent hydrologically edited GLO30 surface; Mapzen water-depth artefacts are not terrain.'}
   p['preferred_copernicus_source']={'provider':'Copernicus GLO30 Public COG, 2021 release','registry':'https://registry.opendata.aws/copernicus-dem/','readme':'https://copernicus-dem-30m.s3.amazonaws.com/readme.html','license_url':LICENSE,'mandatory_modified_data_notice':NOTICE,'vertical_datum':'EGM2008 EPSG3855 orthometric metres','horizontal_crs':'WGS84 EPSG4326','source_windows':sources,'interpolation':'bilinear at exact detail bbox grid centres; no elevation clamp/normalization','sampled_float32_sha256':detail.sha(heights.tobytes()),'qualification':'Hydrologically edited DSM includes buildings/infrastructure/vegetation; not bare earth, cadastral survey or current crater surface.'}
   p['vertical_datum']='Copernicus GLO30 EGM2008 orthometric metres (EPSG3855); WGS84 horizontal. Surface DSM, not ellipsoid or parcel survey.';p['source_licenses']['preferred_copernicus']=NOTICE+'; '+LICENSE
   p.update(dem_min_m=float(heights.min()),dem_max_m=float(heights.max()),uncompressed_sha256=detail.sha(newraw),compressed_sha256=detail.sha(packed),zlib_payload_base64=base64.b64encode(packed).decode('ascii'))
   p['qualification']+=' Preferred Copernicus GLO30 elevation replaces Mapzen for this focus; all OSM masks/distances remain unchanged.'
   detail.write_json(path,p);print(json.dumps({'focus':key,'copernicus_ready':True,'min_m':p['dem_min_m'],'max_m':p['dem_max_m'],'range_bytes_so_far':owner.bytes}),flush=True)
  for key in args.overview_regions:
   path=detail.OUT/(key+'-overview.json');p=json.loads(path.read_text(encoding='utf-8'))
   if 'preferred_copernicus_source' in p:print(json.dumps({'region':key,'overview_already_upgraded':True}),flush=True);continue
   raw=zlib.decompress(base64.b64decode(p['zlib_payload_base64']));detail.require(detail.sha(raw)==p['uncompressed_sha256'],'Invalid overview packet before COG source replacement')
   history=detail.CACHE/'detail-history'/(key+'-overview-'+detail.sha(path.read_bytes())+'.json');history.parent.mkdir(parents=True,exist_ok=True);history.write_bytes(path.read_bytes())
   surface=base.np.frombuffer(raw,dtype=base.np.uint8,offset=257*257*4)
   heights,sources=sample(owner,p['bounds_wgs84'],overview_level=2,surface_flags=surface);newraw=heights.tobytes()+raw[257*257*4:];packed=zlib.compress(newraw,9)
   p['source_upgrade']={'previous_packet_sha256':detail.sha(path.read_bytes()),'previous_elevation_min_m':p['dem_min_m'],'previous_elevation_max_m':p['dem_max_m'],'reason':'Independent source-built Copernicus COG overview replaces gross Mapzen context spikes; no invented/clamped terrain.'}
   p['preferred_copernicus_source']={'provider':'Copernicus GLO30 Public COG,2021release; source-built average overview_level2','registry':'https://registry.opendata.aws/copernicus-dem/','license_url':LICENSE,'mandatory_modified_data_notice':NOTICE,'vertical_datum':'EGM2008 EPSG3855 orthometric metres','horizontal_crs':'WGS84 EPSG4326','source_windows':sources,'sampled_float32_sha256':detail.sha(heights.tobytes()),'qualification':'Wider visual context only. Source built average-downsampled overview, approximately8arcsec latitude (not30m detail). Edited DSM, not bare-earth DTM; NaturalEarth mask remains generalized.'}
   p.update(dem_min_m=float(heights.min()),dem_max_m=float(heights.max()),uncompressed_sha256=detail.sha(newraw),compressed_sha256=detail.sha(packed),zlib_payload_base64=base64.b64encode(packed).decode('ascii'))
   detail.write_json(path,p);print(json.dumps({'region':key,'copernicus_overview_ready':True,'min_m':p['dem_min_m'],'max_m':p['dem_max_m'],'range_bytes_so_far':owner.bytes}),flush=True)
  completed=True
 except Exception as error:
  failure={'exception_type':type(error).__name__,'message':str(error).replace(str(detail.ROOT),'<repo>')};raise
 finally:
  detail.require(all(detail.sha((base.OUT/k).read_bytes())==v for k,v in old_hashes.items()),'Legacy source altered')
  log=detail.OUT/'COPERNICUS_RANGE_SOURCE_LOG.json';history=json.loads(log.read_text(encoding='utf-8')) if log.exists() else {'runs':[]}
  if 'runs' not in history:history={'runs':[history]}
  history['runs'].append({'utc':base.now(),'completion_status':'complete' if completed else 'failed','failure':failure,'requested_detail_regions':args.regions,'requested_overview_regions':args.overview_regions,'source_byte_cap':MAX_BYTES,'unique_range_bytes':owner.bytes,'http_range_requests':owner.requests,'source_objects':list(owner.records.values()),'license_url':LICENSE,'mandatory_modified_data_notice':NOTICE,'legacy_base65_unchanged':True});detail.write_json(log,history)
 if args.regions:detail.generate_native(flat)
 if args.overview_regions:
  import fetch_regional_overview as overview
  overview.generate_native(catalog['regions'])

if __name__=='__main__':main()
