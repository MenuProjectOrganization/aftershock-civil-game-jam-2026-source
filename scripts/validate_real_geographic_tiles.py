"""Offline source-data verification. Does not start Unreal or access the network."""
from __future__ import annotations
import argparse,base64,hashlib,json,math,re,struct,sys,zlib
from pathlib import Path
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
FOLDER=ROOT/"_Imports/Geography/RealTerrain"
DEM_SIDE=65;MASK_SIDE=257;DEM_BYTES=65*65*4;MASK_COUNT=257*257;EXPECTED_BYTES=DEM_BYTES+MASK_COUNT*5


def require(ok,message):
    if not ok:raise ValueError(message)
def sha(raw):return hashlib.sha256(raw).hexdigest()
def decode_packet(path):
    packet=json.loads(path.read_text(encoding="utf-8"));packed=base64.b64decode(packet["zlib_payload_base64"],validate=True);raw=zlib.decompress(packed)
    require(sha(packed)==packet["compressed_sha256"] and sha(raw)==packet["uncompressed_sha256"],"Source packet checksum mismatch")
    require(len(raw)==EXPECTED_BYTES and packet["dem_side"]==65 and packet["surface_side"]==257,"Source packet schema dimensions mismatch")
    heights=np.frombuffer(raw,dtype="<f4",count=4225);flags=np.frombuffer(raw,dtype=np.uint8,count=MASK_COUNT,offset=DEM_BYTES)
    river=np.frombuffer(raw,dtype="<u2",count=MASK_COUNT,offset=DEM_BYTES+MASK_COUNT);coast=np.frombuffer(raw,dtype="<i2",count=MASK_COUNT,offset=DEM_BYTES+MASK_COUNT*3)
    require(np.all(np.isfinite(heights)) and np.min(heights)>-12000 and np.max(heights)<9000,"Invalid actual DEM heights")
    require(np.all((flags&np.uint8(224))==0),"Unknown OSM mask bit")
    require(np.all((flags[(flags&1)!=0]&30)==0),"Ocean incorrectly claims inland water/forest/farm")
    require(abs(float(np.min(heights))-packet["dem_min_m"])<1e-5 and abs(float(np.max(heights))-packet["dem_max_m"])<1e-5,"Published DEM extrema do not match source bytes")
    coverage={"ocean_pixels":int(np.count_nonzero(flags&1)),"inland_water_pixels":int(np.count_nonzero(flags&2)),"mapped_forest_pixels":int(np.count_nonzero(flags&4)),"mapped_agriculture_pixels":int(np.count_nonzero(flags&8)),"mapped_waterway_pixels":int(np.count_nonzero(flags&16)),"unknown_landcover_pixels":int(np.count_nonzero((flags&12)==0))}
    require(coverage==packet["surface_coverage"],"Published OSM coverage differs from actual source raster")
    require(np.all(coast[(flags&1)!=0]<=0) and np.all(coast[(flags&1)==0]>=0),"Signed coast-distance and ocean mask disagree")
    require(packet["has_coastline_source"] or (not np.any(flags&1) and np.all(coast==32767)),"Coastless source invents an ocean")
    return packet,heights.reshape(65,65),flags.reshape(257,257),river.reshape(257,257),coast.reshape(257,257)


