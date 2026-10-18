"""Compile all25 bounded detail rasters and NASA POWER monthly climatologies.

Preserves every legacy/base65 packet. Detail sources use the SAME WGS84 bbox,
real metres, cached Mapzen Terrarium source pixels, USGS bare-earth for US sites,
and OSM's recorded intermittent/seasonal tags. No invented height/river/climate.
"""
from __future__ import annotations
import argparse,base64,hashlib,io,json,math,os,re,sys,time,urllib.parse,zlib
from pathlib import Path
import fetch_real_geographic_tiles as base

np=base.np;Image=base.Image;shapely=base.shapely
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'_Imports/Geography/RegionalEnvironment'
CACHE=ROOT/'Saved/GeographySourceCache'
NATIVE=ROOT/'Source/JamProject/Aftershock/Private/AftershockOfflineGeographicDetail.inl'
CLIMATE_NATIVE=ROOT/'Source/JamProject/Aftershock/Private/AftershockOfflineMonthlyClimate.inl'
SIDE=257;MAX_RAW=32*1024*1024
POWER='https://power.larc.nasa.gov/api/temporal/climatology/point'
MONTHS=('JAN','FEB','MAR','APR','MAY','JUN','JUL','AUG','SEP','OCT','NOV','DEC')
PARAMETERS={'T2M':('C',-95,65),'PRECTOTCORR':('mm/day',0,80),'RH2M':('%',0,100),'WS10M':('m/s',0,80)}

def sha(raw):return hashlib.sha256(raw).hexdigest()
def require(ok,message):
 if not ok:raise ValueError(message)
def write_json(path,value):
 path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix(path.suffix+'.tmp')
 tmp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n');os.replace(tmp,path)
def detail_heights(source,focus):
 """Vectorized sampling, not artificial detail beyond the z12 source grid."""
 w,s,e,n=base.bounds(focus);lon,lat=np.meshgrid(np.linspace(w,e,SIDE),np.linspace(n,s,SIDE))
 tx=(lon+180)/360*(2**base.ZOOM);ty=(1-np.arcsinh(np.tan(np.radians(lat)))/math.pi)*.5*(2**base.ZOOM)
 px=tx*256-.5;py=ty*256-.5;ix=np.floor(px).astype(np.int64);iy=np.floor(py).astype(np.int64);fx=px-ix;fy=py-iy
 result=np.zeros((SIDE,SIDE),dtype=np.float64);used={}
 for dx,dy in ((0,0),(1,0),(0,1),(1,1)):
  gx=ix+dx;gy=iy+dy;xx=gx//256;yy=gy//256
  weight=(fx if dx else 1-fx)*(fy if dy else 1-fy)
  for x,y in np.unique(np.column_stack((xx.ravel(),yy.ravel())),axis=0):
   key=(base.ZOOM,int(x),int(y));image,meta=source.image(key);used[key]=meta;mask=(xx==x)&(yy==y)
   rgb=image[gy[mask]%256,gx[mask]%256].astype(np.float64)
   result[mask]+=(rgb[:,0]*256+rgb[:,1]+rgb[:,2]/256-32768)*weight[mask]
 require(np.all(np.isfinite(result)) and np.min(result)>-12000 and np.max(result)<9000,'Invalid real source elevation')
 return result.astype('<f4'),[{'z':k[0],'x':k[1],'y':k[2],**v} for k,v in sorted(used.items())]

