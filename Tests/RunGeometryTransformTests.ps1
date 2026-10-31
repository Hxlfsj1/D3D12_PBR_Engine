param([switch]$Debug, [string]$OutputDirectory = 'tmp/geometry-transform-tests')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Geometry test output must remain inside this workspace.'
}
$output = Join-Path $output $(if ($Debug) { 'debug' } else { 'release' })
New-Item -ItemType Directory -Force -Path $output | Out-Null
$relative = [IO.Path]::GetRelativePath($repo, $output)
$optimization = if ($Debug) { '/Od /Zi /MTd' } else { '/O2 /MT /DNDEBUG' }
$build = Join-Path $output 'build.cmd'
$command = @"
@echo off
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 $optimization /EHsc /W4 /WX /I "." /Fo"$relative\GeometryTransformTests.obj" /Fd"$relative\GeometryTransformTests.pdb" /Fe"$relative\GeometryTransformTests.exe" "Tests\GeometryTransformTests.cpp"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($build, ($command -replace "`r?`n", "`r`n") + "`r`n", [Text.Encoding]::ASCII)
Push-Location $repo
try {
    & $env:ComSpec /d /c ([IO.Path]::GetRelativePath($repo, $build))
    if ($LASTEXITCODE -ne 0) { throw 'Geometry transform CPU test build failed.' }
    & (Join-Path $output 'GeometryTransformTests.exe') (Join-Path $relative 'report.json')
    if ($LASTEXITCODE -ne 0) { throw 'Geometry transform CPU regression failed.' }
}
finally { Pop-Location }
