"""All20 bounded regional context datasets; no construction/physics fallback.

Terrarium z9 DEM, Natural Earth public-domain1:10M land/lakes/rivers.
The cartographic scale is NOT10-metre resolution. Exact focus OSM/detail takes
precedence inside its own source bbox; generalized context is not a street map.
"""
from __future__ import annotations
import argparse,base64,io,json,os,zipfile,zlib
from pathlib import Path
import fetch_regional_environment_detail as detail
import fetch_real_geographic_tiles as base
import shapefile
from shapely.geometry import shape,box,mapping

ROOT=detail.ROOT;OUT=detail.OUT;CACHE=detail.CACHE
NATIVE=ROOT/'Source/JamProject/Aftershock/Private/AftershockOfflineRegionalOverview.inl'
BOUNDS={
 'atakent':(28.25,40.65,29.4,41.45),'dereyolu':(39.7,40.3,40.6,41.2),'yelova':(35.8,36.1,36.6,36.9),
 'kiyikent':(35.5,35.7,36.6,36.6),'mugla':(27.7,36.45,28.9,37.2),'etna':(14.65,37.35,15.4,38.0),
 'konya':(33.1,37.35,34.,38.1),'miami_keys':(-82.3,24.3,-79.9,26.1),'cyprus':(32.15,34.5,34.65,35.75),
 'yakutsk':(128.8,61.65,130.6,62.5),'valparaiso':(-72.,-33.5,-71.,-32.6),'valdivia':(-73.65,-40.15,-72.5,-39.35),
 'sendai':(140.5,37.95,141.4,38.65),'rotterdam':(3.5,51.4,5.,52.2),'new_orleans':(-90.65,29.35,-89.3,30.5),
 'grindavik':(-23.,63.65,-21.65,64.25),'vesuvius':(13.85,40.5,14.8,41.1),
 'christchurch':(172.15,-43.95,173.2,-43.15),'kathmandu':(84.85,27.35,85.9,28.15),'jakarta':(106.4,-6.75,107.2,-5.85)}
FEATURES={
 'atakent':'Marmara Sea, Küçükçekmece lake, Bosphorus and regional hills','dereyolu':'Black Sea coastal valleys and Pontic mountain relief',
 'yelova':'Amanos mountain spine, Belen pass and Iskenderun bay','kiyikent':'Asi delta, Samandağ coast and Kızıldağ/Keldağ relief',
 'mugla':'Marmaris mountain peninsula, forest-country relief and Aegean bays','etna':'Etna cone/summit and Catania–Ionian coast',
 'konya':'Karapınar basin, volcanic uplands and closed drainage landscape','miami_keys':'Miami–Everglades–Florida Keys corridor, shallow maritime island chain',
 'cyprus':'Whole Cyprus island, Troodos and Pentadaktylos ranges, Mesaoria lowland','yakutsk':'Yakutsk reach of Lena, river islands and neighbouring plateau; not Arctic delta',
 'valparaiso':'Pacific coast and Valparaíso coastal hills','valdivia':'Valdivia estuary/river basin and Pacific coastal range',
 'sendai':'Sendai coastal plain, hills and Pacific coastline','rotterdam':'Rhine–Meuse delta, estuary arms and North Sea coastline',
 'new_orleans':'Mississippi delta reach, Pontchartrain lake and coastal wetlands','grindavik':'Reykjanes volcanic peninsula, lava country and coastline',
 'vesuvius':'Somma–Vesuvius cone, Naples bay and surrounding coastal relief','christchurch':'Canterbury plain and Banks Peninsula/Port Hills volcanic relief',
 'kathmandu':'Kathmandu basin, enclosing ridges and Himalayan foothills','jakarta':'Jakarta Bay coastal plain, river catchment and Bogor–Salak uplands'}

def focus_for(bounds):
 w,s,e,n=bounds;return {'longitude':(w+e)/2,'latitude':(s+n)/2,'half_longitude_extent':(e-w)/2,'half_latitude_extent':(n-s)/2}
def fetch_layers(source):
 layers={};records={}
 for name in ('land','lakes','rivers_lake_centerlines'):
  url=f'https://naturalearth.s3.amazonaws.com/10m_physical/ne_10m_{name}.zip'
  raw,meta=source.request(url,CACHE/f'natural-earth/ne_10m_{name}.zip',8*1024*1024,'natural-earth-public-domain')
  archive=zipfile.ZipFile(io.BytesIO(raw));stem=f'ne_10m_{name}'
  require=detail.require;require(sum(i.file_size for i in archive.infolist())<=64*1024*1024,'Natural Earth archive expands beyond bounded budget')
  layers[name]=shapefile.Reader(shp=io.BytesIO(archive.read(stem+'.shp')),shx=io.BytesIO(archive.read(stem+'.shx')),dbf=io.BytesIO(archive.read(stem+'.dbf')))
  records[name]={**meta,'license':'Public domain','terms_url':'https://www.naturalearthdata.com/about/terms-of-use/','cartographic_scale':'1:10000000; not10m raster resolution'}
 return layers,records