def seasonal_surface(old_packet,focus):
 """Only explicit source tags change; unknown flow permanence stays unknown."""
 raw=zlib.decompress(base64.b64decode(old_packet['zlib_payload_base64']))
 flags=np.frombuffer(raw,dtype=np.uint8,count=SIDE*SIDE,offset=65*65*4).copy()
 river=raw[65*65*4+SIDE*SIDE:65*65*4+SIDE*SIDE*3]
 coast=raw[65*65*4+SIDE*SIDE*3:]
 osm=CACHE/f"osm/{old_packet['region_id']}_{old_packet['focus_id']}-{old_packet['osm_source']['query_sha256'][:12]}.json"
 osm_raw=osm.read_bytes();require(sha(osm_raw)==old_packet['osm_source']['sha256'],'OSM cache byte provenance mismatch')
 document=json.loads(osm_raw);dry=[];wet=[];examples=[];lon0=focus['longitude'];lat0=focus['latitude'];mx,my=base.axes(lat0)
 for el in document['elements']:
  tags=el.get('tags',{})
  if el['type']!='way' or tags.get('waterway') not in ('river','stream','canal','drain'):continue
  coords=[((p['lon']-lon0)*mx,(p['lat']-lat0)*my) for p in el.get('geometry',[]) if 'lon' in p and 'lat' in p]
  if len(coords)<2:continue
  intermittent=tags.get('intermittent','').lower() in ('yes','true','1') or tags.get('seasonal','').lower() not in ('','no','false','0')
  (dry if intermittent else wet).append(base.LineString(coords))
  if intermittent:examples.append({'osm_id':el['id'],'intermittent':tags.get('intermittent'),'seasonal':tags.get('seasonal'),'waterway':tags.get('waterway')})
 if dry:
  w,s,e,n=base.bounds(focus);xs=np.linspace((w-lon0)*mx,(e-lon0)*mx,SIDE);ys=np.linspace((n-lat0)*my,(s-lat0)*my,SIDE)
  x,y=np.meshgrid(xs,ys);points=shapely.points(np.column_stack((x.ravel(),y.ravel())));radius=math.hypot(xs[1]-xs[0],ys[1]-ys[0])*.5
  is_dry=shapely.distance(base.unary_union(dry),points)<=radius
  is_wet=shapely.distance(base.unary_union(wet),points)<=radius if wet else np.zeros(SIDE*SIDE,dtype=bool)
  land=(flags&1)==0;flags[is_dry&land]|=32
  flags[is_dry&~is_wet&land]&=np.uint8(239) # remove only the old centreline16 bit, keep actual water polygons2.
 return flags,river,coast,{'explicit_intermittent_ways':len(dry),'unspecified_or_perennial_ways':len(wet),'intermittent_source_examples':examples,'intermittent_pixels':int(np.count_nonzero(flags&32))}

def usgs_heights(source,focus,key,baseline,flags):
 w,s,e,n=base.bounds(focus);dx=(e-w)/(SIDE-1);dy=(n-s)/(SIDE-1)
 query={'bbox':','.join(map(str,(w-dx*.5,s-dy*.5,e+dx*.5,n+dy*.5))),'bboxSR':4326,'imageSR':4326,'size':f'{SIDE},{SIDE}',
        'format':'tiff','pixelType':'F32','interpolation':'RSP_BilinearInterpolation','noData':-999999,'f':'image','adjustAspectRatio':'false','renderingRule':json.dumps({'rasterFunction':'None'},separators=(',',':'))}
 url=base.USGS_EXPORT+'?'+urllib.parse.urlencode(query)
 raw,record=source.request(url,CACHE/f'usgs/{key}-detail-{sha(url.encode())[:12]}.tif',4*1024*1024,'usgs-bare-earth-detail')
 image=Image.open(io.BytesIO(raw));require(image.size==(SIDE,SIDE) and image.mode=='F','Invalid USGS257 F32 raster')
 scale=image.tag_v2.get(33550);tie=image.tag_v2.get(33922);geokeys=image.tag_v2.get(34735)
 require(scale and tie and geokeys and len(scale)>=2 and len(tie)>=6,'USGS257 georeferencing missing')
 require(abs(scale[0]-dx)<1e-11 and abs(scale[1]-dy)<1e-11 and abs(tie[3]-(w-dx*.5))<1e-10 and abs(tie[4]-(n+dy*.5))<1e-10,'USGS257 source moved/rescaled sample centres')
 keys={geokeys[i]:tuple(geokeys[i+1:i+4]) for i in range(4,len(geokeys),4)}
 require(keys.get(2048)==(0,1,4326),'USGS257 source is not EPSG4326')
 values=np.asarray(image,dtype='<f4');valid=np.isfinite(values)&(values>-12000)&(values<9000)
 ocean=(flags.reshape(SIDE,SIDE)&1)!=0
 require(not np.any(~valid&~ocean),'USGS NoData on land: cannot replace with mixed DSM')
 merged=np.where(valid,values,baseline).astype('<f4')
 return merged,{**record,'provider':'USGS 3DEP Bare Earth DEM Dynamic Service','query':query,'usgs_valid_samples':int(np.count_nonzero(valid)),
                'explicit_osm_ocean_mapzen_fallback_samples':int(np.count_nonzero(~valid)),'land_fallback_samples':0,
                'license_url':'https://www.usgs.gov/3d-elevation-program/about-3dep-products-services'}

