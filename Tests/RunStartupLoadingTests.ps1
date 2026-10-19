param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$taskProjectRoot = (Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    $taskExecutable = Join-Path $taskProjectRoot "x64\$Configuration\LearnDirectX.exe"
    if (-not (Test-Path -LiteralPath $taskExecutable)) { throw "Build $Configuration before running startup acceptance." }
    $taskStart = [System.Diagnostics.ProcessStartInfo]::new()
    $taskStart.FileName = $taskExecutable
    $taskStart.Arguments = '--verify-startup'
    $taskStart.WorkingDirectory = $taskProjectRoot
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $taskProcess = [System.Diagnostics.Process]::Start($taskStart)
    $taskProcess.WaitForExit()
    $taskExitCode = $taskProcess.ExitCode
    $taskProcess.Dispose()
    if ($taskExitCode -ne 0) { throw "Hidden startup/shutdown failed: exit $taskExitCode" }
    $taskReport = Get-Content -LiteralPath 'tmp/Loading-startup-verification.json' -Raw | ConvertFrom-Json
    if (-not $taskReport.passed) { throw 'Initialization report failed.' }
    $taskSamples = @(Get-Content -LiteralPath 'tmp/Loading-startup-progress.jsonl' | ForEach-Object { $_ | ConvertFrom-Json })
    if ($taskSamples.Count -eq 0) { throw 'No loading frames were recorded.' }
    $taskPrevious = 0.0
    foreach ($taskSample in $taskSamples) {
        $taskFraction = [double]$taskSample.progress
        if ([double]::IsNaN($taskFraction) -or [double]::IsInfinity($taskFraction) -or
            $taskFraction -lt $taskPrevious -or $taskFraction -gt 1.0) {
            throw 'Loading progress is invalid or decreases.'
        }
        if ([string]::IsNullOrWhiteSpace($taskSample.status)) { throw 'A loading frame has no status message.' }
        $taskPrevious = $taskFraction
    }
    if ($taskPrevious -ne 1.0) { throw 'Loading progress never reached 100%.' }
    [pscustomobject]@{
        configuration = $Configuration
        process_exit_code = $taskExitCode
        loading_frames = $taskSamples.Count
        monotonic_progress = $true
        completed = $true
    } | ConvertTo-Json | Set-Content -LiteralPath 'tmp/Loading-startup-acceptance.json' -Encoding utf8
    Write-Output "Hidden $Configuration startup/shutdown passed; $($taskSamples.Count) loading frames, monotonic progress to 100%."
}
finally { Pop-Location }
