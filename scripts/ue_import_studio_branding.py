"""Import the approved logo only; leave Config, splash BMPs and cook references to callers.

Run inside UE Python (ExecutePythonScript or the existing project script runner).
Offline preflight: python scripts/ue_import_studio_branding.py --preflight
No SVG rasterization, resizing, alpha premultiplication, deletion or asset migration occurs.
"""
import datetime
import hashlib
import json
from pathlib import Path
import struct
import sys
import traceback
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "_Imports/Branding/MetuCyberStudios"
MANIFEST = SOURCE_DIR / "SOURCE_AND_BUILD_MANIFEST.json"
SOURCE_NAME = "cyber-security-studios-white-2048.png"
SOURCE_SHA256 = "f20ff616cca6688aecd16f9b20cca29221719d550c561d65846654f6022c4e04"
ORIGINAL_SHA256 = "506cc6350fe2a1972bdc9ea92ce92e325254fd81deddc9dae6359e6d9aeb7f09"
DEST = "/Game/UI/Branding"
NAME = "T_CyberSecurityStudios"
ASSET = DEST + "/" + NAME
RECEIPT = ROOT / "Saved/PresentationValidation/studio_branding_import.json"


def need(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path, algorithm="sha256"):
    return hashlib.new(algorithm, Path(path).read_bytes()).hexdigest()


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_receipt(report):
    RECEIPT.parent.mkdir(parents=True, exist_ok=True)
    temporary = RECEIPT.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(RECEIPT)


def local_file(relative):
    path = (SOURCE_DIR / relative).resolve()
    need(path.is_relative_to(SOURCE_DIR.resolve()) and path.is_file(),
         "Missing or out-of-directory source: " + str(relative))
    return path


