"""Run the actual UE commandlets; preserve a separate, dated result per run.

Usage: python scripts/validate_first_playable.py --editor /path/to/UnrealEditor-Cmd.exe
Build the project's Editor target first. This does not test rendering or OS input.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from validation_settings_isolation import RunSettingsIsolation


BASE_SUITES = [
    ("AftershockCityLifeAudit", "city_life_audit.json"),
    ("AftershockTransportAudit", None),
    ("AftershockGeographicHazardsAudit", "geographic_hazards_audit.json"),
    ("AftershockPlayableAudit", "playable_audit.json"),
    ("AftershockFloodVisualAudit", "flood_visual_audit.json"),
    ("AftershockCitizenPlanning", None),
    ("AftershockCoastalInfrastructureAudit", "coastal_infrastructure_audit.json"),
    ("AftershockHUDInputAudit", "hud_input_audit.json"),
]
FOUNDATION_SUITES = [
    ("AftershockRegionCatalogAudit", "region_catalog_audit.json"),
    ("AftershockRegionSettlementAudit", "region_settlement_audit.json"),
    ("AftershockBridgeAudit", "bridge_audit.json"),
    ("AftershockCrowdSpacingAudit", "crowd_spacing_audit.json"),
    ("AftershockSessionSaveAudit", "session_save_audit.json"),
    ("AftershockRubblePersistenceAudit", "rubble_persistence_audit.json"),
    ("AftershockCasualtyLedgerAudit", "casualty_ledger_audit.json"),
    ("AftershockCityScaleAudit", "city_scale_audit.json"),
]
CHUNK_SUITES = [
    ("AftershockChunkAudit", "chunk_audit.json"),
    ("AftershockChunkSaveAudit", "chunk_save_audit.json"),
]
PERFORMANCE_SUITES = [
    ("AftershockHydrologyAsyncStepperAudit", "hydrology_async_stepper.json"),
    ("AftershockAutomaticRoadAccessAudit", "automatic_road_access_audit.json"),
    ("AftershockMunicipalAudit", "municipal_audit.json"),
    ("AftershockFireVisualAudit", "fire_visual_audit.json"),
    ("AftershockSettingsAudit", "settings_audit.json"),
    ("AftershockCityGenerationAudit", "city_generation_audit.json"),
    ("AftershockCitySampleAudit", "citysample_asset_audit.json"),
    ("AftershockPresentationPerformanceAudit", "presentation_performance_audit.json"),
    ("AftershockPresentationBudgetAudit", "presentation_budget_audit.json"),
    ("AftershockMutationSoak", "mutation_soak.json"),
    ("AftershockRealGeographyAudit", "real_geography_audit.json"),
    ("AftershockRealRegionSettlementAudit", "real_region_settlement_audit.json"),
    ("AftershockPhysicalHazardsAudit", "physical_hazards_audit.json"),
    ("AftershockRegionalEnvironmentAudit", "regional_environment_audit.json"),
    ("AftershockSaveCodecAudit", "save_codec_audit.json"),
]
SCENARIO_RUNS = [(f"scenario-{i}", "AftershockScenarioAudit", [f"-AftershockScenario={i}"],
                  f"scenario_audit_{i}.json") for i in range(7)]


def suite_run(name: str, report: str | None) -> tuple:
    if name == "AftershockHydrologyAsyncStepperAudit":
        return name, "AftershockEnvironmentAudit", ["-HydrologyAsyncStepper"], report
    # Preserve existing authored V1/chunk/city-capacity regression semantics
    # explicitly. Real coastal source windows can contain less buildable land;
    # their actual startup counts belong to the separate V2 region suite.
    # The separate real-region label uses the same production startup auditor
    # with strict V2 DEM/OSM checks and its own fresh JSON report.
    if name == "AftershockRealRegionSettlementAudit":
        return name, "AftershockRegionSettlementAudit", ["-AftershockRealRegionSettlementAudit"], report
    extra = (["-AftershockVisualAudit", "-AftershockLegacyGeographyAudit"]
             if name in ("AftershockChunkSaveAudit", "AftershockRegionSettlementAudit", "AftershockCityScaleAudit") else [])
    return name, name, extra, report


def main() -> int:
    all_runs = [suite_run(name, report) for name, report in
                BASE_SUITES + FOUNDATION_SUITES + CHUNK_SUITES + PERFORMANCE_SUITES] + SCENARIO_RUNS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--user-dir-root", type=Path,
                        help="Private UE user/Saved roots; each process gets a fresh numbered directory")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--foundation", action="store_true", help="Also audit regions, bridges, spacing, session persistence and rubble poses")
    parser.add_argument("--chunks", action="store_true", help="Also audit global terrain storage and expanded save migration")
    parser.add_argument("--performance", action="store_true", help="Also audit presentation work/cache bounds, render budgets, mutation soak and real geography")
    parser.add_argument("--suites", nargs="+", choices=[run[0] for run in all_runs],
                        help="Run only these labels, including optional suites without their group flag; scenario-0 through scenario-6 are individual scenarios")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    project = repo / "JamProject.uproject"
    if not args.editor.is_file() or not project.is_file():
        parser.error("The installed editor executable and JamProject.uproject must exist.")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or repo / "Saved/PackageValidation" / ("first-playable-" + stamp)).resolve()
    output.mkdir(parents=True, exist_ok=False)
    user_root = (args.user_dir_root or output / "users").resolve()
    # Isolate save slots as well as settings. Short numeric names avoid the
    # Windows MAX_PATH limit when a commandlet has a long descriptive label.
    user_root.mkdir(parents=True, exist_ok=False)
    settings_isolation = RunSettingsIsolation(repo, output)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, text=True, capture_output=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=repo, text=True, capture_output=True, check=True).stdout.strip())
    suites = list(BASE_SUITES)
    if args.foundation:
        suites += FOUNDATION_SUITES
    if args.chunks:
        suites += CHUNK_SUITES
    if args.performance:
        suites += PERFORMANCE_SUITES
    runs = [suite_run(name, report) for name, report in suites] + SCENARIO_RUNS
    if args.suites:
        by_label = {run[0]: run for run in all_runs}
        runs = [by_label[label] for label in dict.fromkeys(args.suites)]
    results = []
    for case_index, (label, commandlet, extra, report) in enumerate(runs):
        started = time.time()
        engine_log = output / (label + ".log")
        settings_folder = output / "isolated-settings" / label
        isolated_settings = settings_isolation.prepare_case(settings_folder)
        private_user = user_root / f"{case_index:03d}"
        private_user.mkdir(exist_ok=False)
        command = [str(args.editor.resolve()), str(project), f"-run={commandlet}",
                   "-unattended", "-nop4", "-NullRHI", "-nosound", f"-abslog={engine_log}",
                   f"-GameUserSettingsINI={isolated_settings}", f"-UserDir={private_user}", *extra]
        timed_out = False
        with (output / (label + "-console.txt")).open("wb") as console:
            try:
                code = subprocess.run(command, cwd=repo, stdout=console, stderr=subprocess.STDOUT,
                                      timeout=args.timeout, check=False).returncode
            except subprocess.TimeoutExpired:
                code, timed_out = -1, True
            finally:
                user_settings_proof = settings_isolation.finish_case(settings_folder, isolated_settings)
        log = engine_log.read_text(encoding="utf-8", errors="replace") if engine_log.exists() else ""
        row = {"name": label, "exit_code": code, "timed_out": timed_out,
               "seconds": round(time.time() - started, 2), "valid": False,
               "private_project_user_dir": str(private_user)}
        if report:
            source = private_user / "Saved/PresentationValidation" / report
            fresh = source.exists() and source.stat().st_mtime >= started - 1
            if fresh:
                shutil.copy2(source, output / report)
                try:
                    data = json.loads(source.read_text(encoding="utf-8-sig"))
                    if commandlet == "AftershockMutationSoak":
                        samples = data.get("samples", [])
                        if not isinstance(samples, list) or any(not isinstance(sample, dict) for sample in samples):
                            raise ValueError("Malformed mutation sample rows")
                        row.update(buildings=data.get("buildings"), samples=len(samples),
                                   monotonic_ids=data.get("monotonic_ids"),
                                   frame_counter_advanced=data.get("frame_counter_advanced"),
                                   deferred_frame_probes=data.get("deferred_frame_probes"),
                                   logical_frame_probes=data.get("logical_frame_probes"),
                                   canonical_ism_slots_before=data.get("canonical_ism_slots_before"),
                                   canonical_ism_slots_after=data.get("canonical_ism_slots_after"))
                        row["valid"] = (code == 0 and data.get("valid") is True
                                        and data.get("buildings") == 1000 and data.get("monotonic_ids") is True
                                        and data.get("frame_counter_advanced") is True and data.get("deferred_frame_probes") == 16
                                        and data.get("logical_frame_probes") == 119
                                        and all(sample.get("deferred_frame_probe_ran") is True for sample in samples if sample.get("cycle") in range(16))
                                        and [sample.get("cycle") for sample in samples] == list(range(17)))
                    else:
                        row.update(passed=data.get("passed"), failed=data.get("failed"))
                        row["valid"] = code == 0 and data.get("failed") == 0 and data.get("passed", 0) > 0
                        if commandlet == "AftershockCityScaleAudit":
                            row["geography_mode"] = data.get("geography_mode")
                            row["valid"] = row["valid"] and data.get("geography_mode") == "V1LegacyAuthored"
                except (ValueError, TypeError):
                    row["report_error"] = "Unreadable result JSON"
            else:
                row["report_error"] = "No fresh report; an older report is never accepted"
        elif commandlet == "AftershockTransportAudit":
            match = re.search(r"AFTERSHOCK_TRANSPORT_AUDIT result=(PASS|FAIL) checks=(\d+)", log)
            row["valid"] = code == 0 and bool(match and match[1] == "PASS")
            row["checks"] = int(match[2]) if match else 0
        else:
            checks = re.findall(r"AFTERSHOCK_CITIZEN_CHECK case=\S+ passed=([01])", log)
            row["valid"] = code == 0 and bool(checks) and all(value == "1" for value in checks)
            row["checks"] = len(checks)
        if re.search(r"Fatal error:|Ensure condition failed|Assertion failed|LogShaderCompilers: Error", log):
            row["valid"] = False
            row["engine_error"] = True
        row["user_settings_isolation"] = user_settings_proof
        row["valid"] = bool(row["valid"]) and user_settings_proof["originals_unchanged"]
        results.append(row)
        (output / "summary.json").write_text(json.dumps({"utc": stamp, "source_sha": revision, "working_tree_dirty_at_start": dirty, "scope": "Real UE commandlet fixtures; no GPU, OS input or packaged execution claim", "user_settings_preservation": settings_isolation.current(), "results": results}, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(row), flush=True)
    print(f"Evidence: {output}", flush=True)
    return 0 if all(row["valid"] for row in results) and settings_isolation.current()["originals_unchanged"] else 1


if __name__ == "__main__":
    sys.exit(main())
