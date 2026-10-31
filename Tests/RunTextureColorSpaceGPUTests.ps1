param(
    [switch]$EnableGPUValidation,
    [switch]$BuildOnly,
    [string]$OutputDirectory = 'tmp/texture-colorspace'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'GPU test output must remain inside this workspace.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sources = @('Resources/TextureColorSpace.h','Resources/MaterialTextureLoader.h','Tests/TextureColorSpaceGPUTests.hlsl',
    'Tests/TextureColorSpaceGPUTests.cpp','Tests/RunTextureColorSpaceGPUTests.ps1')
$sourceHashes = @{}
foreach ($path in $sources) { $sourceHashes[$path] = (Get-FileHash -LiteralPath (Join-Path $repo $path)).Hash }
$dxc = 'D:/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'
$shader = Join-Path $output 'TextureColorSpaceGPUTests.dxil'
& $dxc -nologo -T cs_6_6 -E CSMain -O3 -WX -Fo $shader (Join-Path $PSScriptRoot 'TextureColorSpaceGPUTests.hlsl')
if ($LASTEXITCODE -ne 0) { throw 'Texture color space GPU shader compilation failed.' }
$relativeOutput = [IO.Path]::GetRelativePath($repo,$output)
$build = Join-Path $output 'build.cmd'
$exe = Join-Path $output 'TextureColorSpaceGPUTests.exe'
$command = @"
@echo off
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 /O2 /EHsc /W4 /WX /MD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "." /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include" /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12" /I "packages\directxtk12_desktop_win10.2026.4.1.1\include" /Fo"$relativeOutput\TextureColorSpaceGPUTests.obj" /Fe"$relativeOutput\TextureColorSpaceGPUTests.exe" "Tests\TextureColorSpaceGPUTests.cpp" "packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\Release\DirectXTK12.lib" d3d12.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($build,$command.Replace("`r`n","`n").Replace("`n","`r`n"),[Text.Encoding]::ASCII)
Push-Location $repo
try { & $env:ComSpec /d /c ([IO.Path]::GetRelativePath($repo,$build)) }
finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'Texture color space standalone GPU test build failed.' }
foreach ($path in $sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $repo $path)).Hash -ne $sourceHashes[$path]) {
        throw "Source changed while compiling GPU validation: $path"
    }
}
if ($BuildOnly) { return }
$report = Join-Path $output 'report.json'
$arguments = @([IO.Path]::GetRelativePath($repo,$shader),[IO.Path]::GetRelativePath($repo,$report))
if ($EnableGPUValidation) { $arguments += '--gbv' }
Push-Location $repo
try { & $exe @arguments }
finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw "Texture color space GPU regression failed. Report: $report" }
foreach ($path in $sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $repo $path)).Hash -ne $sourceHashes[$path]) {
        throw "Source changed during GPU validation: $path"
    }
}
@{sourceHashes=$sourceHashes;executableSha256=(Get-FileHash -LiteralPath $exe).Hash;
    shaderSha256=(Get-FileHash -LiteralPath $shader).Hash;sourcesUnchangedDuringBuildAndRun=$true} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'metadata.json') -Encoding utf8
