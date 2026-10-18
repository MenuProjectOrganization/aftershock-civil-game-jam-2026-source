"""Import the repo's 93 raw WAV files as explicitly saved UE SoundWaves.

Run inside Unreal Editor Python. The portable, read-only build_import_plan()
also works in ordinary Python. This imports assets only: it creates no scene
playback bindings and makes no listening, cooked-package or runtime claim.
The legacy 73-entry production manifest remains unchanged; stale hashes are
reported alongside the actual current WAV byte hashes.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import wave


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_RELATIVE_ROOT = Path("Content/Aftershock/Audio/ElevenLabs")
DEST_ROOT = "/Game/Aftershock/Audio/ElevenLabs"
EXPECTED_ASSETS = 93
EXPECTED_BASES = 73
EXPECTED_VARIATIONS = 20


def require(value, message):
    if not value:
        raise ValueError(message)


def file_sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_import_plan(repo_root=REPO_ROOT):
    """Validate exact catalog/source coverage and WAV data without Unreal."""
    repo_root = Path(repo_root).resolve()
    sound_root = repo_root / SOURCE_RELATIVE_ROOT
    manifest_path = sound_root / "AFTERSHOCK_SFX_MANIFEST.json"
    catalog_path = repo_root / "Source/JamProject/Aftershock/Public/AftershockSoundCatalog.h"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    entries = manifest.get("assets")
    require(isinstance(entries, list) and len(entries) == EXPECTED_BASES, "Manifest must contain exactly 73 base entries")
    require(manifest.get("total_assets") == EXPECTED_BASES, "Manifest base count is inconsistent")
    sample_rate = manifest.get("sample_rate_hz")
    require(type(sample_rate) is int and sample_rate > 0, "Manifest sample rate is invalid")
    bases = {}
    for entry in entries:
        asset_id, category = entry.get("asset_id"), entry.get("category")
        require(isinstance(asset_id, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", asset_id), "Invalid manifest asset ID")
        require(isinstance(category, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", category), "Invalid manifest category")
        require(asset_id not in bases and type(entry.get("loop")) is bool and type(entry.get("channels")) is int and entry["channels"] in (1, 2), "Duplicate base or invalid inherited loop/channel flags")
        bases[asset_id] = entry
    paths = re.findall(r'TEXT\("(/Game/Aftershock/Audio/ElevenLabs/[^"\r\n]+)"\)', catalog_path.read_text(encoding="utf-8-sig"))
    require(len(paths) == EXPECTED_ASSETS and len(set(paths)) == EXPECTED_ASSETS, "Catalog must declare exactly 93 unique object paths")
    raw_files = list(sound_root.rglob("*.wav"))
    require(len(raw_files) == EXPECTED_ASSETS, "Raw source library must contain exactly 93 WAV files")
    require(len({p.stem.casefold() for p in raw_files}) == EXPECTED_ASSETS, "WAV stems must be unique, including case")
    actual_files = set()
    for path in raw_files:
        require(path.is_file() and not path.is_symlink(), "Missing or linked raw WAV source")
        require(not any(parent.is_symlink() or getattr(parent, "is_junction", lambda: False)() for parent in path.parents), "Linked source directory is forbidden")
        actual_files.add(path.resolve())
    plan, catalog_stems, used_files, seen_bases, stale = [], set(), set(), set(), []
    for object_path in paths:
        package, separator, asset_id = object_path.rpartition(".")
        relative = package.removeprefix(DEST_ROOT + "/").split("/")
        require(separator and len(relative) == 2 and relative[1] == asset_id and re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", asset_id), "Malformed catalog package/object path")
        category = relative[0]
        require(asset_id.casefold() not in catalog_stems, "Duplicate catalog stem")
        catalog_stems.add(asset_id.casefold())
        base_id, variation = asset_id, 0
        if asset_id not in bases:
            match = re.fullmatch(r"(.+)_(0[1-4])", asset_id)
            require(match and match[1] in bases, "Unknown catalog variation; expected a base's _01..04")
            base_id, variation = match[1], int(match[2])
        else:
            seen_bases.add(asset_id)
        base = bases[base_id]
        require(category == base["category"], "Catalog category differs from base manifest")
        source = sound_root / category / (asset_id + ".wav")
        require(source.resolve() in actual_files and source.resolve() not in used_files, f"Missing or duplicate source WAV: {category}/{asset_id}.wav")
        used_files.add(source.resolve())
        with wave.open(str(source), "rb") as audio:
            channels, rate, frames, sample_width = audio.getnchannels(), audio.getframerate(), audio.getnframes(), audio.getsampwidth()
            require(audio.getcomptype() == "NONE" and sample_width == 2, f"Source is not PCM16: {asset_id}")
        duration = frames / rate if rate > 0 else 0.0
        require(rate == sample_rate and channels == base["channels"] and frames > 0 and math.isfinite(duration) and duration > 0, f"Invalid source WAV format: {asset_id}")
        digest = file_sha256(source)
        manifest_digest = base.get("sha256") if variation == 0 else None
        if variation == 0 and manifest_digest != digest:
            stale.append(asset_id)
        plan.append({
            "asset_id": asset_id, "category": category, "base_asset_id": base_id, "variation_index": variation,
            "source_relative_path": source.relative_to(repo_root).as_posix(), "source_sha256": digest,
            "legacy_manifest_sha256": manifest_digest, "legacy_manifest_hash_matches_current_source": manifest_digest == digest if variation == 0 else None,
            "asset_path": package, "object_path": object_path, "loop": base["loop"],
            "source_channels": channels, "source_sample_rate_hz": rate, "source_duration_seconds": duration,
        })
    require(seen_bases == set(bases) and used_files == actual_files, "Catalog/source/base manifest coverage is incomplete")
    require(sum(item["variation_index"] > 0 for item in plan) == EXPECTED_VARIATIONS, "Expected exactly 20 inherited variations")
    return {"items": plan, "legacy_manifest_stale_hash_ids": stale, "base_entries": EXPECTED_BASES, "inherited_variations": EXPECTED_VARIATIONS}


def enum_value(cls, normalized):
    for key in dir(cls):
        if key.replace("_", "").upper() == normalized:
            return getattr(cls, key)
    raise RuntimeError(f"Missing reflected enum: {normalized}")


def _positive_number(value, label):
    require(type(value) in (int, float) and math.isfinite(value) and value > 0, f"Invalid readback {label}")
    return value


def import_elevenlabs_sound_library(repo_root=REPO_ROOT, unreal_module=None, report_path=None, source_sha=None):
    if unreal_module is None:
        import unreal as unreal_module
    ue = unreal_module
    repo_root = Path(repo_root).resolve()
    report_path = Path(report_path) if report_path else repo_root / "Saved/PresentationValidation/elevenlabs_audio_import.json"
    report = {"schema_version": 1, "utc": datetime.now(timezone.utc).isoformat(),
              "source_sha": source_sha, "source_sha_scope": "Explicit caller stamp; unknown when null. No Git/engine process is started by this importer.",
              "scope": "Editor WAV import, explicit save and loaded SoundWave/package readback only. Not cold-process reload, scene playback binding, listening QA, cooked package or runtime acceptance.",
              "expected_assets": EXPECTED_ASSETS, "planned": 0, "imported": 0, "saved": 0, "verified": 0, "failed": 0,
              "valid": False, "source_wavs_modified": None, "source_manifest_modified": None, "results": [], "failures": []}
    def write_report():
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    try:
        require(source_sha is None or isinstance(source_sha, str) and re.fullmatch(r"[0-9a-fA-F]{40}", source_sha), "Supplied source SHA must contain exactly 40 hexadecimal characters")
        plan = build_import_plan(repo_root)
        manifest_file = repo_root / SOURCE_RELATIVE_ROOT / "AFTERSHOCK_SFX_MANIFEST.json"
        report["source_manifest_sha256_before"] = file_sha256(manifest_file)
        report["planned"] = len(plan["items"])
        report["base_entries"], report["inherited_variations"] = plan["base_entries"], plan["inherited_variations"]
        report["legacy_manifest_stale_hash_ids"] = plan["legacy_manifest_stale_hash_ids"]
        report["legacy_manifest_stale_hash_count"] = len(plan["legacy_manifest_stale_hash_ids"])
        if plan["legacy_manifest_stale_hash_ids"]:
            ue.log_warning(f"AFTERSHOCK_AUDIO_MANIFEST_STALE hashes={len(plan['legacy_manifest_stale_hash_ids'])}/73; source manifest remains unchanged; actual WAV hashes are reported separately")
        editor_lib = ue.EditorAssetLibrary
        # Reject foreign/redirected assets before any import mutation.
        for item in plan["items"]:
            if editor_lib.does_asset_exist(item["asset_path"]):
                existing = ue.load_asset(item["object_path"])
                require(isinstance(existing, ue.SoundWave) and existing.get_path_name() == item["object_path"], f"Foreign/redirected asset already occupies {item['object_path']}")
        asset_tools = ue.AssetToolsHelpers.get_asset_tools()
    except Exception as error:
        report["failed"] = 1
        report["not_attempted"] = EXPECTED_ASSETS
        report["failures"].append({"stage": "preflight", "error": str(error)})
        write_report()
        ue.log_error(f"AFTERSHOCK_AUDIO_IMPORT_FAIL preflight=1 error={error}")
        raise RuntimeError("Audio import preflight failed; see Saved report") from error
    for item in plan["items"]:
        row = dict(item)
        row["valid"] = False
        try:
            destination = item["asset_path"].rsplit("/", 1)[0]
            require(editor_lib.does_directory_exist(destination) or editor_lib.make_directory(destination), f"Cannot create {destination}")
            source = repo_root / item["source_relative_path"]
            require(file_sha256(source) == item["source_sha256"], "Source WAV changed after preflight")
            task = ue.AssetImportTask()
            for key, value in {"filename": str(source), "destination_path": destination, "destination_name": item["asset_id"],
                               "automated": True, "replace_existing": True, "replace_existing_settings": False, "save": False}.items():
                task.set_editor_property(key, value)
            asset_tools.import_asset_tasks([task])
            imported_paths = task.get_editor_property("imported_object_paths")
            require(item["object_path"] in imported_paths, "Import task did not report the requested object; an old loaded asset is not success")
            sound = ue.load_asset(item["object_path"])
            require(isinstance(sound, ue.SoundWave) and sound.get_path_name() == item["object_path"], "Imported object is not the expected SoundWave")
            report["imported"] += 1
            virtualization = enum_value(ue.VirtualizationMode, "PLAYWHENSILENT" if item["loop"] else "RESTART")
            for key, value in {"looping": item["loop"], "virtualization_mode": virtualization, "volume": 1.0, "pitch": 1.0}.items():
                sound.set_editor_property(key, value)
            require(editor_lib.save_loaded_asset(sound, False) is True, "Explicit SoundWave save failed")
            report["saved"] += 1
            restored = ue.load_asset(item["object_path"])
            require(isinstance(restored, ue.SoundWave) and restored.get_path_name() == item["object_path"], "Readback is not the requested SoundWave")
            duration = _positive_number(restored.get_editor_property("duration"), "duration")
            channels = _positive_number(restored.get_editor_property("num_channels"), "channels")
            rate = _positive_number(restored.get_editor_property("sample_rate"), "sample rate")
            imported_rate = _positive_number(restored.get_editor_property("imported_sample_rate"), "imported sample rate")
            volume = _positive_number(restored.get_editor_property("volume"), "volume")
            pitch = _positive_number(restored.get_editor_property("pitch"), "pitch")
            require(channels == item["source_channels"] and rate == item["source_sample_rate_hz"] and imported_rate == item["source_sample_rate_hz"], "Readback channels/sample rate differ from source WAV")
            require(abs(duration - item["source_duration_seconds"]) <= max(.02, 1.0 / rate), "Readback duration differs from source WAV")
            require(restored.get_editor_property("looping") == item["loop"] and restored.get_editor_property("virtualization_mode") == virtualization, "Readback loop/virtualization flags differ")
            require(math.isclose(volume, 1.0, abs_tol=1e-6) and math.isclose(pitch, 1.0, abs_tol=1e-6), "Readback volume/pitch differ")
            package_file = repo_root / ("Content/" + item["asset_path"].removeprefix("/Game/") + ".uasset")
            require(package_file.is_file() and package_file.stat().st_size > 0, "Saved SoundWave package is missing")
            require(file_sha256(source) == item["source_sha256"], "Import modified the raw source WAV")
            row.update(valid=True, duration_seconds=duration, channels=channels, sample_rate_hz=rate, imported_sample_rate_hz=imported_rate,
                       loop_readback=bool(restored.get_editor_property("looping")), volume=volume, pitch=pitch,
                       saved_asset_bytes=package_file.stat().st_size, saved_asset_sha256=file_sha256(package_file))
            report["verified"] += 1
            ue.log(f"AFTERSHOCK_AUDIO_IMPORTED asset={item['asset_id']} saved=1 readback=1 loop={int(item['loop'])}")
        except Exception as error:
            report["failed"] += 1
            row["error"] = str(error)
            report["failures"].append({"asset_id": item["asset_id"], "error": str(error)})
            ue.log_error(f"AFTERSHOCK_AUDIO_IMPORT_FAIL asset={item['asset_id']} error={error}")
        report["results"].append(row)
    try:
        report["source_wavs_modified"] = any(file_sha256(repo_root / item["source_relative_path"]) != item["source_sha256"] for item in plan["items"])
        report["source_manifest_sha256_after"] = file_sha256(manifest_file)
        report["source_manifest_modified"] = report["source_manifest_sha256_before"] != report["source_manifest_sha256_after"]
        require(not report["source_wavs_modified"] and not report["source_manifest_modified"], "Raw source library changed during import")
    except Exception as error:
        report["failed"] += 1
        report["failures"].append({"stage": "source_integrity", "error": str(error)})
        ue.log_error(f"AFTERSHOCK_AUDIO_IMPORT_FAIL sourceIntegrity=1 error={error}")
    report["valid"] = report["planned"] == report["imported"] == report["saved"] == report["verified"] == EXPECTED_ASSETS and report["failed"] == 0
    write_report()
    ue.log(f"AFTERSHOCK_AUDIO_IMPORT_RESULT valid={int(report['valid'])} planned={report['planned']} imported={report['imported']} saved={report['saved']} verified={report['verified']} failed={report['failed']} scope=ImportOnly_NoPlaybackBinding")
    if not report["valid"]:
        raise RuntimeError("Audio import did not verify all 93 SoundWaves; see Saved report")
    return report


if __name__ == "__main__":
    import_elevenlabs_sound_library()