def climate(source,region,focus):
 query={'parameters':','.join(PARAMETERS),'community':'AG','longitude':focus['longitude'],'latitude':focus['latitude'],'format':'JSON'}
 url=POWER+'?'+urllib.parse.urlencode(query)
 raw,record=source.request(url,CACHE/f"power/{region['id']}_{focus['id']}-{sha(url.encode())[:12]}.json",128*1024,'nasa-power-climatology',max_attempts=3)
 j=json.loads(raw);p=j['properties']['parameter'];require(j.get('geometry',{}).get('type')=='Point','POWER response is not a point')
 # POWER echoes coordinates to three decimals; preserve the exact requested
 # anchor and the rounded response separately, never relabel a climate grid.
 coords=j['geometry']['coordinates'];require(abs(coords[0]-focus['longitude'])<=.000500001 and abs(coords[1]-focus['latitude'])<=.000500001 and math.isfinite(coords[2]),'POWER location mismatch')
 for key,(units,lo,hi) in PARAMETERS.items():
  require(j['parameters'][key]['units']==units,'POWER units mismatch')
  require(all(type(p[key].get(m)) in (float,int) and math.isfinite(p[key][m]) and lo<=p[key][m]<=hi for m in MONTHS),'POWER missing/fill/out-of-range monthly value')
 period=j['header']['range'];years=re.findall(r'\b(?:19|20)\d{2}\b',period);require(len(years)==2,'POWER period not explicit')
 return {'schema_version':1,'region_id':region['id'],'focus_id':focus['id'],'source':record,'api_header':j['header'],
         'requested_lat_lon':[focus['latitude'],focus['longitude']],'provider_geometry':j['geometry'],
         'source_grid_degrees_lat_lon':[.5,.625],'source_grid_elevation_metres':coords[2],'start_year':int(years[0]),'end_year':int(years[1]),
         'parameters':j['parameters'],'monthly':[{key:p[key][m] for key in PARAMETERS} for m in MONTHS],
         'qualification':'NASA POWER/MERRA2 spatial-grid monthly climatology; not a station, forecast, eruption predictor or local terrain microclimate. Monthly mean precipitation is mm/day, not storm intensity.',
         'methodology_url':'https://power.larc.nasa.gov/docs/methodology/meteorology/','citation_url':'https://power.larc.nasa.gov/docs/methodology/citations/'}

