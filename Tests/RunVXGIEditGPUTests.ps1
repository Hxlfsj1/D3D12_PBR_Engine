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
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskCompilerMode+' /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIEditGPUTests.obj /Fd:tmp\VXGIEditGPUTests.pdb /Fe:tmp\VXGIEditGPUTests.exe Tests\VXGIEditGPUTests.cpp /link /LIBPATH:"'+$taskVcpkgLib+'" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\'+$taskMode+' /LIBPATH:x64\'+$taskMode+' d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib ole32.lib windowscodecs.lib DirectXTK12.lib '+$taskAssimp+' meshoptimizer.lib'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Material GPU acceptance build failed: $LASTEXITCODE"}
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    $taskPreviousPath=$env:PATH
    try {
        $env:PATH=$taskVcpkgBin+';'+$taskPreviousPath
        & '.\tmp\VXGIEditGPUTests.exe'
        if($LASTEXITCODE -ne 0){throw "Material GPU acceptance failed: $LASTEXITCODE"}
        $taskReportMode=if($DebugBuild){'Debug'}else{'O2'}
        Copy-Item -LiteralPath 'tmp\VXGIEditGPU-report.json' -Destination ('tmp\VXGIEditGPU-'+$taskReportMode+'-report.json') -Force
    }
    finally {$env:PATH=$taskPreviousPath}
}
finally {Pop-Location}
