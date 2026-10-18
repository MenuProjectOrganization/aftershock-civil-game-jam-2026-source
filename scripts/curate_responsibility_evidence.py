"""Curate completed proofs for one exact commit; no Unreal, Git, builds or tests.

--source-sha requires 40 hexadecimal characters. Native, RHI and optional build
summaries must all match that exact commit.

The output directory must not exist. Inputs are read-only. Raw logs, save files,
PDFs, consoles and CSV metadata footers are never copied. A failed invocation
may leave an explicitly incomplete directory; it never removes/replaces one.

Optional build-summary schema:
  {"source_sha":"42c5277...", "targets":[
    {"target":"JamProjectEditor", "configuration":"Development",
     "platform":"Win64", "result":"Succeeded", "seconds":21.0},
    {"target":"JamProject", "configuration":"Development",
     "platform":"Win64", "result":"Succeeded", "seconds":24.0}]}
Only the allowlisted structured build fields are exported, not command/output.
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
import sys


NATIVE_REPORTS = {
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
    **{f"scenario-{i}": f"scenario_audit_{i}.json" for i in range(7)},
}
RHI_CASES = {
    "chaos", "chaos-new-city", "rubble-save", "bridge", "chunks", "chunk-chaos",
    "narrative", "flood", "wind", "tsunami", "fire", "volcano", "drought",
    "globe", "profile-500", "profile-1500",
}
MARKER_WORDS = (
    "RESULT", "CHECK", "PASS", "FAIL", "AUDIT", "PROFILE", "VISIBILITY",
    "PHYSICAL_CLEARANCE", "PHYSICAL_TRAFFIC", "CITIZEN_PLAN", "CITIZEN_CENTRE",
    "VISUAL_RECONCILED", "PRESENTATION_COVERAGE", "SAVE_WRITTEN", "SAVE_RESTORED",
    "NARRATIVE", "TRANSPORT_STATS", "FOUNDATION",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def read_json(path: Path):
    with path.open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def finite_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


class Sanitizer:
    def __init__(self, repo: Path):
        self.roots = [(repo, "<REPO>"), (Path.home(), "<HOME>")]
        if os.environ.get("USERPROFILE"):
            self.roots.append((Path(os.environ["USERPROFILE"]), "<HOME>"))
        self.roots.sort(key=lambda item: len(str(item[0])), reverse=True)

    def text(self, value: str) -> str:
        for root, replacement in self.roots:
            for variant in {str(root), root.as_posix(), str(root).replace("/", "\\")}:
                value = re.sub(re.escape(variant), lambda _: replacement, value, flags=re.I)
        value = re.sub(r"(?i)[a-z]:[\\/]Program Files[\\/]Epic Games[\\/]UE_[^\\/\s\"']+", "<UE>", value)
        # Unknown absolute paths are also removed. Stop at the next marker's
        # key=value field, rather than leaking a local path or swallowing metrics.
        value = re.sub(r"(?i)\b[a-z]:[\\/].*?(?=\s+[A-Za-z_][\w./-]*=|[\r\n\"'<>]|$)", "<LOCAL_PATH>", value)
        value = re.sub(r"\\\\[^\r\n\"'<>]+?(?=\s+[A-Za-z_][\w./-]*=|[\r\n\"'<>]|$)", "<LOCAL_PATH>", value)
        value = re.sub(r"/(?:Users|home)/[^/\s\"']+", "<HOME>", value)
        return value

    def tree(self, value):
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.tree(item) for item in value]
        if isinstance(value, dict):
            return {self.text(str(key)): self.tree(item) for key, item in value.items()}
        if isinstance(value, float):
            require(math.isfinite(value), "Nonfinite JSON metric cannot be published")
        return value


def write_json(path: Path, data, sanitizer: Sanitizer) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sanitizer.tree(data), ensure_ascii=False, allow_nan=False, indent=2) + "\n", encoding="utf-8", newline="\n")


def validate_summary(folder: Path, expected: set[str], key: str, expected_sha: str) -> dict:
    require(folder.is_dir(), f"Missing input directory: {folder.name}")
    summary = read_json(folder / "summary.json")
    require(summary.get("source_sha") == expected_sha, f"{folder.name}: source SHA must be {expected_sha}")
    require(type(summary.get("working_tree_dirty_at_start")) is bool, f"{folder.name}: missing honest dirty flag")
    rows = summary.get("results", [])
    require(isinstance(rows, list) and len(rows) == len(expected), f"{folder.name}: expected exactly {len(expected)} completed cases")
    names = [row.get(key) for row in rows]
    require(len(set(names)) == len(names) and set(names) == expected, f"{folder.name}: missing, duplicate or unexpected cases")
    for row in rows:
        require(row.get("valid") is True and row.get("exit_code") == 0 and not row.get("timed_out", False), f"Failed/incomplete case: {row.get(key)}")
        require(not row.get("engine_errors") and not row.get("engine_error"), f"Engine error in {row[key]}")
        if "failed" in row:
            require(row["failed"] == 0, f"Failed assertions in {row[key]}")
    return summary


def referenced_file(folder: Path, name: str) -> Path:
    require(isinstance(name, str) and name and Path(name).name == name and "/" not in name and "\\" not in name, "Artifact names must be plain relative filenames")
    path = folder / name
    require(path.is_file() and not path.is_symlink(), f"Missing or linked artifact: {name}")
    require(path.resolve().parent == folder.resolve(), "Artifact escapes its case directory")
    return path


def curate_markers(source: Path, target: Path, sanitizer: Sanitizer) -> int:
    require(source.is_file() and not source.is_symlink(), f"Missing evidence log: {source.name}")
    selected = []
    for line in source.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        position = line.find("AFTERSHOCK_")
        if position < 0:
            continue
        marker = line[position:]
        identifier = marker.split(None, 1)[0]
        if any(word in identifier for word in MARKER_WORDS):
            selected.append(sanitizer.text(marker))
    require(bool(selected), f"No relevant AFTERSHOCK markers: {source.name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(selected) + "\n", encoding="utf-8", newline="\n")
    return len(selected)


def csv_number(value: str) -> bool:
    try:
        return math.isfinite(float(value))
    except (ValueError, OverflowError):
        return False


def curate_csv(source: Path, target: Path, sanitizer: Sanitizer) -> dict:
    # CSV parsing handles quoted commas. Repeated headers, events and the
    # account/machine metadata footer are excluded by numeric FrameTime.
    # Unreal may discover new stat columns during capture and append them to
    # later frames, declaring the final schema in its repeated trailing header.
    # Preserve all real frames but publish only the original stable prefix.
    with source.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        header = next(reader)
        require("FrameTime" in header, f"No FrameTime header in {source.name}")
        require(len(set(header)) == len(header), f"Duplicate CSV metric headings: {source.name}")
        frame_column = header.index("FrameTime")
        declared_header = header
        repeated_headers = 0
        for row in reader:
            numeric_frame = len(row) > frame_column and csv_number(row[frame_column]) and float(row[frame_column]) > 0
            if "FrameTime" not in row or numeric_frame:
                continue
            require(len(row) >= len(header) and row[:len(header)] == header,
                    f"{source.name}: repeated CSV header changes the initial metric mapping")
            common_width = min(len(row), len(declared_header))
            require(row[:common_width] == declared_header[:common_width],
                    f"{source.name}: repeated CSV headers are not append-only")
            repeated_headers += 1
            if len(row) > len(declared_header):
                declared_header = row
    declared_max_width = len(declared_header)
    with source.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        next(reader)
        numeric_columns = [True] * len(header)
        observed = [False] * len(header)
        count, excluded, expanded_rows = 0, 0, 0
        for row in reader:
            if len(row) <= frame_column or not csv_number(row[frame_column]) or float(row[frame_column]) <= 0:
                excluded += 1
                continue
            require(len(header) <= len(row) <= declared_max_width,
                    f"{source.name}: numeric frame width is outside the declared append-only schema")
            count += 1
            expanded_rows += len(row) > len(header)
            for index, value in enumerate(row[:len(header)]):
                if value.strip():
                    numeric_columns[index] &= csv_number(value)
                    observed[index] = True
    columns = [i for i in range(len(header)) if numeric_columns[i] and observed[i]]
    require(frame_column in columns and count >= 300, f"{source.name}: fewer than300 real numeric frames")
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open(encoding="utf-8-sig", newline="") as stream, target.open("w", encoding="utf-8", newline="\n") as output:
        reader, writer = csv.reader(stream), csv.writer(output, lineterminator="\n")
        next(reader)
        writer.writerow([sanitizer.text(header[i]) for i in columns])
        for row in reader:
            if len(row) > frame_column and csv_number(row[frame_column]) and float(row[frame_column]) > 0:
                require(len(header) <= len(row) <= declared_max_width,
                        f"{source.name}: numeric frame width changed during export")
                writer.writerow([row[i] for i in columns])
    return {"numeric_frames": count, "numeric_metric_columns": len(columns), "excluded_nonframe_rows": excluded,
            "removed_nonnumeric_columns": [sanitizer.text(header[i]) for i in range(len(header)) if i not in columns],
            "initial_metric_columns": len(header), "declared_max_metric_columns": declared_max_width,
            "repeated_header_rows": repeated_headers, "frames_with_appended_metrics": expanded_rows,
            "appended_metric_columns_omitted": declared_max_width - len(header),
            "omitted_appended_metric_headings": [sanitizer.text(name) for name in declared_header[len(header):]],
            "dynamic_header_policy": "All real numeric frames retain only the initial unique-header prefix; declared appended metrics are intentionally omitted, including appended duplicate names.",
            "metadata_footer_included": False}


def validate_build_summary(path: Path, expected_sha: str) -> dict:
    value = read_json(path)
    require(value.get("source_sha") == expected_sha, "Build summary SHA differs from tested SHA")
    allowed = {"target", "configuration", "platform", "result", "seconds", "exit_code", "success"}
    targets = value.get("targets")
    require(isinstance(targets, list) and len(targets) == 2, "Build summary requires exactly Editor and Game targets")
    sanitized = []
    names = set()
    for target in targets:
        require(isinstance(target, dict) and isinstance(target.get("target"), str), "Invalid structured build target")
        name = target["target"]
        require(name in {"JamProjectEditor", "JamProject"} and name not in names, "Build targets must be unique Editor and Game")
        names.add(name)
        require(target.get("configuration") == "Development" and target.get("platform") == "Win64"
                and target.get("result") == "Succeeded", "Build target must report succeeded Win64 Development")
        require("exit_code" not in target or type(target["exit_code"]) is int and target["exit_code"] == 0, "Build target reports failed exit code")
        require("success" not in target or target["success"] is True, "Build target reports unsuccessful result")
        require(finite_number(target.get("seconds")) and target["seconds"] >= 0, "Build duration must be finite and nonnegative")
        sanitized.append({key: item for key, item in target.items() if key in allowed})
    result = {"source_sha": expected_sha, "targets": sanitized, "scope": "Supplied structured build results; raw build logs/commands excluded"}
    if type(value.get("working_tree_dirty_at_start")) is bool:
        result["working_tree_dirty_at_start"] = value["working_tree_dirty_at_start"]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-folder", required=True, type=Path)
    parser.add_argument("--rhi-folder", required=True, type=Path)
    parser.add_argument("--output-folder", required=True, type=Path)
    parser.add_argument("--profile-summary", type=Path)
    parser.add_argument("--build-summary", type=Path)
    parser.add_argument("--source-sha", required=True, help="Exact 40-hex commit shared by all completed proofs")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.source_sha):
        parser.error("--source-sha must contain exactly40 hexadecimal characters, not a short prefix")
    expected_sha = args.source_sha.lower()
    repo = Path(__file__).resolve().parents[1]
    native, rhi, output = args.native_folder.resolve(), args.rhi_folder.resolve(), args.output_folder.resolve()
    require(not output.exists(), "Output directory already exists; choose a new directory")
    require(output != native and output != rhi and not native.is_relative_to(output) and not rhi.is_relative_to(output), "Output cannot contain/replace either input")
    native_summary = validate_summary(native, set(NATIVE_REPORTS), "name", expected_sha)
    rhi_summary = validate_summary(rhi, RHI_CASES, "case", expected_sha)
    # Preflight every reference before creating the new output directory.
    for name, report in NATIVE_REPORTS.items():
        referenced_file(native, name + ".log")
        if report:
            referenced_file(native, report)
    for row in rhi_summary["results"]:
        folder = rhi / row["case"]
        require(folder.is_dir() and not folder.is_symlink(), "Missing/linked RHI case directory")
        referenced_file(folder, "runtime.log")
        artifacts = row.get("artifacts")
        require(isinstance(artifacts, list) and artifacts and len(set(artifacts)) == len(artifacts), "Invalid artifact inventory")
        for name in artifacts:
            source = referenced_file(folder, name)
            require(source.suffix.lower() in {".png", ".json", ".csv"}, "Only referenced PNG/JSON/numeric CSV artifacts may be exported")
    profile = read_json(args.profile_summary) if args.profile_summary else None
    build = validate_build_summary(args.build_summary, expected_sha) if args.build_summary else None
    sanitizer = Sanitizer(repo)
    output.mkdir(parents=True, exist_ok=False)
    incomplete = output / "INCOMPLETE.txt"
    incomplete.write_text("Curation has not completed. No final manifest exists yet.\n", encoding="utf-8", newline="\n")
    write_json(output / "native/summary.json", native_summary, sanitizer)
    for name, report in NATIVE_REPORTS.items():
        curate_markers(native / (name + ".log"), output / "native/events" / (name + ".txt"), sanitizer)
        if report:
            write_json(output / "native" / report, read_json(native / report), sanitizer)
    csv_reports = {}
    write_json(output / "rhi/summary.json", rhi_summary, sanitizer)
    for row in rhi_summary["results"]:
        case = row["case"]
        folder, destination = rhi / case, output / "rhi" / case
        destination.mkdir(parents=True, exist_ok=False)
        curate_markers(folder / "runtime.log", destination / "events.txt", sanitizer)
        for name in row["artifacts"]:
            source, target = folder / name, destination / name
            if source.suffix.lower() == ".png":
                shutil.copyfile(source, target)
            elif source.suffix.lower() == ".json":
                write_json(target, read_json(source), sanitizer)
            else:
                csv_reports[case] = curate_csv(source, target, sanitizer)
        if case.startswith("profile-"):
            count = int(case.split("-")[1])
            require(case in csv_reports, f"No numeric capture for {case}")
            metadata = read_json(folder / f"crowd-render-{count}.profile.json")
            require(metadata.get("csv_ready") is True and metadata.get("frame_count_valid") is True
                    and metadata.get("timed_out") is False and metadata.get("actual_frames") == csv_reports[case]["numeric_frames"], "Capture completion metadata disagrees with numeric CSV")
    if profile is not None:
        rows = profile.get("results", [])
        require({row.get("agents") for row in rows} == {500, 1500} and len(rows) == 2, "Profile summary must contain500 and1500 cases")
        for row in rows:
            case = f'profile-{row["agents"]}'
            require(row.get("actualFrames") == csv_reports[case]["numeric_frames"], "Profile summary frame count differs from curated CSV")
            with (output / "rhi" / case / f'crowd-render-{row["agents"]}.csv').open(encoding="utf-8", newline="") as stream:
                actual = list(csv.DictReader(stream))
            metrics = row.get("metrics", {})
            require("FrameTime" in metrics and "GPUTime" in metrics, "Supplied profile summary lacks actual frame/GPU metrics")
            for metric, stats in metrics.items():
                values = sorted(float(sample[metric]) for sample in actual if sample.get(metric) and float(sample[metric]) > 0)
                require(bool(values) and stats.get("samples") == len(values), "Profile summary metric sample count differs from capture")
                observed = {"mean": sum(values) / len(values), "p95": values[math.ceil(len(values) * .95) - 1], "max": values[-1]}
                require(all(finite_number(stats.get(name)) and abs(stats[name] - value) <= .00015 for name, value in observed.items()), "Supplied profile metrics do not match this exact capture")
        write_json(output / "profiles/render_profile_summary.json", profile, sanitizer)
    if build is not None:
        write_json(output / "build/results.json", build, sanitizer)
    write_json(output / "curation.json", {
        "source_sha": expected_sha, "native_valid": 25, "rhi_valid": 16,
        "native_working_tree_dirty_at_start": native_summary["working_tree_dirty_at_start"],
        "rhi_working_tree_dirty_at_start": rhi_summary["working_tree_dirty_at_start"],
        "dirty_flag_scope": "Copied verbatim. Runners do not record changed paths; dirty is never relabelled clean or inferred to mean functional changes.",
        "scope": "Native commandlet and offscreen D3D12 development fixtures. No desktop-input, packaged-build, calibrated disaster or real crowd-safety claim.",
        "privacy": "No raw log/console/save/PDF or CSV metadata footer. Local roots sanitized; PNGs are runner-referenced game captures.",
        "csv_curation": csv_reports,
    }, sanitizer)
    files = []
    for path in sorted(output.rglob("*")):
        if path.is_file() and path != incomplete:
            files.append({"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size,
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    write_json(output / "manifest.json", {
        "schema_version": 1, "source_sha": expected_sha, "created_utc": datetime.now(timezone.utc).isoformat(),
        "native_valid": 25, "rhi_valid": 16, "files": files,
        "checksum_scope": "All curated payload files are listed. SHA256SUMS.txt additionally hashes this manifest; a checksum file cannot hash itself.",
    }, sanitizer)
    payload = [(item["path"], item["sha256"]) for item in files]
    payload.append(("manifest.json", hashlib.sha256((output / "manifest.json").read_bytes()).hexdigest()))
    (output / "SHA256SUMS.txt").write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(payload)), encoding="utf-8", newline="\n")
    incomplete.unlink()  # Only our own single sentinel file; no recursive delete.
    print(f"Curated25 native +16 RHI cases; {len(files)} payload files. Output: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, json.JSONDecodeError, csv.Error, StopIteration) as error:
        print(f"Curation refused: {Sanitizer(Path(__file__).resolve().parents[1]).text(str(error))}", file=sys.stderr)
        raise SystemExit(2)
