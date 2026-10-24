param(
    [switch]$Debug,
    [switch]$BuildOnly,
    [string]$OutputDirectory = 'tmp/global-sdf-step1/cpu'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'CPU test output must remain inside this workspace.'
}
$variant = if ($Debug) { 'debug' } else { 'release' }
$output = Join-Path $output $variant
New-Item -ItemType Directory -Force -Path $output | Out-Null
$build = Join-Path $output 'build.cmd'
$exe = Join-Path $output 'VXGIGlobalSDFTests.exe'
$obj = Join-Path $output 'VXGIGlobalSDFTests.obj'
$source = Join-Path $PSScriptRoot 'VXGIGlobalSDFTests.cpp'
$optimization = if ($Debug) { '/Od /Zi /MTd' } else { '/O2 /MT /DNDEBUG' }
$pdb = Join-Path $output 'VXGIGlobalSDFTests.pdb'
$command = @"
@echo off
chcp 65001 >nul
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 $optimization /EHsc /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "$repo" /Fo"$obj" /Fd"$pdb" /Fe"$exe" "$source"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($build, ($command -replace "`r?`n", "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $build
if ($LASTEXITCODE -ne 0) { throw 'Global SDF standalone CPU test build failed.' }
if ($BuildOnly) { return }
$report = Join-Path $output 'report.json'
& $exe $report
if ($LASTEXITCODE -ne 0) { throw "Global SDF CPU validation failed. Report: $report" }
