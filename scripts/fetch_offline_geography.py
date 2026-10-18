"""Build twenty original region identities and twenty-five offline DEM focuses.

The original seven provider response bytes are locked. Runtime never uses the
network. A focus is ready only with a validated, finite real provider response.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import pathlib
import sys
import time
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "_Imports/Geography"
CATALOG = OUT / "REGION_CATALOG.json"
PRIVATE = ROOT / "Source/JamProject/Aftershock/Private"
HAZARDS = ("Earthquake", "Flood", "Wind", "Fire", "Tsunami", "Volcano", "Drought")
PROFILE_KEYS = ("relief_cm", "moisture_01", "forest_fuel_01", "agriculture_01",
                "fault_influence_01", "valley_wind_exposure_01",
                "wind_direction_degrees", "river_center_01")
LEGACY_IDS = ("atakent", "dereyolu", "yelova", "kiyikent", "mugla", "etna", "konya")
LEGACY_WINDOWS = ((41.041, 28.775, .045, .060), (40.912, 40.112, .045, .060),
                  (36.490, 36.190, .045, .060), (36.090, 35.980, .045, .060),
                  (36.855, 28.274, .045, .060), (37.730, 15.015, .070, .080),
                  (37.715, 33.550, .045, .060))
LAST_REQUEST_AT = 0.0


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def load_catalogue() -> dict:
    data = json.loads(CATALOG.read_text(encoding="utf-8"))
    regions = data["regions"]
    if len(regions) != 20 or tuple(data["hazard_order"]) != HAZARDS:
        raise ValueError("Expected twenty regions and the canonical seven-hazard order")
    if tuple(r["id"] for r in regions[:7]) != LEGACY_IDS:
        raise ValueError("Legacy region identity/order changed")
    for index, region in enumerate(regions[:7]):
        focus = region["focuses"][0]
        window = tuple(focus[k] for k in ("latitude", "longitude", "half_latitude_extent", "half_longitude_extent"))
        if window != LEGACY_WINDOWS[index] or focus.get("legacy_cache_key") != LEGACY_IDS[index]:
            raise ValueError("Legacy DEM request coordinates/cache identity changed")
    if len({r["id"] for r in regions}) != len(regions):
        raise ValueError("Duplicate region ID")
    for region in regions:
        if region["primary_hazard"] not in HAZARDS or len(region["weights"]) != 7:
            raise ValueError("Invalid hazard definition: " + region["id"])
        if not all(type(w) in (int, float) and math.isfinite(w) and 0 <= w <= 1
                   for w in region["weights"]):
            raise ValueError("Invalid authored weight: " + region["id"])
        focuses = region["focuses"]
        if not focuses or len({f["id"] for f in focuses}) != len(focuses):
            raise ValueError("Empty/duplicate focus: " + region["id"])
        west, south, east, north = region["regional_bounds"]
        if not (-180 <= west < east <= 180 and -90 <= south < north <= 90):
            raise ValueError("Invalid regional bounds: " + region["id"])
        for focus in focuses:
            lat, lon = focus["latitude"], focus["longitude"]
            dy, dx = focus["half_latitude_extent"], focus["half_longitude_extent"]
            if not (south <= lat <= north and west <= lon <= east
                    and 0 < dy < 1 and 0 < dx < 1):
                raise ValueError("Invalid local focus: " + region["id"] + "/" + focus["id"])
            p = focus["landscape"]
            if set(p) != set(PROFILE_KEYS):
                raise ValueError("Invalid landscape keys: " + focus["id"])
            if not all(type(v) in (int, float) and math.isfinite(v) for v in p.values()):
                raise ValueError("Invalid landscape values: " + focus["id"])
            if not (0 < p["relief_cm"] <= 2000 and
                    all(0 <= p[k] <= 1 for k in PROFILE_KEYS if k.endswith("_01"))):
                raise ValueError("Landscape outside authored range: " + focus["id"])
            if focus["coast_edge"] not in ("None", "North", "East", "South", "West"):
                raise ValueError("Invalid coast edge: " + focus["id"])
            if focus["has_coast"] != (focus["coast_edge"] != "None"):
                raise ValueError("Coast edge/gate mismatch: " + focus["id"])
            if focus["has_volcanic_source"]:
                src = region.get("volcano_source")
                if not src or not (lat-dy <= src["latitude"] <= lat+dy
                                   and lon-dx <= src["longitude"] <= lon+dx):
                    raise ValueError("Mapped volcanic source outside local DEM window")
        if region["weights"][HAZARDS.index(region["primary_hazard"])] <= 0:
            raise ValueError("Disabled primary hazard: " + region["id"])
    return data


def sampling_request(focus: dict) -> tuple[str, str]:
    lat, lon = focus["latitude"], focus["longitude"]
    dy, dx = focus["half_latitude_extent"], focus["half_longitude_extent"]
    lats = [lat + dy - 2*dy*y/8 for y in range(9) for _ in range(9)]
    lons = [lon - dx + 2*dx*x/8 for _ in range(9) for x in range(9)]
    query = "latitude=" + ",".join(f"{v:.6f}" for v in lats)
    query += "&longitude=" + ",".join(f"{v:.6f}" for v in lons)
    return "https://api.open-meteo.com/v1/elevation?" + query, digest(query.encode("ascii"))


def valid_elevation(raw: bytes) -> list:
    values = json.loads(raw)["elevation"]
    if (len(values) != 81 or
            not all(type(v) in (int, float) and math.isfinite(v) and -500 <= v <= 9000
                    for v in values)):
        raise ValueError("Provider DEM must contain 81 finite plausible elevations")
    return values


def fetch_provider(url: str) -> bytes:
    """Respect the public endpoint limit; retry transient failures without proxies."""
    global LAST_REQUEST_AT
    for attempt in range(4):
        delay = max(0.0, 12.0 - (time.monotonic() - LAST_REQUEST_AT))
        if delay:
            time.sleep(delay)
        LAST_REQUEST_AT = time.monotonic()
        request = urllib.request.Request(url, headers={
            "User-Agent": "AFTERSHOCK educational original-game geography cache"})
        try:
            with urllib.request.urlopen(request, timeout=35) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code not in (429, 502, 503, 504) or attempt == 3:
                raise
            retry = error.headers.get("Retry-After", "")
            delay = float(retry) if retry.isdigit() else (60.0 if error.code == 429 else 12.0)
            print(f"Provider {error.code}; respecting backoff {delay:.0f}s (attempt {attempt+1}/4)", flush=True)
            time.sleep(max(12.0, delay))
    raise RuntimeError("Provider retry loop exhausted")


def read_focus(region: dict, focus: dict, catalogue: dict, offline: bool) -> dict:
    key = focus.get("legacy_cache_key", region["id"] + "_" + focus["id"])
    path = OUT / (key + "_glo90.json")
    request_path = OUT / (key + "_glo90_request.json")
    url, request_sha = sampling_request(focus)
    legacy_sha = catalogue["legacy_sample_sha256"].get(key)
    if path.exists():
        raw = path.read_bytes()
        values = valid_elevation(raw)
        if legacy_sha:
            if digest(raw) != legacy_sha:
                raise ValueError("Legacy response bytes changed: " + key)
            metadata = {"retrieved_utc": "2026-10-17T10:32:10.633584+00:00",
                        "request_sha256": request_sha, "url": url}
        else:
            metadata = json.loads(request_path.read_text(encoding="utf-8"))
            if (metadata["url"] != url or metadata["request_sha256"] != request_sha
                    or metadata["response_sha256"] != digest(raw)):
                raise ValueError("Cached response/query mismatch: " + key)
    else:
        if offline:
            raise FileNotFoundError("No provider response: " + key)
        raw = fetch_provider(url)
        values = valid_elevation(raw)
        if legacy_sha and digest(raw) != legacy_sha:
            raise ValueError("Missing legacy fixture has different provider revision: " + key)
        metadata = {"retrieved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "url": url, "request_sha256": request_sha, "response_sha256": digest(raw)}
        path.write_bytes(raw)
        request_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                                encoding="utf-8")
    return {"id": key, "region_id": region["id"], "focus_id": focus["id"],
            "label": region["label"], "focus_label": focus["label"],
            "latitude": focus["latitude"], "longitude": focus["longitude"],
            "bbox_wgs84": [focus["longitude"]-focus["half_longitude_extent"],
                          focus["latitude"]-focus["half_latitude_extent"],
                          focus["longitude"]+focus["half_longitude_extent"],
                          focus["latitude"]+focus["half_latitude_extent"]],
            "width": 9, "height": 9,
            "sample_order": "row-major north-to-south; columns west-to-east",
            "min_m": min(values), "max_m": max(values), "url": url,
            "file": path.name, "sha256": digest(raw), "request_sha256": request_sha,
            "retrieved_utc": metadata["retrieved_utc"], "ready": True, "values": values}


def text(value: str) -> str:
    return "TEXT(" + json.dumps(value, ensure_ascii=False) + ")"


def number(value: float) -> str:
    return format(value, ".12g") + (".0" if float(value).is_integer() else "")


def profile(focus: dict) -> str:
    return "{ " + ", ".join(number(focus["landscape"][k]) + "f" for k in PROFILE_KEYS) + " }"


def boolean(value: bool) -> str:
    return "true" if value else "false"


def generate_native(catalogue: dict, sites: list) -> tuple[str, str]:
    flat = [(r, f) for r in catalogue["regions"] for f in r["focuses"]]
    elevation = "// GENERATED by scripts/fetch_offline_geography.py; see _Imports/Geography provenance.\n"
    elevation += f"static constexpr float OfflineElevationMetres[{len(flat)}][81] = {{\n"
    # Missing entries are deliberately unavailable placeholders, not authored DEM.
    elevation += ",\n".join(
        "{ " + ", ".join(f"{v:.2f}f" for v in s["values"]) + " }" if s["ready"]
        else "{} /* unavailable; ready=false prohibits sampling */" for s in sites) + "\n};\n"
    elevation += "static constexpr bool OfflineElevationFocusReady[] = { " + ", ".join(
        boolean(s["ready"]) for s in sites) + " };\n"
    native = "// GENERATED from REGION_CATALOG.json; authored policy, real DEM cached separately.\n"
    native += "static const FAftershockGeographicLocation GeographicFocuses[] = {\n"
    for r, f in flat:
        native += "\t{ " + ", ".join([text(r["id"]), text(r["label"]), number(f["latitude"]),
            number(f["longitude"]), number(f["half_latitude_extent"]),
            number(f["half_longitude_extent"]), text(f["id"]), text(f["label"])]) + " },\n"
    native += "};\nstatic constexpr FRegionFocusRange GeographicRegions[] = {\n"
    offset = 0
    for r in catalogue["regions"]:
        native += f"\t{{ {offset}, {len(r['focuses'])} }}, // {r['id']}\n"
        offset += len(r["focuses"])
    native += "};\nstatic const FAftershockLocalFocusRules GeographicFocusRules[] = {\n"
    for _, f in flat:
        native += "\t{ " + ", ".join([boolean(f["has_coast"]), boolean(f["has_river"]),
            boolean(f["has_volcanic_source"]), "EAftershockCoastEdge::" + f["coast_edge"],
            profile(f)]) + " },\n"
    native += "};\nstatic const FAftershockRegionRules GeographicRegionRules[] = {\n"
    for r in catalogue["regions"]:
        f = r["focuses"][0]
        src = r.get("volcano_source", {"latitude": 0, "longitude": 0})
        native += "\t{ " + ", ".join([text(r["id"]), "EDisasterType::" + r["primary_hazard"],
            "{ " + ", ".join(number(w) + "f" for w in r["weights"]) + " }",
            boolean(f["has_coast"]), boolean(f["has_river"]), boolean(f["has_volcanic_source"]),
            boolean(r.get("storm_surge_context", False)), profile(f),
            "{ " + ", ".join(number(v) for v in r["regional_bounds"]) + " }",
            number(src["latitude"]), number(src["longitude"]), text(r["qualification"])]) + " },\n"
    return elevation, native + "};\n"


def refresh_natural_earth(manifest: dict) -> None:
    # Optional existing coastline rendering, independent of local DEM readiness.
    url = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_land.geojson"
    geo = OUT / "ne_110m_land.geojson"
    if not geo.exists():
        with urllib.request.urlopen(url, timeout=35) as response:
            geo.write_bytes(response.read())
    from PIL import Image, ImageDraw
    image = Image.new("RGB", (2048, 1024), (7, 25, 48))
    draw = ImageDraw.Draw(image)
    for feature in json.loads(geo.read_text(encoding="utf-8"))["features"]:
        g = feature["geometry"]
        polygons = [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]
        for polygon in polygons:
            for i, ring in enumerate(polygon):
                pts = [((p[0]+180)/360*2048, (90-p[1])/180*1024) for p in ring]
                draw.polygon(pts, fill=(56, 84, 52) if i == 0 else (7, 25, 48))
                if i == 0:
                    draw.line(pts, fill=(92, 115, 69), width=1)
    image.save(OUT / "T_NaturalEarthLand.png")
    manifest["natural_earth"] = {
        "source": url, "terms": "https://www.naturalearthdata.com/about/terms-of-use/",
        "license": "Public domain", "credit": "Made with Natural Earth",
        "source_sha256": digest(geo.read_bytes()), "derived_texture": "T_NaturalEarthLand.png",
        "projection": "equirectangular lon[-180,180],lat[90,-90]",
        "qualification": "1:110m coastline-only cartographic rendering; not satellite imagery"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="Never fetch missing provider data")
    parser.add_argument("--check", action="store_true", help="Read-only cache/generated-file verification")
    parser.add_argument("--refresh-natural-earth", action="store_true",
                        help="Opt in to rebuilding the existing coastline texture")
    parser.add_argument("--skip-natural-earth", action="store_true",
                        help="Compatibility flag; texture refresh is now opt-in")
    args = parser.parse_args()
    catalogue = load_catalogue()
    sites, failures = [], []
    for r in catalogue["regions"]:
        for f in r["focuses"]:
            try:
                site = read_focus(r, f, catalogue, args.offline or args.check)
            except (OSError, ValueError, KeyError, TypeError) as error:
                failures.append(r["id"] + "/" + f["id"] + ": " + str(error))
                site = {"region_id": r["id"], "focus_id": f["id"], "ready": False, "error": str(error)}
            sites.append(site)
            print(r["id"] + "/" + f["id"], "ready" if site["ready"] else "UNAVAILABLE",
                  (site.get("min_m"), site.get("max_m")), flush=True)
    elevation, native = generate_native(catalogue, sites)
    generated = {PRIVATE / "AftershockOfflineElevation.inl": elevation,
                 PRIVATE / "AftershockRegionCatalog.inl": native}
    if args.check:
        for path, expected in generated.items():
            if not path.exists() or path.read_text(encoding="utf-8") != expected:
                failures.append("Generated output stale: " + str(path.relative_to(ROOT)))
    else:
        for path, content in generated.items():
            path.write_text(content, encoding="utf-8", newline="\n")
        manifest_path = OUT / "SOURCE_LICENSE_MANIFEST.json"
        previous = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
        manifest = {
            "schema_version": 2, "catalogue_version": catalogue["catalogue_version"],
            "retrieved_utc": max((s["retrieved_utc"] for s in sites if s["ready"]), default=""),
            "dem": "Copernicus DEM 2021 GLO-90 via Open-Meteo Elevation API",
            "source": "https://open-meteo.com/en/docs/elevation-api",
            "attribution": "Contains modified Copernicus DEM data (2021), supplied by Open-Meteo. DOI:10.5270/ESA-c5d3d65",
            "dem_license": "Open-Meteo API data CC BY 4.0; contains modified Copernicus DEM data (2021)",
            "license_url": "https://creativecommons.org/licenses/by/4.0/",
            "provider_license_url": "https://open-meteo.com/en/licence",
            "qualification": catalogue["qualification"] + " 9x9 sparse samples per local focus, not 90m runtime cells. Regional bounds are not represented as a 250m playable corridor. No real data is fabricated for missing responses.",
            "modifications": "Sparse north-to-south nine-by-nine sampling, bilinear interpolation; runtime relief/river/soil/ecology are separately authored game overlays.",
            "catalogue_canonical_sha256": digest(json.dumps(catalogue, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")),
            "region_count": len(catalogue["regions"]), "focus_count": len(sites),
            "ready_focus_count": sum(s["ready"] for s in sites),
            "legacy_sample_sha256": catalogue["legacy_sample_sha256"],
            "hazard_primary_sources": [{"region": r["id"], "sources": r["sources"],
                "qualification": r["qualification"],
                **({"volcano_source": r["volcano_source"]} if "volcano_source" in r else {})}
                for r in catalogue["regions"]],
            "sites": [{k: v for k, v in s.items() if k != "values"} for s in sites]}
        if "natural_earth" in previous:
            manifest["natural_earth"] = previous["natural_earth"]
        if args.refresh_natural_earth and not args.skip_natural_earth:
            refresh_natural_earth(manifest)
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8", newline="\n")
    for failure in failures:
        print("BLOCKED:", failure, file=sys.stderr)
    print(f"REGION_CATALOG_RESULT regions={len(catalogue['regions'])} focuses={len(sites)} ready={sum(s['ready'] for s in sites)} failed={len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
