param([switch]$DebugBuild,[switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = (Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskCompilerMode = if ($DebugBuild) { '/Od /Zi /MDd' } else { '/O2 /MD' }
    $taskSuffix = if ($DebugBuild) { 'Debug' } else { 'O2' }
    $taskBuildCommand = 'call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX ' + $taskCompilerMode +
        ' /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIMeshSDFThresholdGPUValidation' + $taskSuffix +
        '.obj /Fd:tmp\VXGIMeshSDFThresholdGPUValidation' + $taskSuffix + '.pdb /Fe:tmp\VXGIMeshSDFThresholdGPUValidation' + $taskSuffix +
        '.exe Tests\VXGIMeshSDFThresholdGPUValidation.cpp d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib'
    & cmd.exe /d /c $taskBuildCommand
    if ($LASTEXITCODE -ne 0) { throw "SDF threshold GPU validation build failed: $LASTEXITCODE" }
    if ($BuildOnly) { Write-Output 'Integrated threshold GPU validation build passed; GPU execution skipped.'; return }
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'x64\Debug\D3D12\D3D12SDKLayers.dll' -Destination 'tmp\D3D12\D3D12SDKLayers.dll' -Force
    $taskRunOutput = & ('.\tmp\VXGIMeshSDFThresholdGPUValidation' + $taskSuffix + '.exe') 2>&1
    $taskRunOutput | ForEach-Object { Write-Output $_ }
    $taskRunOutput | Set-Content -LiteralPath ('tmp/VXGIMeshSDFThresholdGPU-' + $taskSuffix + '.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw "SDF threshold GPU validation failed: $LASTEXITCODE" }
}
finally { Pop-Location }
