"""Offline source audit for all20 regional contexts/all25 detail + climate.

This validates source packets/compiler correspondence, not Unreal appearance,
native execution, rendering performance, surveyed ground or hazard prediction.
"""
from __future__ import annotations
import argparse,base64,json,math,re,zlib
from pathlib import Path
import fetch_regional_environment_detail as detail
import fetch_regional_overview as overview
import fetch_real_geographic_tiles as base

ROOT=detail.ROOT;OUT=detail.OUT;np=base.np
EXTRA_SOURCES={
 'atakent':['https://www.kucukcekmece.gov.tr/kucukcekmece-golu','https://tdfh.mta.gov.tr/'],
 'dereyolu':['https://www.mgm.gov.tr/veridegerlendirme/il-ve-ilceler-istatistik.aspx?k=A&m=TRABZON'],
 'yelova':['https://eticaret.mta.gov.tr/index.php?product_id=8312&route=product%2Fproduct'],
 'kiyikent':['https://eticaret.mta.gov.tr/index.php?product_id=7787&route=product%2Fproduct'],
 'mugla':['https://www.tarimorman.gov.tr/DKMP/Belgeler/dkmp%20resmi%20istatistik/kutuphane/58.pdf'],
 'etna':['https://data.ingv.it/dataset/807'],
 'konya':[],
 'miami_keys':['https://www.weather.gov/key/climate','https://www.weather.gov/mfl/climate'],
 'cyprus':['https://www.moa.gov.cy/moa/gsd/gsd.nsf/page53_en/page53_en'],
 'yakutsk':['https://science.nasa.gov/earth/earth-observatory/flooding-on-russias-lena-river-2502/'],
 'valparaiso':[], 'valdivia':[], 'sendai':[],
 'rotterdam':['https://repository.tno.nl/SingleDoc?docId=35461'], 'new_orleans':[],
 'grindavik':[], 'vesuvius':[], 'christchurch':[], 'kathmandu':[], 'jakarta':[]}

def load(path):return json.loads(path.read_text(encoding='utf-8'))
def decode(p,expected):
 packed=base64.b64decode(p['zlib_payload_base64'],validate=True);d=zlib.decompressobj();raw=d.decompress(packed,expected+1)
 detail.require(d.eof and not d.unused_data and not d.unconsumed_tail and len(raw)==expected,'Invalid bounded packet length/trailing bytes')
 detail.require(detail.sha(packed)==p['compressed_sha256'] and detail.sha(raw)==p['uncompressed_sha256'],'Source packet SHA mismatch');return raw
def compiled_arrays(path):
 text=path.read_text(encoding='utf-8');arrays={}
 for name,body in re.findall(r'static constexpr const ANSICHAR\* (\w+)\[\] = \{\n(.*?)\n\};',text,re.S):
  parts=re.findall(r'"([A-Za-z0-9+/=]+)"',body);detail.require(parts and all(len(p)<=8192 for p in parts),'Compiler Base64 literal exceeds8192')
  detail.require(name not in arrays,'Duplicate compiled array name');arrays[name]=''.join(parts)
 return arrays,text
def stats(h,flags):
 land=h.ravel()[(flags&1)==0];return {'all_min_m':float(h.min()),'all_max_m':float(h.max()),'non_ocean_min_m':float(land.min()) if land.size else None,'non_ocean_max_m':float(land.max()) if land.size else None,
 'non_ocean_below_minus20m_samples':int(np.count_nonzero(((flags&1)==0)&(h.ravel()<-20)))}

