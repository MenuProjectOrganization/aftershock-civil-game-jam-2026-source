"""Bounded offline real-distance terrain compiler; no Unreal/runtime networking.

DEM: public Mapzen Terrarium tiles on AWS (source-specific open-data attribution).
Surface geometry: OpenStreetMap contributors, ODbL 1.0. Derived OSM tables remain
available in the openly distributed source packets; source WAV/engine assets are
unrelated. Raw network caches live only in Saved, never the production Content.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import http.client
import io
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"Saved/GeographyPythonPackages"))
import numpy as np
from PIL import Image
import shapely
from shapely.geometry import LineString,Polygon,MultiLineString,box,mapping
from shapely.ops import polygonize,unary_union

OUT=ROOT/"_Imports/Geography/RealTerrain"
CACHE=ROOT/"Saved/GeographySourceCache"
CATALOG=ROOT/"_Imports/Geography/REGION_CATALOG.json"
NATIVE=ROOT/"Source/JamProject/Aftershock/Private/AftershockOfflineGeographicTiles.inl"
DEM_SIDE=65
MASK_SIDE=257
ZOOM=12
MAX_TILES=200
MAX_DEM_BYTES=30*1024*1024
MAX_OSM_RESPONSE=12*1024*1024
MAX_OSM_BYTES=80*1024*1024
TERRARIUM="https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
OVERPASS="https://overpass-api.de/api/interpreter"
OVERPASS_ALTERNATE="https://overpass.private.coffee/api/interpreter"
OVERPASS_THIRD="https://maps.mail.ru/osm/tools/overpass/api/interpreter"
OSM_ENDPOINTS=(OVERPASS_ALTERNATE,OVERPASS,OVERPASS_THIRD)
USGS_EXPORT="https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage"
BARE_EARTH_REGIONS={"miami_keys","new_orleans"}
USER_AGENT="AFTERSHOCK-open-source-game-geography/2 (bounded offline educational cache)"


def digest(data):return hashlib.sha256(data).hexdigest()
def require(ok,message):
    if not ok:raise ValueError(message)
def now():return datetime.now(timezone.utc).isoformat()
def axes(lat):
    phi=math.radians(lat);e2=.0066943799901413165;a=6378137.;d=1-e2*math.sin(phi)**2
    return a/math.sqrt(d)*math.cos(phi)*math.pi/180.,a*(1-e2)/d**1.5*math.pi/180.
def tile_xy(lat,lon):
    n=2**ZOOM
    return (lon+180.)/360.*n,(1.-math.asinh(math.tan(math.radians(lat)))/math.pi)*.5*n
def bounds(f):
    return [f["longitude"]-f["half_longitude_extent"],f["latitude"]-f["half_latitude_extent"],f["longitude"]+f["half_longitude_extent"],f["latitude"]+f["half_latitude_extent"]]
def tile_keys(f):
    w,s,e,n=bounds(f);x0,y0=tile_xy(n,w);x1,y1=tile_xy(s,e)
    return {(ZOOM,x,y) for y in range(math.floor(y0),math.floor(y1)+1) for x in range(math.floor(x0),math.floor(x1)+1)}


class Sources:
    def __init__(self,offline=False,osm_endpoint=OVERPASS_ALTERNATE):
        require(osm_endpoint in OSM_ENDPOINTS,"Only explicitly approved public OSM endpoints are permitted")
        self.offline=offline;self.dem_keys=set();self.dem_bytes=0;self.osm_bytes=0;self.images={};self.log=[];self.last_osm=0.;self.osm_endpoint=osm_endpoint;self.osm_http_attempts=0;self.osm_attempts_by_query={}
    def request(self,url,path,limit,kind,data=None,max_attempts=3):
        path.parent.mkdir(parents=True,exist_ok=True)
        if path.exists():raw=path.read_bytes();cached=True
        else:
            require(not self.offline,"Missing offline source cache: "+path.name)
            request=urllib.request.Request(url,data=data,headers={"User-Agent":USER_AGENT,"Accept":"application/json" if kind=="osm" else "image/png"})
            for attempt in range(max_attempts):
                if kind=="osm":
                    query_key=digest(data or b"");self.osm_http_attempts+=1;self.osm_attempts_by_query[query_key]=self.osm_attempts_by_query.get(query_key,0)+1
                    require(self.osm_http_attempts<=150 and self.osm_attempts_by_query[query_key]<=6,"Bounded OSM HTTP attempt budget exceeded")
                self.log.append({"utc":now(),"kind":kind,"event":"request_attempt","url":url,"attempt":attempt+1})
                try:
                    with urllib.request.urlopen(request,timeout=100 if kind=="osm" else 30) as response:raw=response.read(limit+1)
                    require(len(raw)<=limit,"Provider response exceeds hard byte cap")
                    path.write_bytes(raw)
                    sidecar={"url":url,"sha256":digest(raw),"bytes":len(raw),"original_retrieved_utc":now()}
                    path.with_suffix(path.suffix+".source.json").write_text(json.dumps(sidecar)+"\n",encoding="utf-8",newline="\n");break
                except (urllib.error.HTTPError,urllib.error.URLError,TimeoutError,http.client.HTTPException,ConnectionError) as error:
                    detail={"utc":now(),"kind":kind,"url":url,"error":str(error),"attempt":attempt+1}
                    if isinstance(error,urllib.error.HTTPError):
                        try:detail["response_excerpt"]=error.read(2048).decode("utf-8","replace")
                        except Exception:detail["response_excerpt"]="HTTP error body unavailable"
                    print(json.dumps({"provider_error":str(error),**detail}),flush=True);self.log.append(detail)
                    if attempt==max_attempts-1:raise
                    time.sleep(30 if isinstance(error,urllib.error.HTTPError) and error.code in (429,406) else 3)
            cached=False
        require(len(raw)<=limit,"Cached provider response exceeds byte cap")
        record={"url":url,"sha256":digest(raw),"bytes":len(raw),"cached":cached,"retrieved_or_reused_utc":now()}
        sidecar_path=path.with_suffix(path.suffix+".source.json")
        if sidecar_path.exists():
            original=json.loads(sidecar_path.read_text(encoding="utf-8"));require(original["sha256"]==record["sha256"] and original["bytes"]==record["bytes"],"Cached source provenance sidecar mismatch")
            record["url"]=original["url"];record["original_retrieved_utc"]=original["original_retrieved_utc"]
        elif cached and kind=="osm":
            record["url"]=OVERPASS
            record["cache_provenance_note"]="Pre-fallback cache from the previous FOSSGIS-only compiler; no source sidecar existed then."
        self.log.append({"kind":kind,**record})
        return raw,record
    def image(self,key):
        if key in self.images:return self.images[key]
        self.dem_keys.add(key);require(len(self.dem_keys)<=MAX_TILES,"DEM tile-count hard cap exceeded")
        z,x,y=key;url=TERRARIUM.format(z=z,x=x,y=y)
        raw,meta=self.request(url,CACHE/f"terrarium/{z}/{x}/{y}.png",2*1024*1024,"dem")
        self.dem_bytes+=len(raw);require(self.dem_bytes<=MAX_DEM_BYTES,"DEM 30MiB hard cap exceeded")
        image=Image.open(io.BytesIO(raw));require(image.size==(256,256),"Unexpected Terrarium tile dimensions")
        self.images[key]=(np.asarray(image.convert("RGB"),dtype=np.float64),meta)
        return self.images[key]
    def osm(self,region,focus):
        w,s,e,n=bounds(focus)
        bbox=f"({s:.8f},{w:.8f},{n:.8f},{e:.8f})"
        query='[out:json][timeout:90][maxsize:67108864];('+''.join(selector+bbox+';' for selector in (
            'way["natural"="coastline"]','way["natural"="water"]','relation["natural"="water"]',
            'way["waterway"~"^(river|stream|canal|drain|riverbank)$"]','relation["waterway"="riverbank"]',
            'way["natural"="wood"]','relation["natural"="wood"]',
            'way["landuse"~"^(forest|farmland|farmyard|orchard|vineyard)$"]',
            'relation["landuse"~"^(forest|farmland|farmyard|orchard|vineyard)$"]'))+');out geom;'
        delay=max(0.,1.5-(time.monotonic()-self.last_osm))
        if delay:time.sleep(delay)
        self.last_osm=time.monotonic()
        key=region["id"]+"_"+focus["id"]
        cache_path=CACHE/f"osm/{key}-{digest(query.encode())[:12]}.json";payload=urllib.parse.urlencode({"data":query}).encode()
        endpoints=(self.osm_endpoint,*(url for url in OSM_ENDPOINTS if url!=self.osm_endpoint))
        for index,endpoint in enumerate(endpoints):
            try:
                raw,meta=self.request(endpoint,cache_path,MAX_OSM_RESPONSE,"osm",payload,max_attempts=2);break
            except (urllib.error.HTTPError,urllib.error.URLError,TimeoutError,http.client.HTTPException,ConnectionError):
                if index==len(endpoints)-1:raise
                self.log.append({"utc":now(),"kind":"osm","event":"endorsed_public_endpoint_fallback","from":endpoint,"to":endpoints[index+1]})
        self.osm_bytes+=len(raw);require(self.osm_bytes<=MAX_OSM_BYTES,"OSM total byte cap exceeded")
        document=json.loads(raw);require("elements" in document and "remark" not in document,"Incomplete/error Overpass response")
        return document,{**meta,"query":query,"query_sha256":digest(query.encode()),"source_timestamp_utc":document.get("osm3s",{}).get("timestamp_osm_base"),"license":"ODbL-1.0","attribution":"Map data © OpenStreetMap contributors, https://www.openstreetmap.org/copyright"}


def elevation_grid(source,focus):
    w,s,e,n=bounds(focus);values=[];used={}
    for lat in np.linspace(n,s,DEM_SIDE):
        for lon in np.linspace(w,e,DEM_SIDE):
            x,y=tile_xy(float(lat),float(lon));px=x*256-.5;py=y*256-.5;ix=math.floor(px);iy=math.floor(py);fx=px-ix;fy=py-iy
            corners=[]
            for dx,dy in ((0,0),(1,0),(0,1),(1,1)):
                gx,gy=ix+dx,iy+dy;key=(ZOOM,gx//256,gy//256);image,meta=source.image(key);used[key]=meta
                r,g,b=image[gy%256,gx%256];corners.append(r*256+g+b/256-32768)
            value=(corners[0]*(1-fx)+corners[1]*fx)*(1-fy)+(corners[2]*(1-fx)+corners[3]*fx)*fy
            require(math.isfinite(value) and -12000<value<9000,"NoData/implausible source elevation; no authored substitute")
            values.append(value)
    return np.asarray(values,dtype="<f4"),[{"z":k[0],"x":k[1],"y":k[2],**v} for k,v in sorted(used.items())]


def surface_grid(document,focus):
    lon0,lat0=focus["longitude"],focus["latitude"];mx,my=axes(lat0)
    def coords(sequence):return [((p["lon"]-lon0)*mx,(p["lat"]-lat0)*my) for p in sequence if "lat" in p and "lon" in p]
    def polygon(element):
        if element["type"]=="way":
            ring=coords(element.get("geometry",[]))
            return shapely.make_valid(Polygon(ring)) if len(ring)>=4 and ring[0]==ring[-1] else None
        outer=[];inner=[]
        for member in element.get("members",[]):
            points=coords(member.get("geometry",[]))
            if len(points)>=2:(inner if member.get("role")=="inner" else outer).append(LineString(points))
        if not outer:return None
        shells=list(polygonize(unary_union(outer)))
        if not shells:return None
        result=unary_union(shells)
        holes=list(polygonize(unary_union(inner))) if inner else []
        return shapely.make_valid(result.difference(unary_union(holes))) if holes else shapely.make_valid(result)
    coasts=[];waters=[];forests=[];farms=[];rivers=[];feature_counts={"coastline_ways":0,"water_polygons":0,"forest_polygons":0,"agriculture_polygons":0,"waterway_lines":0}
    for element in document["elements"]:
        tags=element.get("tags",{})
        if tags.get("natural")=="coastline" and element["type"]=="way":
            points=coords(element.get("geometry",[]))
            if len(points)>=2:coasts.extend(zip(points,points[1:]));feature_counts["coastline_ways"]+=1
            continue
        if tags.get("waterway") in ("river","stream","canal","drain") and element["type"]=="way":
            points=coords(element.get("geometry",[]))
            if len(points)>=2:rivers.append(LineString(points));feature_counts["waterway_lines"]+=1
            continue
        shape=polygon(element)
        if shape is None or shape.is_empty:continue
        if tags.get("natural")=="water" or tags.get("waterway")=="riverbank":waters.append(shape);feature_counts["water_polygons"]+=1
        if tags.get("natural")=="wood" or tags.get("landuse")=="forest":forests.append(shape);feature_counts["forest_polygons"]+=1
        if tags.get("landuse") in ("farmland","farmyard","orchard","vineyard"):farms.append(shape);feature_counts["agriculture_polygons"]+=1
    w,s,e,n=bounds(focus);xs=np.linspace((w-lon0)*mx,(e-lon0)*mx,MASK_SIDE);ys=np.linspace((n-lat0)*my,(s-lat0)*my,MASK_SIDE)
    gx,gy=np.meshgrid(xs,ys);xy=np.column_stack((gx.ravel(),gy.ravel()));points=shapely.points(xy)
    flags=np.zeros(len(points),dtype=np.uint8);coast_distance=np.full(len(points),3276.7);river_distance=np.full(len(points),6553.5)
    if coasts:
        require(len(coasts)<=30000,"Source coastline complexity exceeds bounded raster compiler budget")
        a=np.asarray([segment[0] for segment in coasts]);b=np.asarray([segment[1] for segment in coasts]);delta=b-a;length=np.sum(delta*delta,axis=1);length=np.maximum(length,1e-12)
        for start in range(0,len(points),256):
            p=xy[start:start+256];offset=p[:,None,:]-a[None,:,:];t=np.clip(np.sum(offset*delta[None,:,:],axis=2)/length[None,:],0,1)
            distance2=np.sum((offset-t[:,:,None]*delta[None,:,:])**2,axis=2);nearest=np.argmin(distance2,axis=1);index=np.arange(len(p));d=np.sqrt(distance2[index,nearest]);v=offset[index,nearest];edge=delta[nearest]
            # OSM coastline direction has land to its left. Local coordinates use north-positive Y.
            land=edge[:,0]*v[:,1]-edge[:,1]*v[:,0]>=0
            coast_distance[start:start+len(p)]=np.where(land,d,-d)
        flags[coast_distance<0]|=1
    def apply(shapes,bit):
        if shapes:
            union=unary_union(shapes);inside=shapely.intersects(union,points);flags[inside]|=bit
            return union
        return None
    water=apply(waters,2);apply(forests,4);apply(farms,8)
    if water is not None:river_distance=np.minimum(river_distance,shapely.distance(water,points))
    if rivers:
        river=unary_union(rivers);river_distance=np.minimum(river_distance,shapely.distance(river,points))
        # Source centerlines with unrecorded widths get one SOURCE raster-pixel
        # footprint, explicitly quantized rather than a surveyed channel width.
        raster_radius=math.hypot(xs[1]-xs[0],ys[1]-ys[0])*.5
        flags[shapely.distance(river,points)<=raster_radius]|=16
    flags[(flags&1)!=0]&=np.uint8(1) # Ocean is never interpreted as inland forest/farm/river.
    distances=np.rint(np.clip(river_distance,0,6553.5)*10).astype("<u2")
    signed_coast=np.rint(np.clip(coast_distance,-3276.7,3276.7)*10).astype("<i2")
    coverage={"ocean_pixels":int(np.count_nonzero(flags&1)),"inland_water_pixels":int(np.count_nonzero(flags&2)),"mapped_forest_pixels":int(np.count_nonzero(flags&4)),"mapped_agriculture_pixels":int(np.count_nonzero(flags&8)),"mapped_waterway_pixels":int(np.count_nonzero(flags&16)),"unknown_landcover_pixels":int(np.count_nonzero((flags&12)==0))}
    return flags,distances,signed_coast,bool(coasts),feature_counts,coverage


def export_source_vectors(document,focus,path):
    """Small auditable ODbL line/polygon subset; raster compilation uses full originals."""
    clip=box(*bounds(focus));features=[]
    for element in document["elements"]:
        tags=element.get("tags",{})
        if element["type"]!="way" or not (tags.get("natural") in ("coastline","water") or tags.get("waterway") in ("river","canal","riverbank")):continue
        coords=[(p["lon"],p["lat"]) for p in element.get("geometry",[]) if "lat" in p and "lon" in p]
        if len(coords)<2:continue
        polygon=tags.get("natural")=="water" or tags.get("waterway")=="riverbank"
        if polygon and (len(coords)<4 or coords[0]!=coords[-1]):continue
        shape=shapely.make_valid(Polygon(coords) if polygon else LineString(coords)).intersection(clip).simplify(.00001,preserve_topology=True)
        if shape.is_empty:continue
        features.append({"type":"Feature","properties":{"osm_type":"way","osm_id":element["id"],**{k:tags[k] for k in ("name","natural","waterway","water","width") if k in tags}},"geometry":mapping(shape)})
    data={"type":"FeatureCollection","license":"ODbL-1.0","attribution":"© OpenStreetMap contributors; https://www.openstreetmap.org/copyright",
          "scope":"Clipped/simplified coastline, main river/canal and closed water-way geometry examples only, not a complete OSM extract. Raster payload uses complete queried geometry, including relations and landuse.","features":features}
    path.write_text(json.dumps(data,ensure_ascii=False,separators=(",",":"))+"\n",encoding="utf-8",newline="\n")
    return {"file":path.name,"sha256":digest(path.read_bytes()),"features":len(features),"license":"ODbL-1.0"}


def prefer_usgs_bare_earth(source,focus,key,baseline_heights,flags):
    """US bare earth first. Only explicit OSM-ocean NoData may keep Mapzen bathymetry."""
    w,s,e,n=bounds(focus);dx=(e-w)/(DEM_SIDE-1);dy=(n-s)/(DEM_SIDE-1)
    query={"bbox":",".join(map(str,(w-dx*.5,s-dy*.5,e+dx*.5,n+dy*.5))),"bboxSR":4326,"imageSR":4326,
           "size":"65,65","format":"tiff","pixelType":"F32","interpolation":"RSP_BilinearInterpolation",
           "renderingRule":json.dumps({"rasterFunction":"None"},separators=(",",":")),"adjustAspectRatio":"false","f":"image"}
    url=USGS_EXPORT+"?"+urllib.parse.urlencode(query)
    raw,record=source.request(url,CACHE/f"usgs/{key}-{digest(url.encode())[:12]}.tif",2*1024*1024,"usgs-bare-earth")
    image=Image.open(io.BytesIO(raw));require(image.size==(65,65) and image.mode=="F","USGS export is not a 65-square F32 elevation raster")
    scale=image.tag_v2.get(33550);tie=image.tag_v2.get(33922);geo=image.tag_v2.get(34735,())
    require(scale and tie and len(scale)>=2 and len(tie)>=6,"USGS GeoTIFF georeferencing missing")
    require(abs(scale[0]-dx)<1e-11 and abs(scale[1]-dy)<1e-11 and abs(tie[3]-(w-dx*.5))<1e-10 and abs(tie[4]-(n+dy*.5))<1e-10,"USGS export moved/rescaled the requested sample grid")
    keys={geo[i]:tuple(geo[i+1:i+4]) for i in range(4,len(geo),4)}
    require(keys.get(2048)==(0,1,4326),"USGS export is not EPSG4326")
    heights=np.asarray(image,dtype="<f4").ravel();valid=np.isfinite(heights)&(heights>-12000)&(heights<9000)
    nodata=image.tag_v2.get(42113)
    if nodata is not None:
        value=float(str(nodata).strip("\x00"));valid&=heights!=value
    ocean=(np.asarray(flags).reshape(257,257)[::4,::4].ravel()&1)!=0
    require(not np.any((~valid)&(~ocean)),"USGS bare-earth has missing land elevation; no mixed-DSM land substitute permitted")
    require(np.any(valid),"USGS bare-earth raster contains no valid elevation")
    merged=np.asarray(baseline_heights,dtype="<f4").copy();merged[valid]=heights[valid]
    return merged,{**record,"provider":"USGS 3DEP Bare Earth DEM Dynamic Service","license":"US public-domain data; no use restrictions, https://www.usgs.gov/3d-elevation-program/about-3dep-products-services",
                   "query":query,"raster_function":"None","pixel_type":"F32","image_crs":"EPSG:4326","sample_alignment":"65 pixel centres coincide with source bbox endpoints; export extent expanded half a sample at each edge",
                   "usgs_valid_sample_count":int(np.count_nonzero(valid)),"mapzen_ocean_nodata_fallback_count":int(np.count_nonzero(~valid)),"mapzen_land_fallback_count":0,
                   "qualification":"Source bare-earth topography, not surveyed parcel ground or river/ocean bathymetry. Only explicit OSM-ocean NoData uses the recorded Mapzen baseline; every land sample must have valid USGS coverage."}


def upgrade_existing_usgs_packet(source,region,focus,key,packet):
    if region["id"] not in BARE_EARTH_REGIONS or "preferred_bare_earth_source" in packet:return False
    raw=zlib.decompress(base64.b64decode(packet["zlib_payload_base64"]));baseline=np.frombuffer(raw,dtype="<f4",count=DEM_SIDE**2)
    flags=np.frombuffer(raw,dtype=np.uint8,count=MASK_SIDE**2,offset=DEM_SIDE**2*4)
    heights,meta=prefer_usgs_bare_earth(source,focus,key,baseline,flags)
    packet["mapzen_baseline_dem_extrema_m"]=[packet["dem_min_m"],packet["dem_max_m"]]
    raw=heights.tobytes()+raw[DEM_SIDE**2*4:];packed=zlib.compress(raw,9)
    packet.update({"preferred_bare_earth_source":meta,"dem_min_m":float(np.min(heights)),"dem_max_m":float(np.max(heights)),"uncompressed_sha256":digest(raw),"compressed_sha256":digest(packed),"zlib_payload_base64":base64.b64encode(packed).decode("ascii")})
    packet["vertical_datum"]="USGS 3DEP source vertical datum in metres; source-dependent survey/standard DEM datum, not ellipsoid or cadastral survey. Explicit ocean NoData only retains Mapzen source metres."
    packet["source_licenses"]["preferred_bare_earth"]="USGS 3DEP public-domain data without use restrictions, https://www.usgs.gov/3d-elevation-program/about-3dep-products-services"
    print(json.dumps({"focus":key,"usgs_bare_earth_upgrade":True,"dem_min_m":packet["dem_min_m"],"dem_max_m":packet["dem_max_m"],"usgs_valid_samples":meta["usgs_valid_sample_count"],"ocean_fallback_samples":meta["mapzen_ocean_nodata_fallback_count"]}),flush=True)
    return True


def generate_native(catalogue):
    rows=[];arrays=[]
    for region in catalogue["regions"]:
        for focus in region["focuses"]:
            key=region["id"]+"_"+focus["id"];path=OUT/(key+".json")
            if not path.exists():rows.append("{} /* unavailable: no real source substitute */");continue
            packet=json.loads(path.read_text(encoding="utf-8"));packed=base64.b64decode(packet["zlib_payload_base64"]);raw=zlib.decompress(packed)
            require(digest(packed)==packet["compressed_sha256"] and digest(raw)==packet["uncompressed_sha256"],"Corrupt source packet")
            name="GeoTile_"+key
            arrays.append("static constexpr uint8 "+name+"[] = {\n"+"\n".join(",".join(str(b) for b in packed[i:i+32])+"," for i in range(0,len(packed),32))+"\n};\n")
            b=packet["bounds_wgs84"]
            rows.append("{ "+", ".join([*(format(v,".15g") for v in b),name,str(len(packed)),str(len(raw)),"true" if packet["has_coastline_source"] else "false"])+" }")
    output="// GENERATED real bounded source data; scripts/fetch_real_geographic_tiles.py. No authored fallback.\n"+"".join(arrays)+"static const FCompiledGeographicTile CompiledGeographicTiles[] = {\n"+",\n".join(rows)+"\n};\n"
    temporary=NATIVE.with_suffix(NATIVE.suffix+".tmp")
    temporary.write_text(output,encoding="utf-8",newline="\n")
    os.replace(temporary,NATIVE) # UBT can only observe a complete old/new archive.


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--offline",action="store_true");parser.add_argument("--focus",action="append",help="region/focus; default all 25");parser.add_argument("--osm-endpoint",choices=OSM_ENDPOINTS,default=OVERPASS_ALTERNATE);args=parser.parse_args()
    catalogue=json.loads(CATALOG.read_text(encoding="utf-8"));flat=[(r,f) for r in catalogue["regions"] for f in r["focuses"]]
    selected=[(r,f) for r,f in flat if not args.focus or r["id"]+"/"+f["id"] in args.focus]
    require(selected and (not args.focus or len(selected)==len(set(args.focus))),"Unknown/duplicate focus selection")
    planned=set().union(*(tile_keys(f) for _,f in selected));require(len(planned)<=MAX_TILES,"Planned DEM tile hard cap exceeded")
    OUT.mkdir(parents=True,exist_ok=True);CACHE.mkdir(parents=True,exist_ok=True);source=Sources(args.offline,args.osm_endpoint)
    print(json.dumps({"begin_utc":now(),"focus_count":len(selected),"planned_dem_tiles":len(planned),"hard_tile_cap":MAX_TILES,"hard_dem_bytes":MAX_DEM_BYTES,"dem_side":DEM_SIDE,"surface_side":MASK_SIDE}),flush=True)
    try:
        for region,focus in selected:
            key=region["id"]+"_"+focus["id"];file=OUT/(key+".json")
            if file.exists():
                existing=json.loads(file.read_text(encoding="utf-8"));require(existing["bounds_wgs84"]==bounds(focus),"Cached packet bounds changed")
                changed=upgrade_existing_usgs_packet(source,region,focus,key,existing)
                if "preferred_bare_earth_source" in existing:
                    datum="USGS 3DEP source vertical datum in metres; source-dependent survey/standard DEM datum, not ellipsoid or cadastral survey. Explicit ocean NoData only retains Mapzen source metres."
                    credit="USGS 3DEP public-domain data without use restrictions, https://www.usgs.gov/3d-elevation-program/about-3dep-products-services"
                    if existing["vertical_datum"]!=datum or existing["source_licenses"].get("preferred_bare_earth")!=credit:
                        existing["vertical_datum"]=datum;existing["source_licenses"]["preferred_bare_earth"]=credit;changed=True
                if "source_geometry_examples" not in existing:
                    original=CACHE/f"osm/{key}-{existing['osm_source']['query_sha256'][:12]}.json"
                    require(original.exists() and digest(original.read_bytes())==existing["osm_source"]["sha256"],"Missing original OSM cache for vector provenance")
                    existing["source_geometry_examples"]=export_source_vectors(json.loads(original.read_text(encoding="utf-8")),focus,OUT/(key+"-waterways.geojson"))
                    changed=True
                if changed:file.write_text(json.dumps(existing,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
                print(json.dumps({"focus":key,"packet_cache":True}),flush=True);continue
            heights,dem_sources=elevation_grid(source,focus);document,osm_source=source.osm(region,focus)
            flags,river,coast,has_coast,features,coverage=surface_grid(document,focus)
            bare_earth_meta=None
            if region["id"] in BARE_EARTH_REGIONS:heights,bare_earth_meta=prefer_usgs_bare_earth(source,focus,key,heights,flags)
            raw=heights.tobytes()+flags.tobytes()+river.tobytes()+coast.tobytes();packed=zlib.compress(raw,9)
            packet={"schema_version":1,"region_id":region["id"],"focus_id":focus["id"],"compiled_utc":now(),"bounds_wgs84":bounds(focus),
                    "anchor_latitude":focus["latitude"],"anchor_longitude":focus["longitude"],"dem_side":DEM_SIDE,"surface_side":MASK_SIDE,
                    "projection":"WGS84 geographic source bbox; local WGS84 ellipsoid tangent metres east/north for OSM rasterization; game +Y south",
                    "vertical_datum":"Source terrain mean-sea-level metres; Mapzen source-dependent vertical datum, not cadastral/ellipsoid survey",
                    "dem_min_m":float(np.min(heights)),"dem_max_m":float(np.max(heights)),"has_coastline_source":has_coast,
                    "layout":"little-endian float32 DEM[65*65], uint8 OSM flags[257*257], uint16 river-distance decimetres[257*257], int16 signed-coast-distance decimetres[257*257]; row-major north to south",
                    "mask_bits":{"ocean":1,"inland_water":2,"mapped_forest":4,"mapped_agriculture":8,"waterway_centerline_raster_footprint":16},
                    "qualification":"Mapped source geometry rasterized to a bounded 257-square grid; OSM absence remains unknown landuse. Unspecified waterway widths are quantized to one raster-pixel footprint. No DEM-based ocean inference, geotechnical claim, new hazard model or runtime network.",
                    "osm_features":features,"surface_coverage":coverage,"dem_sources":dem_sources,"osm_source":osm_source,
                    "source_licenses":{"dem":"Source-specific Mapzen terrain attribution, https://github.com/tilezen/joerd/blob/master/docs/attribution.md","osm_surface_tables":"ODbL-1.0 https://opendatacommons.org/licenses/odbl/1-0/"},
                    "uncompressed_sha256":digest(raw),"compressed_sha256":digest(packed),"zlib_payload_base64":base64.b64encode(packed).decode("ascii")}
            packet["source_geometry_examples"]=export_source_vectors(document,focus,OUT/(key+"-waterways.geojson"))
            if bare_earth_meta:
                packet["preferred_bare_earth_source"]=bare_earth_meta
                packet["vertical_datum"]="USGS 3DEP source vertical datum in metres; source-dependent survey/standard DEM datum, not ellipsoid or cadastral survey. Explicit ocean NoData only retains Mapzen source metres."
                packet["source_licenses"]["preferred_bare_earth"]="USGS 3DEP public-domain data without use restrictions, https://www.usgs.gov/3d-elevation-program/about-3dep-products-services"
            file.write_text(json.dumps(packet,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
            generate_native(catalogue)
            print(json.dumps({"focus":key,"dem_min_m":packet["dem_min_m"],"dem_max_m":packet["dem_max_m"],"source_features":features,"coverage":coverage,"compressed_bytes":len(packed),"utc":now()}),flush=True)
    finally:
        log={"utc":now(),"dem_tile_count":len(source.dem_keys),"dem_source_bytes":source.dem_bytes,"osm_source_bytes":source.osm_bytes,"osm_http_attempts":source.osm_http_attempts,"osm_attempts_by_query":source.osm_attempts_by_query,"hard_caps":{"dem_tiles":MAX_TILES,"dem_bytes":MAX_DEM_BYTES,"osm_total_bytes":MAX_OSM_BYTES,"osm_attempts_per_focus":6,"osm_attempts_per_run":150},"requests":source.log}
        log_path=OUT/"DOWNLOAD_LOG.json"
        history=json.loads(log_path.read_text(encoding="utf-8")) if log_path.exists() else {"runs":[]}
        if "runs" not in history:history={"runs":[history]}
        history["runs"].append(log)
        log_path.write_text(json.dumps(history,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
    generate_native(catalogue)
    print(json.dumps({"complete_utc":now(),"packets":len(list(OUT.glob('*.json')))-1,"dem_tiles":len(source.dem_keys),"dem_bytes":source.dem_bytes,"osm_bytes":source.osm_bytes}),flush=True)


if __name__=="__main__":main()