def source_shapes(reader,bounds):
 clip=box(*bounds);out=[]
 for item in reader.iterShapes(bbox=bounds):
  value=base.shapely.make_valid(shape(item.__geo_interface__)).intersection(clip)
  if not value.is_empty:out.append(value)
 return out
def generate_native(regions):
 arrays=[];rows=[]
 for region in regions:
  path=OUT/(region['id']+'-overview.json')
  if not path.exists():rows.append('{}');continue
  p=json.loads(path.read_text(encoding='utf-8'));packed=base64.b64decode(p['zlib_payload_base64']);raw=zlib.decompress(packed)
  detail.require(len(raw)==257*257*5 and detail.sha(raw)==p['uncompressed_sha256'] and detail.sha(packed)==p['compressed_sha256'],'Corrupt overview source packet')
  encoded=base64.b64encode(packed).decode('ascii');chunks=[encoded[i:i+8192] for i in range(0,len(encoded),8192)];name='Overview_'+region['id']
  arrays.append('static constexpr const ANSICHAR* '+name+'[] = {\n'+',\n'.join('"'+v+'"' for v in chunks)+'\n};\n')
  rows.append('{ '+', '.join([*(format(v,'.15g') for v in p['bounds_wgs84']),name,str(len(chunks)),str(len(packed)),str(len(raw)),'true'])+' }')
 body='// GENERATED boundedregionalcontext; NaturalEarth1:10M scale is not10metre resolution.\n'+''.join(arrays)+'static const FCompiledGeographicDetailTile CompiledRegionalOverviewTiles[] = {\n'+',\n'.join(rows)+'\n};\n'
 tmp=NATIVE.with_suffix(NATIVE.suffix+'.tmp');tmp.write_text(body,encoding='utf-8',newline='\n');os.replace(tmp,NATIVE)

