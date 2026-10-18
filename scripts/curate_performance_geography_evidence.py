"""Export completed 18 October performance/geography evidence; never run UE or Git.

Required inputs are the clean baseline memory/RHI root, final native runner root,
final RHI runner root and final memory/profile root. Repeat --fixed-rhi and
--fixed-profile for separate completed runs. A root may contain several
completed runner summaries. --baseline-native is a separate, explicitly dirty
diagnostic baseline; its known CityLife 26/2 failure is never relabelled passing.
The default native inventory is the current33-suite set. Historical30-suite
exports require --historical-native-30 (or native(..., historical_30=True)) and
are labelled historical; they cannot imply the three new suites passed.

Output must be a NEW directory under a 2026-10-18 path. Only allowlisted JSON,
runner-referenced game PNGs and explicitly selected numeric CSV columns leave
the inputs. No raw logs, EVENTS, CSV machine/account footer, PID or local paths.
An interrupted export keeps INCOMPLETE.txt; it never removes an existing output.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import zlib


BASELINE_SHA = "5fa54a81e860890f93772250af6201fad1dac9eb"
HARNESS_FILES = {
    "?? Source/JamProject/Aftershock/Private/AftershockMutationSoakCommandlet.cpp",
    "?? Source/JamProject/Aftershock/Private/AftershockMutationSoakCommandlet.h",
}
CORRECTED_HARNESS_HASHES = {
    "cpp": "ff25025d07df041a2fcd23a2eddb2fb05dcda59c1b29d3651e7fc39241cf8aa0",
    "h": "37a15aa8c84fcaa1e70eb1975fad09946c81271cdf313ba4c73c0b5dc55323dc",
}
CASES = {
    "chaos", "chaos-new-city", "rubble-save", "bridge", "chunks", "chunk-chaos",
    "narrative", "flood", "wind", "tsunami", "fire", "volcano", "drought", "globe",
    "profile-500", "profile-1500", "geo-atakent", "geo-surmene", "geo-miami",
    "geo-cyprus", "geo-etna", "geo-globe", "geo-valparaiso", "geo-sendai", "geo-new-orleans",
    "physical-flood",
}


def geographic_focus_cases(catalog: Path | None = None) -> dict[str, tuple[int, int]]:
    """Derive exact identities from the catalogue compiled by the actual game."""
    catalog = catalog or Path(__file__).resolve().parents[1] / "Source/JamProject/Aftershock/Private/AftershockRegionCatalog.inl"
    source = catalog.read_text(encoding="utf-8")
    if source.count("GeographicFocuses[] = {") != 1 or source.count("GeographicRegions[] = {") != 1:
        raise ValueError("Missing/ambiguous compiled geographic catalogue")
    focuses = source.split("GeographicFocuses[] = {", 1)[1].split("};", 1)[0]
    identities = [re.findall(r'TEXT\("([^"\\]*)"\)', line)
                  for line in focuses.splitlines() if line.strip().startswith("{")]
    regions = source.split("GeographicRegions[] = {", 1)[1].split("};", 1)[0]
    ranges = re.findall(r"\{\s*(\d+),\s*(\d+)\s*\},\s*//\s*([a-z0-9_]+)", regions)
    if len(identities) != 25 or len(ranges) != 20 or any(len(row) != 4 for row in identities):
        raise ValueError("Expected the actual20-region/25-focus compiled catalogue")
    result, seen, region_ids = {}, set(), set()
    for region, (first, count, region_id) in enumerate(ranges):
        if region_id in region_ids or not re.fullmatch(r"[a-z][a-z0-9_]*", region_id) or int(count) < 1:
            raise ValueError("Duplicate/invalid compiled geographic region identity")
        region_ids.add(region_id)
        for focus in range(int(count)):
            flat = int(first) + focus
            if flat in seen or not 0 <= flat < 25 or identities[flat][0] != region_id:
                raise ValueError("Compiled geographic focus ranges overlap or differ")
            seen.add(flat)
            focus_id = identities[flat][2]
            if not re.fullmatch(r"[a-z][a-z0-9_]*", focus_id):
                raise ValueError("Invalid compiled geographic focus identity")
            label = f"geo-focus-{region_id}-{focus_id}"
            if label in result:
                raise ValueError("Duplicate compiled geographic focus label")
            result[label] = (region, focus)
    if seen != set(range(25)):
        raise ValueError("Compiled geographic focus coverage is incomplete")
    return result


GEOGRAPHIC_FOCUS_CASES = geographic_focus_cases()
GEOGRAPHIC_CASES = {"geo-atakent": (0, 0), "geo-surmene": (1, 0), "geo-miami": (7, 0),
                    "geo-cyprus": (8, 0), "geo-etna": (5, 0), "geo-globe": (0, 0),
                    "geo-valparaiso": (10, 0), "geo-sendai": (12, 0), "geo-new-orleans": (14, 0),
                    **GEOGRAPHIC_FOCUS_CASES}
CASES.update(GEOGRAPHIC_FOCUS_CASES)
HISTORICAL_NATIVE_REPORTS = {
    "AftershockCityLifeAudit": "city_life_audit.json",
    "AftershockTransportAudit": None,
    "AftershockGeographicHazardsAudit": "geographic_hazards_audit.json",
    "AftershockPlayableAudit": "playable_audit.json",
    "AftershockFloodVisualAudit": "flood_visual_audit.json",
    "AftershockCitizenPlanning": None,
    "AftershockCoastalInfrastructureAudit": "coastal_infrastructure_audit.json",
    "AftershockHUDInputAudit": "hud_input_audit.json",
    "AftershockRegionCatalogAudit": "region_catalog_audit.json",
    "AftershockRegionSettlementAudit": "region_settlement_audit.json",
    "AftershockBridgeAudit": "bridge_audit.json",
    "AftershockCrowdSpacingAudit": "crowd_spacing_audit.json",
    "AftershockSessionSaveAudit": "session_save_audit.json",
    "AftershockRubblePersistenceAudit": "rubble_persistence_audit.json",
    "AftershockCasualtyLedgerAudit": "casualty_ledger_audit.json",
    "AftershockCityScaleAudit": "city_scale_audit.json",
    "AftershockChunkAudit": "chunk_audit.json",
    "AftershockChunkSaveAudit": "chunk_save_audit.json",
    "AftershockPresentationPerformanceAudit": "presentation_performance_audit.json",
    "AftershockPresentationBudgetAudit": "presentation_budget_audit.json",
    "AftershockMutationSoak": "mutation_soak.json",
    "AftershockRealGeographyAudit": "real_geography_audit.json",
    "AftershockRealRegionSettlementAudit": "real_region_settlement_audit.json",
    **{f"scenario-{i}": f"scenario_audit_{i}.json" for i in range(7)},
}
NEW_NATIVE_REPORTS = {
    "AftershockPhysicalHazardsAudit": "physical_hazards_audit.json",
    "AftershockRegionalEnvironmentAudit": "regional_environment_audit.json",
    "AftershockSaveCodecAudit": "save_codec_audit.json",
}
NATIVE_REPORTS = {**HISTORICAL_NATIVE_REPORTS, **NEW_NATIVE_REPORTS}
NATIVE_JSON = {name for name in NATIVE_REPORTS.values() if name is not None}
RHI_JSON = {"chunk_runtime_audit.json", "bridge_runtime_audit.json", "narrative_runtime_audit.json",
            "geographic_view_audit.json", "profile-view-metadata.json",
            "physical_flood_runtime_audit.json", "volcano_runtime_audit.json",
            "crowd-render-500.profile.json", "crowd-render-1500.profile.json"}
PNG_NAMES = {
    "studio-runtime-scene.png", "studio-runtime-hud.png", "studio-induced-collapse.png",
    "globe-overview-scene.png", "globe-overview-hud.png", "studio-rubble-restored.png",
    "studio-bridge-runtime.png", "studio-chunk-runtime.png",
    *{f"narrative-{stage}.png" for stage in ("intro", "pour", "playing", "epilogue", "ledger")},
    *{f"{hazard}-peak-{layer}.png" for hazard in ("flood", "wind", "tsunami", "fire", "volcano", "drought") for layer in ("scene", "hud")},
    *{f"geo-view-{stage}-{layer}.png" for stage in ("street", "1250m", "5000m", "globe", "regional", "street-static-0", "street-static-1") for layer in ("scene", "hud")},
    *{f"physical-flood-{stage}-scene.png" for stage in ("arrival-start", "arrival-intermediate", "fully-arrived-before-drain")},
}
FRAME_COLUMNS = {
    "FrameTime", "GameThreadTime", "RenderThreadTime", "RHIThreadTime", "GPUTime", "MaxFrameTime",
    "GameThreadTime_CriticalPath", "RenderThreadTime_CriticalPath",
    "AftershockCrowd/GameThread/LocalSeparation", "AftershockCrowd/GameThread/UpdatePositions",
    "AftershockCrowd/SyntheticAgents", "AftershockCrowd/SyntheticMovingAgents", "NumInstanceTransformUpdates",
    "Ticks/Total", "RHI/DrawCalls", "RHI/PrimitivesDrawn", "GPUSceneInstanceCount",
    "PhysicalUsedMB", "VirtualUsedMB", "GPUMem/LocalUsedMB", "GPUMem/SystemUsedMB",
    "TextureStreaming/ResidentMeshMem", "TextureStreaming/StreamedMeshMem",
    "TextureStreaming/CachedMips", "TextureStreaming/WantedMips", "TextureStreaming/NonStreamingMips",
    "TransientMemoryUsedMB", "TransientMemoryAliasedMB", "RenderTargetPool/PeakUsedMB", "RenderTargetPoolSize",
}
MEMORY_COLUMNS = ("monitor_seconds", "process_observed_seconds", "working_set_bytes", "private_commit_bytes",
                  "peak_working_set_bytes", "page_fault_count")
TEXT_FIELDS = {
    "scope", "detail", "description", "name", "case", "stage", "report", "target", "configuration", "platform",
    "result", "capture_mode", "utc", "created_utc", "mode", "region_id", "focus_id", "source_sha",
    "sha256", "monitor_sha256", "canonical_roster_hash", "component", "mesh", "dirty_files", "artifacts",
    "unit", "type", "status", "capture_policy", "dynamic_header_policy", "checksum_scope", "geography_mode",
    "native_suite_scope", "schema_fingerprint", "texture", "material", "receipt", "ue_texture_path", "source_matrix_sha256",
}
FORBIDDEN_FIELDS = {
    "command", "commands", "args", "arguments", "console", "log", "logs", "raw_log", "output", "stdout", "stderr",
    "account", "account_id", "username", "user", "hostname", "computername", "machine", "machine_id", "pid",
    "path", "filepath", "filename", "module_path", "dll", "pdb", "stack", "stacktrace", "engine_errors", "engine_error",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path: Path) -> dict:
    require(path.is_file() and not path.is_symlink(), f"Missing or linked JSON: {path.name}")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    require(isinstance(value, dict), f"JSON root is not an object: {path.name}")
    return value


def number(value) -> bool:
    return type(value) in (int, float) and math.isfinite(value)


def valid_sha(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{40}", value) is not None


class Exporter:
    def __init__(self, output: Path):
        self.output = output
        self.provenance = []
        self.csv_reports = []
        self.redactions = 0
        self.omitted_text_fields = 0
        identities = {Path.home().name, os.environ.get("USERNAME", ""), os.environ.get("COMPUTERNAME", "")}
        self.identities = tuple(value for value in identities if len(value) >= 3)

    def text(self, value: str) -> str:
        original = value
        value = re.sub(r"(?i)(?:file:/+)?\b[a-z]:[\\/][^\r\n\"<>]*", "<LOCAL_PATH>", value)
        value = re.sub(r"\\\\[^\r\n\"<>]+", "<LOCAL_PATH>", value)
        value = re.sub(r"(?<!\w)/(?!Game/|Engine/|Script/|Temp/)[\w.-]+(?:/[\w. -]+)+", "<LOCAL_PATH>", value)
        value = re.sub(r"[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}", "<ACCOUNT>", value)
        value = re.sub(r"(?i)\b(?:EpicAccountId|MachineId|LoginId|UserName|ComputerName|AuthToken)\s*[:=]\s*[A-Za-z0-9_.-]+", "<ACCOUNT>", value)
        for identity in self.identities:
            value = re.sub(r"\b" + re.escape(identity) + r"\b", "<ACCOUNT>", value, flags=re.I)
        self.redactions += value != original
        return value

    def tree(self, value, key: str = ""):
        if value is None or type(value) is bool:
            return value
        if number(value):
            return value
        if isinstance(value, float):
            raise ValueError("Nonfinite JSON metric")
        if isinstance(value, str):
            if key not in TEXT_FIELDS:
                self.omitted_text_fields += 1
                return None
            return self.text(value)
        if isinstance(value, list):
            return [self.tree(item, key) for item in value if not isinstance(item, str) or key in TEXT_FIELDS]
        require(isinstance(value, dict), "Unsupported JSON value")
        result = {}
        for field, item in value.items():
            require(isinstance(field, str) and (re.fullmatch(r"[A-Za-z_][A-Za-z0-9_./-]*", field)
                    or key == "image_metrics" and field in {"1250m", "5000m"}), "Unsafe structured JSON key")
            sensitive_key = re.search(r"(?i)account|user_name|user_id|username|hostname|computername|machine_id|machineid|password|credential|auth_token|login_id", field)
            if field.lower() in FORBIDDEN_FIELDS or sensitive_key:
                if field in {"engine_errors", "engine_error"} and item in (None, [], False):
                    result[field] = item  # Preserve recorded absence, never raw error text.
                continue
            if isinstance(item, str) and field not in TEXT_FIELDS:
                self.omitted_text_fields += 1
                continue
            result[field] = self.tree(item, field)
        return result

    def record(self, source: Path, target: Path, kind: str) -> None:
        self.provenance.append({"path": target.relative_to(self.output).as_posix(), "kind": kind,
                                "input_bytes": source.stat().st_size, "input_sha256": digest(source)})

    def json(self, target: Path, value: dict, source: Path | None = None) -> None:
        target.parent.mkdir(parents=True, exist_ok=True)
        # Generated manifests contain only our own validated relative output
        # paths. Raw source JSON still passes the strict field/text policy.
        portable = self.tree(value) if source is not None else value
        target.write_text(json.dumps(portable, ensure_ascii=False, allow_nan=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        if source:
            self.record(source, target, "structured_json_allowlist")

    def png(self, source: Path, target: Path) -> None:
        data = source.read_bytes()
        require(data[:8] == b"\x89PNG\r\n\x1a\n", "Invalid PNG signature")
        position, ended = 8, False
        while position + 12 <= len(data):
            size = struct.unpack_from(">I", data, position)[0]
            kind = data[position+4:position+8]
            require(size <= 32 * 1024 * 1024 and position + size + 12 <= len(data), "Invalid PNG chunk length")
            require(kind in {b"IHDR", b"IDAT", b"IEND", b"sRGB", b"gAMA", b"cHRM", b"pHYs", b"PLTE", b"tRNS"}, "PNG contains nonallowlisted metadata")
            expected_crc = struct.unpack_from(">I", data, position+8+size)[0]
            require(zlib.crc32(data[position+4:position+8+size]) & 0xffffffff == expected_crc, "PNG CRC mismatch")
            position += size + 12
            if kind == b"IEND":
                ended = True
                break
        require(ended and position == len(data), "PNG incomplete or has trailing data")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        self.record(source, target, "referenced_game_png_unchanged_pixels_and_bytes")


def reference(folder: Path, name: str) -> Path:
    require(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name), "Artifact must be a plain portable filename")
    path = folder / name
    require(path.is_file() and not path.is_symlink() and path.resolve().parent == folder.resolve(), "Missing/linked/escaping artifact")
    return path


def validate_summary(value: dict, expected_sha: str | None = None, allow_known_failure: bool = False) -> str:
    source = value.get("source_sha")
    require(valid_sha(source) and (expected_sha is None or source == expected_sha), "Summary source SHA mismatch or missing full SHA")
    require(type(value.get("working_tree_dirty_at_start")) is bool, "Missing honest source dirty flag")
    require(allow_known_failure or value["working_tree_dirty_at_start"] is False,
            "Final/clean runner source is dirty; only the separately scoped instrumented baseline may be dirty")
    rows = value.get("results")
    require(isinstance(rows, list) and rows, "Empty/incomplete runner summary")
    identifiers = []
    for row in rows:
        require(isinstance(row, dict), "Invalid runner result")
        name = row.get("case", row.get("name"))
        require(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9_-]+", name), "Invalid result identity")
        identifiers.append(name)
        require(not row.get("engine_errors") and not row.get("engine_error") and not row.get("timed_out", False), "Engine error or incomplete/timed-out runner result")
        known = allow_known_failure and name == "AftershockCityLifeAudit"
        if known:
            require(row.get("valid") is False and row.get("exit_code") == 1 and row.get("passed") == 26
                    and row.get("failed") == 2 and row.get("baseline_known_failure") is True, "Instrumented baseline CityLife result differs from declared 26/2 known failure")
        else:
            require(row.get("valid") is True and row.get("exit_code") == 0 and row.get("failed", 0) == 0, "Failed final runner result")
    require(len(set(identifiers)) == len(identifiers), "Duplicate results in one runner summary")
    return source


def validate_corrected_mutation(report: dict) -> None:
    """Reject the old timer-blind harness even if a stale report says valid."""
    require(report.get("valid") is True and report.get("buildings") == 1000
            and report.get("monotonic_ids") is True and report.get("frame_counter_advanced") is True
            and report.get("deferred_frame_probes") == 16 and report.get("logical_frame_probes") == 119,
            "Mutation report does not use the corrected 16/119 logical-frame harness")
    samples = report.get("samples")
    require(isinstance(samples, list) and len(samples) == 17
            and all(isinstance(sample, dict) for sample in samples)
            and [sample.get("cycle") for sample in samples] == list(range(17)), "Incomplete mutation cycle inventory")
    for sample in samples[:16]:
        require(sample.get("deferred_frame_probe_ran") is True, "A measured damage cycle did not execute its deferred timer probe")
        require(all(number(sample.get(field)) and sample[field] >= 0 for field in
                    ("damage1000_cpu_ms", "timer_flush_cpu_ms", "flood_build_clear_cpu_ms")),
                "Invalid measured mutation timings")
    # Cycle16 observes collection after the measured16 cycles; it must never be
    # treated as a zero-cost damage sample when comparing native CPU means.
    final = samples[16]
    require(final.get("after_explicit_gc") is True and final.get("deferred_frame_probe_ran") is False
            and all(number(final.get(field)) and final[field] == 0 for field in
                    ("damage1000_cpu_ms", "timer_flush_cpu_ms", "flood_build_clear_cpu_ms")),
            "Final mutation row is not the explicitly separate GC-only observation")


def frame_csv(source: Path, target: Path, exporter: Exporter) -> dict:
    with source.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader)
        require(header.count("FrameTime") == 1, "Missing/ambiguous FrameTime")
        frame_index = header.index("FrameTime")
        selected = [(index, name) for index, name in enumerate(header) if name in FRAME_COLUMNS]
        require(len({name for _, name in selected}) == len(selected), "Ambiguous allowlisted metric column")
        require({"FrameTime", "GameThreadTime", "RenderThreadTime", "GPUTime"}.issubset({name for _, name in selected}), "Required frame/GT/RT/GPU metric missing")
        declared = header
        for row in reader:
            if "FrameTime" not in row:
                continue
            require(len(row) >= len(header) and row[:len(header)] == header, "CSV trailing header changes initial mapping")
            require(row[:min(len(row), len(declared))] == declared[:min(len(row), len(declared))], "CSV header is not append-only")
            if len(row) > len(declared):
                declared = row
    values = {name: [] for _, name in selected}
    frames, excluded, expanded = 0, 0, 0
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open(encoding="utf-8-sig", newline="") as stream, target.open("w", encoding="utf-8", newline="") as out:
        reader, writer = csv.reader(stream), csv.writer(out, lineterminator="\n")
        next(reader);writer.writerow([name for _, name in selected])
        for row in reader:
            try:
                real_frame = len(row) > frame_index and math.isfinite(float(row[frame_index])) and float(row[frame_index]) > 0
            except (ValueError, OverflowError):
                real_frame = False
            if not real_frame:
                excluded += 1
                continue
            require(len(header) <= len(row) <= len(declared), "Numeric CSV frame outside declared schema")
            output_row = []
            for index, name in selected:
                cell = row[index].strip()
                require(cell, "Empty selected metric in real frame")
                metric = float(cell)
                require(math.isfinite(metric) and metric >= 0, "Nonfinite/negative selected frame metric")
                values[name].append(metric);output_row.append(cell)
            writer.writerow(output_row);frames += 1;expanded += len(row) > len(header)
    require(frames >= 300 and any(value > 0 for value in values["GPUTime"]), "Incomplete/non-GPU frame capture")
    metrics = {}
    for name, samples in values.items():
        positive = sorted(value for value in samples if value > 0)
        metrics[name] = {"all_frame_samples": len(samples), "positive_samples": len(positive), "zero_samples": len(samples)-len(positive),
                         "positive_mean": sum(positive)/len(positive) if positive else None,
                         "positive_p95": positive[math.ceil(len(positive)*.95)-1] if positive else None,
                         "max": max(samples)}
    exporter.record(source, target, "numeric_frame_allowlist_no_events_no_footer")
    report = {"path": target.relative_to(exporter.output).as_posix(), "numeric_frames": frames,
              "captured_frame_seconds": sum(values["FrameTime"])/1000., "excluded_nonframe_rows": excluded,
              "frames_with_appended_metrics": expanded, "omitted_appended_columns": len(declared)-len(header),
              "metrics": metrics, "scope": "All positive-FrameTime rows retained, including hitches. Callback means use positive calls, not zero-filled frames. Only an explicit numeric column allowlist leaves the initial stable header prefix; no EVENTS or footer."}
    exporter.csv_reports.append(report)
    return report


def memory_csv(source: Path, target: Path, exporter: Exporter, expected_samples: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with source.open(encoding="utf-8-sig", newline="") as stream, target.open("w", encoding="utf-8", newline="") as out:
        reader = csv.DictReader(stream);require(set(MEMORY_COLUMNS).issubset(reader.fieldnames or []), "Memory sample columns missing")
        writer = csv.writer(out, lineterminator="\n");writer.writerow(MEMORY_COLUMNS)
        for row in reader:
            cells = [row[name] for name in MEMORY_COLUMNS]
            require(all(cell and math.isfinite(float(cell)) and float(cell) >= 0 for cell in cells), "Invalid numeric memory sample")
            writer.writerow(cells);count += 1
    require(count == expected_samples and count >= 10, "Memory sample count differs from summary")
    exporter.record(source, target, "numeric_memory_allowlist_pid_omitted")


def native(root: Path, target: Path, exporter: Exporter, expected_sha: str | None = None,
           known_baseline: bool = False, historical_30: bool = False) -> dict:
    summary_path = root / "summary.json";summary = read_json(summary_path)
    source = validate_summary(summary, expected_sha, known_baseline)
    require(not (known_baseline and historical_30), "Diagnostic baseline and historical30 scopes cannot be combined")
    reports = HISTORICAL_NATIVE_REPORTS if historical_30 else NATIVE_REPORTS
    suite_scope = "diagnostic-known-red-baseline-2" if known_baseline else "historical-30-not-current33-acceptance" if historical_30 else "current-33"
    if known_baseline:
        require(source == BASELINE_SHA and summary.get("working_tree_dirty_at_start") is True
                and summary.get("tracked_production_diff_empty") is True, "Instrumented baseline provenance differs")
        require(set(summary.get("dirty_files", [])) == HARNESS_FILES, "Instrumented baseline must contain exactly two untracked diagnostic harness files")
        require({row.get("name") for row in summary["results"]} == {"AftershockMutationSoak", "AftershockCityLifeAudit"}, "Unexpected instrumented baseline cases")
        for extension, expected_hash in CORRECTED_HARNESS_HASHES.items():
            item = summary.get("harness", {}).get(extension, {})
            require(item.get("sha256") == expected_hash and item.get("identical_to_changed_worktree") is True,
                    "Corrected identical diagnostic-harness hash mismatch")
    else:
        require({row.get("name") for row in summary["results"]} == set(reports),
                "Native summary must contain the exact declared33 suites, or explicit historical30 scope")
        for row in summary["results"]:
            if reports[row["name"]] is None:
                require(type(row.get("checks")) is int and row["checks"] > 0,
                        "A log-only native suite has no recorded passing checks")
    exporter.json(target / "summary.json", dict(summary, native_suite_scope=suite_scope), summary_path)
    names = {path.name for path in root.iterdir() if path.is_file() and path.name in NATIVE_JSON}
    if known_baseline:
        require(names == {"mutation_soak.json", "city_life_audit.json"}, "Instrumented baseline reports missing or unexpected")
    else:
        require(names == {name for name in reports.values() if name is not None}, "Native suite report inventory is incomplete or mixes historical/current scope")
    rows_by_report = {reports[row["name"]]: row for row in summary["results"] if row["name"] in reports}
    for name in sorted(names):
        path = reference(root, name);report = read_json(path)
        known_red = known_baseline and name == "city_life_audit.json"
        if known_red:
            require(report.get("passed") == 26 and report.get("failed") == 2, "Known-red report count mismatch")
        else:
            require(report.get("valid", True) is not False and report.get("failed", 0) == 0, "Failed final native report")
            check_items = report.get("checks", [])
            if isinstance(check_items, list):
                require(all(check.get("passed") is not False for check in check_items if isinstance(check, dict)), "Failed final native check")
        if name == "mutation_soak.json":
            validate_corrected_mutation(report)
        if name in NEW_NATIVE_REPORTS.values():
            row = rows_by_report[name]
            checks = report.get("checks")
            require(isinstance(checks, list) and checks and all(isinstance(item, dict) and item.get("passed") is True for item in checks),
                    "A current native suite has incomplete or failed check detail")
            require(type(report.get("passed")) is int and report["passed"] == len(checks)
                    and type(report.get("failed")) is int and report["failed"] == 0
                    and row.get("passed") == report["passed"] and row.get("failed") == 0,
                    "A current native suite count disagrees with its summary/check details")
            if name == "save_codec_audit.json":
                require(isinstance(report.get("schema_fingerprint"), str)
                        and re.fullmatch(r"[A-Fa-f0-9]{64}", report["schema_fingerprint"]),
                        "Save-codec schema fingerprint is missing or malformed")
        exporter.json(target / name, report, path)
    return {"source_sha": source, "working_tree_dirty_at_start": summary["working_tree_dirty_at_start"],
            "results": len(summary["results"]), "native_suite_scope": suite_scope,
            "current_full_33_native_accepted": not known_baseline and not historical_30,
            "known_failure_retained_not_accepted": known_baseline}


def validate_geographic_artifacts(row: dict, report: dict, inventory: set[str]) -> None:
    case = row["case"];region, focus = GEOGRAPHIC_CASES[case]
    if case not in GEOGRAPHIC_FOCUS_CASES and row.get("region_index_requested") is not None:
        # Historical generic aliases may have explicit --region/--focus. Keep
        # that recorded selection, but never reinterpret an exact focus label.
        requested = (row.get("region_index_requested"), row.get("focus_index_requested"))
        require(requested in GEOGRAPHIC_FOCUS_CASES.values(), "Generic geographic alias requests an unknown focus")
        region, focus = requested
    require(report.get("valid") is True and report.get("region_index") == region
            and report.get("focus_index") == focus, "Geographic report identity/acceptance differs")
    if case in GEOGRAPHIC_FOCUS_CASES:
        require(row.get("region_index_requested") == region and row.get("focus_index_requested") == focus
                and row.get("source_version_requested") in (2, 3), "Geographic case does not match the actual compiled focus")
    snapshots = report.get("snapshots")
    stages = {"street", "1250m", "5000m", "globe"}
    require(isinstance(snapshots, list) and len(snapshots) == 4
            and all(isinstance(s, dict) for s in snapshots) and {s.get("stage") for s in snapshots} == stages,
            "Geographic four-view snapshots are incomplete")
    require({f"geo-view-{stage}-scene.png" for stage in stages}.issubset(inventory), "Geographic four-view PNGs missing")
    require(report.get("same_canonical_roster_across_views") is True
            and report.get("unchanged_ground_builds_and_cooks_across_views") is True,
            "Geographic source/canonical invariance was not accepted")
    for snapshot in snapshots:
        require(snapshot.get("scene_saved") is True and snapshot.get("real_geography") is True
                and snapshot.get("region_index") == region and snapshot.get("focus_index") == focus,
                "Incomplete or mismatched geographic frame")
        if case in GEOGRAPHIC_FOCUS_CASES:
            require(snapshot.get("continuation_version") == row["source_version_requested"], "Geographic source version differs")
    temporal = report.get("temporal_snapshots", [])
    temporal_artifacts = {f"geo-view-street-static-{index}-scene.png" for index in range(2)}
    if report.get("temporal_pair_requested") is True or temporal or inventory & temporal_artifacts:
        require(report.get("temporal_pair_requested") is True and report.get("temporal_pair_complete") is True
                and temporal_artifacts.issubset(inventory), "Temporal pair is pending or missing its PNGs")
        require(isinstance(temporal, list) and len(temporal) == 2 and all(isinstance(s, dict) for s in temporal)
                and [s.get("stage") for s in temporal] == ["street-static-0", "street-static-1"], "Temporal pair snapshots incomplete")
        require(all(s.get("scene_saved") is True and s.get("actual_camera_matches_pinned_capture_view") is True
                    and s.get("moving_world_was_not_frozen") is True and s.get("flicker_free_claim") is False for s in temporal),
                "Temporal frames are incomplete or make unsupported frozen/flicker claims")
        require(all(number(s.get(field)) for s in temporal for field in ("frame_counter", "world_time_seconds"))
                and temporal[1]["frame_counter"] > temporal[0]["frame_counter"]
                and temporal[1]["world_time_seconds"] > temporal[0]["world_time_seconds"], "Temporal frames do not advance")
    regional = report.get("regional_snapshots", [])
    regional_artifact = "geo-view-regional-scene.png"
    if row.get("geographic_regional_requested") is True or report.get("regional_overview_requested") is True or regional or regional_artifact in inventory:
        require(report.get("regional_overview_requested") is True and report.get("regional_overview_complete") is True
                and regional_artifact in inventory, "Regional overview is pending or missing its PNG")
        require(isinstance(regional, list) and len(regional) == 1 and isinstance(regional[0], dict), "Regional snapshot incomplete")
        snapshot = regional[0]
        require(snapshot.get("stage") == "regional" and snapshot.get("scene_saved") is True and snapshot.get("metadata_valid") is True
                and snapshot.get("region_index") == region and snapshot.get("focus_index") == focus
                and snapshot.get("continuation_version") == 3 and snapshot.get("source_context_mesh_present") is True
                and snapshot.get("actual_mesh_covers_source_roi_xy") is True and snapshot.get("whole_source_roi_in_frame") is True,
                "Regional source-ROI frame not accepted")


def validate_physical_flood_artifacts(row: dict, report: dict, inventory: set[str]) -> None:
    stages = ("arrival-start", "arrival-intermediate", "fully-arrived-before-drain")
    require(row.get("region_index_requested") == 1 and row.get("focus_index_requested") == 0
            and row.get("source_version_requested") == 3 and row.get("legacy_geography_requested") is False,
            "Physical flood fixture source differs")
    require(report.get("valid") is True and report.get("region_index") == 1 and report.get("focus_index") == 0
            and report.get("source_version") == 3 and report.get("flood_model_version") == 1
            and report.get("terrain_or_water_edits_by_fixture") is False, "Physical flood report not accepted")
    frames = report.get("frames")
    require(isinstance(frames, list) and len(frames) == 3 and all(isinstance(f, dict) for f in frames)
            and tuple(f.get("stage") for f in frames) == stages, "Physical flood three-frame timeline incomplete")
    require({f"physical-flood-{stage}-scene.png" for stage in stages}.issubset(inventory), "Physical flood PNG missing")
    require(all(number(f.get("event_age_seconds")) and type(f.get("frame_counter")) is int for f in frames)
            and len({f["frame_counter"] for f in frames}) == 3
            and all(a["event_age_seconds"] < b["event_age_seconds"] for a, b in zip(frames, frames[1:])),
            "Physical flood frames do not advance")
    for frame in frames:
        require(all(frame.get(key) is True for key in (
            "valid", "scene_saved", "metadata_valid", "source_geometry_unchanged", "canonical_water_snapshot_unchanged",
            "water_maps_paired_finite_bed_head_consistent", "cpu_depth_matches_actual_material_timeline",
            "actual_material_parameters_read", "actual_flood_component_visible", "actual_flood_single_nonzero_instance",
            "actual_flood_full_precision_uvs", "real_wet_land_focus_in_frame")), "Physical flood frame contains failed/pending checks")
        require(frame.get("geometry_hash") == frames[0].get("geometry_hash") and frame.get("geometry_hash") is not None
                and frame.get("water_snapshot_hash") == frames[0].get("water_snapshot_hash") and frame.get("water_snapshot_hash") is not None,
                "Physical flood geometry or water snapshot changed")
        budget = frame.get("canonical_event_mass_budget", {})
        require(budget.get("committed_storage") is True and budget.get("mass_conserved") is True, "Physical flood mass budget not accepted")


def validate_real_volcano_artifacts(row: dict, report: dict, inventory: set[str]) -> None:
    """Same-capture mapped vent, visible plume/lava and viewport receipt.

    This is the existing offscreen fixture contract, not desktop input, calibrated
    eruption physics, animation/flicker quality or general volcano acceptance.
    """
    proof_fields = ("valid", "scene_saved", "actual_mapped_vent", "mapped_vent_in_frame",
                    "plume_300m_top_in_frame", "actual_vent_lava_plume_meshes")
    pair = (row.get("region_index_requested"), row.get("focus_index_requested"))
    require(row.get("case") == "volcano" and row.get("legacy_geography_requested") is False
            and all(type(value) is int for value in pair) and pair in GEOGRAPHIC_FOCUS_CASES.values(),
            "Volcano receipt requires an explicit real compiled-region fixture")
    require({"volcano_runtime_audit.json", "volcano-peak-scene.png"}.issubset(inventory),
            "Mapped-volcano receipt or same-capture scene PNG missing")
    require(report.get("schema_version") == 1 and report.get("stage") == "volcano-peak"
            and report.get("region_index") == pair[0] and report.get("focus_index") == pair[1]
            and report.get("real_geography") is True and report.get("continuation_version") in (2, 3),
            "Volcano source frame/region identity differs")
    proof = row.get("volcano_frame_proof")
    require(isinstance(proof, dict) and proof.get("receipt") == "volcano_runtime_audit.json"
            and all(report.get(field) is True and proof.get(field) is True for field in proof_fields),
            "Volcano receipt or summary contains failed/pending frame checks")
    for field, expected in (("viewport_width", 1600), ("viewport_height", 900)):
        require(number(report.get(field)) and report[field] == expected
                and number(proof.get(field)) and proof[field] == expected, "Volcano capture viewport differs")
    for field, maximum in (("vent_pixel_x", 1600), ("vent_pixel_y", 900)):
        require(number(report.get(field)) and 0 <= report[field] <= maximum
                and number(proof.get(field)) and proof[field] == report[field],
                "Volcano vent projection is invalid or differs from same-frame receipt")


def rhi(root: Path, target: Path, exporter: Exporter, expected_sha: str) -> list[dict]:
    summaries = sorted(root.rglob("summary.json"))
    require(summaries, "No completed RHI runner summaries")
    runs = []
    for run_index, summary_path in enumerate(summaries, 1):
        require(not summary_path.parent.is_symlink(), "Linked RHI summary directory")
        summary = read_json(summary_path);source = validate_summary(summary, expected_sha)
        run_target = target / f"run-{run_index:03d}"
        exporter.json(run_target / "summary.json", summary, summary_path)
        for row in summary["results"]:
            case = row.get("case");require(case in CASES, "Nonallowlisted RHI case")
            folder = summary_path.parent / case
            require(folder.is_dir() and not folder.is_symlink(), "Missing/linked RHI case directory")
            inventory = row.get("artifacts");require(isinstance(inventory, list) and inventory and len(set(inventory)) == len(inventory), "Incomplete artifact inventory")
            inventory_set = set(inventory)
            if case in GEOGRAPHIC_CASES:
                require("geographic_view_audit.json" in inventory_set, "Geographic report not referenced by runner")
                validate_geographic_artifacts(row, read_json(reference(folder, "geographic_view_audit.json")), inventory_set)
            if case == "physical-flood":
                require("physical_flood_runtime_audit.json" in inventory_set, "Physical flood report not referenced by runner")
                validate_physical_flood_artifacts(row, read_json(reference(folder, "physical_flood_runtime_audit.json")), inventory_set)
            real_volcano = (case == "volcano" and row.get("region_index_requested") is not None
                            and row.get("legacy_geography_requested") is not True)
            if real_volcano or "volcano_runtime_audit.json" in inventory_set or row.get("volcano_frame_proof"):
                require(real_volcano and "volcano_runtime_audit.json" in inventory_set,
                        "Unexpected or missing explicit-real-volcano receipt")
                validate_real_volcano_artifacts(row, read_json(reference(folder, "volcano_runtime_audit.json")), inventory_set)
                with reference(folder, "volcano-peak-scene.png").open("rb") as stream:
                    header = stream.read(24)
                require(len(header) == 24 and header[:8] == b"\x89PNG\r\n\x1a\n" and header[12:16] == b"IHDR"
                        and struct.unpack_from(">II", header, 16) == (1600, 900), "Volcano scene PNG viewport differs from receipt")
            curated_capture = None
            for name in inventory:
                path = reference(folder, name);destination = run_target / case / name
                require(not name.startswith("physical-flood-") and name != "physical_flood_runtime_audit.json" or case == "physical-flood",
                        "Physical flood artifact belongs to a different case")
                require(not name.startswith("geo-view-") and name != "geographic_view_audit.json" or case in GEOGRAPHIC_CASES,
                        "Geographic artifact belongs to a different case")
                require(name != "volcano_runtime_audit.json" or real_volcano,
                        "Mapped-volcano receipt belongs to a different fixture")
                if name in PNG_NAMES:
                    exporter.png(path, destination)
                elif name in RHI_JSON:
                    exporter.json(destination, read_json(path), path)
                elif name in {"crowd-render-500.csv", "crowd-render-1500.csv"}:
                    curated_capture = frame_csv(path, destination, exporter)
                else:
                    raise ValueError("Nonallowlisted runner artifact")
            if case.startswith("profile-"):
                require(curated_capture is not None, "Profile has no numeric capture")
                sidecar = read_json(reference(folder, f"crowd-render-{case[8:]}.profile.json"))
                require(sidecar.get("csv_ready") is True and sidecar.get("frame_count_valid") is True and sidecar.get("timed_out") is False
                        and sidecar.get("actual_frames") == curated_capture["numeric_frames"], "Profile sidecar disagrees with actual CSV rows")
                require(abs(sidecar.get("captured_frame_seconds", -1)-curated_capture["captured_frame_seconds"]) <= .002, "Profile CSV duration mismatch")
        runs.append({"source_sha": source, "working_tree_dirty_at_start": summary["working_tree_dirty_at_start"], "results": len(summary["results"])})
    for memory_index, memory_path in enumerate(sorted(root.rglob("memory-summary.json")), 1):
        summary = read_json(memory_path);validate_summary(summary, expected_sha)
        require(summary.get("valid") is True, "Memory runner not complete/valid")
        if expected_sha == BASELINE_SHA:
            require(summary["working_tree_dirty_at_start"] is False, "Clean baseline memory summary reports dirty source")
        destination = target / f"memory-{memory_index:03d}"
        exporter.json(destination / "memory-summary.json", summary, memory_path)
        for row in summary["results"]:
            case = row.get("case");require(case in CASES and row.get("runner_source_matches") is True, "Invalid isolated memory case")
            memory_csv(reference(memory_path.parent, case+"-memory.csv"), destination/(case+"-memory.csv"), exporter, row.get("samples", -1))
    return runs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("baseline", "fixed-native", "output"):
        parser.add_argument("--"+flag, type=Path, required=True)
    parser.add_argument("--fixed-rhi", type=Path, action="append", required=True,
                        help="Completed RHI runner root; repeat for extra same-source regressions")
    parser.add_argument("--fixed-profile", type=Path, action="append", required=True,
                        help="Completed memory/profile root; repeat for legacy-near and overview runs")
    parser.add_argument("--baseline-native", type=Path, help="Separate instrumented baseline with exact two-harness dirty provenance and known CityLife26/2 failure")
    parser.add_argument("--historical-native-30", action="store_true",
                        help="Explicitly export an old30-suite run as historical; never current33-suite acceptance")
    args = parser.parse_args()
    inputs = [args.baseline, args.fixed_native, *args.fixed_rhi, *args.fixed_profile]+([args.baseline_native] if args.baseline_native else [])
    roots = [path.resolve() for path in inputs]
    require(all(path.is_dir() and not original.is_symlink() for path, original in zip(roots, inputs)), "Missing/linked input root")
    output = args.output.resolve()
    require(not output.exists() and "2026-10-18" in output.parts and "2026-10-17" not in output.parts, "Choose a new 18 October output, never the immutable17 October evidence")
    require(all(not output.is_relative_to(root) and not root.is_relative_to(output) for root in roots), "Input/output trees overlap")
    fixed_summary = read_json(args.fixed_native.resolve()/"summary.json")
    fixed_sha = validate_summary(fixed_summary)
    require(fixed_sha != BASELINE_SHA, "Final source must differ from baseline")
    output.mkdir(parents=True, exist_ok=False)
    sentinel = output/"INCOMPLETE.txt";sentinel.write_text("Evidence curation incomplete; no final acceptance manifest yet.\n", encoding="utf-8", newline="\n")
    exporter = Exporter(output)
    baseline_runs = rhi(args.baseline.resolve(), output/"baseline-clean-rhi", exporter, BASELINE_SHA)
    require(all(run["working_tree_dirty_at_start"] is False for run in baseline_runs), "Clean baseline runner reports dirty source")
    baseline_native = native(args.baseline_native.resolve(), output/"baseline-instrumented-native", exporter, BASELINE_SHA, True) if args.baseline_native else None
    fixed_native = native(args.fixed_native.resolve(), output/"fixed-native", exporter, fixed_sha,
                          historical_30=args.historical_native_30)
    fixed_rhi, fixed_profile = [], []
    for group, folder in enumerate(args.fixed_rhi, 1):
        fixed_rhi.extend(dict(run, input_group=group) for run in rhi(folder.resolve(), output/"fixed-rhi"/f"input-{group:03d}", exporter, fixed_sha))
    for group, folder in enumerate(args.fixed_profile, 1):
        fixed_profile.extend(dict(run, input_group=group) for run in rhi(folder.resolve(), output/"fixed-profile"/f"input-{group:03d}", exporter, fixed_sha))
    exporter.json(output/"numeric-capture-metrics.json", {"scope": "Derived only from exported explicit numeric columns; all frame hitches retained. No process leak, production readiness or isolated crowd GPU claim.", "captures": exporter.csv_reports})
    exporter.json(output/"curation.json", {
        "schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(), "source_sha": fixed_sha,
        "baseline_source_sha": BASELINE_SHA, "baseline_clean_runs": baseline_runs, "baseline_instrumented_native": baseline_native,
        "fixed_native": fixed_native, "fixed_rhi_runs": fixed_rhi, "fixed_profile_runs": fixed_profile,
        "input_payload_provenance": exporter.provenance, "redacted_string_values": exporter.redactions,
        "omitted_nonallowlisted_string_fields": exporter.omitted_text_fields,
        "scope": "Offline curation of supplied completed results. Source SHA, dirty flags and report scopes are preserved. Instrumented known-red baseline is separate and is not accepted as passing. Curation performs no Git, engine, build, mutation or new runtime verification.",
        "freshness_scope": "Runner freshness decisions are trusted as recorded; the curator verifies content/provenance, not wall-clock freshness.",
        "memory_scope": "External working-set/private-commit samples include startup/exit, renderer and fixture overhead. Final exit samples are not steady-state leak evidence. Estimated mesh bytes are not measured GPU VRAM.",
        "privacy": "Only allowlisted structured JSON/unchanged game PNG/numeric CSV columns. No raw logs, console, save, PDF, PID, EVENTS or CSV machine/account footer. No desktop-input, packaged-game, Mac/Metal or production-ready acceptance.",
        "fixed_runner_results_valid": True, "baseline_citylife_accepted_as_passing": False,
    })
    files = [{"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
             for path in sorted(output.rglob("*")) if path.is_file() and path != sentinel]
    exporter.json(output/"manifest.json", {"schema_version": 1, "source_sha": fixed_sha, "baseline_source_sha": BASELINE_SHA,
        "created_utc": datetime.now(timezone.utc).isoformat(), "files": files,
        "checksum_scope": "All curated payload files listed. SHA256SUMS additionally hashes manifest.json; it cannot hash itself."})
    sums = [(item["path"], item["sha256"]) for item in files]+[("manifest.json", digest(output/"manifest.json"))]
    (output/"SHA256SUMS.txt").write_text("".join(f"{sha}  {name}\n" for name, sha in sorted(sums)), encoding="utf-8", newline="\n")
    sentinel.unlink()  # Only this invocation's own sentinel; no recursive delete.
    print(f"Curated {len(files)} portable payload files; final source {fixed_sha}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, TypeError, KeyError, csv.Error, json.JSONDecodeError, StopIteration):
        # Error messages deliberately do not echo an input path, account or raw report.
        print("Evidence curation refused: missing, invalid, incomplete or nonallowlisted proof; no final acceptance produced.", file=sys.stderr)
        raise SystemExit(2)
