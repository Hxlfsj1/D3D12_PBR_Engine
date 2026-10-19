param([switch]$BuildOnly,[switch]$SkipBuild,[string]$InputStem='tmp/vxgi-stage75-performance/threshold-realtime-live-probes-1707x1067-camera0_mode2',[string]$Report='tmp/VXGIRenderTail-performance-report.json',[ValidateRange(1,1024)][int]$Warmup=24,[ValidateRange(1,2048)][int]$Samples=72)
$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskRoot
try {
    New-Item -ItemType Directory -Path 'tmp','tmp/D3D12','tmp/render-tail-localappdata' -Force | Out-Null
    $taskExe='tmp/VXGIRenderTailPerformance.exe'
    if(-not $SkipBuild){
        $taskCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /WX /Od /Zi /MDd /D_DEBUG /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I ThirdParty\NVIDIA\DLSS\include /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /sourceDependencies tmp\VXGIRenderTailPerformance-deps.json /Fo:tmp\VXGIRenderTailPerformance.obj /Fd:tmp\VXGIRenderTailPerformance.pdb /Fe:tmp\VXGIRenderTailPerformance.exe Tests\VXGIRenderTailPerformance.cpp /link /LIBPATH:"C:\Users\28499\vcpkg\installed\x64-windows\debug\lib" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\Debug /LIBPATH:x64\Debug /LIBPATH:ThirdParty\NVIDIA\DLSS\lib\Windows_x86_64\x64 nvsdk_ngx_d_dbg.lib d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib advapi32.lib ole32.lib windowscodecs.lib DirectXTK12.lib assimp-vc145-mtd.lib meshoptimizer.lib'
        & cmd.exe /d /c $taskCommand
        if($LASTEXITCODE -ne 0){throw "Render tail performance build failed: $LASTEXITCODE"}
        $taskIncludes=(Get-Content -LiteralPath 'tmp/VXGIRenderTailPerformance-deps.json' -Raw -Encoding UTF8 | ConvertFrom-Json).Data.Includes
        $taskInputs=@('Tests/VXGIRenderTailPerformance.cpp','Tests/RunVXGIRenderTailPerformance.ps1','Settings/Pipeline.json')
        $taskInputs+=@($taskIncludes | Where-Object {$_.StartsWith($taskRoot,[StringComparison]::OrdinalIgnoreCase)} | ForEach-Object {$_.Substring($taskRoot.Length+1).Replace('\','/')})
        $taskInputs+=@(Get-ChildItem -LiteralPath 'Shaders' -File -Recurse | Where-Object {$_.Extension -in '.hlsl','.hlsli'} | ForEach-Object {$_.FullName.Substring($taskRoot.Length+1).Replace('\','/')})
        $taskInputs=@($taskInputs | Sort-Object -Unique)
        @($taskInputs | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256;[pscustomobject]@{path=$_;sha256=$taskHash.Hash}}) | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath 'tmp/VXGIRenderTailPerformance-build-inputs.json' -Encoding utf8
    }
    if($BuildOnly){return}
    $taskBefore=@(Get-Content -LiteralPath 'tmp/VXGIRenderTailPerformance-build-inputs.json' -Raw | ConvertFrom-Json)
    $taskCurrent=@($taskBefore | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_.path -Algorithm SHA256;[pscustomobject]@{path=$_.path;sha256=$taskHash.Hash}})
    if(@(Compare-Object $taskBefore $taskCurrent -Property path,sha256).Count){throw 'Render tail source changed since compilation; rebuild first.'}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp/dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp/dxil.dll' -Force
    Copy-Item -LiteralPath 'packages/Microsoft.Direct3D.D3D12.1.619.1/build/native/bin/x64/D3D12Core.dll' -Destination 'tmp/D3D12/D3D12Core.dll' -Force
    Copy-Item -LiteralPath 'ThirdParty/NVIDIA/DLSS/lib/Windows_x86_64/dev/nvngx_dlss.dll' -Destination 'tmp/nvngx_dlss.dll' -Force
    $taskPreviousPath=$env:PATH;$taskPreviousLocalAppData=$env:LOCALAPPDATA
    try {
        $env:PATH='C:\Users\28499\vcpkg\installed\x64-windows\debug\bin;'+$taskPreviousPath
        $env:LOCALAPPDATA=(Resolve-Path -LiteralPath 'tmp/render-tail-localappdata').Path
        & ('.\'+$taskExe) --input-stem $InputStem --report $Report --warmup $Warmup --samples $Samples
        if($LASTEXITCODE -ne 0){throw "Render tail performance failed: $LASTEXITCODE"}
    } finally {$env:PATH=$taskPreviousPath;$env:LOCALAPPDATA=$taskPreviousLocalAppData}
    $taskAfter=@($taskBefore | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_.path -Algorithm SHA256;[pscustomobject]@{path=$_.path;sha256=$taskHash.Hash}})
    if(@(Compare-Object $taskBefore $taskAfter -Property path,sha256).Count){throw 'Render tail inputs changed during run.'}
    $taskResult=Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
    $taskResult | Add-Member -NotePropertyName executable_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash -Force
    $taskResult | Add-Member -NotePropertyName input_hdr_sha256 -NotePropertyValue (Get-FileHash -LiteralPath ($InputStem+'.float4') -Algorithm SHA256).Hash -Force
    $taskResult | Add-Member -NotePropertyName input_depth_sha256 -NotePropertyValue (Get-FileHash -LiteralPath ($InputStem+'-depth.float32') -Algorithm SHA256).Hash -Force
    $taskResult | Add-Member -NotePropertyName source_hashes -NotePropertyValue $taskAfter -Force
    $taskResult | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Report -Encoding utf8
} finally {Pop-Location}