def run(write_matrix=False):
 catalog=load(base.CATALOG);regions=catalog['regions'];flat=[(r,f) for r in regions for f in r['focuses']]
 detail.require(len(regions)==20 and len(flat)==25,'All20/25 source coverage required')
 d_arrays,d_cpp=compiled_arrays(detail.NATIVE);o_arrays,o_cpp=compiled_arrays(overview.NATIVE);climate_cpp=detail.CLIMATE_NATIVE.read_text(encoding='utf-8')
 detail.require(len(d_arrays)==25 and len(o_arrays)==20,'Compiled ready array coverage incomplete')
 profiles=(ROOT/'Source/JamProject/Aftershock/Private/AftershockRegionEnvironmentProfile.cpp').read_text(encoding='utf-8')
 profile_rows=re.findall(r'\{TEXT\("([^"]+)"\),EAftershockRegionalBiome::(\w+),\{[^}]+\},\{[^}]+\},TEXT\("([^"]+)"\),TEXT\("([^"]+)"\)\}',profiles)
 detail.require([p[0] for p in profile_rows]==[r['id'] for r in regions],'Runtime profile/catalog order differs');pmap={p[0]:p for p in profile_rows}
 checks=[];matrix=[];detail_raw_total=0;overview_raw_total=0;warnings=[];blockers=[];periods=set();source_manifest=[]
 for region in regions:
  rid=region['id'];op=OUT/(rid+'-overview.json');o=load(op);raw=decode(o,257*257*5);h=np.frombuffer(raw,dtype='<f4',count=257*257).reshape(257,257);flags=np.frombuffer(raw,dtype=np.uint8,offset=257*257*4)
  detail.require(o['region_id']==rid and o['bounds_wgs84']==list(overview.BOUNDS[rid]),'Overview identity/bounds mismatch')
  detail.require(np.all(np.isfinite(h)) and h.min()>-12000 and h.max()<9000 and not np.any(flags&~np.uint8(19)),'Overview has invalid source values')
  detail.require(not np.any(((flags&1)!=0)&((flags&~np.uint8(1))!=0)),'Ocean overview has contradictory surface tags')
  detail.require(o_arrays['Overview_'+rid]==o['zlib_payload_base64'],'Compiled overview bytes differ from source packet')
  vector=OUT/o['vector_file'];detail.require(vector.parent==OUT and detail.sha(vector.read_bytes())==o['vector_sha256'],'Missing/corrupt overview physical vector')
  detail.require(all(s['license']=='Public domain' and s['cartographic_scale'].startswith('1:10000000') for s in o['physical_sources'].values()),'NaturalEarth licence/scale missing')
  overview_raw_total+=len(raw);entry={'region_id':rid,'label':region['label'],'biome':pmap[rid][1],'regional_interpretation':pmap[rid][2],
   'primary_context_references':[pmap[rid][3],*EXTRA_SOURCES[rid]],'overview':{'bounds_wgs84':o['bounds_wgs84'],'physical_features':o['intended_context_features'],'elevation':stats(h,flags),'source_packet':op.name,'source_packet_sha256':detail.sha(op.read_bytes()),'surface_scale':'NaturalEarth1:10million cartographic generalization, NOT10metre resolution','narrow_island_geometry_guaranteed':False,'local_osm_detail_must_take_priority':True,'whole_region_buildable':False,'thin_feature_limit':'Generalized257mask may omit narrow islands/channels, especially FloridaKeys; numerical source coverage does not prove their visible shoreline geometry. No full-corridor simulation or rendered visibility claim.'},
   'hazards':{'primary_authored_scenario':region['primary_hazard'],'order':catalog['hazard_order'],'authored_relative_game_weights':region['weights'],'qualification':'Original scenario policy; not annual event probabilities, local recurrence model, soil survey or eruption forecast. Existing hazard equations are unchanged.'},'focuses':[]}
  if rid=='christchurch' and entry['overview']['elevation']['non_ocean_max_m']>1200:
   blockers.append({'region':rid,'scope':'wide_context_only','actual_non_ocean_max_m':entry['overview']['elevation']['non_ocean_max_m'],'reason':'Source spike in bounded Canterbury/BanksPeninsula region. DOC identifies the peninsula highest summit as920m; source must be replaced, never clamped. Local detail is separate.','reference':'https://www.doc.govt.nz/parks-and-recreation/places-to-go/canterbury/places/banks-peninsula-area/things-to-do/te-ara-pataka/mt-herbert-walkway/'})
  if entry['overview']['elevation']['non_ocean_below_minus20m_samples']:
   blockers.append({'region':rid,'scope':'wide_context_only','actual_non_ocean_min_m':entry['overview']['elevation']['non_ocean_min_m'],'reason':'Deep negative non-ocean context samples require an independent source comparison; this is a source quality gate, not a mathematical claim that below-sea terrain cannot exist.'})
  if rid in base.BARE_EARTH_REGIONS:
   detail.require('preferred_bare_earth_source' in o,'US overview must use USGS bare-earth source')
   actual,receipt=detail.usgs_heights(base.Sources(True),overview.focus_for(o['bounds_wgs84']),rid+'-overview',h,flags)
   detail.require(receipt['sha256']==o['preferred_bare_earth_source']['sha256'] and np.array_equal(actual,h),'USGS overview source transform/bytes differ')
  if 'preferred_copernicus_source' in o:
   cp=o['preferred_copernicus_source'];detail.require(cp['sampled_float32_sha256']==detail.sha(h.tobytes()) and cp['mandatory_modified_data_notice'].startswith('produced using Copernicus WorldDEM-30'),'Overview Copernicus source hash/mandatory notice absent')
  for focus in region['focuses']:
   key=rid+'_'+focus['id'];path=OUT/(key+'-detail.json');p=load(path);raw=decode(p,257*257*9)
   detail.require(p['region_id']==rid and p['focus_id']==focus['id'] and p['bounds_wgs84']==base.bounds(focus),'Detail identity/bounds mismatch')
   oldpath=base.OUT/(key+'.json');detail.require(detail.sha(oldpath.read_bytes())==p['base_packet_sha256'],'Legacy packet bytes changed')
   oldraw=zlib.decompress(base64.b64decode(load(oldpath)['zlib_payload_base64']));h=np.frombuffer(raw,dtype='<f4',count=257*257).reshape(257,257);flags=np.frombuffer(raw,dtype=np.uint8,count=257*257,offset=257*257*4)
   detail.require(np.all(np.isfinite(h)) and h.min()>-12000 and h.max()<9000 and not np.any(flags&~np.uint8(63)),'Invalid detail source value/flag')
   detail.require(not np.any(((flags&1)!=0)&((flags&~np.uint8(1))!=0)),'Ocean detail has contradictory surface tags')
   detail.require(raw[257*257*5:]==oldraw[65*65*4+257*257:],'Source river/coast distances changed unexpectedly')
   detail.require(d_arrays['DetailTile_'+key]==p['zlib_payload_base64'],'Compiled detail bytes differ from source packet')
   detail.require(p['dem_min_m']==float(h.min()) and p['dem_max_m']==float(h.max()),'Detail elevation metadata stale')
   expected='true' if p['has_coastline_source'] else 'false';row='{ '+', '.join([*(format(v,'.15g') for v in p['bounds_wgs84']),'DetailTile_'+key,str(math.ceil(len(p['zlib_payload_base64'])/8192)),str(len(base64.b64decode(p['zlib_payload_base64']))),str(len(raw)),expected])+' }'
   detail.require(row in d_cpp,'Compiled descriptor bounds/length differs');detail_raw_total+=len(raw)
   if rid in base.BARE_EARTH_REGIONS:
    actual,receipt=detail.usgs_heights(base.Sources(True),focus,key,h,flags)
    detail.require(receipt['sha256']==p['preferred_bare_earth_source']['sha256'] and np.array_equal(actual,h),'USGS detail source transform/bytes differ')
   if rid in ('valdivia','sendai','jakarta','rotterdam'):
    cp=p.get('preferred_copernicus_source',{});detail.require(cp.get('vertical_datum','').startswith('EGM2008') and cp.get('mandatory_modified_data_notice','').startswith('produced using Copernicus WorldDEM-30'),'Critical corrected DSM source licence/datum missing')
    detail.require(cp['sampled_float32_sha256']==detail.sha(h.tobytes()),'Copernicus sampled raster SHA differs')
   cpath=OUT/(key+'-climate.json');c=load(cpath);detail.require(c['region_id']==rid and c['focus_id']==focus['id'] and len(c['monthly'])==12,'Climate identity/month coverage differs')
   detail.require(c['requested_lat_lon']==[focus['latitude'],focus['longitude']] and c['source_grid_degrees_lat_lon']==[.5,.625],'Climate request or grid scale mismatch')
   detail.require(c['start_year']<=c['end_year'] and str(c['start_year']) in c['api_header']['range'] and str(c['end_year']) in c['api_header']['range'],'Climate period differs from actual response')
   for k,(unit,lo,hi) in detail.PARAMETERS.items():detail.require(c['parameters'][k]['units']==unit and all(type(m[k]) in (int,float) and math.isfinite(m[k]) and lo<=m[k]<=hi for m in c['monthly']),'Climate fill/nonfinite/units invalid')
   months=['{'+','.join(format(m[k],'.9g')+'f' if isinstance(m[k],float) and not float(m[k]).is_integer() else format(float(m[k]),'.1f')+'f' for k in detail.PARAMETERS)+'}' for m in c['monthly']]
   crow='{ true, '+', '.join([format(c['source_grid_elevation_metres'],'.6f')+'f',str(c['start_year']),str(c['end_year'])])+', { '+', '.join(months)+' } }'
   detail.require(crow in climate_cpp,'Compiled climate differs from recorded12month source response')
   periods.add((c['start_year'],c['end_year']));s=stats(h,flags)
   if s['non_ocean_below_minus20m_samples']:blockers.append({'focus':key,'kind':'non_ocean_below_minus20m_source_review','samples':s['non_ocean_below_minus20m_samples'],'reason':'Requires independent source comparison; no arbitrary clamp.'})
   entry['focuses'].append({'focus_id':focus['id'],'label':focus['label'],'anchor_lat_lon':c['requested_lat_lon'],'bounds_wgs84':p['bounds_wgs84'],'elevation':s,'dem_output_samples':257,'mask_output_samples':257,
    'elevation_source':'USGS3DEP bare-earth' if 'preferred_bare_earth_source' in p else 'CopernicusGLO30 edited DSM' if 'preferred_copernicus_source' in p else 'Mapzen source-dependent mixed DEM',
    'detail_packet':path.name,'detail_packet_sha256':detail.sha(path.read_bytes()),'intermittent_channel_pixels':p['intermittent_mapping']['intermittent_pixels'],
    'monthly_climate':{'source':'NASA POWER/MERRA2','period':[c['start_year'],c['end_year']],'grid_lat_lon_degrees':[.5,.625],'grid_elevation_m':c['source_grid_elevation_metres'],'monthly':c['monthly'],'annual_t2m_mean_c':round(sum(m['T2M'] for m in c['monthly'])/12,4),'monthly_t2m_range_c':[min(m['T2M'] for m in c['monthly']),max(m['T2M'] for m in c['monthly'])],'source_packet':cpath.name,'source_packet_sha256':detail.sha(cpath.read_bytes())},
    'qualification':'Real bounded elevation/coast/surface geometry; city roads/buildings are original procedural gameplay, not a reconstruction of existing streets. DEM/DSM sample density is not guaranteed source accuracy; regional geology is not mapped per parcel.'})
  matrix.append(entry);checks.append({'region':rid,'focus_count':len(entry['focuses']),'source_ready':True})
 detail.require(detail_raw_total<=32*1024*1024 and 2*257*257*5<=4*1024*1024,'Decoded cache exceeds budget')
 # Real source landmarks; they are numerical source tests, not camera screenshots.
 etna=next(e for e in matrix if e['region_id']=='etna');cyprus=next(e for e in matrix if e['region_id']=='cyprus');miami=next(e for e in matrix if e['region_id']=='miami_keys')
 detail.require(etna['focuses'][0]['elevation']['all_max_m']>3000 and etna['overview']['elevation']['non_ocean_max_m']>3000,'Etna source cone missing')
 detail.require(cyprus['overview']['elevation']['non_ocean_max_m']>1800 and len(cyprus['focuses'])==3,'Cyprus broad mountain context/focus coverage missing')
 detail.require(all(f['elevation']['non_ocean_max_m']<80 for f in miami['focuses']),'Florida focus contains implausible major DSM hills')
 detail.require(next(e for e in matrix if e['region_id']=='valdivia')['focuses'][0]['elevation']['non_ocean_min_m']>-20,'Valdivia critical river-floor artifact remains')
 data={'schema_version':1,'source_only':True,'regions':matrix,'qualification':'Auditable regional interpretation and offline source/compiler data. No Unreal rendering/performance/native result is asserted here. Region profile colours and hazard weights are original authored interpretations; coordinates, raster heights, mapped OSM surfaces and NASA monthly data retain their own provenance.'}
 if write_matrix:detail.write_json(OUT/'REGION_ENVIRONMENT_MATRIX.json',data)
 for path in sorted(OUT.glob('*')):
  if path.is_file():source_manifest.append({'file':path.name,'bytes':path.stat().st_size,'sha256':detail.sha(path.read_bytes())})
 result={'scope':'offline_source_and_compiler_only','unreal_execution_verified':False,'source_quality_accepted':not blockers,'blocking_source_quality_issues':blockers,'region_count':len(matrix),'detail_focus_count':len(flat),'monthly_climate_focus_count':len(flat),'detail_decoded_bytes':detail_raw_total,'overview_source_raw_total_bytes_not_live_cache':overview_raw_total,'overview_max_live_raw_bytes':2*257*257*5,'periods':[list(p) for p in sorted(periods)],'regions':checks,'warnings':warnings,'files':source_manifest,'native_files':[{'file':p.relative_to(ROOT).as_posix(),'bytes':p.stat().st_size,'sha256':detail.sha(p.read_bytes())} for p in (detail.NATIVE,detail.CLIMATE_NATIVE,overview.NATIVE)]}
 return result

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write-matrix',action='store_true');parser.add_argument('--output',default='Saved/PresentationValidation/regional_environment_source_audit.json');args=parser.parse_args()
 result=run(args.write_matrix);detail.write_json(ROOT/args.output,result);print(json.dumps({k:v for k,v in result.items() if k not in ('files','regions','native_files')},ensure_ascii=False))
 if not result['source_quality_accepted']:raise SystemExit(2)

if __name__=='__main__':main()