def upgrade_usgs_overviews(source):
 """Actual US bare-earth context, including the entire Miami–Keys corridor."""
 for key in ('miami_keys','new_orleans'):
  path=OUT/(key+'-overview.json');p=json.loads(path.read_text(encoding='utf-8'))
  if 'preferred_bare_earth_source' in p:continue
  raw=zlib.decompress(base64.b64decode(p['zlib_payload_base64']));detail.require(detail.sha(raw)==p['uncompressed_sha256'],'Invalid overview pre-upgrade packet')
  h=base.np.frombuffer(raw,dtype='<f4',count=257*257).reshape(257,257);flags=base.np.frombuffer(raw,dtype=base.np.uint8,offset=257*257*4)
  actual,record=detail.usgs_heights(source,focus_for(p['bounds_wgs84']),key+'-overview',h,flags)
  record['explicit_generalized_ne_ocean_mapzen_fallback_samples']=record.pop('explicit_osm_ocean_mapzen_fallback_samples')
  record['qualification']='USGS bare-earth topography, not bathymetry or parcel survey. Explicit generalized NaturalEarth ocean NoData only retains the recorded Mapzen bathymetry; all mapped land requires valid USGS.'
  newraw=actual.tobytes()+flags.tobytes();packed=zlib.compress(newraw,9)
  p['preferred_bare_earth_source']=record;p['source_upgrade']={'previous_packet_sha256':detail.sha(path.read_bytes()),'reason':'Avoid urban/building DSM spikes in low-relief US coastal terrain; source bare-earth, no terrain clamp.'}
  p.update(dem_min_m=float(actual.min()),dem_max_m=float(actual.max()),uncompressed_sha256=detail.sha(newraw),compressed_sha256=detail.sha(packed),zlib_payload_base64=base64.b64encode(packed).decode('ascii'))
  detail.write_json(path,p);print(json.dumps({'region':key,'usgs_overview':True,'min_m':p['dem_min_m'],'max_m':p['dem_max_m'],'valid_samples':record['usgs_valid_samples']}),flush=True)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--offline',action='store_true');parser.add_argument('--compile-only',action='store_true');parser.add_argument('--upgrade-usgs',action='store_true');args=parser.parse_args()
 regions=json.loads(base.CATALOG.read_text(encoding='utf-8'))['regions'];detail.require({r['id'] for r in regions}==set(BOUNDS),'All20 region context policies required')
 OUT.mkdir(parents=True,exist_ok=True)
 if args.compile_only:generate_native(regions);return
 source=base.Sources(args.offline);base.ZOOM=9;planned=set().union(*(base.tile_keys(focus_for(BOUNDS[r['id']])) for r in regions))
 if args.upgrade_usgs:
  upgrade_usgs_overviews(source);generate_native(regions)
  detail.write_json(OUT/'USGS_OVERVIEW_SOURCE_LOG.json',{'utc':base.now(),'requests':source.log,'scope':'Two US regional context rasters only; legacy base65/focus masks untouched'});return
 detail.require(len(planned)<=200,'Overview exceeds200 source tiles');print(json.dumps({'begin_utc':base.now(),'regions':20,'planned_z9_tiles':len(planned),'max_dem_bytes':base.MAX_DEM_BYTES,'cartographic_scale':'1:10000000'}),flush=True)
 layers,records=fetch_layers(source)
 try:
  for r in regions:
   key=r['id'];path=OUT/(key+'-overview.json')
   if not path.exists():
    b=BOUNDS[key];focus=focus_for(b);h,dem=detail.detail_heights(source,focus);w,s,e,n=b
    lon,lat=base.np.meshgrid(base.np.linspace(w,e,257),base.np.linspace(n,s,257));points=base.shapely.points(base.np.column_stack((lon.ravel(),lat.ravel())))
    clipped={name:source_shapes(reader,b) for name,reader in layers.items()};land=base.unary_union(clipped['land']);flags=base.np.zeros(257*257,dtype=base.np.uint8)
    flags[~base.shapely.intersects(land,points)]|=1
    if clipped['lakes']:flags[base.shapely.intersects(base.unary_union(clipped['lakes']),points)&((flags&1)==0)]|=2
    if clipped['rivers_lake_centerlines']:
     # A generalized overview centreline is rasterized at half one source sample;
     # this is not a surveyed river width and never enters the hazard solver.
     radius=max((e-w)/256,(n-s)/256)*.5
     flags[(base.shapely.distance(base.unary_union(clipped['rivers_lake_centerlines']),points)<=radius)&((flags&1)==0)]|=16
    raw=h.tobytes()+flags.tobytes();packed=zlib.compress(raw,9)
    vector={'type':'FeatureCollection','license':'Public domain','source':'Natural Earth1:10000000 generalized physical vectors',
            'features':[{'type':'Feature','properties':{'layer':name},'geometry':mapping(value)} for name,values in clipped.items() for value in values]}
    vector_path=OUT/(key+'-overview-physical.geojson');detail.write_json(vector_path,vector)
    packet={'schema_version':1,'region_id':key,'bounds_wgs84':b,'dem_side':257,'surface_side':257,'source_zoom':9,
            'intended_context_features':FEATURES[key],'dem_min_m':float(base.np.min(h)),'dem_max_m':float(base.np.max(h)),
            'dem_sources':dem,'physical_sources':records,'vector_file':vector_path.name,'vector_sha256':detail.sha(vector_path.read_bytes()),
            'coverage':{'ocean_pixels':int(base.np.count_nonzero(flags&1)),'lake_pixels':int(base.np.count_nonzero(flags&2)),'generalized_river_pixels':int(base.np.count_nonzero(flags&16))},
            'qualification':'Regional context only; actual boundedsource footprints, MSLmetres, no invented extension. Natural Earth coastline/lake/river vectors are generalized1:10million, NOT10metre precision; narrow Keys/islands may be omitted atthisscale. Exactfocus OSM/detail wins insideitsbounds. Outsidefocus is not simulated/buildable. No geology, forest or channel-width claim from overview masks.',
            'uncompressed_sha256':detail.sha(raw),'compressed_sha256':detail.sha(packed),'zlib_payload_base64':base64.b64encode(packed).decode('ascii')}
    detail.write_json(path,packet)
   p=json.loads(path.read_text(encoding='utf-8'));print(json.dumps({'region':key,'overview_ready':True,'min_m':p['dem_min_m'],'max_m':p['dem_max_m'],'coverage':p['coverage']}),flush=True);generate_native(regions)
 finally:
  log=OUT/'OVERVIEW_DOWNLOAD_LOG.json';history=json.loads(log.read_text(encoding='utf-8')) if log.exists() else {'runs':[]}
  history['runs'].append({'utc':base.now(),'planned_dem_tiles':len(planned),'used_dem_tiles':len(source.dem_keys),'dem_bytes':source.dem_bytes,'requests':source.log});detail.write_json(log,history)
 generate_native(regions);print(json.dumps({'complete':True,'regions':20,'source_tiles':len(source.dem_keys),'dem_bytes':source.dem_bytes,'native_sha256':detail.sha(NATIVE.read_bytes())}),flush=True)

if __name__=='__main__':main()
