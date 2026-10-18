"""Real paid road transaction, native captures and opt-in CPU attribution.

Runs exactly one Unreal process. CPU scopes overlap; phase times are never
summed into a fabricated frame time. User settings are isolated through UE's
native destination override and checked after the process exits.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import time

from validation_settings_isolation import RunSettingsIsolation
from validate_runtime_presentations import read_requested_graphics_quality



def read_capture_final_pp(case: Path, started: float, require_lumen: bool) -> dict:
    """Actual RT final settings only; configuration does not prove GI contribution."""
    path = case / 'single-road-after-scene.png.capture.json'
    data = {}
    fresh = path.is_file() and path.stat().st_mtime >= started - 1
    if fresh:
        try:
            data = json.loads(path.read_text(encoding='utf-8-sig'))
        except (ValueError, OSError):
            pass
    checks = {
        'fresh_actual_capture_receipt': fresh and data.get('schema_version') == 1,
        'native_receipt_valid': data.get('valid') is True,
        'render_thread_observed': data.get('render_thread_observed') is True,
        'resolved_actual_key_settings_match': data.get('resolved_to_actual_key_settings_match') is True,
        'pinned_pose_resolution_preserved': data.get('pinned_pose_resolution_preserved') is True,
        'pixels_saved': data.get('pixels_saved') is True,
        'final_color_receipt': data.get('material_buffer') is False,
        'lumen_gate_requested_when_needed': not require_lumen or data.get('lumen_required') is True,
        'actual_lumen_configured_when_needed': not require_lumen or data.get('actual_capture_lumen_configured') is True,
        'no_false_convergence_claim': data.get('lumen_convergence_verified') is False,
    }
    return {'confirmed': all(checks.values()), 'checks': checks, 'native_receipt': data,
            'scope': 'Actual capture render-thread final GI/reflection/exposure. Necessary configuration acceptance only; not Lumen convergence, measured indirect contribution or facade/art acceptance.'}


def read_physical_sun_diagnostic(native: dict, requested: bool) -> dict:
    """Fail closed on native observed properties, never infer application from CLI."""
    if not requested:
        return {'requested': False, 'confirmed': True, 'scope': 'Diagnostic not requested'}
    lighting = native.get('after', {}).get('lighting_audit', {})
    receipt = lighting.get('physical_sun_preview', {})
    def near(a, b, tolerance=1e-4):
        return (isinstance(a, (float, int)) and not isinstance(a, bool)
                and isinstance(b, (float, int)) and not isinstance(b, bool)
                and math.isfinite(a) and math.isfinite(b) and abs(a-b) <= tolerance)
    sun_before = receipt.get('sun_lux_before')
    bias_before = receipt.get('manual_bias_before')
    expected = (bias_before - math.log2(50000.0 / sun_before)
                if isinstance(sun_before, (float, int)) and math.isfinite(sun_before) and sun_before > 0
                and isinstance(bias_before, (float, int)) and math.isfinite(bias_before) else None)
    active_suns = [row for row in lighting.get('directional_lights', []) if row.get('component_enabled_observed') is True]
    active_skies = [row for row in lighting.get('sky_lights', []) if row.get('component_enabled_observed') is True]
    checks = {
        'native_requested_attempted_applied': all(receipt.get(k) is True for k in ('requested', 'attempted', 'applied')),
        'one_selected_sun_sky_grade': all(receipt.get(k) == 1 for k in ('sun_candidates', 'sky_candidates', 'grade_candidates')),
        'visible_atmosphere_present': receipt.get('atmosphere_candidates', 0) >= 1,
        'one_recapture_request': receipt.get('recapture_requests') == 1,
        'sun_50000_lux_readback': near(receipt.get('sun_lux_observed'), 50000.0, .01),
        'sky_multiplier_unchanged': near(receipt.get('sky_intensity_before'), receipt.get('sky_intensity_observed')),
        'paired_manual_exposure_readback': near(receipt.get('manual_bias_observed'), expected) and near(receipt.get('manual_bias_expected'), expected),
        'direction_color_lower_unchanged': all(receipt.get(k) is True for k in ('sun_direction_unchanged', 'sun_color_unchanged', 'sky_color_unchanged', 'sky_lower_hemisphere_unchanged')),
        'manual_mode_preserved': receipt.get('manual_mode_preserved') is True,
        'selected_components_alive': receipt.get('selected_components_still_valid') is True,
        'actual_world_one_sun_50000': len(active_suns) == 1 and near(active_suns[0].get('intensity'), 50000.0, .01),
        'actual_world_one_ready_sky': len(active_skies) == 1 and active_skies[0].get('processed_resource_ready_observed') is True
             and active_skies[0].get('cpu_sh_finite') is True and active_skies[0].get('cpu_sh_nonzero_observed') is True,
        'no_save_requested': receipt.get('map_or_config_save_requested') is False,
    }
    return {'requested': True, 'confirmed': all(checks.values()), 'checks': checks, 'native_receipt': receipt,
            'scope': 'Property/capture-resource acceptance only. Native nonzero SH is not facade illumination; new images still require review.'}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quality', type=int, choices=range(5), default=3)
    parser.add_argument('--material-buffers', action='store_true')
    parser.add_argument('--ground-bounce-preview', action='store_true', help='Opt-in lighting comparison; does not enable this experiment in ordinary games')
    parser.add_argument('--diagnostic-no-dfao', action='store_true', help='Separate-process lighting A/B; no project setting is written')
    parser.add_argument('--diagnostic-lumen', action='store_true', help='Software Lumen A/B only; no project setting is written')
    parser.add_argument('--diagnostic-physical-sun', action='store_true', help='Isolated 50k-lux sun + matched manual-bias + one sky recapture; no map/config write')
    parser.add_argument('--require-performance', action='store_true')
    parser.add_argument('--callback-budget-ms', type=float, default=16.0, help='Post-transaction crowd callback CPU budget, separate from native transaction budget')
    args = parser.parse_args()
    if args.diagnostic_physical_sun and (args.diagnostic_lumen or args.diagnostic_no_dfao or args.ground_bounce_preview):
        parser.error('Physical-sun comparison requires a separate process; do not combine other lighting experiments')
    if not math.isfinite(args.callback_budget_ms) or args.callback_budget_ms <= 0:
        parser.error('--callback-budget-ms must be finite and positive')
    repo = Path(__file__).resolve().parents[1]
    if not args.editor.is_file():
        parser.error('Installed UnrealEditor executable required')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    isolation = RunSettingsIsolation(repo, output)
    case = output / 'single-road'
    private_ini = isolation.prepare_case(case)
    source_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=repo, text=True).strip())
    command = [str(args.editor.resolve()), str(repo / 'JamProject.uproject'),
               '/Game/Aftershock/Levels/L_Level_01_Deprem', '-game', '-RenderOffscreen',
               '-windowed', '-ResX=1600', '-ResY=900', '-ForceRes', '-d3d12', '-NoSplash',
               '-unattended', '-nop4', '-AftershockVisualAudit', '-AftershockConfiguredSessionAudit',
               '-AftershockAuditRegion=8', '-AftershockAuditFocus=0', '-AftershockAuditSourceVersion=3',
               '-AftershockSingleRoadConstructionAudit', '-AftershockRoadCpuDiagnostics',
               f'-AftershockGraphicsQuality={args.quality}', f'-GameUserSettingsINI={private_ini}',
               f'-abslog={case / "runtime.log"}']
    if args.material_buffers:
        command += ['-AftershockAlbedoAudit', '-AftershockNormalAudit', '-AftershockGroundOcclusionAudit']
    if args.diagnostic_physical_sun:
        command.append('-AftershockPhysicalSunPreview')
    if args.ground_bounce_preview:
        command.append('-AftershockGroundBouncePreview')
    if args.diagnostic_no_dfao:
        command.append('-ExecCmds=r.DistanceFieldAO 0')
    if args.diagnostic_lumen:
        if args.diagnostic_no_dfao:
            parser.error('Use a separate process for each lighting diagnostic')
        command.append('-AftershockCaptureRequireLumen')
        command.append('-ExecCmds=r.DynamicGlobalIlluminationMethod 1,r.Lumen.HardwareRayTracing 0,r.Lumen.TraceMeshSDFs 1,r.Lumen.DiffuseIndirect.Allow 1')
    started = time.time()
    with (case / 'console.txt').open('wb') as console:
        try:
            code = subprocess.run(command, cwd=repo, stdout=console, stderr=subprocess.STDOUT,
                                  timeout=180, check=False).returncode
        except subprocess.TimeoutExpired:
            code = -1
        finally:
            preservation = isolation.finish_case(case, private_ini)
    log_path = case / 'runtime.log'
    log = log_path.read_text(encoding='utf-8', errors='replace') if log_path.is_file() else ''
    errors = [line for line in log.splitlines() if re.search(
        r'Fatal error:|Ensure condition failed|Assertion failed|LogShaderCompilers: Error|LogWindows: Error|Failed to compile Material|Default Material will be used', line)]
    for path in (repo / 'Saved/PresentationValidation').glob('single*road*'):
        if path.is_file() and path.suffix in ('.json', '.png') and path.stat().st_mtime >= started - 1:
            shutil.copy2(path, case / path.name)
    receipt_path = case / 'single_road_construction_audit.json'
    native = json.loads(receipt_path.read_text(encoding='utf-8-sig')) if receipt_path.is_file() else {}
    phases = defaultdict(list)
    for phase, ms, frame in re.findall(r'AFTERSHOCK_ROAD_CPU phase=(\S+) cpuMs=([\d.]+) frame=(\d+)', log):
        phases[phase].append({'cpu_ms': float(ms), 'frame': int(frame)})
    transaction_frame = native.get('transaction_frame_counter', -1)
    phase_summary = {}
    for phase, samples in phases.items():
        following = [sample for sample in samples if sample['frame'] > transaction_frame]
        phase_summary[phase] = {'calls': len(samples), 'max_cpu_ms': max(x['cpu_ms'] for x in samples),
                                'transaction_frame_samples': [s for s in samples if s['frame'] == transaction_frame],
                                'following_max_cpu_ms': max((x['cpu_ms'] for x in following), default=None)}
    # A deferred timer can execute later in the SAME engine frame as the paid
    # transaction. Include frame>=transaction here; strict 'following' above is
    # retained only as a descriptive statistic, never the acceptance boundary.
    callback = [s for s in phases.get('Crowd.UpdateCrowdPositions', []) if s['frame'] >= transaction_frame]
    safety = [s for s in phases.get('Crowd.RefreshRoutingSafety', []) if s['frame'] > transaction_frame]
    callbacks_finite = bool(callback) and all(math.isfinite(s['cpu_ms']) and s['cpu_ms'] >= 0 for s in callback)
    safety_finite = bool(safety) and all(math.isfinite(s['cpu_ms']) and s['cpu_ms'] >= 0 for s in safety)
    callback_peak = max((s['cpu_ms'] for s in callback), default=None)
    callback_evidence = {
        'present': callbacks_finite and safety_finite,
        'transaction_frame_included': True,
        'callback_samples': len(callback),
        'strictly_following_safety_samples': len(safety),
        'callback_peak_cpu_ms': callback_peak,
        'callback_budget_ms': args.callback_budget_ms,
        'callback_budget_accepted': callbacks_finite and callback_peak <= args.callback_budget_ms,
        'scope': 'Actual complete crowd callback includes coalesced sync and safety work; nested phase times are not summed. No GPU or full-frame-time claim.',
    }
    late_performance = callback_evidence['present'] and callback_evidence['callback_budget_accepted']
    quality_proof = read_requested_graphics_quality(case, args.quality, started, private_ini)
    physical_sun_proof = read_physical_sun_diagnostic(native, args.diagnostic_physical_sun)
    capture_final_pp = read_capture_final_pp(case, started, args.diagnostic_lumen)
    (case / 'events.txt').write_text('\n'.join(line for line in log.splitlines() if 'AFTERSHOCK_' in line) + '\n', encoding='utf-8')
    valid = code == 0 and not errors and native.get('valid') is True and preservation['originals_unchanged'] and quality_proof['confirmed']
    valid &= 'AFTERSHOCK_VISUAL_AUDIT_END singleRoadConstruction=1' in log
    valid &= physical_sun_proof['confirmed'] and capture_final_pp['confirmed']
    if args.require_performance:
        valid &= native.get('performance_accepted') is True and late_performance
    result = {'valid': bool(valid), 'exit_code': code, 'source_sha': source_sha,
              'working_tree_dirty': dirty, 'seconds': round(time.time() - started, 2),
              'utc': datetime.now(timezone.utc).isoformat(), 'engine_errors': errors,
              'quality_requested': args.quality, 'material_buffers_requested': args.material_buffers,
              'ground_bounce_preview_requested': args.ground_bounce_preview,
              'physical_sun_diagnostic': physical_sun_proof,
              'capture_final_postprocess': capture_final_pp,
              'graphics_quality_proof': quality_proof,
              'performance_required': args.require_performance,
              'native_transaction_cpu_ms': native.get('production_transaction_cpu_ms'),
              'native_performance_accepted': native.get('performance_accepted'),
              'cpu_phases': phase_summary, 'post_transaction_callback_evidence': callback_evidence,
              'user_settings_isolation': preservation,
              'scope': 'One paid 200TL road via production API. Nested CPU scopes overlap. Not GPU timing, not a memory-leak verdict. Following callback peaks reported separately.'}
    (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key not in ('user_settings_isolation', 'graphics_quality_proof', 'physical_sun_diagnostic', 'capture_final_postprocess')}
                     | {'quality_confirmed': quality_proof['confirmed'], 'native_settings_destination_confirmed': quality_proof.get('native_settings_destination_confirmed'), 'physical_sun_confirmed': physical_sun_proof['confirmed'], 'capture_final_pp_confirmed': capture_final_pp['confirmed']}), flush=True)
    return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
