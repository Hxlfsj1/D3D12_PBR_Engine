param([switch]$DebugBuild)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = (Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskCompilerMode = if ($DebugBuild) { '/Od /Zi /MDd' } else { '/O2 /MD' }
    $taskSuffix = if ($DebugBuild) { 'Debug' } else { 'O2' }
    $taskBuildCommand = 'call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX ' + $taskCompilerMode +
        ' /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIMeshSDFGPUValidation' + $taskSuffix +
        '.obj /Fd:tmp\VXGIMeshSDFGPUValidation' + $taskSuffix + '.pdb /Fe:tmp\VXGIMeshSDFGPUValidation' + $taskSuffix +
        '.exe Tests\VXGIMeshSDFGPUValidation.cpp d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib'
    & cmd.exe /d /c $taskBuildCommand
    if ($LASTEXITCODE -ne 0) { throw "Mesh SDF GPU validation build failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    & ('.\tmp\VXGIMeshSDFGPUValidation' + $taskSuffix + '.exe')
    if ($LASTEXITCODE -ne 0) { throw "Mesh SDF GPU validation failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath 'tmp\VXGIMeshSDFGPUValidation-report.json' -Destination ('tmp\VXGIMeshSDFGPUValidation-' + $taskSuffix + '-report.json') -Force
}
finally { Pop-Location }
