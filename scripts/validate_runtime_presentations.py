"""Opt-in D3D12 development fixtures; render real frames without desktop input.

Uses the production scene and existing AftershockVisualAudit flags. Induced
damage/weather and synthetic render populations are explicitly labelled fixtures.
"""
from __future__ import annotations

import argparse
from functools import lru_cache
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import time

from validation_settings_isolation import RunSettingsIsolation, normalized_ini_path_sha1


CASES = {
    "chaos": (0, ["-AftershockChaosAudit", "-AftershockChaosAuditAllTypes"]),
    "chaos-new-city": (0, ["-AftershockChaosAudit", "-AftershockChaosAuditAllTypes", "-AftershockConfiguredSessionAudit"]),
    "rubble-save": (0, ["-AftershockRubbleSaveAudit"]),
    "bridge": (0, ["-AftershockBridgeVisualAudit"]),
    "chunks": (0, ["-AftershockChunkVisualAudit"]),
    "chunk-chaos": (0, ["-AftershockChunkVisualAudit", "-AftershockChunkChaosAudit"]),
    "narrative": (0, ["-AftershockNarrativeAudit", "-AftershockConfiguredSessionAudit"]),
    "flood": (1, ["-AftershockWeatherAudit=Flood"]),
    "wind": (2, ["-AftershockWeatherAudit=Wind"]),
    "tsunami": (3, ["-AftershockWeatherAudit=Tsunami"]),
    "fire": (4, ["-AftershockWeatherAudit=Fire"]),
    "volcano": (5, ["-AftershockWeatherAudit=Volcano"]),
    "drought": (6, ["-AftershockWeatherAudit=Drought"]),
    "globe": (0, ["-AftershockGlobeAudit"]),
    "profile-500": (0, ["-AftershockProfileAgents=500", "-csvGpuStats"]),
    "profile-1500": (0, ["-AftershockProfileAgents=1500", "-csvGpuStats"]),
}
LEGACY_CASES = tuple(CASES)
# Named opt-in only: the historical16-case default remains byte-for-byte selected.
CASES["physical-flood"] = (1, ["-AftershockPhysicalFloodAudit", "-AftershockConfiguredSessionAudit",
                              "-AftershockAuditRegion=1", "-AftershockAuditFocus=0", "-AftershockAuditSourceVersion=3"])
CASES["fire-live"] = (4, ["-AftershockFireRuntimeAudit", "-AftershockStartupAudit", "-AftershockConfiguredSessionAudit",
                          "-AftershockAuditRegion=4", "-AftershockAuditFocus=0", "-AftershockAuditSourceVersion=3"])
GEOGRAPHIC_REGIONS = {"geo-atakent": 0, "geo-surmene": 1, "geo-miami": 7,
                      "geo-cyprus": 8, "geo-etna": 5, "geo-globe": 0,
                      "geo-valparaiso": 10, "geo-sendai": 12, "geo-new-orleans": 14}


def geographic_focus_cases() -> dict[str, tuple[int, int]]:
    """Read the same checked-in identities/ranges compiled by the actual game.

    No private _Imports download, stale Saved result or second terrain catalogue
    is required. An unexpected generated catalogue shape fails closed.
    """
    source = (Path(__file__).resolve().parents[1] /
              "Source/JamProject/Aftershock/Private/AftershockRegionCatalog.inl").read_text(encoding="utf-8")
    focus_block = source.split("GeographicFocuses[] = {", 1)[1].split("};", 1)[0]
    identities = [re.findall(r'TEXT\("([^"\\]*)"\)', line) for line in focus_block.splitlines() if line.strip().startswith("{")]
    ranges_block = source.split("GeographicRegions[] = {", 1)[1].split("};", 1)[0]
    ranges = re.findall(r"\{\s*(\d+),\s*(\d+)\s*\},\s*//\s*([a-z0-9_]+)", ranges_block)
    if len(identities) != 25 or len(ranges) != 20 or any(len(row) != 4 for row in identities):
        raise ValueError("Expected the actual20-region/25-focus compiled catalogue")
    cases, seen = {}, set()
    for region, (first, count, region_id) in enumerate(ranges):
        for focus in range(int(count)):
            flat = int(first) + focus
            if flat in seen or not 0 <= flat < len(identities) or identities[flat][0] != region_id:
                raise ValueError("Compiled geographic focus ranges overlap or have mismatched identities")
            seen.add(flat)
            label = f"geo-focus-{region_id}-{identities[flat][2]}"
            if label in cases:
                raise ValueError("Duplicate compiled geographic focus case")
            cases[label] = (region, focus)
    if len(seen) != 25:
        raise ValueError("Compiled geographic focus coverage is incomplete")
    # Additive anchors can reuse one immutable DEM/OSM payload. The original
    # 25 sources and stable focus IDs are not shifted when a playable lower
    # flank is added beside the historical Etna summit inspection window.
    alias_file = Path(__file__).resolve().parents[1] / "_Imports/Geography/ADDITIONAL_FOCUSES.json"
    aliases = json.loads(alias_file.read_text(encoding="utf-8-sig"))
    if aliases.get("schema_version") != 1 or aliases.get("unique_source_focus_count") != 25:
        raise ValueError("Invalid additive focus source contract")
    local_counts = [int(row[1]) for row in ranges]
    for row in aliases.get("focuses", []):
        region, focus = row["region_index"], row["focus_index"]
        if (type(region) is not int or not 0 <= region < len(ranges)
                or type(focus) is not int or focus != local_counts[region]
                or row["region_id"] != ranges[region][2]
                or row["source_region_id"] != ranges[region][2]
                or type(row["source_focus_index"]) is not int
                or not 0 <= row["source_focus_index"] < int(ranges[region][1])):
            raise ValueError("Additive focus has mismatched stable identity or payload")
        source = identities[int(ranges[region][0]) + row["source_focus_index"]]
        if row["source_focus_id"] != source[2] or not re.fullmatch(r"[a-z0-9_]+", row["focus_id"]):
            raise ValueError("Additive focus points to an unknown source")
        label = f"geo-focus-{row['region_id']}-{row['focus_id']}"
        if label in cases:
            raise ValueError("Duplicate additive focus identity")
        cases[label] = (region, focus)
        local_counts[region] += 1
    if aliases.get("selectable_focus_count") != len(cases) or len(cases) != 26:
        raise ValueError("Expected25 immutable payloads and26 actual focus identities")
    return cases


GEOGRAPHIC_FOCUS_CASES = geographic_focus_cases()
GEOGRAPHIC_REGION_IDS = {region: name.removeprefix("geo-focus-").rsplit("-", 1)[0]
                         for name, (region, _) in GEOGRAPHIC_FOCUS_CASES.items()}
GEOGRAPHIC_CASES = {name: (region, 0) for name, region in GEOGRAPHIC_REGIONS.items()}
GEOGRAPHIC_CASES.update(GEOGRAPHIC_FOCUS_CASES)
for geographic_case in GEOGRAPHIC_CASES:
    CASES[geographic_case] = (0, ["-AftershockGeographyViewAudit", "-AftershockConfiguredSessionAudit"])


def confirmed_uncapped_capture(metadata: dict) -> bool:
    """Require both the applied runtime marker and live console/engine readback.

    Historical legacy captures did not emit these fields. Only new geographic
    and explicitly requested overview fixtures require this extra contract.
    """
    limits = (metadata.get("max_fps_engine"), metadata.get("frame_rate_limit_cvar_fps"))
    return (metadata.get("capture_policy") == "UncappedEphemeralNoSettingsWrite"
            and metadata.get("frame_pacing_marker_applied") is True
            and metadata.get("uncapped_ephemeral_audit_confirmed") is True
            and all(type(value) in (int, float) and math.isfinite(value) and abs(value) <= .0001
                    for value in limits)
            and type(metadata.get("vsync_cvar")) is int and metadata["vsync_cvar"] == 0)


