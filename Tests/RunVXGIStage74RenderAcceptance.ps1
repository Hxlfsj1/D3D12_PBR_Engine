param([switch]$BuildOnly)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskInputPaths=@('Tests/VXGIStage74RenderAcceptance.cpp','Tests/RunVXGIStage74RenderAcceptance.ps1','Shaders/VXGIStage74RasterValidate.hlsl','Settings/Scene.json','Settings/Pipeline.json','Settings/Lighting.json','Models/CyberPunk_Car.glb','Models/Plain.glb','HDRs/citrus_orchard_road_puresky_4k.hdr')
    $taskInputPaths+=@(Get-ChildItem -LiteralPath 'Shaders','Passes','Resources','Graphics','Logic','RDG','Core' -File -Recurse | Where-Object {$_.Extension -in '.h','.hlsl','.hlsli'} | ForEach-Object {[IO.Path]::GetRelativePath($taskProjectRoot,$_.FullName).Replace('\','/')})
    $taskInputPaths=$taskInputPaths | Sort-Object -Unique
    $taskBefore=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
    $taskVcpkgLib='C:\Users\28499\vcpkg\installed\x64-windows\lib'
    $taskVcpkgBin='C:\Users\28499\vcpkg\installed\x64-windows\bin'
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /O2 /MD /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /Fo:tmp\VXGIStage74RenderAcceptance.obj /Fe:tmp\VXGIStage74RenderAcceptance.exe Tests\VXGIStage74RenderAcceptance.cpp /link /LIBPATH:"'+$taskVcpkgLib+'" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\Release /LIBPATH:x64\Release d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib ole32.lib windowscodecs.lib DirectXTK12.lib assimp-vc145-mt.lib meshoptimizer.lib'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Stage 7.4 render acceptance build failed: $LASTEXITCODE"}
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    $taskPreviousPath=$env:PATH
    try {$env:PATH=$taskVcpkgBin+';'+$taskPreviousPath; & '.\tmp\VXGIStage74RenderAcceptance.exe'; if($LASTEXITCODE -ne 0){throw "Stage 7.4 render acceptance failed: $LASTEXITCODE"}}
    finally {$env:PATH=$taskPreviousPath}
    $taskAfter=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
    if((Compare-Object $taskBefore $taskAfter -Property path,sha256).Count -ne 0){throw 'Render acceptance inputs changed during build/run; report is stale.'}
    $taskAfter | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath 'tmp/VXGIStage74RenderAcceptance-input-hashes.json' -Encoding utf8
}
finally {Pop-Location}