def verify_white_rgba_png(path):
    """Verify actual source alpha, including AA pixels, using only the standard library."""
    raw = path.read_bytes()
    need(raw[:8] == b"\x89PNG\r\n\x1a\n", "Not a PNG")
    offset, compressed, header, ended = 8, bytearray(), None, False
    while offset + 12 <= len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        kind = raw[offset + 4:offset + 8]
        end = offset + 12 + length
        need(end <= len(raw), "Truncated PNG chunk")
        data = raw[offset + 8:offset + 8 + length]
        need(zlib.crc32(kind + data) & 0xffffffff == struct.unpack_from(">I", raw, end - 4)[0],
             "PNG CRC failure")
        if kind == b"IHDR":
            need(header is None and len(data) == 13, "Invalid PNG IHDR")
            header = struct.unpack(">IIBBBBB", data)
        elif kind == b"IDAT":
            compressed.extend(data)
        elif kind == b"IEND":
            need(end == len(raw), "Trailing PNG bytes")
            ended = True
            break
        offset = end
    need(ended and header == (2048, 403, 8, 6, 0, 0, 0),
         "Expected exactly 2048x403, RGBA8, non-interlaced PNG")
    width, height = header[:2]
    stride = width * 4
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    pixels = decoder.decompress(bytes(compressed), expected + 1)
    need(len(pixels) == expected and decoder.eof and not decoder.unused_data,
         "Invalid PNG pixel payload length")
    previous = bytearray(stride)
    counts = {"opaque": 0, "transparent": 0, "antialiased": 0}
    for y in range(height):
        start = y * (stride + 1)
        mode = pixels[start]
        need(0 <= mode <= 4, "Invalid PNG filter")
        row = bytearray(pixels[start + 1:start + 1 + stride])
        for x in range(stride):
            a = row[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if mode == 1:
                predictor = a
            elif mode == 2:
                predictor = b
            elif mode == 3:
                predictor = (a + b) // 2
            elif mode == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            else:
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        for x in range(0, stride, 4):
            need(row[x:x + 3] == b"\xff\xff\xff", "Source RGB is not straight-alpha white")
            alpha = row[x + 3]
            counts["transparent" if alpha == 0 else "opaque" if alpha == 255 else "antialiased"] += 1
        previous = row
    need(all(counts.values()), "Logo must contain transparent, opaque and AA pixels")
    return {"width": width, "height": height, "channels": 4,
            "straight_alpha_white_rgb": True, "pixel_counts": counts}


def preflight():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    need(manifest.get("valid") is True and manifest.get("in_progress") is False,
         "Branding build manifest is not complete/valid")
    need(manifest.get("source_url") == "https://metucyber.org/assets/img/logo-white.svg"
         and manifest.get("source_sha256") == ORIGINAL_SHA256,
         "Unexpected original logo provenance")
    edit = manifest.get("edit", {})
    need(edit.get("replaced") == "SOCIETY" and edit.get("replacement") == "STUDIOS"
         and edit.get("preserved_source_geometry_verified") is True
         and edit.get("eagle_header_cyber_security_subtrees_exact") is True
         and edit.get("preserved_text") == ["METU NCC", "CYBER SECURITY"]
         and edit.get("tagline") == "gaming for today",
         "Unapproved logo edit")
    need(manifest.get("additional_fonts_or_third_party_art") == 1
         and manifest.get("font_provenance", {}).get("modified_font") is False
         and manifest["font_provenance"].get("license_file") == "fonts/bitstream-vera-license.txt",
         "Expected the bundled, licensed, unmodified tagline font")
    frozen = {str(MANIFEST): digest(MANIFEST)}
    seen = set()
    for row in manifest["files"]:
        path = local_file(row["path"])
        need(path not in seen, "Duplicate manifest file")
        seen.add(path)
        need(path.stat().st_size == row["bytes"] and digest(path) == row["sha256"],
             "Manifest hash/size mismatch: " + row["path"])
        frozen[str(path)] = row["sha256"]
    for row in manifest["generators"]:
        path = local_file(row["path"])
        need(digest(path) == row["sha256"], "Generator hash mismatch")
        frozen[str(path)] = row["sha256"]
    source = local_file(SOURCE_NAME)
    need(str(source) in frozen and frozen[str(source)] == SOURCE_SHA256,
         "Approved 2048 master missing/changed")
    need(digest(local_file(manifest["source_file"])) == ORIGINAL_SHA256
         and digest(local_file(manifest["edited_svg"])) == manifest["edited_sha256"],
         "Original/edited SVG mismatch")
    png = verify_white_rgba_png(source)
    master = [r for r in manifest["raster"]["outputs"] if r["file"] == SOURCE_NAME]
    need(len(master) == 1 and master[0]["width"] == png["width"]
         and master[0]["height"] == png["height"]
         and master[0]["straight_alpha_white_rgb"] is True, "Raster manifest mismatch")
    for label in ("opaque", "transparent", "antialiased"):
        need(master[0][label + "_pixels"] == png["pixel_counts"][label], "Alpha count mismatch")
    return manifest, source, frozen, png


def enum_value(cls, normalized):
    for name in dir(cls):
        if name.replace("_", "").upper() == normalized:
            return getattr(cls, name)
    raise RuntimeError("Missing reflected enum: " + cls.__name__ + "." + normalized)


def native_import(unreal, source, report):
    # UE5.8 TextureCompiler.cpp registers this synchronous console command via
    # AsyncCompilationHelpers.cpp -> FTextureCompilingManager::FinishAllCompilation.
    # KismetSystemLibrary.cpp routes even a null World through IConsoleManager.
    # Saving alone does not guarantee the temporary 32x32 resource has gone away.
    finish_command = "Editor.AsyncTextureCompilationFinishAll"
    report["texture_compilation_barrier_command"] = finish_command
    report["texture_compilation_barrier_calls"] = 0

    def finish_texture_compilation():
        unreal.SystemLibrary.execute_console_command(None, finish_command)
        report["texture_compilation_barrier_calls"] += 1

    expected = {
        "lod_group": enum_value(unreal.TextureGroup, "TEXTUREGROUPUI"),
        "compression_settings": enum_value(unreal.TextureCompressionSettings, "TCEDITORICON"),
        "mip_gen_settings": enum_value(unreal.TextureMipGenSettings, "TMGSNOMIPMAPS"),
        "power_of_two_mode": enum_value(unreal.TexturePowerOfTwoSetting, "NONE"),
        "address_x": enum_value(unreal.TextureAddress, "TACLAMP"),
        "address_y": enum_value(unreal.TextureAddress, "TACLAMP"),
        "filter": enum_value(unreal.TextureFilter, "TFBILINEAR"),
        "srgb": True, "never_stream": True, "virtual_texture_streaming": False,
        "compression_no_alpha": False, "do_scale_mips_for_alpha_coverage": False,
        "max_texture_size": 2048, "lod_bias": 0,
        "adjust_min_alpha": 0.0, "adjust_max_alpha": 1.0,
    }
    texture = unreal.load_asset(ASSET) if unreal.EditorAssetLibrary.does_asset_exist(ASSET) else None
    if texture is not None:
        need(isinstance(texture, unreal.Texture2D), "Existing target is not a Texture2D; preserved")
        data = texture.get_editor_property("asset_import_data")
        previous_source = Path(data.get_first_filename()).resolve() if data else None
        need(previous_source == source.resolve(), "Existing asset belongs to another source; preserved")
        report["import_action"] = "reuse_existing_matching_source"
    else:
        unreal.EditorAssetLibrary.make_directory(DEST)
        task = unreal.AssetImportTask()
        for key, value in {"filename": str(source), "destination_path": DEST,
                           "destination_name": NAME, "automated": True,
                           "replace_existing": False, "save": False,
                           "factory": unreal.TextureFactory()}.items():
            task.set_editor_property(key, value)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        texture = unreal.load_asset(ASSET)
        need(isinstance(texture, unreal.Texture2D), "Native logo import failed")
        report["import_action"] = "new_texture_import"
        report["imported_object_paths"] = list(task.get_editor_property("imported_object_paths"))
    for key, value in expected.items():
        texture.set_editor_property(key, value)
    unreal.EditorAssetLibrary.set_metadata_tag(texture, "AftershockBrandingSourceSHA256", SOURCE_SHA256)
    finish_texture_compilation()
    need(unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False),
         "Native texture package save failed")
    loaded = unreal.load_asset(ASSET)
    need(isinstance(loaded, unreal.Texture2D), "Load after save failed")
    finish_texture_compilation()
    report["settings"] = {}
    for key, value in expected.items():
        actual = loaded.get_editor_property(key)
        need(actual == value, "Native setting mismatch: " + key)
        report["settings"][key] = actual if isinstance(actual, (bool, int, float, str)) else str(actual)
    size = [int(loaded.blueprint_get_size_x()), int(loaded.blueprint_get_size_y())]
    need(size == [2048, 403], "Native dimensions resized/padded: " + str(size))
    data = loaded.get_editor_property("asset_import_data")
    need(data is not None, "Native import data missing")
    native_source = Path(data.get_first_filename()).resolve()
    need(native_source == source.resolve() and digest(native_source) == SOURCE_SHA256,
         "Native import source/hash mismatch")
    # UObject::SourceFileTagName is AssetImportData (Obj.cpp); Texture.cpp emits
    # FAssetImportInfo::ToJson with the native FileMD5. This is NOT a fabricated metadata hash.
    tags = {str(k): str(v) for k, v in unreal.EditorAssetLibrary.get_tag_values(ASSET).items()}
    need("AssetImportData" in tags, "Native AssetImportData registry tag missing")
    source_info = json.loads(tags["AssetImportData"])
    need(len(source_info) == 1 and source_info[0].get("FileMD5", "").lower() == digest(source, "md5"),
         "Native stored import MD5 does not match source")
    report.update({"asset": loaded.get_path_name(), "native_dimensions": size,
                   "native_import_source": str(native_source),
                   "native_import_source_sha256": digest(native_source),
                   "native_stored_import_data": source_info,
                   "native_stored_md5_matches": True, "saved_then_loaded_same_process": True,
                   "fresh_process_reload_verified": False, "native_pixels_read_back": False,
                   "alpha_policy": "RGBA source verified; lossless UI RGBA, no mips, no alpha scaling; no premultiplication",
                   "cook_and_runtime_reference_verified": False})
    package = ROOT / "Content/UI/Branding" / NAME
    report["package_files"] = []
    for suffix in (".uasset", ".uexp", ".ubulk"):
        path = package.with_suffix(suffix)
        if path.is_file():
            report["package_files"].append({"path": path.relative_to(ROOT).as_posix(),
                                             "bytes": path.stat().st_size, "sha256": digest(path)})
    need(any(r["path"].endswith(".uasset") and r["bytes"] > 0 for r in report["package_files"]),
         "Saved native .uasset is missing/empty")


