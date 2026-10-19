param([switch]$DebugBuild,[switch]$BuildOnly,[string]$RunLabel='')
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskCompilerMode=if($DebugBuild){'/Od /Zi /MDd'}else{'/O2 /MD'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskCompilerMode+' /I Core /I Graphics /I RDG /I Resources /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIProbeQueryGPUTests.obj /Fd:tmp\VXGIProbeQueryGPUTests.pdb /Fe:tmp\VXGIProbeQueryGPUTests.exe Tests\VXGIProbeQueryGPUTests.cpp d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Probe query GPU acceptance build failed: $LASTEXITCODE"}
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    $taskInputs=@('Tests/VXGIProbeQueryGPUTests.cpp','Tests/RunVXGIProbeQueryGPUTests.ps1',
        'Shaders/VXGIProbeQueryValidate.hlsl','Shaders/VXGIProbeQueryCommon.hlsli','Shaders/VXGIProbeCommon.hlsli',
        'Shaders/VXGISurfaceCommon.hlsli','Shaders/VXGIMaterialCommon.hlsli','Shaders/TangentBasis.hlsli',
        'Shaders/VXGISDFCommon.hlsli','Shaders/VXGISDFTraceBody.hlsli','Shaders/Shaders_For_VXGI_Probe_Filter.hlsl',
        'Shaders/Shaders_For_VXGI_Probe_Debug.hlsl','Shaders/Shaders_For_VXGI_SDF_Debug.hlsl',
        'Passes/VXGIProbeFilterPass.h','Resources/VXGIWorld.h','Resources/VXGISDF.h',
        'Resources/VXGITraceAcceleration.h','Resources/VXGISurface.h','Resources/VXGIProbeState.h',
        'Resources/VXGIProbeTopology.h','Resources/VXGIGPUScene.h','Graphics/PBR_Shader.h',
        'Graphics/RenderDevice.h','RDG/RDG.h','Editor/VXGIProbeDebugRenderer.h','Editor/VXGISDFDebugRenderer.h')
    $taskHashesBefore=@{};foreach($taskInput in $taskInputs){$taskHashesBefore[$taskInput]=(Get-FileHash -LiteralPath $taskInput -Algorithm SHA256).Hash}
    & '.\tmp\VXGIProbeQueryGPUTests.exe'
    if($LASTEXITCODE -ne 0){throw "Probe query GPU acceptance failed: $LASTEXITCODE"}
    $taskHashEntries=@();foreach($taskInput in $taskInputs){
        $taskAfter=(Get-FileHash -LiteralPath $taskInput -Algorithm SHA256).Hash
        if($taskAfter -ne $taskHashesBefore[$taskInput]){throw "Source changed during probe query GPU validation: $taskInput"}
        $taskHashEntries+=[ordered]@{path=$taskInput;sha256=$taskAfter}
    }
    if($RunLabel){
        if($RunLabel -notmatch '^[a-zA-Z0-9_-]+$'){throw 'RunLabel must contain only letters, digits, underscores, or hyphens'}
        $taskBuildName=if($DebugBuild){'Debug'}else{'O2'}
        $taskArchive="tmp/VXGIProbeQueryGPU-$RunLabel-$taskBuildName"
        Copy-Item -LiteralPath 'tmp/VXGIProbeQueryGPU-report.json' -Destination "$taskArchive-report.json" -Force
        [ordered]@{source_inputs_stable=$true;source_inputs=$taskHashEntries}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath "$taskArchive-input-hashes.json" -Encoding utf8
    }
}
finally{Pop-Location}
