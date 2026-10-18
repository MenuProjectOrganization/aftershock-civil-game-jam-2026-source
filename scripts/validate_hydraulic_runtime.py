"""One isolated real-D3D12 Model2 flood process, unchanged production duration.

No native commandlet fixture and no manufactured water/source/time. The observer
owns a 300-wall-second failure deadline; external process timeout is 330 seconds.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

from validation_settings_isolation import RunSettingsIsolation
from validate_runtime_presentations import read_requested_graphics_quality


def image_evidence(folder: Path) -> dict:
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError as exc:
        return {'valid':False,'error':repr(exc),'scope':'Pillow missing; no pixel acceptance was performed.'}
    results=[]
    for stage in (2,4):
        stem=f'hydraulic-stage-{stage}'
        paths=[folder/(stem+suffix+'.png') for suffix in ('','-without-water','-shown-control')]
        row={'stage':stage,'valid':False}
        try:
            images=[Image.open(p).convert('RGB') for p in paths]
            dimensions=all(im.size==(1600,1000) for im in images)
            removed=ImageChops.difference(images[0],images[1]);control=ImageChops.difference(images[0],images[2])
            signal=sum(ImageStat.Stat(removed).sum);noise=sum(ImageStat.Stat(control).sum)
            # A/B/A: reject a blank/occluded water surface and changes that are
            # indistinguishable from same-camera repeat-render noise.
            changed=sum(1 for p in removed.getdata() if max(p)>2)
            row.update(valid=dimensions and changed>=64 and signal>max(256.,4.*noise+128.),
                       changed_pixels=changed,water_hidden_l1=signal,shown_repeat_l1=noise)
        except (OSError,ValueError) as exc:
            row['error']=repr(exc)
        results.append(row)
    return {'valid':all(row['valid'] for row in results),'pairs':results,
            'scope':'Capture-only water A/B/A visibility attribution, not photorealism or performance acceptance.'}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--quality',type=int,choices=range(5),default=3)
    parser.add_argument('--region',type=int,choices=range(20),default=1)
    parser.add_argument('--focus',type=int,default=0)
    parser.add_argument('--user-dir-root',type=Path)
    parser.add_argument('--deadline',type=int,choices=(300,480),default=300,
                        help='Real observation window only; never changes source duration or physical time.')
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1]
    if not args.editor.is_file():parser.error('Installed UnrealEditor executable required')
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    case=output/'hydraulic-runtime'
    isolation=RunSettingsIsolation(repo,output);private_ini=isolation.prepare_case(case)
    private_user=args.user_dir_root.resolve() if args.user_dir_root else case/'user'
    private_user.mkdir(parents=True,exist_ok=False)
    source_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
    dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=repo,text=True).strip())
    command=[str(args.editor.resolve()),str(repo/'JamProject.uproject'),'/Game/Aftershock/Levels/L_Level_01_Deprem',
             '-game','-RenderOffscreen','-windowed','-ResX=1600','-ResY=900','-ForceRes','-d3d12','-NoSplash','-unattended','-nop4',
             # Exercise the real player resource-preparation gate as well. The
             # visual-audit flag alone intentionally bypasses that gate.
             '-AftershockVisualAudit','-AftershockStartupAudit','-AftershockConfiguredSessionAudit',f'-AftershockAuditRegion={args.region}',f'-AftershockAuditFocus={args.focus}',
             '-AftershockAuditSourceVersion=3','-AftershockHydraulicRuntimeAudit',f'-AftershockGraphicsQuality={args.quality}',
             f'-AftershockHydraulicRuntimeDeadline={args.deadline}',
             f'-UserDir={private_user}',f'-GameUserSettingsINI={private_ini}',f'-abslog={case/"runtime.log"}']
    started=time.time()
    with (case/'console.txt').open('wb') as console:
        try:
            code=subprocess.run(command,cwd=repo,stdout=console,stderr=subprocess.STDOUT,timeout=args.deadline+30,check=False).returncode
        except subprocess.TimeoutExpired:
            code=-1
        finally:
            preservation=isolation.finish_case(case,private_ini)
    log=(case/'runtime.log').read_text(encoding='utf-8',errors='replace') if (case/'runtime.log').is_file() else ''
    errors=[line for line in log.splitlines() if re.search(r'Fatal error:|Ensure condition failed|Assertion failed|LogShaderCompilers: Error|LogWindows: Error|Failed to compile Material|Default Material will be used',line)]
    native_source=private_user/'Saved/PresentationValidation/HydraulicRuntime'
    copied=[]
    if native_source.is_dir():
        for path in native_source.iterdir():
            if path.is_file() and path.suffix in ('.json','.png') and path.stat().st_mtime>=started-1 and path.stat().st_size<=32*1024*1024:
                shutil.copy2(path,case/path.name);copied.append(path.name)
    receipt=case/'hydraulic_runtime_audit.json'
    try:
        native=json.loads(receipt.read_text(encoding='utf-8-sig')) if receipt.is_file() else {}
    except (OSError,ValueError) as exc:
        native={};errors.append('Native receipt unreadable: '+repr(exc))
    quality=read_requested_graphics_quality(case,args.quality,started,private_ini)
    pixels=image_evidence(case)
    labels=['hydraulic-before']+[f'hydraulic-stage-{i}' for i in range(5)]
    frames=native.get('frames',[])
    source_unchanged=(native.get('source_shortened') is False and native.get('source_physical_seconds')==3600
                      and native.get('production_simulation_time_scale')==20)
    native_frames=(len(frames)==6 and [f.get('label') for f in frames]==labels
                   and all(f.get('gpu_readback_valid') is True and f.get('same_camera_scene_saved') is True
                           and f.get('world_clock_unchanged_during_capture_batch') is True
                           and f.get('actual_translucent_own_material_current_resource_ready') is True for f in frames))
    valid=(code==0 and not errors and native.get('valid') is True and native.get('in_progress') is False
           and native.get('production_source_and_tail_completed') is True and native.get('actual_gpu_readback_performed') is True
           and native.get('actual_gpu_flow_observed') is True and native.get('actual_gpu_field_changed') is True
           and source_unchanged and native_frames and pixels['valid'] and quality['confirmed'] and preservation['originals_unchanged']
           and 'AFTERSHOCK_HYDRAULIC_RUNTIME_RESULT passed=1 saved=1 frames=6' in log)
    result={'valid':bool(valid),'source_sha':source_sha,'working_tree_dirty':dirty,'exit_code':code,
            'region':args.region,'focus':args.focus,'player_save_slots_isolated':True,
            'seconds':round(time.time()-started,2),'engine_errors':errors,'native_result':native.get('valid'),
            'full_production_source_and_tail':native.get('production_source_and_tail_completed'),
            'source_unchanged':source_unchanged,'native_frames_confirmed':native_frames,'quality_confirmed':quality['confirmed'],
            'pixel_visibility_evidence':pixels,'artifacts':copied,'user_settings_isolation':preservation,
            'graphics_quality_proof':quality,'scope':'Real-game GPU field/mask/source/visibility acceptance only; native mass-conservation, full visual art, FPS and platform portability remain separate.'}
    (case/'events.txt').write_text('\n'.join(x for x in log.splitlines() if 'AFTERSHOCK_' in x)+'\n',encoding='utf-8')
    (output/'summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('user_settings_isolation','graphics_quality_proof')},ensure_ascii=False))
    return 0 if valid else 1


if __name__=='__main__':
    raise SystemExit(main())