def main():
    report = {"schema": 1, "run_id": str(uuid.uuid4()), "valid": False,
              "in_progress": True, "started_utc": utc(), "target": ASSET,
              "script_sha256": digest(__file__), "errors": []}
    write_receipt(report)
    try:
        manifest, source, frozen, png = preflight()
        report.update({"source": source.relative_to(ROOT).as_posix(),
                       "source_sha256": SOURCE_SHA256, "source_png": png,
                       "build_manifest_sha256": frozen[str(MANIFEST)],
                       "rights_basis": manifest["source_rights_basis"],
                       "verified_input_file_count": len(frozen),
                       "splash_bmps_modified": False, "config_modified": False})
        import unreal
        native_import(unreal, source, report)
        report["all_source_output_and_generator_hashes_unchanged"] = all(
            Path(path).is_file() and digest(path) == sha for path, sha in frozen.items())
        need(report["all_source_output_and_generator_hashes_unchanged"],
             "Branding source/output files changed during import")
        report["valid"] = True
        unreal.log("AFTERSHOCK_STUDIO_BRANDING_READY " + ASSET + " master=2048x403 straightAlpha=True")
    except Exception as error:
        report["errors"].append(str(error))
        report["traceback"] = traceback.format_exc()
        raise
    finally:
        report["in_progress"] = False
        report["finished_utc"] = utc()
        write_receipt(report)


if __name__ == "__main__":
    if "--preflight" in sys.argv:
        _, _, inputs, png = preflight()
        print(json.dumps({"source_preflight_valid": True, "native_import_executed": False,
                          "verified_input_file_count": len(inputs), "png": png}, indent=2))
    else:
        main()