@lru_cache(maxsize=1)
def _surround_source_regions() -> dict:
    """Bounded20-row fixture manifest; absence/partial input is never ready."""
    path = Path(__file__).resolve().parents[1] / "_Imports/Geography/RegionalImagery/Surround/IMAGERY_MANIFEST.json"
    try:
        manifest = json.loads(path.read_text(encoding="utf-8-sig"))
        rows = manifest.get("regions")
        if (manifest.get("complete") is not True or not isinstance(rows, list) or len(rows) != 20
                or not isinstance(manifest.get("source_matrix_sha256"), str)
                or not re.fullmatch(r"[a-f0-9]{64}", manifest["source_matrix_sha256"])):
            return {}
        result = {}
        def bounds(value):
            return (isinstance(value, list) and len(value) == 4
                    and all(type(v) in (int, float) and math.isfinite(v) for v in value)
                    and -180 <= value[0] < value[2] <= 180 and -90 <= value[1] < value[3] <= 90)
        for row in rows:
            if not isinstance(row, dict):
                return {}
            identity = row.get("region_id")
            if identity not in GEOGRAPHIC_REGION_IDS.values() or identity in result:
                return {}
            original, expanded = row.get("context_bounds_wgs84"), row.get("bounds_wgs84")
            if (not bounds(original) or not bounds(expanded) or row.get("expansion_factor") != 8
                    or row.get("width") != 2048 or row.get("height") != 2048
                    or row.get("filename") != identity + "-eox2016-surround.jpg"
                    or row.get("ue_texture_path") != "/Game/Environment/Textures/Regional/Surround/T_RegionalSurround_" + identity
                    or not isinstance(row.get("sha256"), str) or not re.fullmatch(r"[a-f0-9]{64}", row["sha256"])):
                return {}
            cx, cy = (original[0] + original[2]) * .5, (original[1] + original[3]) * .5
            hx, hy = (original[2] - original[0]) * 4., (original[3] - original[1]) * 4.
            unclamped = [cx-hx, cy-hy, cx+hx, cy+hy]
            expected = [max(-180.,unclamped[0]), max(-90.,unclamped[1]), min(180.,unclamped[2]), min(90.,unclamped[3])]
            requested = row.get("requested_unclamped_bounds_wgs84")
            if (not isinstance(requested, list) or len(requested) != 4
                    or not all(type(v) in (int,float) and math.isfinite(v) for v in requested)
                    or any(abs(a-b) > 1.e-8 for a,b in zip(requested,unclamped))
                    or any(abs(a-b) > 1.e-8 for a,b in zip(expanded,expected))):
                return {}
            result[identity] = row
        return result if set(result) == set(GEOGRAPHIC_REGION_IDS.values()) else {}
    except (OSError, ValueError, TypeError, KeyError):
        return {}


def valid_regional_surround_slots(snapshot: dict, region: int) -> bool:
    """Actual4-MID source binding, not image appearance or resident VRAM proof."""
    if not isinstance(snapshot, dict) or region not in GEOGRAPHIC_REGION_IDS:
        return False
    slots = snapshot.get("source_context_material_slots", [])
    if not isinstance(slots,list) or not all(isinstance(slot,dict) for slot in slots):
        return False
    materials = [*slots,snapshot.get("ground_material_slot0",{}),snapshot.get("planet_material_slot0",{})]
    if snapshot.get("continuation_version") != 3:
        # Older metadata can omit the additive optional parameter. A present
        # enabled surround on legacy/non-V3 is not accepted as disabled fallback.
        return all(isinstance(slot,dict) and slot.get("RegionalSurroundReady",0) == 0 for slot in materials)
    if (snapshot.get("regional_surround_schema_version") != 1 or len(slots) != 2
            or {slot.get("slot") for slot in slots} != {0,1}):
        return False
    row = _surround_source_regions().get(GEOGRAPHIC_REGION_IDS[region])
    if not row:
        return False
    west,south,east,north = row["bounds_wgs84"]
    expected_vectors = {"RegionalSurroundGeoMin":[west,south,0.],"RegionalSurroundGeoMax":[east,north,0.]}
    package = row["ue_texture_path"]
    expected_texture = package + "." + package.rsplit("/",1)[-1]
    for material in materials:
        if (not isinstance(material,dict) or material.get("material_present") is not True
                or material.get("RegionalSurroundReady") != 1
                or material.get("RegionalSurroundReady_present") is not True
                or material.get("RegionalSurroundReady_finite") is not True):
            return False
        texture = material.get("RegionalSurroundTexture",{})
        if (not isinstance(texture,dict) or texture.get("parameter_present") is not True
                or texture.get("texture") != expected_texture or texture.get("width") != 2048
                or texture.get("height") != 2048 or texture.get("mips") != 12
                or texture.get("linear") is not False or texture.get("streaming_enabled") is not True):
            return False
        for name,expected in expected_vectors.items():
            actual = material.get(name)
            if (material.get(name+"_present") is not True or material.get(name+"_finite") is not True
                    or not isinstance(actual,list) or len(actual) != 3
                    or not all(type(v) in (int,float) and math.isfinite(v) for v in actual)
                    # FLinearColor stores float32;2e-5 degrees bounds its ULP at
                    # 180deg. It is not a claimed survey/imagery accuracy.
                    or any(abs(a-b) > 2.e-5 for a,b in zip(actual,expected))
                    or actual != slots[0].get(name)):
                return False
    return True


def valid_v3_material_slots(snapshot: dict, region: int) -> bool:
    """Actual per-slot readback, rather than a manifest or default flag alone."""
    if not isinstance(snapshot, dict) or region not in GEOGRAPHIC_REGION_IDS:
        return False
    slots = snapshot.get("source_context_material_slots", [])
    ground = snapshot.get("ground_material_slot0", {})
    if (not isinstance(slots, list) or not isinstance(ground, dict) or not all(isinstance(s, dict) for s in slots)
            or snapshot.get("source_context_material_slot_count") != 2 or len(slots) != 2 or {s.get("slot") for s in slots} != {0, 1}):
        return False
    expected_name = "T_RegionalMacro_" + GEOGRAPHIC_REGION_IDS[region]
    expected_texture = "/Game/Environment/Textures/Regional/Macro/" + expected_name + "." + expected_name
    for slot in [*slots, ground]:
        if not all(slot.get(field) == 1 for field in ("RegionalStyleEnabled", "RegionalStyleTexturesReady", "RegionalMacroReady")):
            return False
        texture = slot.get("RegionalMacroTexture", {})
        if texture.get("parameter_present") is not True or texture.get("texture") != expected_texture or texture.get("linear") is not False:
            return False
        if not all(type(texture.get(field)) in (int, float) and math.isfinite(texture[field])
                   and 1 <= texture[field] <= 1024 for field in ("width", "height")):
            return False
        if type(texture.get("mips")) not in (int, float) or not 1 <= texture["mips"] <= 11:
            return False
        for bound in ("RegionalMacroBoundsMin", "RegionalMacroBoundsMax"):
            values = slot.get(bound, [])
            if slot.get(bound + "_present") is not True or len(values) != 3 or not all(type(v) in (int, float) and math.isfinite(v) for v in values):
                return False
        if any(slot["RegionalMacroBoundsMin"][axis] >= slot["RegionalMacroBoundsMax"][axis] for axis in (0, 1)):
            return False
        if any(slot[bound] != slots[0].get(bound) for bound in ("RegionalMacroBoundsMin", "RegionalMacroBoundsMax")):
            return False
        for bound in ("GlobalRegionWorldMin", "GlobalRegionWorldMax", "GlobalRegionWGS84Min", "GlobalRegionWGS84Max"):
            value = slot.get(bound, [])
            if (slot.get(bound + "_present") is not True or len(value) != 3
                    or not all(type(number) in (int, float) and math.isfinite(number) for number in value)
                    or value != slots[0].get(bound)):
                return False
        if any(slot["GlobalRegionWorldMin"][axis] >= slot["GlobalRegionWorldMax"][axis]
               or slot["GlobalRegionWGS84Min"][axis] >= slot["GlobalRegionWGS84Max"][axis] for axis in (0, 1)):
            return False
    global_path = "/Game/Environment/Textures/Regional/Macro/T_GlobalMacro_EOX2016.T_GlobalMacro_EOX2016"
    for slot in [*slots, ground, snapshot.get("planet_material_slot0", {})]:
        if (not isinstance(slot, dict) or slot.get("GlobalImageryReady") != 1
                or slot.get("GlobalImageryReady_present") is not True or slot.get("GlobalImageryReady_finite") is not True):
            return False
        texture = slot.get("GlobalImageryTexture", {})
        if (texture.get("parameter_present") is not True or texture.get("texture") != global_path
                or texture.get("width") != 8192 or texture.get("height") != 4096
                or texture.get("mips") != 14 or texture.get("linear") is not False):
            return False
    for slot in slots:
        mask = slot.get("RegionalSourceMask", {})
        if (slot.get("RegionalSourceMaskEnabled") != 1 or mask.get("parameter_present") is not True
                or mask.get("linear") is not True or mask.get("width") != 257
                or mask.get("height") != 257 or mask.get("mips") != 9):
            return False
    return valid_regional_surround_slots(snapshot,region)


