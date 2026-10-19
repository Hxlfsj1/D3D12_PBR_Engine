param([switch]$DebugBuild,[switch]$BuildOnly)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskMode=if($DebugBuild){'Debug'}else{'Release'}
    $taskCompilerMode=if($DebugBuild){'/Od /Zi /MDd'}else{'/O2 /MD'}
    $taskVcpkgLib=if($DebugBuild){'C:\Users\28499\vcpkg\installed\x64-windows\debug\lib'}else{'C:\Users\28499\vcpkg\installed\x64-windows\lib'}
    $taskVcpkgBin=if($DebugBuild){'C:\Users\28499\vcpkg\installed\x64-windows\debug\bin'}else{'C:\Users\28499\vcpkg\installed\x64-windows\bin'}
    $taskAssimp=if($DebugBuild){'assimp-vc145-mtd.lib'}else{'assimp-vc145-mt.lib'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskCompilerMode+' /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIHybridGPUTests.obj /Fd:tmp\VXGIHybridGPUTests.pdb /Fe:tmp\VXGIHybridGPUTests.exe Tests\VXGIHybridGPUTests.cpp /link /LIBPATH:"'+$taskVcpkgLib+'" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\'+$taskMode+' /LIBPATH:x64\'+$taskMode+' d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib ole32.lib windowscodecs.lib DirectXTK12.lib '+$taskAssimp+' meshoptimizer.lib'
    $taskBuildCommand=$taskBuildCommand.Replace('/Fo:','/sourceDependencies tmp\VXGIHybrid-stage77-dependencies.json /Fo:')
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Hybrid GPU acceptance build failed: $LASTEXITCODE"}
    if($BuildOnly){Write-Output 'Hybrid GPU acceptance build passed; GPU execution skipped.'; return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    $taskInputPaths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($taskInput in @('Tests/VXGIHybridGPUTests.cpp','Tests/VXGIHybridRDGAcceptance.h','Tests/RunVXGIHybridGPUTests.ps1')) {
        [void]$taskInputPaths.Add((Resolve-Path -LiteralPath $taskInput).Path)
    }
    $taskDependencies=Get-Content -LiteralPath 'tmp/VXGIHybrid-stage77-dependencies.json' -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach($taskInclude in $taskDependencies.Data.Includes) {
        if($taskInclude.StartsWith($taskProjectRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { [void]$taskInputPaths.Add($taskInclude) }
    }
    $taskShaderQueue=[Collections.Generic.Queue[string]]::new()
    foreach($taskShader in @('Shaders/VXGIHybridValidate.hlsl','Shaders/Shaders_For_SSGI.hlsl','Shaders/Shaders_For_SSGI_Temporal.hlsl','Shaders/Shaders_For_SSGI_Spatial.hlsl')) {
        $taskShaderQueue.Enqueue((Resolve-Path -LiteralPath $taskShader).Path)
    }
    while($taskShaderQueue.Count) {
        $taskShader=$taskShaderQueue.Dequeue()
        if(-not $taskInputPaths.Add($taskShader)){continue}
        foreach($taskMatch in [regex]::Matches((Get-Content -LiteralPath $taskShader -Raw),'(?m)^\s*#include\s+"([^"]+)"')) {
            $taskShaderQueue.Enqueue((Resolve-Path -LiteralPath (Join-Path (Split-Path $taskShader -Parent) $taskMatch.Groups[1].Value)).Path)
        }
    }
    function Get-TaskHybridInputHashes {
        @($taskInputPaths | Sort-Object | ForEach-Object { [pscustomobject]@{path=$_.Substring($taskProjectRoot.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash} })
    }
    $taskReportMode=if($DebugBuild){'Debug'}else{'O2'}
    $taskBeforeHashes=Get-TaskHybridInputHashes
    $taskBeforeHashes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath ('tmp/VXGIHybridGPU-stage77-'+$taskReportMode+'-inputs-before.json') -Encoding UTF8
    $taskPreviousPath=$env:PATH
    try {
        $env:PATH=$taskVcpkgBin+';'+$taskPreviousPath
        & '.\tmp\VXGIHybridGPUTests.exe'
        if($LASTEXITCODE -ne 0){throw "Hybrid GPU acceptance failed: $LASTEXITCODE"}
        $taskAfterHashes=Get-TaskHybridInputHashes
        $taskAfterHashes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath ('tmp/VXGIHybridGPU-stage77-'+$taskReportMode+'-inputs-after.json') -Encoding UTF8
        if(($taskBeforeHashes | ConvertTo-Json -Compress) -cne ($taskAfterHashes | ConvertTo-Json -Compress)){throw 'Hybrid acceptance sources changed during execution.'}
        Copy-Item -LiteralPath 'tmp\VXGIHybridGPU-stage77-report.json' -Destination ('tmp\VXGIHybridGPU-stage77-'+$taskReportMode+'-report.json') -Force
    }
    finally {$env:PATH=$taskPreviousPath}
}
finally {Pop-Location}
