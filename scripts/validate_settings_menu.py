"""Exercise actual five-page UMG settings and pause/main return without OS input.
Does not apply display/audio preferences, write settings, invoke application quit,
test packaged builds, or measure GPU performance.
"""
from __future__ import annotations
import argparse, hashlib, json, re, shutil, subprocess, time
from datetime import datetime, timezone
from pathlib import Path

def digest(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None

def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--editor",required=True,type=Path);p.add_argument("--output",required=True,type=Path)
    args=p.parse_args();repo=Path(__file__).resolve().parents[1];output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=False)
    if not args.editor.is_file():p.error("Actual installed UnrealEditor executable required")
    ini=repo/"Saved/Config/WindowsEditor/GameUserSettings.ini";before=digest(ini)
    revision=subprocess.run(["git","rev-parse","HEAD"],cwd=repo,text=True,capture_output=True,check=True).stdout.strip()
    dirty=bool(subprocess.run(["git","status","--porcelain"],cwd=repo,text=True,capture_output=True,check=True).stdout.strip())
    started=time.time();log=output/"runtime.log";receipt=repo/"Saved/PresentationValidation/settings_menu_runtime_audit.json"
    command=[str(args.editor.resolve()),str(repo/"JamProject.uproject"),"/Game/Aftershock/Levels/L_Level_01_Deprem",
             "-game","-d3d12","-windowed","-ResX=1600","-ResY=900","-ForceRes","-RenderOffscreen","-unattended","-nosplash","-nop4",
             "-AftershockSettingsMenuAudit","-AftershockSettingsMenuAuditExit",f"-abslog={log}"]
    with (output/"console.txt").open("wb") as console:
        try:code=subprocess.run(command,cwd=repo,stdout=console,stderr=subprocess.STDOUT,timeout=150,check=False).returncode
        except subprocess.TimeoutExpired:code=-1
    errors=[];valid=False;report={};images=[]
    text=log.read_text(encoding="utf-8",errors="replace") if log.exists() else ""
    errors=[line for line in text.splitlines() if re.search(r"Fatal error:|Ensure condition failed|Assertion failed|LogShaderCompilers: Error|LogWindows: Error|Failed to compile Material|Default Material will be used",line)]
    try:
        if receipt.stat().st_mtime<started-1:raise ValueError("stale receipt")
        report=json.loads(receipt.read_text(encoding="utf-8-sig"))
        shutil.copy2(receipt,output/receipt.name)
        image_folder=repo/"Saved/PresentationValidation"/report["image_folder"]
        if image_folder.parent!=(repo/"Saved/PresentationValidation") or not image_folder.name.startswith("settings-menu-"):raise ValueError("invalid image folder")
        expected=["settings-pause.png",*[f"settings-page-{i}.png" for i in range(5)],"settings-return-pause.png","settings-main-menu.png","settings-from-main.png","settings-return-main.png"]
        from PIL import Image,ImageStat
        for name in expected:
            f=image_folder/name
            if f.stat().st_mtime<started-1:raise ValueError("stale PNG")
            with Image.open(f) as image:
                deviation=max(ImageStat.Stat(image.convert("RGB")).stddev)
                good=image.size==(1600,900) and deviation>=2
            shutil.copy2(f,output/name);images.append({"image":name,"dimensions":[1600,900],"rgb_standard_deviation":round(deviation,3),"valid":good})
        valid=(code==0 and not errors and report.get("valid") is True and report.get("pages_exercised")==5
               and report.get("user_settings_writes_disabled") is True and report.get("user_settings_ini_unchanged") is True
               and report.get("apply_or_quit_button_invoked") is False
               and all(row.get("passed") is True for row in report.get("steps",[]))
               and len(report.get("steps",[]))==10 and all(row["valid"] for row in images)
               and bool(re.search(r"AFTERSHOCK_SETTINGS_MENU_RUNTIME_RESULT valid=1 pages=5 reportSaved=1",text)))
    except (OSError,ValueError,KeyError,TypeError,ImportError) as e:errors.append(type(e).__name__+": "+str(e))
    valid=valid and before==digest(ini)
    summary={"source_sha":revision,"working_tree_dirty_at_start":dirty,"valid":valid,"exit_code":code,"seconds":round(time.time()-started,3),
             "utc":datetime.now(timezone.utc).isoformat(),"settings_sha256_before":before,"settings_sha256_after":digest(ini),
             "images":images,"engine_errors":errors,"scope":__doc__.strip()}
    (output/"summary.json").write_text(json.dumps(summary,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(summary),flush=True);return 0 if valid else 1
if __name__=="__main__":raise SystemExit(main())
