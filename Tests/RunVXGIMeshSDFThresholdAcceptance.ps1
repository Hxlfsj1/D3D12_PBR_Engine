param([switch]$BuildOnly,[switch]$SkipBuild,[switch]$DebugBuild,[switch]$CPUOnly,[ValidateRange(32,131072)][int]$Rays=8192)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskSuffix=if($DebugBuild){'Debug'}else{'O2'}
    $taskMode=if($DebugBuild){'/Od /Zi /MDd /D_DEBUG'}else{'/O2 /MD'}
    $taskLibraryRoot=if($DebugBuild){'C:\Users\28499\vcpkg\installed\x64-windows\debug'}else{'C:\Users\28499\vcpkg\installed\x64-windows'}
    $taskAssimp=if($DebugBuild){'assimp-vc145-mtd.lib'}else{'assimp-vc145-mt.lib'}
    $taskBuild='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskMode+' /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIMeshSDFThresholdAcceptance'+$taskSuffix+'.obj /Fd:tmp\VXGIMeshSDFThresholdAcceptance'+$taskSuffix+'.pdb /Fe:tmp\VXGIMeshSDFThresholdAcceptance'+$taskSuffix+'.exe Tests\VXGIMeshSDFThresholdAcceptance.cpp /link /LIBPATH:"'+$taskLibraryRoot+'\lib" d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib '+$taskAssimp
    if(-not $SkipBuild){& cmd.exe /d /c $taskBuild;if($LASTEXITCODE -ne 0){throw "Threshold actual-scene acceptance build failed: $LASTEXITCODE"}}
    if($BuildOnly){return}
    # CPU-only mode does not create a D3D12 device or copy SDK runtime DLLs.
    if(-not $CPUOnly){
        Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
        Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
        Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
        Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    }
    $taskPreviousPath=$env:PATH
    try {
        $env:PATH=$taskLibraryRoot+'\bin;'+$env:PATH
        $taskArguments=@('--rays',"$Rays");if($CPUOnly){$taskArguments+='--cpu-only'}
        & ('.\tmp\VXGIMeshSDFThresholdAcceptance'+$taskSuffix+'.exe') @taskArguments
        if($LASTEXITCODE -ne 0){throw "Threshold actual-scene acceptance failed: $LASTEXITCODE"}
    }
    finally {$env:PATH=$taskPreviousPath}
    $taskBackend=if($CPUOnly){'CPU'}else{'GPU'}
    Copy-Item -LiteralPath ('tmp\VXGIMeshSDFThresholdAcceptance-'+$taskBackend+'-report.json') -Destination ('tmp\VXGIMeshSDFThresholdAcceptance-'+$taskBackend+'-'+$taskSuffix+'-report.json') -Force
}
finally {Pop-Location}
