param(
    [switch]$EnableGPUValidation,
    [switch]$BuildOnly,
    [string]$OutputDirectory = 'tmp/global-sdf-step2/gpu'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'GPU test output must remain inside this workspace.'
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sourcePaths = @(
    'Resources/VXGIGlobalSDF.h', 'Resources/VXGIGlobalSDFState.h', 'Passes/VXGIGlobalSDFPass.h',
    'Resources/VXGIMeshSDFGPUData.h', 'Resources/VXGIMeshSDFScene.h',
    'Resources/VXGIMeshSDF.h', 'Resources/VXGISDF.h', 'Resources/VXGIMeshSDFThreshold.h',
    'Resources/VXGIInstanceBVH.h', 'Resources/VXGIWorld.h', 'Resources/VXGITraceAcceleration.h',
    'Shaders/VXGIGlobalSDFCommon.hlsli', 'Shaders/Shaders_For_VXGI_GlobalSDF.hlsl',
    'Shaders/VXGIMeshSDFData.hlsli', 'Shaders/VXGISDFThresholdCommon.hlsli',
    'Tests/VXGIGlobalSDFGPUTests.cpp', 'Tests/VXGIGlobalSDFGPUTests.hlsl',
    'Tests/RunVXGIGlobalSDFGPUTests.ps1', 'ThirdParty/json.hpp'
)
$sourceHashes = @{}
foreach ($path in $sourcePaths) { $sourceHashes[$path] = (Get-FileHash -LiteralPath (Join-Path $repo $path) -Algorithm SHA256).Hash }
$started = [DateTime]::UtcNow.ToString('o')
$dxc = 'D:/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe'
$compose = Join-Path $output 'compose.dxil'
$query = Join-Path $output 'query.dxil'
& $dxc -nologo -T cs_6_0 -E CSCompose -O3 -WX -I $repo -Fo $compose (Join-Path $repo 'Shaders/Shaders_For_VXGI_GlobalSDF.hlsl')
if ($LASTEXITCODE -ne 0) { throw 'Global SDF composition shader compilation failed.' }
& $dxc -nologo -T cs_6_0 -E CSQuery -O3 -WX -I $repo -Fo $query (Join-Path $PSScriptRoot 'VXGIGlobalSDFGPUTests.hlsl')
if ($LASTEXITCODE -ne 0) { throw 'Global SDF query test shader compilation failed.' }
$build = Join-Path $output 'build.cmd'
$exe = Join-Path $output 'VXGIGlobalSDFGPUTests.exe'
$obj = Join-Path $output 'VXGIGlobalSDFGPUTests.obj'
$source = Join-Path $PSScriptRoot 'VXGIGlobalSDFGPUTests.cpp'
$relativeOutput = [IO.Path]::GetRelativePath($repo, $output)
$command = @"
@echo off
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 /O2 /EHsc /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "." /I "Resources" /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include" /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12" /Fo"$relativeOutput\VXGIGlobalSDFGPUTests.obj" /Fe"$relativeOutput\VXGIGlobalSDFGPUTests.exe" "Tests\VXGIGlobalSDFGPUTests.cpp" d3d12.lib dxgi.lib dxguid.lib
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($build, $command.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.Encoding]::ASCII)
Push-Location $repo
try { & $env:ComSpec /d /c ([IO.Path]::GetRelativePath($repo, $build)) }
finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { throw 'Global SDF standalone GPU test build failed.' }
foreach ($path in $sourcePaths) {
    if ((Get-FileHash -LiteralPath (Join-Path $repo $path) -Algorithm SHA256).Hash -ne $sourceHashes[$path]) {
        throw "Source changed while compiling GPU validation: $path"
    }
}
if ($BuildOnly) { return }
$report = Join-Path $output 'report.json'
$arguments = @($compose, $query, $report)
if ($EnableGPUValidation) { $arguments += '--gbv' }
& $exe @arguments
if ($LASTEXITCODE -ne 0) { throw "Global SDF GPU validation failed. Report: $report" }
foreach ($path in $sourcePaths) {
    if ((Get-FileHash -LiteralPath (Join-Path $repo $path) -Algorithm SHA256).Hash -ne $sourceHashes[$path]) {
        throw "Source changed during GPU validation: $path"
    }
}
$metadata = @{
    startedUtc = $started; completedUtc = [DateTime]::UtcNow.ToString('o')
    sourceHashes = $sourceHashes; sourcesUnchangedDuringBuildAndRun = $true
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    composeSha256 = (Get-FileHash -LiteralPath $compose -Algorithm SHA256).Hash
    querySha256 = (Get-FileHash -LiteralPath $query -Algorithm SHA256).Hash
}
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'metadata.json') -Encoding utf8
$result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
[pscustomobject]@{passed=$result.passed;adapter=$result.adapter;totals=$result.totals;updates=$result.updates.totals;diagnostics=$result.diagnostics;report=$report} | ConvertTo-Json -Depth 5
