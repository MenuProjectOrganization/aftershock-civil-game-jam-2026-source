# Run serially, after UE5.8 is installed and the user-acquired source is present.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidateSet('Probe','Extract','BindBake')][string]$Phase,
 [string]$CitySampleContent='C:/ProgramData/Epic/EpicGamesLauncher/VaultCache/CitySample_5.8/data/Content',
 [string]$Editor='C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
)
$ErrorActionPreference='Stop'
$repoRoot=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$receipts=Join-Path $repoRoot 'Saved/PresentationValidation/CitySampleArchitecture'
New-Item -ItemType Directory -Path $receipts -Force|Out-Null
$script=Join-Path $PSScriptRoot 'ue_prepare_citysample_architecture.py'
$project=Join-Path $repoRoot 'JamProject.uproject'
$receiptName='native_modules_'+$Phase.ToLower()+'.json'
$argsNative=@('-unattended','-nosplash','-nop4','-NoAutoSave','-NoSound','-UTF8Output','-stdout','-FullStdOutLogOutput')
if($Phase -eq 'BindBake'){
 $script=Join-Path $PSScriptRoot 'ue_bind_and_bake_citysample_facades.py'
 $receiptName='native_facade_bake.json'
 # Native merge must retain real Wall/Glass/Metal resources. NullRHI can
 # silently substitute DefaultMaterial before the strict semantic gate.
 $argsNative+=@('-RenderOffscreen')
}else{
 $sourceRoot=(Resolve-Path -LiteralPath $CitySampleContent).Path
 $scratch=Join-Path $repoRoot 'Saved/CitySampleExtract'
 $content=Join-Path $scratch 'Content'
 New-Item -ItemType Directory -Path $content -Force|Out-Null
 if((Get-Item -LiteralPath $content).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Scratch Content must be real, never a Vault junction'}
 $owned=Join-Path $content 'Aftershock'
 New-Item -ItemType Directory -Path $owned -Force|Out-Null
 if((Get-Item -LiteralPath $owned).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Owned output must be real scratch content'}
 foreach($folder in @('Megascans','Building')){
  $source=Join-Path $sourceRoot $folder
  if(-not(Test-Path -LiteralPath $source -PathType Container)){throw "Missing acquired source folder $source"}
  $link=Join-Path $content $folder
  if(Test-Path -LiteralPath $link){
   $existing=Get-Item -LiteralPath $link
   if(-not($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Preserve unexpected real source folder $link"}
   $target=[IO.Path]::GetFullPath([string](@($existing.Target)[0])).TrimEnd('\','/')
   if(-not $target.Equals([IO.Path]::GetFullPath($source).TrimEnd('\','/'),[StringComparison]::OrdinalIgnoreCase)){throw "Preserve mismatched junction $link"}
  }else{New-Item -ItemType Junction -Path $link -Target $source|Out-Null}
 }
 $project=Join-Path $scratch 'CitySampleExtract.uproject'
 if(-not(Test-Path -LiteralPath $project)){
  $descriptor=@{FileVersion=3;EngineAssociation='5.8';Description='Bounded private-project native extraction';Plugins=@(@{Name='PythonScriptPlugin';Enabled=$true},@{Name='EditorScriptingUtilities';Enabled=$true})}
  [IO.File]::WriteAllText($project,($descriptor|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
 }else{
  $descriptor=Get-Content -LiteralPath $project -Raw|ConvertFrom-Json
  if(($descriptor.PSObject.Properties.Name -contains 'Modules') -and @($descriptor.Modules).Count -gt 0){throw 'Do not run the full CitySample gameplay modules in this scratch extractor'}
 }
 $argsNative+=@('-NullRHI')
}
foreach($required in @($Editor,$project,$script)){if(-not(Test-Path -LiteralPath $required -PathType Leaf)){throw "Required file missing $required"}}
$keys=@('AFTERSHOCK_CITY_MODULE_PHASE','AFTERSHOCK_CITY_SAMPLE_CONTENT','AFTERSHOCK_CITY_ARCH_RECEIPTS')
$old=@{};foreach($key in $keys){$old[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try{
 $env:AFTERSHOCK_CITY_MODULE_PHASE=$Phase.ToUpper()
 $env:AFTERSHOCK_CITY_SAMPLE_CONTENT=$CitySampleContent
 $env:AFTERSHOCK_CITY_ARCH_RECEIPTS=$receipts
 $started=[DateTime]::UtcNow
 $log=Join-Path $receipts ($Phase.ToLower()+'.log')
 & $Editor $project "-ExecutePythonScript=$script" @argsNative "-abslog=$log"
 if($LASTEXITCODE -ne 0){throw "Native $Phase failed exit $LASTEXITCODE; inspect $log"}
 $path=Join-Path $receipts $receiptName
 if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw 'Native receipt missing'}
 if((Get-Item -LiteralPath $path).LastWriteTimeUtc -lt $started){throw 'Reject stale receipt'}
 $result=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
 if(-not $result.valid -or $result.in_progress){throw "Native acceptance failed $path"}
 if($Phase -eq 'BindBake'){
  if($result.meshes.Count -ne 16){throw 'All16 whole-height meshes are required'}
 }else{
  if(-not $result.source_assets_unchanged -or $result.candidates.Count -ne 9){throw 'Source allowlist unchanged proof failed'}
  if($Phase -eq 'Extract' -and $result.native_migration_to_private_project.Count -ne 2){throw 'Exactly2 approved native derivatives are required'}
 }
 Write-Output "Native geometry metadata only; live/cook/performance acceptance remains separate: $path"
}finally{foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$old[$key],'Process')}}