GRAPHICS_GROUP_NAMES = ("view_distance", "shadows", "global_illumination", "reflections",
                        "anti_aliasing", "textures", "effects", "post_process", "foliage", "shading")


def read_requested_graphics_quality(folder: Path, requested: int, started: float, isolated_ini: Path | None = None) -> dict:
    """Require current native readback; a command-line request is not proof.

    This does not compare images or declare a quality tier prettier/faster.
    Fixtures without ReadRenderAuditMetadata evidence remain explicitly unverified.
    """
    observations, errors = [], []
    expected_ini_hash = normalized_ini_path_sha1(isolated_ini) if isolated_ini is not None else None
    receipts = ("hydraulic_runtime_audit.json", "geographic_view_audit.json", "physical_flood_runtime_audit.json",
                "profile-view-metadata.json", "volcano_runtime_audit.json",
                "single_road_construction_audit.json", "fire_runtime_audit.json")
    def visit(value, receipt):
        if isinstance(value, dict):
            quality = value.get("graphics_quality")
            if isinstance(quality, dict):
                groups = quality.get("groups")
                destination = quality.get("user_settings_destination")
                destination_valid = (expected_ini_hash is None or
                    (isinstance(destination, dict) and destination.get("schema_version") == 2
                     and destination.get("config_branch_present") is True
                     and isinstance(destination.get("config_branch_name"), str)
                     and destination["config_branch_name"].lower() == "gameusersettings"
                     and destination.get("effective_destination_source") == "LoadedGConfigBranch.IniPath"
                     and destination.get("override_requested") is True
                     and destination.get("effective_destination_present") is True
                     and destination.get("effective_destination_matches_override") is True
                     and destination.get("effective_normalized_path_sha1") == expected_ini_hash
                     and destination.get("requested_normalized_path_sha1") == expected_ini_hash
                     and isinstance(destination.get("effective_filename"), str)
                     and destination["effective_filename"].lower() == "isolated-gameusersettings.ini"))
                valid = (quality.get("user_settings_present") is True
                         and quality.get("fixture_requested") is True
                         and quality.get("requested_quality") == requested
                         and quality.get("actual_matching_preset_index") == requested
                         and isinstance(groups, dict)
                         and set(groups) == set(GRAPHICS_GROUP_NAMES)
                         and all(type(groups.get(key)) in (int, float)
                                 and math.isfinite(groups[key]) and groups[key] == requested
                                 for key in GRAPHICS_GROUP_NAMES))
                observations.append({"receipt": receipt, "stage": value.get("stage"),
                                     "matched": bool(valid and destination_valid),
                                     "quality_groups_matched": bool(valid),
                                     "settings_destination_matched": bool(destination_valid), "actual": quality})
            for key, child in value.items():
                if key != "graphics_quality":
                    visit(child, receipt)
        elif isinstance(value, list):
            for child in value:
                visit(child, receipt)
    for name in receipts:
        path = folder / name
        if not path.is_file():
            continue
        if path.stat().st_mtime < started - 1 or path.stat().st_size > 32 * 1024 * 1024:
            errors.append(name + ": stale or oversized native metadata")
            continue
        try:
            visit(json.loads(path.read_text(encoding="utf-8-sig")), name)
        except (OSError, ValueError, TypeError) as exc:
            errors.append(name + ": " + repr(exc))
    return {"requested": requested, "confirmed": bool(observations) and not errors
            and all(row["matched"] for row in observations), "observations": observations,
            "errors": errors, "native_settings_destination_required": expected_ini_hash is not None,
            "expected_isolated_path_sha1": expected_ini_hash,
            "native_settings_destination_confirmed": bool(observations) and not errors
                and all(row["settings_destination_matched"] for row in observations),
            "scope": "Actual native scalability/CVar/component and GGameUserSettingsIni path readback; not visual acceptance or GPU performance proof. Original bytes are checked separately after process exit."}



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quality", type=int, choices=range(5), default=None,
                        help="0..4 production scalability in a process-private settings INI; the original user INI must remain byte-identical after exit. Actual native metadata must confirm quality.")
    parser.add_argument("--tutorial-skip", action="store_true", help="Narrative case: call the real tutorial Skip handler after its second rendered step; default observes full real-clock autoplay.")
    parser.add_argument("--startup-audit", action="store_true", help="Narrative case only: include the real shared preload/branding gate before the unchanged tutorial clock.")
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cases", nargs="+", choices=CASES)
    parser.add_argument("--geographic-all", "--geographic-all25", dest="geographic_all25", action="store_true",
                        help="Run all26 actual focus identities over25 immutable source packets; --geographic-all25 is retained as a legacy alias")
    parser.add_argument("--geography-version", type=int, choices=(2, 3), default=3,
                        help="Explicit real-source version for geographic cases; default3,2 retains the original source regression")
    parser.add_argument("--geographic-temporal", action="store_true",help="Capture two additional pinned-camera frames; image inspection is separate from automated geometry checks")
    parser.add_argument("--urban-garden-audit", action="store_true", help="Read back actual bounded native garden instances; visual acceptance is separate")
    parser.add_argument("--geographic-regional", action="store_true",
                        help="Add a full-source-ROI regional frame after the unchanged four views; actual8-corner framing and V3 masks/macros must validate")
    parser.add_argument("--legacy-geography", action="store_true",
                        help="Explicit V1 geography for every selected legacy case; preserves historical configured fixtures")
    parser.add_argument("--region", type=int, help="Real-source region index (0..19), through the actual New Game contract")
    parser.add_argument("--focus", type=int, help="Real-source local focus index (default selected case focus or0)")
    parser.add_argument("--overview", type=float,
                        help="Profile-only requested camera overview blend 0..1; does not change canonical residents")
    parser.add_argument("--profile-duration", type=int, default=60,
                        help="Actual CSV capture seconds per profile; zero preserves legacy 300-frame fixture")
    args = parser.parse_args()
    if args.geographic_all25:
        if args.cases is not None or args.region is not None or args.focus is not None or args.legacy_geography or args.overview is not None:
            parser.error("--geographic-all25 selects all actual region/focus pairs and cannot be combined with case/region/focus/legacy/overview overrides")
        args.cases = tuple(GEOGRAPHIC_FOCUS_CASES)
    elif args.cases is None:
        args.cases = LEGACY_CASES
    if (args.tutorial_skip or args.startup_audit) and args.cases != ["narrative"]:
        parser.error("--tutorial-skip and --startup-audit require --cases narrative only")
    if args.profile_duration != 0 and not 14 <= args.profile_duration <= 180:
        parser.error("--profile-duration must be zero or 14..180 seconds")
    if args.region is not None and not 0 <= args.region < 20:
        parser.error("--region must be 0..19")
    if args.focus is not None and not 0 <= args.focus < 4:
        parser.error("--focus must be 0..3 (the selected region must contain that focus)")
    if args.focus is not None and args.region is None and not all(name in GEOGRAPHIC_CASES for name in args.cases):
        parser.error("--focus requires --region, or named geo cases")
    if args.legacy_geography and (args.region is not None or args.focus is not None or any(name in GEOGRAPHIC_CASES for name in args.cases)):
        parser.error("--legacy-geography is incompatible with real-source region/focus and geo cases")
    if args.geography_version == 2 and not all(name in GEOGRAPHIC_CASES for name in args.cases):
        parser.error("--geography-version2 applies only to explicitly selected geographic cases")
    if args.geographic_regional and (args.geography_version != 3 or not all(name in GEOGRAPHIC_CASES for name in args.cases)):
        parser.error("--geographic-regional applies only to V3 geographic cases")
    if "physical-flood" in args.cases and (args.legacy_geography or args.geography_version != 3
                                          or args.region not in (None, 1) or args.focus not in (None, 0)):
        parser.error("physical-flood is the explicit V3 real-source region1/focus0 acceptance fixture")
    if "fire-live" in args.cases and (args.legacy_geography or args.geography_version != 3
                                      or args.region not in (None, 4) or args.focus not in (None, 0)):
        parser.error("fire-live is the explicit V3 real-source Marmaris region4/focus0 startup/Model2 fixture")
    for selected in args.cases:
        pair = GEOGRAPHIC_CASES.get(selected)
        region = args.region if args.region is not None else pair[0] if pair else None
        focus = args.focus if args.focus is not None else pair[1] if pair else 0
        if region is not None and (region, focus) not in GEOGRAPHIC_FOCUS_CASES.values():
            parser.error(f"Region{region} does not contain actual compiled focus{focus}")
    if args.overview is not None and (not math.isfinite(args.overview) or not 0 <= args.overview <= 1
                                      or not all(name.startswith("profile-") for name in args.cases)):
        parser.error("--overview must be finite 0..1 and applies only to profile cases")
    repo = Path(__file__).resolve().parents[1]
    if not args.editor.is_file():
        parser.error("The installed UnrealEditor executable is required.")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    settings_isolation = RunSettingsIsolation(repo, output)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, text=True, capture_output=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=repo, text=True, capture_output=True, check=True).stdout.strip())
    captures = repo / "Saved/PresentationValidation"
    results = []
    for name in args.cases:
        started = time.time()
        folder = output / name
        folder.mkdir()
        isolated_settings = settings_isolation.prepare_case(folder)
        index, flags = CASES[name]
        flags = list(flags)
        if args.urban_garden_audit:
            flags.append("-AftershockUrbanGardenAudit")
        if args.quality is not None:
            flags.append(f"-AftershockGraphicsQuality={args.quality}")
        if name == "narrative" and args.tutorial_skip:
            flags.append("-AftershockTutorialSkipAudit")
        if name == "narrative" and args.startup_audit:
            flags.append("-AftershockStartupAudit")
        geographic_case = name in GEOGRAPHIC_CASES
        physical_flood_case = name == "physical-flood"
        fire_live_case = name == "fire-live"
        requested_pair = GEOGRAPHIC_CASES.get(name)
        region = 4 if fire_live_case else 1 if physical_flood_case else args.region if args.region is not None else requested_pair[0] if requested_pair else None
        focus = 0 if physical_flood_case or fire_live_case else args.focus if args.focus is not None else requested_pair[1] if requested_pair else 0
        real_volcano_case = name == "volcano" and region is not None and not args.legacy_geography
        if args.legacy_geography:
            flags.append("-AftershockLegacyGeographyAudit")
        if region is not None:
            flags.extend(["-AftershockConfiguredSessionAudit", f"-AftershockAuditRegion={region}", f"-AftershockAuditFocus={focus}"])
        if geographic_case:
            flags.append(f"-AftershockAuditSourceVersion={args.geography_version}")
            if args.geographic_temporal: flags.append("-AftershockGeographyTemporalAudit")
            if args.geographic_regional: flags.append("-AftershockRegionalOverviewAudit")
        if args.overview is not None:
            flags.append(f"-AftershockOverviewAudit={args.overview:.9g}")
        if name.startswith("profile-") or name in {"chunks", "chunk-chaos"}:
            flags.append("-AftershockConfiguredSessionAudit")
        if name.startswith("profile-") and args.profile_duration:
            flags.append(f"-AftershockProfileDuration={args.profile_duration}")
        log_path = folder / "runtime.log"
        command = [str(args.editor.resolve()), str(repo / "JamProject.uproject"),
                   "/Game/Aftershock/Levels/L_Level_01_Deprem", "-game", "-RenderOffscreen",
                   "-windowed", "-ResX=1600", "-ResY=900", "-ForceRes", "-d3d12", "-NoSplash",
                   "-unattended", "-nop4", "-AftershockVisualAudit",
                   f"-AftershockScenario={index}", f"-abslog={log_path}",
                   f"-GameUserSettingsINI={isolated_settings}", *flags]
        with (folder / "console.txt").open("wb") as console:
            try:
                code = subprocess.run(command, cwd=repo, stdout=console,
                                      stderr=subprocess.STDOUT, timeout=270 if fire_live_case else 420 if name=="narrative" and args.startup_audit else 240 if name.startswith("profile-") else 180, check=False).returncode
            except subprocess.TimeoutExpired:
                code = -1
            finally:
                user_settings_proof = settings_isolation.finish_case(folder, isolated_settings)
        log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        errors = [line for line in log.splitlines() if re.search(
            r"Fatal error:|Ensure condition failed|Assertion failed|LogShaderCompilers: Error|LogWindows: Error|Failed to compile Material|Default Material will be used", line)]
        artifact_names = []
        for source in captures.iterdir():
            relevant = source.name in {"studio-runtime-scene.png", "studio-runtime-hud.png",
                                      "studio-induced-collapse.png", "globe-overview-hud.png",
                                      "globe-overview-scene.png", "studio-rubble-restored.png", "studio-bridge-runtime.png"} or source.name.startswith(name + "-peak-")
            relevant |= name.startswith("profile-") and source.name == "crowd-render-" + name[8:] + ".csv"
            relevant |= name.startswith("profile-") and source.name == "crowd-render-" + name[8:] + ".profile.json"
            relevant |= name in {"chunks", "chunk-chaos"} and source.name in {"studio-chunk-runtime.png", "chunk_runtime_audit.json"}
            relevant |= geographic_case and (source.name.startswith("geo-view-") or source.name == "geographic_view_audit.json")
            relevant |= physical_flood_case and (source.name.startswith("physical-flood-") or source.name == "physical_flood_runtime_audit.json")
            relevant |= real_volcano_case and source.name == "volcano_runtime_audit.json"
            relevant |= name.startswith("profile-") and source.name == "profile-view-metadata.json"
            relevant |= args.urban_garden_audit and source.name == "urban_garden_native.json"
            if source.is_file() and relevant and source.stat().st_mtime >= started - 1:
                shutil.copy2(source, folder / source.name)
                artifact_names.append(source.name)
        if name == "narrative":
            for source in (captures / "Narrative").glob("*"):
                if source.is_file() and source.suffix in {".json", ".png"} and source.stat().st_mtime >= started - 1:
                    shutil.copy2(source, folder / source.name)
                    artifact_names.append(source.name)
        if fire_live_case:
            for source in (captures / "FireRuntime").glob("*"):
                if source.is_file() and source.suffix in {".json", ".png"} and source.stat().st_mtime >= started - 1:
                    shutil.copy2(source, folder / source.name)
                    artifact_names.append(source.name)
        chunk_case = name in {"chunks", "chunk-chaos"}
        completion = "AFTERSHOCK_CHUNK_RUNTIME_RESULT passed=1" if chunk_case else "AFTERSHOCK_VISUAL_AUDIT_END"
        if name.startswith("profile-"):
            completion = "AFTERSHOCK_RENDER_PROFILE_COMPLETE"
        if name == "narrative":
            completion = "AFTERSHOCK_NARRATIVE_RUNTIME_RESULT valid=1"
        if physical_flood_case:
            completion = "AFTERSHOCK_PHYSICAL_FLOOD_RHI_RESULT valid=1 frames=3 jsonSaved=1"
        if fire_live_case:
            completion = "AFTERSHOCK_FIRE_RUNTIME_RESULT valid=1 saved=1 frames=4 triggerCalls=1"
        valid = code == 0 and not errors and completion in log and bool(artifact_names)
        if args.urban_garden_audit:
            try:
                garden = json.loads((folder / "urban_garden_native.json").read_text(encoding="utf-8-sig"))
                valid &= garden.get("valid") is True and garden.get("failures") == 0
                garden_events = re.findall(r"AFTERSHOCK_URBAN_GARDEN_NATIVE valid=(\d+) failures=(\d+)", log)
                valid &= bool(garden_events) and all(a == "1" and b == "0" for a, b in garden_events)
            except (OSError, ValueError, TypeError):
                valid = False
        image_metrics = {}
        volcano_frame_proof = {}
        if geographic_case:
            try:
                report = json.loads((folder / "geographic_view_audit.json").read_text(encoding="utf-8-sig"))
                valid &= report.get("valid") is True and report.get("region_index") == region and report.get("focus_index") == focus
                valid &= report.get("same_canonical_roster_across_views") is True
                valid &= report.get("unchanged_ground_builds_and_cooks_across_views") is True
                snapshots = report.get("snapshots", [])
                valid &= len(snapshots) == 4 and {s.get("stage") for s in snapshots} == {"street", "1250m", "5000m", "globe"}
                if len(snapshots) != 4:
                    raise ValueError("The four unchanged geographic frames are required")
                valid &= all(s.get("scene_saved") is True and s.get("real_geography") is True
                             and s.get("region_index") == region and s.get("focus_index") == focus
                             and s.get("continuation_version") == args.geography_version and s.get("ground_lod0_triangles", 0) > 0
                             and s.get("source_context_mesh_present") is True
                             and s.get("source_surface_mask_present") is True
                             and s.get("source_surface_mask_linear") is True
                             and s.get("source_surface_mask_enabled") == 1
                             and s.get("source_surface_mask_width") == 257 and s.get("source_surface_mask_height") == 257
                             and s.get("source_surface_mask_mips") == 9 for s in snapshots)
                if args.geography_version == 3:
                    valid &= all(valid_v3_material_slots(s, region) for s in snapshots)
                valid &= all("geo-view-" + stage + "-scene.png" in artifact_names for stage in ("street", "1250m", "5000m", "globe"))
                valid &= [s.get("tier") for s in snapshots] == [0, 2, 2, 3]
                valid &= all(s.get("planet_clip_parameter_present") is True and s.get("planet_material_masked") is True for s in snapshots)
                valid &= [s.get("planet_regional_clip_enabled") for s in snapshots] == [0, 1, 1, 0]
                valid &= all(confirmed_uncapped_capture(s) for s in snapshots)
                for snapshot in snapshots:
                    ground = [component for component in snapshot.get("render_components", [])
                              if component.get("component", "").endswith(".ContinuousTerrainSurface")]
                    valid &= len(ground) == 1
                    if ground:
                        triangles = ground[0].get("triangles_per_render_lod", [])
                        valid &= len(triangles) == 4 and all(type(n) is int and n > 0 for n in triangles)
                        valid &= all(far <= near for near, far in zip(triangles, triangles[1:]))
                # Exported pixels are evidence of real RHI frames, not a proof of
                # coastline accuracy, occlusion, mood or art quality.
                from PIL import Image, ImageStat
                for snapshot in snapshots:
                    stage = snapshot["stage"]
                    with Image.open(folder / f"geo-view-{stage}-scene.png") as frame:
                        rgb = frame.convert("RGB")
                        deviation = max(ImageStat.Stat(rgb).stddev)
                        valid &= frame.size == (1600, 900) and deviation >= 2
                        image_metrics[stage] = {"rgb_standard_deviation": round(deviation, 4)}
                if args.geographic_temporal:
                    temporal = report.get("temporal_snapshots", [])
                    valid &= report.get("temporal_pair_requested") is True and report.get("temporal_pair_complete") is True
                    valid &= len(temporal) == 2 and [s.get("stage") for s in temporal] == ["street-static-0", "street-static-1"]
                    valid &= all(s.get("scene_saved") is True and s.get("actual_camera_matches_pinned_capture_view") is True
                                 and s.get("moving_world_was_not_frozen") is True and s.get("flicker_free_claim") is False
                                 and s.get("canonical_roster_hash") == snapshots[0].get("canonical_roster_hash") for s in temporal)
                    if len(temporal) == 2:
                        valid &= temporal[1].get("frame_counter", -1) > temporal[0].get("frame_counter", -1)
                        valid &= temporal[1].get("world_time_seconds", -1) > temporal[0].get("world_time_seconds", -1)
                        valid &= all(temporal[0].get(field) == temporal[1].get(field) for field in ("pinned_capture_location_cm", "pinned_capture_rotation_degrees", "pinned_capture_fov_degrees"))
                    for stage in ("street-static-0", "street-static-1"):
                        with Image.open(folder / f"geo-view-{stage}-scene.png") as frame:
                            deviation = max(ImageStat.Stat(frame.convert("RGB")).stddev)
                            valid &= frame.size == (1600, 900) and deviation >= 2
                            image_metrics[stage] = {"rgb_standard_deviation": round(deviation, 4), "zero_flicker_claim": False}
                if args.geographic_regional:
                    regional = report.get("regional_snapshots", [])
                    valid &= report.get("regional_overview_requested") is True and report.get("regional_overview_complete") is True and len(regional) == 1
                    if len(regional) == 1:
                        view = regional[0]
                        valid &= view.get("stage") == "regional" and view.get("scene_saved") is True and view.get("metadata_valid") is True
                        valid &= view.get("region_index") == region and view.get("focus_index") == focus and view.get("continuation_version") == 3
                        valid &= view.get("tier") == 2 and 0 <= view.get("overview_blend", -1) < .2 and confirmed_uncapped_capture(view)
                        valid &= view.get("source_context_mesh_present") is True and view.get("actual_mesh_covers_source_roi_xy") is True
                        valid &= view.get("whole_source_roi_in_frame") is True and view.get("projection_viewport_width") == 1600 and view.get("projection_viewport_height") == 900
                        valid &= view.get("canonical_roster_hash") == snapshots[0].get("canonical_roster_hash") and view.get("canonical_living_residents") == snapshots[0].get("canonical_living_residents")
                        valid &= view.get("ground_builds") == snapshots[0].get("ground_builds") and view.get("ground_collision_cooks") == snapshots[0].get("ground_collision_cooks")
                        valid &= valid_v3_material_slots(view, region)
                        corners = view.get("source_roi_projected_corners", [])
                        valid &= len(corners) == 8 and {p.get("corner") for p in corners} == set(range(8))
                        valid &= all(p.get("projected") is True and p.get("inside_frame") is True
                                     and type(p.get("pixel_x")) in (int, float) and type(p.get("pixel_y")) in (int, float)
                                     and math.isfinite(p["pixel_x"]) and math.isfinite(p["pixel_y"])
                                     and 0 <= p["pixel_x"] <= 1600 and 0 <= p["pixel_y"] <= 900 for p in corners)
                    with Image.open(folder / "geo-view-regional-scene.png") as frame:
                        deviation = max(ImageStat.Stat(frame.convert("RGB")).stddev)
                        valid &= frame.size == (1600, 900) and deviation >= 2
                        image_metrics["regional"] = {"rgb_standard_deviation": round(deviation, 4), "source_roi_projection_is_geometry_not_image_accuracy_proof": True}
            except (ValueError, TypeError, KeyError, OSError, ImportError):
                valid = False
        elif fire_live_case:
            try:
                report = json.loads((folder / "fire_runtime_audit.json").read_text(encoding="utf-8-sig"))
                valid &= (report.get("valid") is True and report.get("in_progress") is False
                          and report.get("real_startup_preparation_completed") is True
                          and report.get("actual_model2_active_before_fire") is True
                          and report.get("one_production_fire_request_accepted") is True
                          and report.get("fixture_mutated_fire_or_terrain_cells") is False
                          and report.get("production_trigger_calls") == 1
                          and report.get("event_duration_seconds") == 24)
                trigger_ms = report.get("production_trigger_inclusive_game_thread_ms")
                valid &= type(trigger_ms) in (int, float) and math.isfinite(trigger_ms) and trigger_ms >= 0
                frames = report.get("frames", [])
                labels = ("fire-live-before", "fire-live-active-2", "fire-live-active-6", "fire-live-persistent-26")
                valid &= isinstance(frames, list) and len(frames) == 4 and tuple(row.get("label") for row in frames) == labels
                valid &= len({row.get("frame_counter") for row in frames}) == 4
                from PIL import Image, ImageChops, ImageStat
                for index, frame in enumerate(frames):
                    valid &= all(frame.get(key) is True for key in (
                        "valid", "scene_saved", "metadata_valid", "same_pinned_camera", "actual_model2_active"))
                    valid &= (frame.get("region_index") == 4 and frame.get("focus_index") == 0
                              and frame.get("source_version") == 3 and frame.get("actual_published_hydraulic_seconds", 0) > 0
                              and frame.get("fixture_set_time_dilation") is False
                              and frame.get("fixture_mutated_fire_or_terrain_cells") is False
                              and frame.get("fixture_advanced_or_shortened_event") is False)
                    valid &= confirmed_uncapped_capture(frame)
                    if index == 0:
                        valid &= frame.get("canonical_before_fire") is True
                    else:
                        valid &= all(frame.get(key) is True for key in (
                            "same_unchanged_24_second_fire_event", "actual_fire_display_ready",
                            "all_display_sources_are_canonical_burning_cells", "actual_nonzero_fire_source_in_frame"))
                        valid &= (frame.get("snapshot_revision") == report.get("snapshot_revision")
                                  and 0 < frame.get("source_count", 0) <= frame.get("source_limit", 0))
                        valid &= len(frame.get("layers", [])) == 4 and all(
                            layer.get("valid") is True and layer.get("mesh_is_non_planar") is True
                            and layer.get("exact_parent") is True and layer.get("own_current_resource_ready") is True
                            and layer.get("actual_vertex_and_index_rhi_buffers_ready") is True
                            and layer.get("actual_scene_proxy_present") is True
                            and layer.get("transforms_finite") is True and layer.get("lod0_triangles", 0) > 24
                            for layer in frame.get("layers", []))
                        age = frame.get("event_age_seconds", -1)
                        valid &= math.isfinite(age) and age >= (2 if index == 1 else 6 if index == 2 else 26)
                        valid &= frame.get("persistent_handoff") is (index == 3)
                    image_name = frame.get("label", "") + "-scene.png"
                    valid &= image_name in artifact_names
                    with Image.open(folder / image_name) as image:
                        deviation = max(ImageStat.Stat(image.convert("RGB")).stddev)
                        valid &= image.size == (1600, 900) and deviation >= 2
                        image_metrics[frame["label"]] = {"rgb_standard_deviation": round(deviation, 4), "artistic_quality_accepted": False}
                if len(frames) == 4:
                    active, persistent = frames[2], frames[3]
                    valid &= all(active.get(key) is True for key in (
                        "matched_outer_hidden_capture_saved", "matched_smoke_hidden_capture_saved",
                        "diagnostic_pairs_same_frame_and_world_time"))
                    valid &= persistent.get("actual_persistent_handoff_game_thread_ms", 0) > 0
                    valid &= persistent.get("event_flame_smoke_instances_released_after_handoff") is True
                    for row, field in ((active, "active_animation_cpu"), (persistent, "persistent_animation_cpu")):
                        timing = row.get(field, {})
                        valid &= timing.get("updates", 0) > 0
                        valid &= all(type(timing.get(key)) in (int, float) and math.isfinite(timing[key]) and timing[key] >= 0
                                     for key in ("total_game_thread_ms", "average_game_thread_ms", "peak_game_thread_ms"))
                    with Image.open(folder / "fire-live-active-6-scene.png") as image:
                        visible = image.convert("RGB")
                    for kind in ("outer", "smoke"):
                        hidden_name = f"fire-live-active-6-hidden-{kind}-scene.png"
                        valid &= hidden_name in artifact_names
                        with Image.open(folder / hidden_name) as image:
                            valid &= image.size == (1600, 900)
                            delta = ImageChops.difference(visible, image.convert("RGB"))
                        difference = sum(ImageStat.Stat(delta).mean) / 3
                        changed = sum(max(pixel) > 3 for pixel in delta.getdata())
                        valid &= difference >= .01 and changed >= 32
                        image_metrics["matched_hidden_" + kind] = {
                            "mean_absolute_rgb_difference": round(difference, 6), "changed_pixels_above_3_rgb": changed,
                            "scope": "Same-frame single-component SceneCapture exclusion supports actual rendered contribution; independent renderer noise and artistic quality still require inspection.",
                            "gpu_timing_measured": False, "zero_flicker_claim": False}
            except (ValueError, TypeError, KeyError, OSError, ImportError, IndexError):
                valid = False
        elif physical_flood_case:
            try:
                report = json.loads((folder / "physical_flood_runtime_audit.json").read_text(encoding="utf-8-sig"))
                valid &= (report.get("valid") is True and report.get("region_index") == 1 and report.get("focus_index") == 0
                          and report.get("source_version") == 3 and report.get("flood_model_version") == 1
                          and report.get("terrain_or_water_edits_by_fixture") is False)
                frames = report.get("frames", [])
                stages = ("arrival-start", "arrival-intermediate", "fully-arrived-before-drain")
                valid &= isinstance(frames, list) and len(frames) == 3 and tuple(frame.get("stage") for frame in frames) == stages
                valid &= len({frame.get("frame_counter") for frame in frames}) == 3
                valid &= all(frames[index].get("event_age_seconds", -1) < frames[index + 1].get("event_age_seconds", -1)
                             for index in range(len(frames) - 1))
                from PIL import Image, ImageStat
                for frame in frames:
                    valid &= all(frame.get(field) is True for field in (
                        "valid", "scene_saved", "metadata_valid", "source_geometry_unchanged", "canonical_water_snapshot_unchanged",
                        "water_maps_paired_finite_bed_head_consistent", "cpu_depth_matches_actual_material_timeline",
                        "actual_material_parameters_read", "actual_flood_component_visible", "actual_flood_single_nonzero_instance", "actual_flood_full_precision_uvs",
                        "real_wet_land_focus_in_frame"))
                    valid &= confirmed_uncapped_capture(frame)
                    valid &= frame.get("continuation_version") == 3 and frame.get("region_index") == 1 and frame.get("focus_index") == 0
                    valid &= frame.get("flood_model_version") == 1 and frame.get("rainfall_pulse_mm") == 140 and frame.get("physical_pulse_duration_seconds") == 3600
                    counts = [frame.get(key) for key in ("depth_map_count", "head_map_count", "arrival_map_count")]
                    valid &= all(type(count) is int and count > 0 for count in counts) and len(set(counts)) == 1
                    valid &= frame.get("actual_flood_lod0_triangles", 0) > 0 and frame.get("canonical_above_ground_land_cells", 0) > 0
                    valid &= frame.get("actual_flood_uv_channels", 0) >= 3
                    valid &= frame.get("water_map_allocated_bytes", 0) > 0 and bool(frame.get("bounded_actual_land_samples"))
                    valid &= frame.get("geometry_hash") == frames[0].get("geometry_hash") and frame.get("water_snapshot_hash") == frames[0].get("water_snapshot_hash")
                    valid &= frame.get("snapshot_revision") == frames[0].get("snapshot_revision")
                    material = frame.get("actual_flood_material", {})
                    valid &= all(material.get(key + "_present") is True and material.get(key + "_finite") is True
                                 for key in ("FloodEncodingCm", "FloodPhysicalArrivalEnabled", "FloodPhysicalDataEnabled", "FloodArrivalProgress", "FloodEnvelope"))
                    valid &= material.get("FloodEncodingCm", 0) >= 512 and material.get("FloodPhysicalArrivalEnabled") == 1 and material.get("FloodPhysicalDataEnabled") == 1
                    budget = frame.get("canonical_event_mass_budget", {})
                    valid &= budget.get("committed_storage") is True and budget.get("mass_conserved") is True
                    values = [budget.get(key) for key in ("rainfall_m3", "initial_river_storage_m3", "infiltration_m3", "drainage_m3", "outlet_m3", "dam_retained_m3", "final_storage_m3", "mass_residual_m3", "tolerance_m3")]
                    valid &= all(type(value) in (int, float) and math.isfinite(value) for value in values)
                    if all(type(value) in (int, float) and math.isfinite(value) for value in values):
                        rain, initial, infiltration, drainage, outlet, dam, final, residual, tolerance = values
                        valid &= rain > 0 and all(value >= 0 for value in values[:7]) and tolerance > 0
                        valid &= abs(residual) <= tolerance and abs(initial + rain - infiltration - drainage - outlet - dam - final - residual) <= tolerance
                    valid &= budget.get("active_cells", 0) > 0 and budget.get("iterations", 0) > 0 and budget.get("edge_visits", 0) > 0
                    stage = frame["stage"]
                    progress, envelope = material.get("FloodArrivalProgress", -1), material.get("FloodEnvelope", -1)
                    if stage == stages[0]:
                        valid &= progress == 0 and envelope == 0 and frame.get("maximum_cpu_displayed_cm") == 0
                    elif stage == stages[1]:
                        valid &= 0 < progress < 1 and envelope >= .99 and frame.get("drawable_displayed_land_cells", 0) > 0
                    else:
                        valid &= progress == 1 and envelope >= .99 and frame.get("drawable_displayed_land_cells", 0) > 0
                        valid &= frame.get("event_age_seconds", math.inf) < frame.get("event_duration_seconds", 0) - 8
                    for sample in frame.get("bounded_actual_land_samples", []):
                        numeric = [sample.get(key) for key in ("ground_cm", "bed_column_cm", "absolute_head_cm", "arrival_fraction", "above_ground_peak_cm", "cpu_displayed_cm", "expected_from_actual_material_cm")]
                        valid &= all(type(value) in (int, float) and math.isfinite(value) for value in numeric)
                        if all(type(value) in (int, float) and math.isfinite(value) for value in numeric):
                            ground, depth, head, arrival, peak, actual, expected = numeric
                            valid &= depth >= 0 and 0 <= arrival <= 1 and abs(max(0, head - ground) - peak) <= .05 and abs(actual - expected) <= .05
                    image_name = "physical-flood-" + stage + "-scene.png"
                    valid &= image_name in artifact_names
                    with Image.open(folder / image_name) as image:
                        deviation = max(ImageStat.Stat(image.convert("RGB")).stddev)
                        valid &= image.size == (1600, 900) and deviation >= 2
                        image_metrics[stage] = {"rgb_standard_deviation": round(deviation, 4), "physical_accuracy_not_proven_by_png": True}
            except (ValueError, TypeError, KeyError, OSError, ImportError, IndexError):
                valid = False
        elif name == "narrative":
            try:
                report = json.loads((folder / "narrative_runtime_audit.json").read_text(encoding="utf-8-sig"))
                valid &= report.get("valid") is True and report.get("failed") == 0 and report.get("passed", 0) > 0
                valid &= 0 <= report.get("tutorial_completed_elapsed_real_seconds", -1) <= 30.15
                valid &= report.get("schema_version") == 4 and report.get("tutorial_step_frame_processed") is True
                valid &= 1 <= report.get("tutorial_step_requested", -1) <= report.get("tutorial_step_processed", -1) < 6
                valid &= report.get("tutorial_skip_mode") is args.tutorial_skip and report.get("tutorial_skip_requested") is args.tutorial_skip
                valid &= all("narrative-" + stage + ".png" in artifact_names for stage in ("tutorial", "tutorial-step", "playing", "epilogue", "ledger"))
                valid &= all(report.get(field) is True for field in (
                    "explicit_induced_resident_death_fixture", "resident_death_fixture_applied",
                    "ledger_double_settlement_unchanged", "ledger_widget_identity_bound",
                    "ledger_widget_story_bound", "ledger_widget_snapshot_bound", "ledger_frame_captured"))
                ledger_numbers = ("ledger_resident_id", "ledger_deaths_before", "ledger_deaths_after",
                                  "ledger_treasury_before", "ledger_treasury_after",
                                  "ledger_pending_aid_before", "ledger_pending_aid_after")
                valid &= all(type(report.get(field)) is int for field in ledger_numbers)
                valid &= report.get("ledger_resident_id", 0) > 0
                valid &= bool(re.fullmatch(r"[0-9A-Fa-f]{32}", report.get("ledger_event_id", "")))
                valid &= report.get("ledger_event_id", "") != "0" * 32
                valid &= report["ledger_deaths_after"] == report["ledger_deaths_before"] + 1
                valid &= report["ledger_treasury_after"] == report["ledger_treasury_before"] - 500
                valid &= report["ledger_pending_aid_after"] == report["ledger_pending_aid_before"]
                required_tutorial_checks = {
                    "tutorial_is_actual_visible_widget_and_canonical_intro_flow",
                    "tutorial_pauses_city_and_locks_visible_dashboard", "tutorial_consumes_newgame_marker_without_config_writes",
                    "tutorial_blocks_public_pan_zoom_and_rotation", "tutorial_returns_to_playing_using_real_autoplay_or_skip",
                    "paused_tutorial_keeps_canonical_turn_treasury_and_living_count",
                    "consumed_newgame_marker_cannot_replay_tutorial", "actual_live_continue_returns_without_tutorial_or_city_reset"}
                passed_tutorial = {check.get("name") for check in report.get("checks", []) if check.get("passed") is True}
                valid &= required_tutorial_checks <= passed_tutorial
                for stage in ("tutorial", "tutorial-step", "playing"):
                    from PIL import Image, ImageStat
                    with Image.open(folder / ("narrative-" + stage + ".png")) as frame:
                        deviation = max(ImageStat.Stat(frame.convert("RGB")).stddev)
                        valid &= frame.size == (1600, 900) and deviation >= 2
                        image_metrics[stage] = {"rgb_standard_deviation": round(deviation, 4), "manual_readability_review_still_required": True}
                coverage = re.findall(r"AFTERSHOCK_ROAD_PRESENTATION_COVERAGE [^\r\n]*omittedInitialRoads=(\d+)", log)
                valid &= bool(coverage) and all(value == "0" for value in coverage)
                # Structural widget checks alone missed a white Slate fill.
                # Inspect actual exported pixels for the authored dark reading
                # surface and a visible, contrasting title on both end frames.
                from PIL import Image, ImageStat
                for stage, title_y in (("epilogue", (112, 160)),):
                    with Image.open(folder / ("narrative-" + stage + ".png")) as frame:
                        frame = frame.convert("RGB")
                        background = ImageStat.Stat(frame.crop((20, 400, 70, 450))).mean
                        title = frame.crop((96, title_y[0], 700, title_y[1]))
                        brightest = max(sum(pixel) / 3 for pixel in title.get_flattened_data())
                        contrast = brightest - sum(background) / 3
                        image_metrics[stage] = {"background_rgb": [round(v, 2) for v in background], "title_contrast_rgb": round(contrast, 2)}
                        valid &= frame.size == (1600, 900) and max(background) < 120 and contrast >= 80
            except (ValueError, TypeError, KeyError, OSError, ImportError):
                valid = False
        elif chunk_case:
            valid &= {"studio-chunk-runtime.png", "chunk_runtime_audit.json"}.issubset(artifact_names)
            try:
                report = json.loads((folder / "chunk_runtime_audit.json").read_text(encoding="utf-8"))
                valid &= report.get("passed") is True and all(c.get("passed") is True for c in report.get("checks", []))
                valid &= bool(report.get("checks")) and report.get("focus_y", 0) < 0
                if name == "chunk-chaos":
                    valid &= report.get("optional_chaos_fixture_requested") is True
                    valid &= report.get("near_released_before4s") is True and report.get("far_static_started") is True
            except (ValueError, OSError):
                valid = False
        elif name.startswith("profile-"):
            count = int(name[8:])
            valid &= f"AFTERSHOCK_SYNTHETIC_RENDER_PROFILE_CPU agents={count} " in log
            valid &= f"crowd-render-{count}.csv" in artifact_names
            visibility = re.findall(r"AFTERSHOCK_CROWD_VISIBILITY [^\r\n]*invalidPlayable=(\d+)", log)
            valid &= bool(visibility) and visibility[-1] == "0"
            clearance = re.findall(r"AFTERSHOCK_CROWD_PHYSICAL_CLEARANCE outdoor=(\d+) [^\r\n]*crowdBodyOverlaps=(\d+) peopleIntersectingVehicleBodies=(\d+)", log)
            valid &= bool(clearance) and clearance[-1][1:] == ("0", "0")
            if args.overview is None or args.overview == 0:
                valid &= bool(clearance) and int(clearance[-1][0]) > 0
            if args.overview is not None:
                try:
                    view = json.loads((folder / "profile-view-metadata.json").read_text(encoding="utf-8-sig"))
                    valid &= view.get("metadata_valid") is True and view.get("synthetic_render_profile") is True
                    valid &= confirmed_uncapped_capture(view)
                    valid &= view.get("visual_agents") == count and abs(view.get("overview_blend", -1) - args.overview) <= .01
                    if args.overview >= .2:
                        valid &= view.get("citizen_component_visible") is False and view.get("budget_draw_citizens") is False
                except (ValueError, TypeError, OSError):
                    valid = False
            if args.profile_duration or args.overview is not None:
                try:
                    profile = json.loads((folder / f"crowd-render-{count}.profile.json").read_text(encoding="utf-8-sig"))
                    valid &= profile.get("csv_ready") is True and profile.get("frame_count_valid") is True
                    valid &= profile.get("timed_out") is False
                    if args.profile_duration:
                        valid &= profile.get("captured_frame_seconds", 0) >= args.profile_duration
                    if args.overview is not None:
                        valid &= confirmed_uncapped_capture(profile)
                except (ValueError, TypeError, OSError):
                    valid = False
        elif name in {"chaos", "chaos-new-city"}:
            valid &= "AFTERSHOCK_CHAOS_AUDIT_RESULT" in log and "ccd=1" in log
            valid &= "uniqueAssets=5 allFiveRequested=1" in log
            below = [int(value) for value in re.findall(r"unanchoredBelowTerrainLeaves=(\d+)", log)]
            valid &= len(below) == 5 and all(value == 0 for value in below)
            escaped = [int(value) for value in re.findall(r"escapedFoundationLeaves=(\d+)", log)]
            valid &= len(escaped) == 5 and all(value == 0 for value in escaped)
            foundations = re.findall(r"anchoredFoundationLeaves=(\d+).*?kinematicFoundationLeaves=(\d+)", log)
            valid &= len(foundations) == 5 and all(int(a) == int(k) for a, k in foundations)
            valid &= "geometryActors=5 frozenAfter4Seconds=5" in log
            missing = [int(value) for value in re.findall(r"missingCookedImplicits=(\d+)", log)]
            valid &= len(missing) == 5 and all(value == 0 for value in missing)
            valid &= "studio-induced-collapse.png" in artifact_names
        elif name == "rubble-save":
            valid &= "AFTERSHOCK_RUBBLE_RUNTIME_RESULT passed=1" in log
            valid &= "studio-rubble-restored.png" in artifact_names
        elif name == "bridge":
            valid &= "AFTERSHOCK_BRIDGE_RUNTIME_RESULT passed=1" in log
            valid &= "studio-bridge-runtime.png" in artifact_names
        elif name == "globe":
            valid &= "outsideSphere=1" in log
            valid &= "globe-overview-scene.png" in artifact_names
        else:
            valid &= "AFTERSHOCK_SCENARIO_RENDER_AUDIT" in log and "AFTERSHOCK_DISASTER_PRESENTATION_PEAK" in log
            valid &= f"{name}-peak-scene.png" in artifact_names
            if name == "flood":
                # A later PEAK marker cannot validate an earlier screenshot.
                # Read the state logged by the SAME scene-capture callback.
                flood_capture_states = re.findall(r"AFTERSHOCK_SCENARIO_RENDER_AUDIT scenario=flood-peak ([^\r\n]+)", log)
                fields = dict(re.findall(r"([A-Za-z][A-Za-z0-9_]*)=([0-9.]+)", flood_capture_states[-1])) if flood_capture_states else {}
                try:
                    valid &= fields.get("sceneSaved") == "1" and fields.get("floodVisible") == "1"
                    valid &= float(fields.get("floodEnvelope", -1)) >= .99
                    valid &= float(fields.get("riverFillProgress", -1)) >= .99
                    valid &= int(fields.get("visibleFloodLandCells", 0)) > 0 and int(fields.get("floodFrontTriangles", 0)) > 0
                except (ValueError, TypeError):
                    valid = False
            if real_volcano_case:
                try:
                    receipt = folder / "volcano_runtime_audit.json"
                    report = json.loads(receipt.read_text(encoding="utf-8-sig"))
                    proof_fields = ("valid", "scene_saved", "actual_mapped_vent", "mapped_vent_in_frame",
                                    "plume_300m_top_in_frame", "actual_vent_lava_plume_meshes")
                    valid &= receipt.name in artifact_names and receipt.stat().st_mtime >= started - 1
                    valid &= all(report.get(field) is True for field in proof_fields)
                    valid &= report.get("viewport_width") == 1600 and report.get("viewport_height") == 900
                    valid &= "AFTERSHOCK_VOLCANO_FRAME_AUDIT valid=1" in log
                    from PIL import Image
                    with Image.open(folder / "volcano-peak-scene.png") as image:
                        valid &= image.size == (1600, 900)
                    volcano_frame_proof = {field: report.get(field) for field in proof_fields}
                    volcano_frame_proof.update({"receipt": receipt.name,
                                               "viewport_width": report.get("viewport_width"),
                                               "viewport_height": report.get("viewport_height"),
                                               "vent_pixel_x": report.get("vent_pixel_x"),
                                               "vent_pixel_y": report.get("vent_pixel_y")})
                except (OSError, ValueError, TypeError):
                    valid = False
        events = [line for line in log.splitlines() if "AFTERSHOCK_" in line]
        graphics_quality_proof = None
        if args.quality is not None:
            graphics_quality_proof = read_requested_graphics_quality(folder, args.quality, started, isolated_settings)
            valid &= graphics_quality_proof["confirmed"] is True
        (folder / "events.txt").write_text("\n".join(events) + "\n", encoding="utf-8")
        valid = bool(valid) and user_settings_proof["originals_unchanged"]
        row = {"case": name, "scenario": index, "exit_code": code, "valid": bool(valid),
               "seconds": round(time.time() - started, 2), "artifacts": artifact_names,
               "engine_errors": errors, "utc": datetime.now(timezone.utc).isoformat()}
        row.update({"legacy_geography_requested": args.legacy_geography, "region_index_requested": region,
                    "focus_index_requested": focus if region is not None else None,
                    "source_version_requested": 3 if physical_flood_case or fire_live_case else args.geography_version if geographic_case else None,
                    "geographic_regional_requested": args.geographic_regional,
                    "overview_requested": args.overview})
        row["graphics_quality_requested"] = args.quality
        if graphics_quality_proof is not None:
            row["graphics_quality_proof"] = graphics_quality_proof
        if image_metrics:
            row["image_metrics"] = image_metrics
        if real_volcano_case:
            row["volcano_frame_proof"] = volcano_frame_proof
        if name.startswith("profile-"):
            row["requested_capture_seconds"] = args.profile_duration
        row["user_settings_isolation"] = user_settings_proof
        results.append(row)
        (output / "summary.json").write_text(json.dumps({"source_sha": revision, "working_tree_dirty_at_start": dirty, "scope": "Real D3D12 offscreen development fixtures, 1600x900. Not mouse input, packaged execution or calibrated disaster prediction.", "user_settings_preservation": settings_isolation.current(), "results": results}, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(row), flush=True)
    return 0 if all(row["valid"] for row in results) and settings_isolation.current()["originals_unchanged"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
