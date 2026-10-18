"""Windows-only read-only UE process memory sampler around the existing RHI runner.
A process working set is not proof of a leak. No GC, input or engine settings are changed.
"""
from __future__ import annotations
import argparse, ctypes, csv, hashlib, json, subprocess, sys, time
from ctypes import wintypes as w
from pathlib import Path
class PROCESSENTRY32W(ctypes.Structure):
    _fields_=[('dwSize',w.DWORD),('cntUsage',w.DWORD),('th32ProcessID',w.DWORD),('th32DefaultHeapID',ctypes.c_size_t),('th32ModuleID',w.DWORD),('cntThreads',w.DWORD),('th32ParentProcessID',w.DWORD),('pcPriClassBase',w.LONG),('dwFlags',w.DWORD),('szExeFile',w.WCHAR*260)]
class MEMORY(ctypes.Structure):
    _fields_=[('cb',w.DWORD),('PageFaultCount',w.DWORD),('PeakWorkingSetSize',ctypes.c_size_t),('WorkingSetSize',ctypes.c_size_t),('QuotaPeakPagedPoolUsage',ctypes.c_size_t),('QuotaPagedPoolUsage',ctypes.c_size_t),('QuotaPeakNonPagedPoolUsage',ctypes.c_size_t),('QuotaNonPagedPoolUsage',ctypes.c_size_t),('PagefileUsage',ctypes.c_size_t),('PeakPagefileUsage',ctypes.c_size_t),('PrivateUsage',ctypes.c_size_t)]
k=ctypes.WinDLL('kernel32',use_last_error=True);p=ctypes.WinDLL('psapi',use_last_error=True)
k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD];k.CreateToolhelp32Snapshot.restype=w.HANDLE
k.Process32FirstW.argtypes=[w.HANDLE,ctypes.POINTER(PROCESSENTRY32W)];k.Process32FirstW.restype=w.BOOL
k.Process32NextW.argtypes=k.Process32FirstW.argtypes;k.Process32NextW.restype=w.BOOL
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
p.GetProcessMemoryInfo.argtypes=[w.HANDLE,ctypes.POINTER(MEMORY),w.DWORD];p.GetProcessMemoryInfo.restype=w.BOOL

def processes():
    snap=k.CreateToolhelp32Snapshot(2,0)
    if snap==ctypes.c_void_p(-1).value:raise OSError('Process snapshot failed')
    try:
        entry=PROCESSENTRY32W();entry.dwSize=ctypes.sizeof(entry);ok=k.Process32FirstW(snap,ctypes.byref(entry))
        while ok:
            yield entry.th32ProcessID,entry.th32ParentProcessID,entry.szExeFile
            ok=k.Process32NextW(snap,ctypes.byref(entry))
    finally:k.CloseHandle(snap)

def main():
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('--repo',type=Path,required=True);a.add_argument('--editor',type=Path,required=True);a.add_argument('--output',type=Path,required=True);a.add_argument('--cases',nargs='+',default=['profile-500','profile-1500','flood']);a.add_argument('--duration',type=int,default=30)
    a.add_argument('--legacy-geography',action='store_true',help='Compare the original configured terrain fixture without changing V1 saves')
    a.add_argument('--overview',type=float,help='Opt-in presentation audit overview blend; actual state is recorded by the RHI runner')
    args=a.parse_args()
    repo=args.repo.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    active=[(pid,name) for pid,parent,name in processes() if name.casefold() in {'unrealeditor.exe','unrealeditor-cmd.exe','jamproject.exe','shadercompileworker.exe','unrealbuildtool.exe'}]
    if active:raise RuntimeError('Isolated profile requires no pre-existing Unreal/UBT/shader worker: '+str(active))
    source=subprocess.run(['git','rev-parse','HEAD'],cwd=repo,capture_output=True,text=True,check=True).stdout.strip();dirty=bool(subprocess.run(['git','status','--porcelain'],cwd=repo,capture_output=True,text=True,check=True).stdout.strip())
    report={'source_sha':source,'working_tree_dirty_at_start':dirty,'scope':'Single real D3D12 fixture per fresh process, 250ms external Windows GetProcessMemoryInfo sampling; private commit and working set, not live allocations or a leak proof. Matching renderer/capture overhead remains included. No other UE process at start.','monitor_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'results':[],'valid':False}
    for case in args.cases:
        started=time.monotonic();samples=[];handles={};first_seen={};failure=None
        cmd=[sys.executable,str(repo/'scripts/validate_runtime_presentations.py'),'--editor',str(args.editor),'--output',str(output/case),'--cases',case,'--profile-duration',str(args.duration)]
        if args.legacy_geography:cmd.append('--legacy-geography')
        if args.overview is not None:cmd.extend(['--overview',str(args.overview)])
        with (output/(case+'-runner.txt')).open('wb') as log:
            child=subprocess.Popen(cmd,cwd=repo,stdout=log,stderr=subprocess.STDOUT)
            try:
                while child.poll() is None:
                    now=time.monotonic()
                    if now-started>300:child.kill();failure='Outer fixture runner exceeded 300s';break
                    for pid,parent,name in processes():
                        if parent==child.pid and name.casefold() in {'unrealeditor.exe','unrealeditor-cmd.exe'} and pid not in handles:
                            handle=k.OpenProcess(0x410,False,pid)
                            if handle:handles[pid]=handle;first_seen[pid]=now
                    for pid,handle in handles.items():
                        memory=MEMORY();memory.cb=ctypes.sizeof(memory)
                        if p.GetProcessMemoryInfo(handle,ctypes.byref(memory),ctypes.sizeof(memory)):
                            samples.append({'monitor_seconds':round(now-started,3),'process_observed_seconds':round(now-first_seen[pid],3),'pid':pid,'working_set_bytes':memory.WorkingSetSize,'private_commit_bytes':memory.PrivateUsage,'peak_working_set_bytes':memory.PeakWorkingSetSize,'page_fault_count':memory.PageFaultCount})
                    time.sleep(.25)
            finally:
                for handle in handles.values():k.CloseHandle(handle)
            code=child.wait()
        with (output/(case+'-memory.csv')).open('w',encoding='utf-8',newline='') as out:
            writer=csv.DictWriter(out,fieldnames=list(samples[0]) if samples else ['monitor_seconds','pid']);writer.writeheader();writer.writerows(samples)
        summary_path=output/case/'summary.json';summary=json.loads(summary_path.read_text(encoding='utf-8')) if summary_path.is_file() else None
        accepted=summary is not None and summary.get('source_sha')==source and all(item.get('valid') for item in summary.get('results',[])) and len(summary.get('results',[]))==1
        row={'case':case,'exit_code':code,'samples':len(samples),'seconds':round(time.monotonic()-started,3),'runner_source_matches':summary is not None and summary.get('source_sha')==source,'valid':code==0 and accepted and len(samples)>=10 and failure is None,'error':failure,'peak_working_set_bytes':max((s['working_set_bytes'] for s in samples),default=0),'peak_private_commit_bytes':max((s['private_commit_bytes'] for s in samples),default=0),'final_private_commit_bytes':samples[-1]['private_commit_bytes'] if samples else 0}
        report['results'].append(row);report['valid']=len(report['results'])==len(args.cases) and all(r['valid'] for r in report['results']);(output/'memory-summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(row),flush=True)
    return 0 if report['valid'] else 1
if __name__=='__main__':raise SystemExit(main())