def generate_native(flat):
 arrays=[];rows=[];climate_rows=[];raw_total=0
 for region,focus in flat:
  key=region['id']+'_'+focus['id'];p=OUT/(key+'-detail.json');c=OUT/(key+'-climate.json')
  if p.exists():
   packet=json.loads(p.read_text(encoding='utf-8'));packed=base64.b64decode(packet['zlib_payload_base64'],validate=True);raw=zlib.decompress(packed)
   require(sha(raw)==packet['uncompressed_sha256'] and sha(packed)==packet['compressed_sha256'] and len(raw)==SIDE*SIDE*9,'Invalid detail packet')
   raw_total+=len(raw);name='DetailTile_'+key
   encoded=base64.b64encode(packed).decode('ascii');chunks=[encoded[i:i+8192] for i in range(0,len(encoded),8192)]
   arrays.append('static constexpr const ANSICHAR* '+name+'[] = {\n'+',\n'.join('"'+chunk+'"' for chunk in chunks)+'\n};\n')
   rows.append('{ '+', '.join([*(format(v,'.15g') for v in packet['bounds_wgs84']),name,str(len(chunks)),str(len(packed)),str(len(raw)),'true' if packet['has_coastline_source'] else 'false'])+' }')
  else:rows.append('{}')
  if c.exists():
   packet=json.loads(c.read_text(encoding='utf-8'));month_rows=['{'+','.join(format(m[k],'.9g')+'f' if isinstance(m[k],float) and not float(m[k]).is_integer() else format(float(m[k]),'.1f')+'f' for k in PARAMETERS)+'}' for m in packet['monthly']]
   climate_rows.append('{ true, '+', '.join([format(packet['source_grid_elevation_metres'],'.6f')+'f',str(packet['start_year']),str(packet['end_year'])])+', { '+', '.join(month_rows)+' } }')
  else:climate_rows.append('{}')
 require(raw_total<=MAX_RAW,'Detail decoded source exceeds32MiB cap')
 for path,body in ((NATIVE,'// GENERATED bounded8192-character Base64 chunks; source detail only, base65 byte unchanged.\n'+''.join(arrays)+'static const FCompiledGeographicDetailTile CompiledGeographicDetailTiles[] = {\n'+',\n'.join(rows)+'\n};\n'),
                   (CLIMATE_NATIVE,'// GENERATED NASA POWER/MERRA2 monthly climatologies, no weather/hazard forecast.\nstatic const FCompiledMonthlyClimate CompiledMonthlyClimates[] = {\n'+',\n'.join(climate_rows)+'\n};\n')):
  tmp=path.with_suffix(path.suffix+'.tmp');tmp.write_text(body,encoding='utf-8',newline='\n');os.replace(tmp,path)

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--offline',action='store_true');parser.add_argument('--compile-only',action='store_true');parser.add_argument('--upgrade-usgs',action='store_true');args=parser.parse_args()
 catalog=json.loads(base.CATALOG.read_text(encoding='utf-8'));flat=[(r,f) for r in catalog['regions'] for f in r['focuses']]
 require(len(flat)==25 and len(catalog['regions'])==20,'Expected all20 regions/all25 focuses')
 if args.compile_only:generate_native(flat);return
 source=base.Sources(args.offline);OUT.mkdir(parents=True,exist_ok=True);base_hashes={p.name:sha(p.read_bytes()) for p in base.OUT.glob('*') if p.is_file()}
 print(json.dumps({'begin_utc':base.now(),'focuses':25,'detail_dem_side':SIDE,'surface_side':SIDE,'maximum_decoded_bytes':MAX_RAW,'legacy_base65_unchanged':True}),flush=True)
 try:
  for region,focus in flat:
   key=region['id']+'_'+focus['id'];packet_path=OUT/(key+'-detail.json');climate_path=OUT/(key+'-climate.json')
   if args.upgrade_usgs and region['id'] in base.BARE_EARTH_REGIONS and packet_path.exists():
    current=json.loads(packet_path.read_text(encoding='utf-8'))
    if current.get('preferred_bare_earth_source',{}).get('query',{}).get('adjustAspectRatio')!='false':
     raw=zlib.decompress(base64.b64decode(current['zlib_payload_base64']));require(sha(raw)==current['uncompressed_sha256'],'USGS detail packet corrupt before source alignment correction')
     history=CACHE/'detail-history'/(key+'-'+sha(packet_path.read_bytes())+'.json');history.parent.mkdir(parents=True,exist_ok=True);history.write_bytes(packet_path.read_bytes())
     flags=np.frombuffer(raw,dtype=np.uint8,count=SIDE*SIDE,offset=SIDE*SIDE*4);baseline,_=detail_heights(source,focus)
     height,usgs=usgs_heights(source,focus,key,baseline,flags);newraw=height.tobytes()+raw[SIDE*SIDE*4:];packed=zlib.compress(newraw,9)
     current['source_alignment_correction']={'previous_packet_sha256':sha(packet_path.read_bytes()),'previous_source_sha256':current['preferred_bare_earth_source']['sha256'],'reason':'Disable ArcGIS automatic bbox aspect-ratio adjustment; validate exact EPSG4326 pixel centre transform. Never alter source grid labels to fit a response.'}
     current.update(preferred_bare_earth_source=usgs,dem_min_m=float(height.min()),dem_max_m=float(height.max()),uncompressed_sha256=sha(newraw),compressed_sha256=sha(packed),zlib_payload_base64=base64.b64encode(packed).decode('ascii'));write_json(packet_path,current)
   if not packet_path.exists():
    old=json.loads((base.OUT/(key+'.json')).read_text(encoding='utf-8'));height,dem_sources=detail_heights(source,focus)
    flags,river,coast,intermittent=seasonal_surface(old,focus);usgs=None
    if region['id'] in base.BARE_EARTH_REGIONS:height,usgs=usgs_heights(source,focus,key,height,flags)
    raw=height.tobytes()+flags.tobytes()+river+coast;packed=zlib.compress(raw,9)
    packet={'schema_version':1,'region_id':region['id'],'focus_id':focus['id'],'compiled_utc':base.now(),'bounds_wgs84':old['bounds_wgs84'],
            'anchor_latitude':focus['latitude'],'anchor_longitude':focus['longitude'],'dem_side':SIDE,'surface_side':SIDE,
            'base_packet_sha256':sha((base.OUT/(key+'.json')).read_bytes()),'has_coastline_source':old['has_coastline_source'],
            'dem_min_m':float(np.min(height)),'dem_max_m':float(np.max(height)),'dem_sources':dem_sources,'osm_source':old['osm_source'],
            'intermittent_mapping':intermittent,'vertical_datum':old['vertical_datum'],'source_licenses':old['source_licenses'],
            'qualification':'Same bounded focus bbox, source MSL metres, no height exaggeration or invented mountains.257 samples preserve more of source z12 DEM (~19-37m latitude-dependent nominal pixels); output sample spacing is not claimed source resolution. OSM untagged flow permanence stays unknown; dry-tagged channels are bit32, not automatic perennial water. No per-cell soil survey or hazard equations.',
            'layout':'little-endian float32 DEM[257*257], uint8 flags[257*257], uint16 river-distance dm[257*257], int16 signed-coast-distance dm[257*257]',
            'mask_bits':{'ocean':1,'inland_water':2,'mapped_forest':4,'mapped_agriculture':8,'waterway_unspecified_or_perennial':16,'mapped_intermittent_waterway':32},
            'uncompressed_sha256':sha(raw),'compressed_sha256':sha(packed),'zlib_payload_base64':base64.b64encode(packed).decode('ascii')}
    if usgs:packet['preferred_bare_earth_source']=usgs
    write_json(packet_path,packet)
   if not climate_path.exists() or 'provider_geometry' not in json.loads(climate_path.read_text(encoding='utf-8')):write_json(climate_path,climate(source,region,focus))
   p=json.loads(packet_path.read_text(encoding='utf-8'));print(json.dumps({'focus':key,'ready':True,'min_m':p['dem_min_m'],'max_m':p['dem_max_m'],'intermittent_pixels':p['intermittent_mapping']['intermittent_pixels']}),flush=True)
   generate_native(flat)
 finally:
  require(all(sha((base.OUT/name).read_bytes())==value for name,value in base_hashes.items()),'Base65 source modified')
  log_path=OUT/'DETAIL_DOWNLOAD_LOG.json';history=json.loads(log_path.read_text(encoding='utf-8')) if log_path.exists() else {'runs':[]}
  history['runs'].append({'utc':base.now(),'dem_tiles':len(source.dem_keys),'dem_bytes':source.dem_bytes,'requests':source.log,'base_source_hashes_unchanged':base_hashes})
  write_json(log_path,history)
 generate_native(flat);print(json.dumps({'complete':True,'detail_focuses':25,'climate_focuses':25,'native_sha256':sha(NATIVE.read_bytes()),'climate_native_sha256':sha(CLIMATE_NATIVE.read_bytes())}),flush=True)

if __name__=='__main__':main()
