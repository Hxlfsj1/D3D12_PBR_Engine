param([switch]$BuildOnly,[switch]$SkipBuild,[ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$Label='')
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskVcpkgLib='C:\Users\28499\vcpkg\installed\x64-windows\lib'
    $taskVcpkgBin='C:\Users\28499\vcpkg\installed\x64-windows\bin'
    $taskStem='tmp/VXGIStage74Benchmark'+$(if($Label){'-'+$Label}else{''})
    $taskInputPaths=@('Tests/VXGIStage7Benchmark.cpp','Tests/RunVXGIStage7Benchmark.ps1','Settings/Scene.json','Settings/Pipeline.json','Settings/Lighting.json','Models/CyberPunk_Car.glb','Models/Plain.glb','Passes/VXGIProbePass.h','Passes/VXGIProbeFilterPass.h','Resources/VXGIGPUScene.h','Resources/VXGIWorld.h','Resources/VXGISDF.h','Resources/VXGITraceAcceleration.h','Resources/VXGIProbeTopology.h','Resources/VXGIProbeState.h','Resources/VXGISurface.h','Resources/ResourceManager.h','Resources/PBR_Model.h','Resources/Settings_Manager.h','Graphics/PBR_Shader.h','Graphics/RenderDevice.h','Shaders/Shaders_For_VXGI_Probe_Update.hlsl','Shaders/Shaders_For_VXGI_Probe_Filter.hlsl','Shaders/VXGIProbeQueryCommon.hlsli','Shaders/VXGISDFCommon.hlsli','Shaders/VXGISDFTraceBody.hlsli','Shaders/VXGIMaterialCommon.hlsli','Shaders/VXGISurfaceCommon.hlsli','Shaders/VXGIProbeCommon.hlsli','Shaders/GISourceCommon.hlsli','Shaders/MaterialCommon.hlsli','Shaders/TangentBasis.hlsli','Shaders/VXGIStage74SupportValidate.hlsl')
    if(-not $SkipBuild){
        $taskBaseInputPaths=$taskInputPaths
        $taskInputPaths+=@(Get-ChildItem -LiteralPath 'Core','Graphics','Logic','Passes','RDG','Resources','ThirdParty','packages' -File -Recurse | Where-Object {$_.Extension -in '.h','.hpp','.inl'} | ForEach-Object {$_.FullName.Substring($taskProjectRoot.Length+1).Replace('\','/')})
        $taskInputPaths=@($taskInputPaths | Sort-Object -Unique)
        $taskBeforeBuild=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
        $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /O2 /MD /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /sourceDependencies tmp\VXGIStage7Benchmark-deps.json /Fo:tmp\VXGIStage7Benchmark.obj /Fe:tmp\VXGIStage7Benchmark.exe Tests\VXGIStage7Benchmark.cpp /link /LIBPATH:"'+$taskVcpkgLib+'" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\Release /LIBPATH:x64\Release d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib ole32.lib windowscodecs.lib DirectXTK12.lib assimp-vc145-mt.lib meshoptimizer.lib'
        & cmd.exe /d /c $taskBuildCommand
        if($LASTEXITCODE -ne 0){throw "Stage 7 benchmark build failed: $LASTEXITCODE"}
        $taskDependencies=(Get-Content -LiteralPath 'tmp/VXGIStage7Benchmark-deps.json' -Raw | ConvertFrom-Json).Data.Includes
        $taskInputPaths=@(($taskBaseInputPaths+@($taskDependencies | Where-Object {$_.StartsWith($taskProjectRoot,[StringComparison]::OrdinalIgnoreCase)} | ForEach-Object {$_.Substring($taskProjectRoot.Length+1).Replace('\','/')})) | Sort-Object -Unique)
        $taskBeforeBuild=@($taskBeforeBuild | Where-Object {$_.path -in $taskInputPaths})
        $taskManifest=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
        if(@(Compare-Object $taskBeforeBuild $taskManifest -Property path,sha256).Count -ne 0){throw 'Benchmark inputs changed during compilation; rebuild first.'}
        $taskManifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath 'tmp/VXGIStage7Benchmark-build-input-hashes.json' -Encoding utf8
    }else{
        $taskBuilt=@(Get-Content -LiteralPath 'tmp/VXGIStage7Benchmark-build-input-hashes.json' -Raw | ConvertFrom-Json)
        $taskInputPaths=@($taskBuilt | ForEach-Object {$_.path})
        $taskManifest=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
        if(@(Compare-Object $taskBuilt $taskManifest -Property path,sha256).Count -ne 0){throw 'Compiled benchmark inputs differ; rebuild first.'}
    }
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    $taskArguments=@();if($Label){$taskArguments=@('--label',$Label)}
    $taskPreviousPath=$env:PATH
    try {$env:PATH=$taskVcpkgBin+';'+$taskPreviousPath; & '.\tmp\VXGIStage7Benchmark.exe' @taskArguments; if($LASTEXITCODE -ne 0){throw "Stage 7 benchmark failed: $LASTEXITCODE"}}
    finally {$env:PATH=$taskPreviousPath}
    $taskAfterManifest=$taskInputPaths | ForEach-Object { $taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash} }
    if(Compare-Object -ReferenceObject $taskManifest -DifferenceObject $taskAfterManifest -Property path,sha256){throw 'Benchmark inputs changed during the run; report is not source-stable.'}
    $taskManifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath ($taskStem+'-input-hashes.json') -Encoding utf8
}
finally {Pop-Location}