def sample_height(packet,heights,lat,lon):
    west,south,east,north=packet["bounds_wgs84"]
    if not(math.isfinite(lat) and math.isfinite(lon) and west<=lon<=east and south<=lat<=north):return None
    x=(lon-west)/(east-west)*64;y=(north-lat)/(north-south)*64;x0=min(64,math.floor(x));y0=min(64,math.floor(y));x1=min(64,x0+1);y1=min(64,y0+1)
    return float((heights[y0,x0]*(1-(x-x0))+heights[y0,x1]*(x-x0))*(1-(y-y0))+(heights[y1,x0]*(1-(x-x0))+heights[y1,x1]*(x-x0))*(y-y0))


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--allow-incomplete",action="store_true");args=parser.parse_args()
    catalog=json.loads((ROOT/"_Imports/Geography/REGION_CATALOG.json").read_text(encoding="utf-8"));checks=[];focus_rows=[];urls={};dem_byte_counts={};osm_byte_counts={};usgs_byte_counts={};compiled_bytes=0
    native_bytes=(ROOT/"Source/JamProject/Aftershock/Private/AftershockOfflineGeographicTiles.inl").read_bytes();native=native_bytes.decode("utf-8")
    for region in catalog["regions"]:
        for focus in region["focuses"]:
            key=region["id"]+"_"+focus["id"];path=FOLDER/(key+".json")
            if not path.exists():
                require(args.allow_incomplete,"Missing real focus: "+key);continue
            packet,h,f,r,c=decode_packet(path);west,south,east,north=packet["bounds_wgs84"]
            compiled_bytes+=len(base64.b64decode(packet["zlib_payload_base64"]))
            require(packet["region_id"]==region["id"] and packet["focus_id"]==focus["id"],"Focus identity mismatch")
            require(packet["bounds_wgs84"]==[focus["longitude"]-focus["half_longitude_extent"],focus["latitude"]-focus["half_latitude_extent"],focus["longitude"]+focus["half_longitude_extent"],focus["latitude"]+focus["half_latitude_extent"]],"Focus bbox changed")
            require(sample_height(packet,h,north+.001,west) is None and sample_height(packet,h,south,east+.001) is None,"Outside source was clamped")
            pattern=r"static constexpr uint8 GeoTile_"+re.escape(key)+r"\[\] = \{(.*?)\};"
            match=re.search(pattern,native,re.S);require(match is not None,"Source packet missing from native archive")
            packed=bytes(int(n) for n in re.findall(r"\d+",match[1]));require(sha(packed)==packet["compressed_sha256"],"Native compiled source bytes differ from source packet")
            for record in packet["dem_sources"]:
                previous=urls.get(record["url"]);require(previous is None or previous==record["sha256"],"One DEM source URL claims different cached revisions");urls[record["url"]]=record["sha256"]
                dem_byte_counts[record["url"]]=record["bytes"]
            require(packet["osm_source"]["license"]=="ODbL-1.0" and packet["osm_source"]["query_sha256"]==sha(packet["osm_source"]["query"].encode()),"OSM license/query provenance missing")
            osm_byte_counts[packet["osm_source"]["query_sha256"]]=packet["osm_source"]["bytes"]
            geometry=packet.get("source_geometry_examples");require(geometry and geometry["license"]=="ODbL-1.0","Missing distributed source geometry examples")
            geometry_path=FOLDER/geometry["file"];require(geometry_path.parent==FOLDER and geometry_path.suffix==".geojson" and geometry_path.is_file() and sha(geometry_path.read_bytes())==geometry["sha256"],"Source vector filename/hash mismatch")
            vectors=json.loads(geometry_path.read_text(encoding="utf-8"));require(vectors["license"]=="ODbL-1.0" and len(vectors["features"])==geometry["features"],"Source vector feature count/license mismatch")
            if region["id"] in {"miami_keys","new_orleans"}:
                preferred=packet.get("preferred_bare_earth_source");require(preferred and preferred["url"].startswith("https://elevation.nationalmap.gov/") and preferred["raster_function"]=="None" and preferred["pixel_type"]=="F32" and preferred["image_crs"]=="EPSG:4326","US focus lacks preferred genuine USGS bare-earth source")
                require(preferred["mapzen_land_fallback_count"]==0 and preferred["usgs_valid_sample_count"]+preferred["mapzen_ocean_nodata_fallback_count"]==4225,"USGS land cannot silently fall back to mixed DSM")
                require(0<=preferred["mapzen_ocean_nodata_fallback_count"]<=int(np.count_nonzero(f[::4,::4]&1)),"USGS ocean fallback count exceeds mapped ocean samples")
                usgs_byte_counts[preferred["url"]]=preferred["bytes"]
            focus_rows.append({"region_id":region["id"],"focus_id":focus["id"],"source_packet_sha256":sha(path.read_bytes()),"actual_dem_samples":4225,"surface_pixels":MASK_COUNT,
                               "bounds_wgs84":packet["bounds_wgs84"],"min_m":float(np.min(h)),"max_m":float(np.max(h)),"source_feature_counts":packet["osm_features"],"coverage":packet["surface_coverage"]})
            checks.append({"name":key+"_native_payload_source_hash_shapes_bounds_no_clamp_and_licenses","passed":True})
    for key,expected in catalog["legacy_sample_sha256"].items():
        require(sha((ROOT/"_Imports/Geography"/(key+"_glo90.json")).read_bytes())==expected,"Legacy provider response changed: "+key)
    checks.append({"name":"seven_legacy_provider_responses_byte_unchanged","passed":True})
    licenses=json.loads((FOLDER/"licenses/SOURCE_LICENSES.json").read_text(encoding="utf-8"))
    for record in licenses["files"]:require(sha((FOLDER/"licenses"/record["file"]).read_bytes())==record["sha256"],"Provider license/attribution text changed")
    checks.append({"name":"provider_license_texts_match_recorded_sha256","passed":True})
    require(len(urls)<=200 and sum(dem_byte_counts.values())<=30*1024*1024,"Actual source tile count/byte hard cap exceeded")
    require(sum(osm_byte_counts.values())<=80*1024*1024 and max(osm_byte_counts.values(),default=0)<=12*1024*1024,"Actual source OSM byte hard cap exceeded")
    require(len(usgs_byte_counts)<=5 and sum(usgs_byte_counts.values())<=10*1024*1024,"USGS bounded-export hard cap exceeded")
    if not args.allow_incomplete:require(len(focus_rows)==25,"Expected all twenty-five real focus packets")
    report={"schema_version":1,"scope":"Offline source packet/native byte equality, provenance and numerical masks only; not rendered visuals, city placement, scientific hazard accuracy or performance acceptance.",
            "complete":len(focus_rows)==25,"focuses":len(focus_rows),"unique_dem_tile_urls":len(urls),"unique_dem_source_bytes":sum(dem_byte_counts.values()),"unique_osm_source_bytes":sum(osm_byte_counts.values()),"usgs_exports":len(usgs_byte_counts),"usgs_source_bytes":sum(usgs_byte_counts.values()),"total_source_heights":len(focus_rows)*4225,
            "compiled_compressed_bytes":compiled_bytes,"decoded_payload_bytes_per_focus":EXPECTED_BYTES,"decoded_payload_bytes_all_focus":EXPECTED_BYTES*len(focus_rows),"native_archive_sha256":sha(native_bytes),
            "passed":len(checks),"failed":0,"checks":checks,"results":focus_rows}
    out=ROOT/"Saved/PresentationValidation/real_geography_source_audit.json";out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
    print(json.dumps({key:report[key] for key in ("complete","focuses","unique_dem_tile_urls","total_source_heights","passed","failed")}))


if __name__=="__main__":
    try:main()
    except Exception as error:
        # A failed repeat must not leave an older successful report as current.
        report={"schema_version":1,"scope":"Offline source validation failed; no Unreal/render/performance acceptance.","complete":False,"passed":0,"failed":1,"error":str(error).replace(str(ROOT),"<repo>")}
        out=ROOT/"Saved/PresentationValidation/real_geography_source_audit.json";out.parent.mkdir(parents=True,exist_ok=True)
        out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
        print(json.dumps(report,ensure_ascii=False),file=sys.stderr);raise SystemExit(1)
