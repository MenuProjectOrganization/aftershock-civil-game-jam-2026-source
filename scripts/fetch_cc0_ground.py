"""Fetch the three lightweight CC0 Poly Haven leafy_grass PBR maps.

Run with ordinary Python 3.11+. No pip packages or Unreal Editor are required.
Only the 1k JPEG diffuse, DirectX normal and roughness maps are downloaded.
Official API file size and MD5 are verified before atomic destination writes;
SHA256, source URLs, API metadata and license attribution are retained locally.
"""

import datetime
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "_Imports" / "PolyHaven" / "leafy_grass"
API_URL = "https://api.polyhaven.com/files/leafy_grass"
INFO_URL = "https://api.polyhaven.com/info/leafy_grass"
USER_AGENT = "AftershockAssetPreparation/1.0"
MAPS = {"diffuse": "Diffuse", "normal_dx": "nor_dx", "roughness": "Rough"}


def request(url):
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=60) as response:
                return response.read()
        except OSError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def main():
    metadata = json.loads(request(API_URL))
    info = json.loads(request(INFO_URL))
    DESTINATION.mkdir(parents=True, exist_ok=True)
    manifest = {
        "asset": "leafy_grass", "name": "Leafy Grass", "provider": "Poly Haven",
        "license": "CC0-1.0", "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
        "provider_license_statement": "https://docs.polyhaven.com/en/faq",
        "source_page": "https://polyhaven.com/a/leafy_grass", "api_url": API_URL,
        "api_terms": "https://docs.polyhaven.com/en/api", "user_agent": USER_AGENT,
        "retrieved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "resolution": "1k", "format": "JPEG", "surface_size_cm": 200,
        "usage": "Ground diffuse, tangent DirectX normal and roughness; no displacement map.",
        "api_info": info, "files": [],
    }
    for role, api_key in MAPS.items():
        item = metadata[api_key]["1k"]["jpg"]
        url = item["url"]
        parsed = urllib.parse.urlparse(url)
        if parsed.scheme != "https" or parsed.hostname != "dl.polyhaven.org":
            raise RuntimeError("Unexpected asset download host: " + url)
        filename = Path(parsed.path).name
        if not filename.startswith("leafy_grass_") or not filename.endswith("_1k.jpg"):
            raise RuntimeError("Unexpected source filename: " + filename)
        destination = DESTINATION / filename
        data = destination.read_bytes() if destination.exists() else b""
        if len(data) != item["size"] or hashlib.md5(data).hexdigest() != item["md5"]:
            data = request(url)
        if len(data) != item["size"] or hashlib.md5(data).hexdigest() != item["md5"]:
            raise RuntimeError("Source size/MD5 mismatch: " + filename)
        if not data.startswith(b"\xff\xd8"):
            raise RuntimeError("Source is not JPEG: " + filename)
        temporary = destination.with_suffix(".jpg.part")
        temporary.write_bytes(data)
        temporary.replace(destination)
        manifest["files"].append({"role": role, "api_key": api_key, "url": url,
            "file": filename, "bytes": len(data), "md5": item["md5"],
            "sha256": hashlib.sha256(data).hexdigest()})
        print("VERIFIED {} bytes={} md5={}".format(filename, len(data), item["md5"]))
    (DESTINATION / "SOURCE_LICENSE_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (DESTINATION / "CREDITS.txt").write_text(
        "Leafy Grass — Poly Haven\nhttps://polyhaven.com/a/leafy_grass\n"
        f"Author(s): {', '.join(info.get('authors', {}))}\n"
        "License: CC0 1.0 Universal — public domain dedication.\n"
        "https://creativecommons.org/publicdomain/zero/1.0/\n"
        "Provider statement: https://docs.polyhaven.com/en/faq\n"
        "Downloaded through the official API using AftershockAssetPreparation/1.0.\n"
        "Original 1k diffuse, DirectX normal and roughness JPEG files.\n"
        "Retain SOURCE_LICENSE_MANIFEST.json for integrity, authors and provenance.\n",
        encoding="utf-8")
    print("GROUND_SOURCE_READY files={} bytes={} manifest={}".format(
        len(manifest["files"]), sum(item["bytes"] for item in manifest["files"]),
        DESTINATION / "SOURCE_LICENSE_MANIFEST.json"))


if __name__ == "__main__":
    main()
