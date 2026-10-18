[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent),
    # Zero accepts a duration capture with an unknown frame count (at least 300).
    [ValidateScript({ $_ -eq 0 -or ($_ -ge 300 -and $_ -le 12000) })]
    [int]$ExpectedFrames = 300
)

$ErrorActionPreference = 'Stop'
$taskFolder = Join-Path $ProjectRoot 'Saved/PresentationValidation'
$taskResults = @()
$taskMetadata = @{}

foreach ($taskAgents in @(500, 1500)) {
    $taskCsvPath = Join-Path $taskFolder ('crowd-render-' + $taskAgents + '.csv')
    $taskRows = Import-Csv -LiteralPath $taskCsvPath
    # Decode the footer as CSV so quoted commas remain intact. The exported
    # report selects hardware/version/resolution only, without account metadata.
    $taskFooterLine = Get-Content -LiteralPath $taskCsvPath -Tail 1
    $taskFieldNames = @(0..($taskFooterLine.Split(',').Count) | ForEach-Object { 'Field' + $_ })
    $taskFooter = $taskFooterLine | ConvertFrom-Csv -Header $taskFieldNames
    $taskFields = @($taskFooter.PSObject.Properties | ForEach-Object { $_.Value })
    for ($taskIndex = 0; $taskIndex -lt $taskFields.Count - 1; $taskIndex += 2) {
        if ($taskFields[$taskIndex]) { $taskMetadata[$taskFields[$taskIndex].Trim([char[]]'[]')] = $taskFields[$taskIndex + 1] }
    }
    $taskMetrics = [ordered]@{}
    foreach ($taskMetric in @('FrameTime', 'GameThreadTime', 'RenderThreadTime', 'GPUTime', 'RHIThreadTime', 'GPUMem/LocalUsedMB', 'AftershockCrowd/GameThread/UpdatePositions', 'AftershockCrowd/GameThread/LocalSeparation', 'AftershockCrowd/GameThread/ReplanEvacuation')) {
        # Unreal appends a second header and metadata footer. Only numeric,
        # positive timing/memory samples belong to the measured distribution.
        $taskValues = @(@(foreach ($taskRow in $taskRows) {
            $taskNumber = 0.0
            if ([double]::TryParse($taskRow.$taskMetric, [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$taskNumber) -and $taskNumber -gt 0) {
                $taskNumber
            }
        }) | Sort-Object)
        if ($taskValues.Count -gt 0) {
            $taskMetrics[$taskMetric] = [ordered]@{
                samples = $taskValues.Count
                mean = [math]::Round(($taskValues | Measure-Object -Average).Average, 4)
                p95 = [math]::Round($taskValues[[math]::Ceiling($taskValues.Count * .95) - 1], 4)
                max = [math]::Round($taskValues[-1], 4)
            }
        }
    }
    $taskActualFrames = if ($taskMetrics.Contains('FrameTime')) { $taskMetrics['FrameTime']['samples'] } else { 0 }
    if (-not $taskMetrics.Contains('GPUTime') -or $taskActualFrames -lt 300 -or ($ExpectedFrames -gt 0 -and $taskActualFrames -ne $ExpectedFrames)) {
        $taskFrameRequirement = if ($ExpectedFrames -eq 0) { 'at least 300 duration-capture frames' } else { "$ExpectedFrames frames" }
        throw "Expected $taskFrameRequirement and real GPU timing for $taskAgents agents; found $taskActualFrames frames. Use -csvGpuStats."
    }
    $taskCapture = $null
    $taskCapturePath = Join-Path $taskFolder ('crowd-render-' + $taskAgents + '.profile.json')
    if (Test-Path -LiteralPath $taskCapturePath) {
        $taskCapture = Get-Content -LiteralPath $taskCapturePath -Raw | ConvertFrom-Json
        if (-not $taskCapture.csv_ready -or -not $taskCapture.frame_count_valid -or $taskCapture.timed_out -or $taskCapture.actual_frames -ne $taskActualFrames) {
            throw "Profile completion metadata does not match the actual CSV for $taskAgents agents."
        }
    }
    # Summing measured whole-frame durations reports the real captured interval,
    # independently of target FPS or the requested duration/frame count.
    $taskActualSeconds = 0.0
    foreach ($taskRow in $taskRows) {
        $taskFrameMs = 0.0
        if ([double]::TryParse($taskRow.FrameTime, [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$taskFrameMs) -and $taskFrameMs -gt 0) {
            $taskActualSeconds += $taskFrameMs / 1000.0
        }
    }
    $taskResults += [ordered]@{ agents = $taskAgents; actualFrames = $taskActualFrames; capturedFrameSeconds = [math]::Round($taskActualSeconds, 3); captureMetadata = $taskCapture; metrics = $taskMetrics }
}

$taskReport = [ordered]@{
    hardware = $taskMetadata['cpu'] + ' / ' + $taskMetadata['gpu']
    engine = $taskMetadata['engineversion'] + ' / ' + $taskMetadata['config']
    resolution = $taskMetadata['systemresolution.resx'] + 'x' + $taskMetadata['systemresolution.resy']
    capture = 'World timers request synthetic setup at 2 seconds and CSV capture at 3 seconds; startup can delay them. Per-case metadata records the actual capture start and completion deadline. Frame counts and captured frame durations come from each completed CSV, one run each.'
    expectedFrames = $ExpectedFrames
    scope = 'Whole scene including terrain, buildings, HUD and synthetic visual citizen population; not isolated citizen GPU cost or a packaged game benchmark.'
    crowdCpuScope = 'AftershockCrowd scoped CPU metrics include only positive sample frames: timer-based UpdatePositions includes schedule, shared route cache work and the ISM batch update. ReplanEvacuation is separate. Counts are sampled frames, not a per-agent cost or a zero-filled frame average.'
    units = 'Milliseconds except GPUMem/LocalUsedMB, which is MB.'
    csvGpuStats = $true
    results = $taskResults
}
$taskReport | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $taskFolder 'render_profile_summary.json') -Encoding utf8
$taskReport
