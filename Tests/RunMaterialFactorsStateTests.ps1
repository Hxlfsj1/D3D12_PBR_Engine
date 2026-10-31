param(
    [switch]$Debug,
    [switch]$BuildOnly,
    [string]$OutputDirectory = 'tmp/material-factors-state-tests'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'CPU test output must remain inside this workspace.'
}
$output = Join-Path $output $(if ($Debug) { 'debug' } else { 'release' })
New-Item -ItemType Directory -Force -Path $output | Out-Null
$build = Join-Path $output 'build.cmd'
$exe = Join-Path $output 'MaterialFactorsStateTests.exe'
$optimization = if ($Debug) { '/Od /Zi /MTd' } else { '/O2 /MT /DNDEBUG' }
$sdk = Join-Path $repo 'packages/Microsoft.Direct3D.D3D12.1.619.1/build/native/include'
$command = @"
@echo off
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 $optimization /EHsc /W4 /WX /wd4324 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$repo" /I "$repo\Core" /I "$sdk" /I "$sdk\d3dx12" /Fo"$output\MaterialFactorsStateTests.obj" /Fd"$output\MaterialFactorsStateTests.pdb" /Fe"$exe" "$PSScriptRoot\MaterialFactorsStateTests.cpp"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($build, ($command -replace "`r?`n", "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'Material factor state CPU test build failed.' }
if ($BuildOnly) { return }
$report = Join-Path $output 'report.json'
& $exe $report
if ($LASTEXITCODE -ne 0) { throw "Material factor state CPU validation failed. Report: $report" }
