"""Fetch a bounded CC0 PBR terrain library from the official Poly Haven API."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ASSETS = {"dry_ground_rocks": "DrySoil", "forrest_ground_01": "ForestFloor", "aerial_rocks_01": "Rock"}
ROLES = {"diffuse": ("Diffuse", "Diffuse"), "normal_dx": ("nor_dx", "NormalDX"), "roughness": ("Rough", "Roughness")}
MAX_TOTAL = 20 * 1024 * 1024


def fetch(url, limit):
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.hostname not in {"api.polyhaven.com", "dl.polyhaven.org"}:
        raise ValueError("Unexpected source host")
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "AftershockAssetPreparation/1.0"})
            with urllib.request.urlopen(request, timeout=30) as response:
                data = response.read(limit + 1)
            if len(data) > limit:
                raise ValueError("Source exceeds byte budget")
            return data
        except OSError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def main():
    total = 0
    for asset, slot in ASSETS.items():
        destination = ROOT / "_Imports/PolyHaven" / asset
        destination.mkdir(parents=True, exist_ok=True)
        metadata = json.loads(fetch("https://api.polyhaven.com/files/" + asset, 2 * 1024 * 1024))
        info = json.loads(fetch("https://api.polyhaven.com/info/" + asset, 1024 * 1024))
        manifest = {"asset": asset, "slot": slot, "license": "CC0-1.0", "source_page": "https://polyhaven.com/a/" + asset,
                    "license_url": "https://polyhaven.com/license", "retrieved_utc": datetime.now(timezone.utc).isoformat(),
                    "api_info": info, "resolution": "1k", "displacement": False, "files": []}
        for role, (api_key, suffix) in ROLES.items():
            item = metadata[api_key]["1k"]["jpg"]
            if item["size"] > 4 * 1024 * 1024 or total + item["size"] > MAX_TOTAL:
                raise ValueError("Terrain texture budget exceeded")
            name = Path(urllib.parse.urlparse(item["url"]).path).name
            if not name.startswith(asset + "_") or not name.endswith("_1k.jpg"):
                raise ValueError("Unexpected source filename")
            path = destination / name
            data = path.read_bytes() if path.exists() else b""
            if len(data) != item["size"] or hashlib.md5(data).hexdigest() != item["md5"]:
                data = fetch(item["url"], 4 * 1024 * 1024)
            if not data.startswith(b"\xff\xd8") or len(data) != item["size"] or hashlib.md5(data).hexdigest() != item["md5"]:
                raise ValueError("Source checksum or format mismatch")
            temporary = path.with_suffix(".jpg.part")
            temporary.write_bytes(data)
            temporary.replace(path)
            total += len(data)
            manifest["files"].append({"role": role, "texture_name": "T_Regional" + slot + "_" + suffix,
                "file": name, "bytes": len(data), "md5": item["md5"], "sha256": hashlib.sha256(data).hexdigest(), "url": item["url"]})
        (destination / "SOURCE_LICENSE_MANIFEST.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps({"asset": asset, "slot": slot, "files": 3, "bytes_total": total}), flush=True)


if __name__ == "__main__":
    main()
