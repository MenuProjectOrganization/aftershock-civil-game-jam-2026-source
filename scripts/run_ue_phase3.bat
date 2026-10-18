@echo off
setlocal enabledelayedexpansion

echo ===============================================================================
echo AFTERSHOCK - UE5 Headless Python Pipeline Runner (Phase 3)
echo ===============================================================================

set "REPO_ROOT=%~dp0.."
set "UPROJECT=%REPO_ROOT%\JamProject.uproject"
set "SCRIPT=%REPO_ROOT%\scripts\ue_import_and_materials.py"

:: Check potential Unreal Engine install paths
set "UE_CMD="

for %%V in (5.8 5.7 5.6 5.5 5.4 5.3 5.2 5.1 5.0) do (
    if exist "C:\Program Files\Epic Games\UE_%%V\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" (
        set "UE_CMD=C:\Program Files\Epic Games\UE_%%V\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
        goto :FoundUE
    )
    if exist "D:\Program Files\Epic Games\UE_%%V\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" (
        set "UE_CMD=D:\Program Files\Epic Games\UE_%%V\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
        goto :FoundUE
    )
)

:: Check if in PATH
where UnrealEditor-Cmd.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    for /f "delims=" %%I in ('where UnrealEditor-Cmd.exe') do (
        set "UE_CMD=%%I"
        goto :FoundUE
    )
)

echo [NOTICE] UnrealEditor-Cmd.exe was not automatically found in standard installation directories.
echo If Unreal Engine is installed in a custom path, pass it as the first argument:
echo    run_ue_phase3.bat "C:\Path\To\UnrealEditor-Cmd.exe"
echo.
echo Running standalone validation pass...
"%REPO_ROOT%\.tools\python\python.exe" "%SCRIPT%"
goto :EOF

:FoundUE
echo [FOUND] Unreal Engine Commandlet: "%UE_CMD%"
echo [RUNNING] Executing Python automation pipeline...
"%UE_CMD%" "%UPROJECT%" -run=pythonscript -script="%SCRIPT%" -stdout -FullStdOutLogOutput
echo [DONE] Commandlet finished.
