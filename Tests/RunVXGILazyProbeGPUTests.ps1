param([switch]$DebugBuild,[switch]$BuildOnly,[switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskMode=if($DebugBuild){'Debug'}else{'O2'}
    $taskCompilerMode=if($DebugBuild){'/Od /Zi /MDd /D_DEBUG'}else{'/O2 /MD'}
    $taskInputs=@('Tests/VXGILazyProbeGPUTests.cpp','Tests/RunVXGILazyProbeGPUTests.ps1','Core/stdafx.h','Core/ErrorLog.h',
        'Resources/VXGIWorld.h','Resources/VXGISDF.h','Resources/VXGITraceAcceleration.h','Resources/VXGIMeshSDF.h','Resources/VXGIMeshSDFScene.h','Resources/VXGIMeshSDFGPUData.h',
        'Resources/VXGIGPUScene.h','Resources/VXGIInstanceBVH.h','Resources/VXGIProbeInvalidation.h','Resources/VXGIProbeCache.h','Resources/VXGIProbeState.h','Resources/VXGIProbeTopology.h','Resources/VXGISurface.h','Graphics/RenderDevice.h','Graphics/PBR_Shader.h','RDG/RDG.h',
        'Passes/VXGIProbePass.h','Passes/VXGIProbeFilterPass.h','Shaders/Shaders_For_VXGI_Probe_Update.hlsl','Shaders/Shaders_For_VXGI_Probe_Filter.hlsl',
        'Shaders/VXGISDFCommon.hlsli','Shaders/VXGISDFTraceBody.hlsli','Shaders/VXGIMeshSDFCommon.hlsli','Shaders/VXGIMeshSDFTraceBody.hlsli',
        'Shaders/VXGIProbeDependency.hlsli','Shaders/VXGIProbeCommon.hlsli','Shaders/VXGIProbeQueryCommon.hlsli','Shaders/VXGIProbeData.hlsli','Shaders/VXGIProbeRadianceCommon.hlsli',
        'Shaders/VXGISurfaceCommon.hlsli','Shaders/VXGIMaterialCommon.hlsli','Shaders/TangentBasis.hlsli','Shaders/GISourceCommon.hlsli')
    $taskBefore=@($taskInputs | ForEach-Object {[pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
    $taskStem='tmp/VXGILazyProbeGPU-'+$taskMode
    if(-not $SkipBuild){
        $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskCompilerMode+' /I Core /I Graphics /I RDG /I Resources /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:'+ $taskStem+'.obj /Fd:'+ $taskStem+'.pdb /Fe:'+ $taskStem+'.exe Tests\VXGILazyProbeGPUTests.cpp d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib'
        & cmd.exe /d /c $taskBuildCommand
        if($LASTEXITCODE -ne 0){throw "Lazy probe GPU acceptance build failed: $LASTEXITCODE"}
        $taskBefore | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath ($taskStem+'-build-hashes.json') -Encoding utf8
    }else{
        $taskBuilt=Get-Content -LiteralPath ($taskStem+'-build-hashes.json') -Raw | ConvertFrom-Json
        if(@(Compare-Object $taskBuilt $taskBefore -Property path,sha256).Count){throw 'Lazy probe compiled inputs changed; rebuild first.'}
    }
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    & ('.\'+$taskStem+'.exe')
    if($LASTEXITCODE -ne 0){throw "Lazy probe GPU acceptance failed: $LASTEXITCODE"}
    $taskAfter=@($taskInputs | ForEach-Object {[pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}})
    if(@(Compare-Object $taskBefore $taskAfter -Property path,sha256).Count){throw 'Lazy probe GPU inputs changed during run.'}
    Copy-Item -LiteralPath 'tmp/VXGILazyProbeGPU-report.json' -Destination ($taskStem+'-report.json') -Force
    [ordered]@{source_inputs_stable=$true;source_inputs=$taskAfter} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($taskStem+'-input-hashes.json') -Encoding utf8
}
finally{Pop-Location}
