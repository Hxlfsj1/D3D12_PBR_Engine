param(
    [ValidateSet('Release','Debug')][string[]]$Configuration = @('Release','Debug'),
    [int]$Frames = 16,
    [string]$OutputDirectory = 'tmp/global-sdf-step2/runtime'
)
$ErrorActionPreference = 'Stop'
if ($Frames -lt 4) { throw 'Runtime smoke needs at least four submitted frames.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Runtime smoke output must remain inside this workspace.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
if (Get-Process LearnDirectX -ErrorAction SilentlyContinue) { throw 'The renderer is already running; it must be idle for the runtime smoke.' }
$saved = @('Settings/Pipeline.json','Settings/Window.json','imgui.ini','PassViewRun.json') | ForEach-Object {
    $path = Join-Path $repo $_
    [pscustomobject]@{path=$path;exists=(Test-Path -LiteralPath $path);bytes=$(if(Test-Path -LiteralPath $path){[IO.File]::ReadAllBytes($path)}else{$null})}
}
$manifest = [ordered]@{passed=$false;settingsRestored=$false;runs=@()}
$process = $null
Push-Location $repo
try {
    $pipeline = Get-Content 'Settings/Pipeline.json' -Raw | ConvertFrom-Json
    $globalConfig = $pipeline.vxgi.global_sdf
    $layerCount = [int]$globalConfig.clipmaps
    $cellCounts = @($globalConfig.cells_per_axis)
    if ($cellCounts.Count -eq 1) { $cellCounts = @(0..($layerCount - 1) | ForEach-Object { $cellCounts[0] }) }
    if ($cellCounts.Count -ne $layerCount) { throw 'Expected one cell count per Global SDF layer.' }
    $expectedNodes = [long]0
    $expectedDepth = 0
    $expectedWidth = 0
    foreach ($count in $cellCounts) {
        $resolution = [int]$count + 1
        $expectedNodes += [long]$resolution * $resolution * $resolution
        $expectedDepth += $resolution
        $expectedWidth = [Math]::Max($expectedWidth, $resolution)
    }
    $window = Get-Content 'Settings/Window.json' -Raw | ConvertFrom-Json
    $window.width = 640; $window.height = 400
    $window | ConvertTo-Json -Depth 15 | Set-Content 'Settings/Window.json' -Encoding utf8
    foreach ($variant in $Configuration) {
        $exe = Join-Path $repo "x64/$variant/LearnDirectX.exe"
        if (-not (Test-Path -LiteralPath $exe)) { throw "Build the $variant main application first." }
        $started = [DateTime]::UtcNow
        $watch = [Diagnostics.Stopwatch]::StartNew()
        Write-Output "Starting $variant runtime smoke ($Frames frames)."
        $process = Start-Process -FilePath $exe -ArgumentList @('--hidden',"--frames=$Frames",'--pass-view=default') -WorkingDirectory $repo -WindowStyle Hidden -PassThru
        while (-not $process.WaitForExit(1000)) {
            if ($watch.Elapsed.TotalSeconds -gt 360) { throw 'Runtime smoke exceeded 360 seconds.' }
        }
        $process.Refresh(); $exitCode = $process.ExitCode; $process = $null
        if (-not (Test-Path 'PassViewRun.json') -or (Get-Item 'PassViewRun.json').LastWriteTimeUtc -lt $started) { throw 'No fresh main application completion report.' }
        $reportPath = Join-Path $output "$variant.json"
        Copy-Item -LiteralPath 'PassViewRun.json' -Destination $reportPath
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($exitCode -ne 0 -or -not $report.passed -or $report.frames -ne $Frames) { throw "Main application $variant failed: $reportPath" }
        $global = $report.global_sdf
        if (-not $global.enabled -or $global.submitted_frames -ne $Frames -or
            $global.idle_frames -ne ($Frames - 1) -or $global.allocations -ne 1 -or
            $global.full_layer_updates -ne $layerCount -or $global.updated_nodes -ne $expectedNodes -or $global.dispatches -ne $layerCount) {
            throw "Unexpected static Global SDF update counters: $($global | ConvertTo-Json -Compress -Depth 8)"
        }
        if (($global.shape -join ',') -ne (@($expectedWidth, $expectedWidth, $expectedDepth) -join ',')) {
            throw 'Runtime Global SDF texture shape differs from configuration.'
        }
        for ($layer = 0; $layer -lt $layerCount; ++$layer) {
            $clipmap = $global.clipmaps[$layer]
            $expectedCellSize = [double]$globalConfig.cell_size * [Math]::Pow(2, $layer)
            if ($clipmap.cells_per_axis -ne $cellCounts[$layer] -or $clipmap.cell_size -ne $expectedCellSize) {
                throw 'Runtime Global SDF layer differs from configuration.'
            }
        }
        if ($report.d3d12_warnings -ne 0 -or $report.d3d12_errors -ne 0 -or
            ($variant -eq 'Debug' -and -not $report.d3d12_validation_available)) { throw 'Runtime D3D12 validation did not pass.' }
        $manifest.runs += [pscustomobject]@{configuration=$variant;frames=$Frames;elapsedSeconds=$watch.Elapsed.TotalSeconds;executableSha256=(Get-FileHash -LiteralPath $exe).Hash;globalSDF=$global;debugLayer=$report.d3d12_validation_available;warnings=$report.d3d12_warnings;errors=$report.d3d12_errors;report=$reportPath}
    }
    $manifest.passed = $true
}
finally {
    if ($process -and -not $process.HasExited) { Stop-Process -InputObject $process }
    foreach ($snapshot in $saved) {
        if ($snapshot.exists) { [IO.File]::WriteAllBytes($snapshot.path,$snapshot.bytes) }
        elseif (Test-Path -LiteralPath $snapshot.path) { Remove-Item -LiteralPath $snapshot.path }
    }
    $manifest.settingsRestored = @($saved | Where-Object { $_.exists -and [Convert]::ToBase64String($_.bytes) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes($_.path)) }).Count -eq 0
    $manifest | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $output 'manifest.json') -Encoding utf8
    Pop-Location
}
$manifest | ConvertTo-Json -Depth 6
