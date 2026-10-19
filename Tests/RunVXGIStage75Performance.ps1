param([switch]$BuildOnly,[switch]$SkipBuild,[int]$Width=640,[int]$Height=400,[string]$Label='baseline',[ValidateRange(1,2048)][int]$Samples=3,[int]$Camera=-1,[switch]$AllCameras,[int]$Mode=99,[string]$ReuseRaw='',[string]$CameraEye='',[string]$CameraTarget='',[string]$CavityRays='',[ValidateSet(2,4,8)][int]$DownsampleFactor=2,[ValidateRange(1,12)][int]$Repeat=1,[switch]$GpuValidation,[switch]$DebugShaderPolicy,[switch]$GIOnly,[switch]$RayRoutingStats,[ValidateRange(-1,2)][int]$ProbePaddingLayers=-1,[ValidateRange(1,32)][int]$WarmupRounds=2,[ValidateRange(0,65535)][int]$WarmupBatches=0,[switch]$VerifyInstanceUpdates,[switch]$ColdStart,[switch]$NativeDebug,[switch]$ExactFallback,[switch]$LazyProbes,[switch]$LazySettle,[ValidateRange(1,2048)][int]$SteadySamples=72,[ValidateRange(1,65535)][int]$SettleMaxBatches=16384,[switch]$CompareWorldModes,[switch]$StartupToggle,[switch]$InitializeProbeCache,[ValidateRange(1,8)][int]$ABRounds=2,[ValidateRange(1,2048)][int]$ABWarmup=24)
$ErrorActionPreference='Stop'
if($LazyProbes -and -not $ColdStart){throw '-LazyProbes requires -ColdStart.'}
if($LazySettle -and -not $LazyProbes){throw '-LazySettle requires -LazyProbes.'}
if($StartupToggle -and (-not $ColdStart -or -not $LazyProbes -or $LazySettle -or $CompareWorldModes -or $GIOnly -or $ExactFallback -or $Samples -lt 16 -or $ReuseRaw -or $Repeat -ne 1 -or $Mode -notin @(99,2) -or $AllCameras)){throw '-StartupToggle requires -ColdStart -LazyProbes, mode 2 and at least 16 samples; no settle/world A-B/GI-only/exact/import/repeat options.'}
if($InitializeProbeCache -and -not $StartupToggle){throw '-InitializeProbeCache requires -StartupToggle (with -ColdStart -LazyProbes and at least 16 samples).'}
if($CompareWorldModes -and ($ColdStart -or $LazyProbes -or $LazySettle -or $GIOnly -or $ExactFallback -or -not $ReuseRaw -or $Repeat -ne 1 -or $Mode -ne 99 -or $AllCameras)){throw '-CompareWorldModes requires -ReuseRaw and a single camera; cannot combine cold/lazy/GI-only/exact/repeat/mode options.'}
if($NativeDebug -and -not $DebugShaderPolicy){throw '-NativeDebug requires -DebugShaderPolicy because _DEBUG selects the production Debug shader policy.'}
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    New-Item -ItemType Directory -Path 'tmp\D3D12' -Force | Out-Null
    $taskInputPaths=@('Tests/VXGIStage75Performance.cpp','Tests/RunVXGIStage75Performance.ps1','Tests/VXGICavityCapture.inl','Tests/VXGIRayRoutingCapture.inl','Tests/SSGIRayRoutingDiagnostics.hlsl','Settings/Scene.json','Settings/Pipeline.json','Settings/Lighting.json','Models/CyberPunk_Car.glb','Models/Plain.glb','HDRs/citrus_orchard_road_puresky_4k.hdr')
    $taskInputPaths+=@(Get-ChildItem -LiteralPath 'Shaders','Passes','Resources','Graphics','Logic','RDG','Core','ThirdParty','packages' -File -Recurse | Where-Object {$_.Extension -in '.h','.hpp','.inl','.hlsl','.hlsli'} | ForEach-Object {$_.FullName.Substring($taskProjectRoot.Length+1).Replace('\','/')})
    $taskInputPaths=$taskInputPaths | Sort-Object -Unique
    $taskBefore=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
    $taskMode=if($NativeDebug){'Debug'}else{'Release'}
    $taskCompilerMode=if($NativeDebug){'/Od /Zi /MDd'}else{'/O2 /MD'}
    $taskNativeConfiguration=if($NativeDebug){'Od_Zi_MDd'}else{'O2_MD'}
    $taskVcpkgLib=if($NativeDebug){'C:\Users\28499\vcpkg\installed\x64-windows\debug\lib'}else{'C:\Users\28499\vcpkg\installed\x64-windows\lib'}
    $taskVcpkgBin=if($NativeDebug){'C:\Users\28499\vcpkg\installed\x64-windows\debug\bin'}else{'C:\Users\28499\vcpkg\installed\x64-windows\bin'}
    $taskAssimp=if($NativeDebug){'assimp-vc145-mtd.lib'}else{'assimp-vc145-mt.lib'}
    # DEBUG alone keeps the release CRT; NativeDebug selects the actual Debug
    # CRT/standard-library ABI and matching Debug native dependencies as well.
    $taskShaderPolicyDefine=if($NativeDebug){' /D_DEBUG'}elseif($DebugShaderPolicy){' /DDEBUG'}else{''}
    $taskFallbackDefine=if($ExactFallback){' /DVXGI_ACCEPTANCE_EXACT_FALLBACK'}else{''}
    # Separate reference binaries prevent SkipBuild from accidentally selecting
    # the other fallback, and let A/B runs reuse their own verified provenance.
    $taskBinaryStem=if($ExactFallback){'VXGIStage75PerformanceExact'}else{'VXGIStage75Performance'}
    $taskDependencyPath='tmp/'+$taskBinaryStem+'-deps.json'
    $taskBuildHashesPath='tmp/'+$taskBinaryStem+'-build-input-hashes.json'
    $taskBuildPolicyPath='tmp/'+$taskBinaryStem+'-build-policy.json'
    $taskExecutablePath='.\tmp\'+$taskBinaryStem+'.exe'
    $taskBuildPolicy=[pscustomobject]@{debug_shader_policy=[bool]$DebugShaderPolicy;native_debug=[bool]$NativeDebug;native_configuration=$taskNativeConfiguration;shader_policy_define=$taskShaderPolicyDefine.Trim();compiler_mode=$taskCompilerMode;exact_fallback=[bool]$ExactFallback;fallback_define=$taskFallbackDefine.Trim()}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /bigobj /std:c++20 /utf-8 /EHsc /W4 /WX '+$taskCompilerMode+$taskShaderPolicyDefine+$taskFallbackDefine+' /I Core /I Graphics /I Logic /I Passes /I RDG /I Resources /external:I ThirdParty /external:I "C:\Users\28499\vcpkg\installed\x64-windows\include" /external:I packages\directxtk12_desktop_win10.2026.4.1.1\include /external:W0 /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include /I packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12 /sourceDependencies tmp\'+$taskBinaryStem+'-deps.json /Fo:tmp\'+$taskBinaryStem+'.obj /Fd:tmp\'+$taskBinaryStem+'.pdb /Fe:tmp\'+$taskBinaryStem+'.exe Tests\VXGIStage75Performance.cpp /link /LIBPATH:"'+$taskVcpkgLib+'" /LIBPATH:packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\'+$taskMode+' /LIBPATH:x64\'+$taskMode+' d3d12.lib dxgi.lib dxguid.lib dxcompiler.lib user32.lib ole32.lib windowscodecs.lib DirectXTK12.lib '+$taskAssimp+' meshoptimizer.lib'
    if(-not $SkipBuild){
        & cmd.exe /d /c $taskBuildCommand
        if($LASTEXITCODE -ne 0){throw "Stage 7.5 performance build failed: $LASTEXITCODE"}
        $taskDependencies=(Get-Content -LiteralPath $taskDependencyPath -Raw -Encoding UTF8 | ConvertFrom-Json).Data.Includes
        $taskUsedPaths=@('Tests/VXGIStage75Performance.cpp','Tests/RunVXGIStage75Performance.ps1','Tests/VXGICavityCapture.inl','Tests/VXGIRayRoutingCapture.inl','Tests/SSGIRayRoutingDiagnostics.hlsl','Settings/Scene.json','Settings/Pipeline.json','Settings/Lighting.json','Models/CyberPunk_Car.glb','Models/Plain.glb','HDRs/citrus_orchard_road_puresky_4k.hdr')
        $taskUsedPaths+=@($taskDependencies | Where-Object {$_.StartsWith($taskProjectRoot,[StringComparison]::OrdinalIgnoreCase)} | ForEach-Object {$_.Substring($taskProjectRoot.Length+1).Replace('\','/')})
        $taskShaderQueue=[Collections.Generic.Queue[string]]::new()
        foreach($taskNativePath in $taskUsedPaths){
            if([IO.Path]::GetExtension($taskNativePath) -in '.h','.hpp','.inl','.cpp'){
                $taskNativeText=Get-Content -LiteralPath $taskNativePath -Raw
                foreach($taskMatch in [regex]::Matches($taskNativeText,'Shaders[/\\][^"\r\n]+\.hlsl')){$taskShaderQueue.Enqueue($taskMatch.Value.Replace('\','/'))}
            }
        }
        $taskShaderUsed=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        while($taskShaderQueue.Count){
            $taskShaderPath=$taskShaderQueue.Dequeue()
            if(-not $taskShaderUsed.Add($taskShaderPath)){continue}
            $taskShaderText=Get-Content -LiteralPath $taskShaderPath -Raw
            foreach($taskMatch in [regex]::Matches($taskShaderText,'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]')){
                $taskInclude=Join-Path (Split-Path $taskShaderPath -Parent) $taskMatch.Groups[1].Value
                if(-not (Test-Path -LiteralPath $taskInclude)){$taskInclude=Join-Path 'Shaders' $taskMatch.Groups[1].Value}
                $taskShaderQueue.Enqueue((Resolve-Path -LiteralPath $taskInclude).Path.Substring($taskProjectRoot.Length+1).Replace('\','/'))
            }
        }
        $taskUsedPaths+=@($taskShaderUsed)
        $taskInputPaths=@($taskUsedPaths | Sort-Object -Unique)
        $taskBefore=@($taskBefore | Where-Object {$_.path -in $taskInputPaths})
        $taskBuildAfter=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
        $taskBuildDifference=@(Compare-Object $taskBefore $taskBuildAfter -Property path,sha256)
        if($taskBuildDifference.Count -ne 0){$taskBuildDifference | Format-Table -AutoSize;throw 'Performance inputs changed during compilation.'}
        $taskBuildPolicy | Add-Member -NotePropertyName executable_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $taskExecutablePath -Algorithm SHA256).Hash -Force
        $taskBuildAfter | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $taskBuildHashesPath -Encoding utf8
        $taskBuildPolicy | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $taskBuildPolicyPath -Encoding utf8
    }else{
        $taskBuiltPolicy=Get-Content -LiteralPath $taskBuildPolicyPath -Raw | ConvertFrom-Json
        if($null -eq $taskBuiltPolicy.exact_fallback -or $taskBuiltPolicy.exact_fallback -ne [bool]$ExactFallback -or $taskBuiltPolicy.fallback_define -ne $taskFallbackDefine.Trim()){throw 'Compiled harness fallback policy differs; rebuild with the requested -ExactFallback choice.'}
        if($taskBuiltPolicy.debug_shader_policy -ne [bool]$DebugShaderPolicy){throw 'Compiled harness shader policy differs; rebuild with the requested -DebugShaderPolicy.'}
        if($taskBuiltPolicy.native_debug -ne [bool]$NativeDebug -or $taskBuiltPolicy.native_configuration -ne $taskNativeConfiguration){throw 'Compiled harness native configuration differs; rebuild with the requested -NativeDebug choice.'}
        $taskExecutableHash=(Get-FileHash -LiteralPath $taskExecutablePath -Algorithm SHA256).Hash
        if(-not $taskBuiltPolicy.executable_sha256 -or $taskBuiltPolicy.executable_sha256 -ne $taskExecutableHash){throw 'Compiled harness binary hash differs; rebuild first.'}
        $taskBuildPolicy | Add-Member -NotePropertyName executable_sha256 -NotePropertyValue $taskExecutableHash -Force
        $taskBuilt=@(Get-Content -LiteralPath $taskBuildHashesPath -Raw | ConvertFrom-Json)
        $taskInputPaths=@($taskBuilt | ForEach-Object {$_.path})
        $taskBefore=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
        if((Compare-Object $taskBuilt $taskBefore -Property path,sha256).Count -ne 0){throw 'Compiled harness inputs differ; rebuild first.'}
    }
    if($BuildOnly){return}
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxcompiler.dll' -Destination 'tmp\dxcompiler.dll' -Force
    Copy-Item -LiteralPath 'D:\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll' -Destination 'tmp\dxil.dll' -Force
    Copy-Item -LiteralPath 'packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\bin\x64\D3D12Core.dll' -Destination 'tmp\D3D12\D3D12Core.dll' -Force
    $taskArgs=@('--width',[string]$Width,'--height',[string]$Height,'--label',$Label,'--samples',[string]$Samples,'--downsample-factor',[string]$DownsampleFactor,'--repeat',[string]$Repeat,'--warmup-rounds',[string]$WarmupRounds,'--warmup-batches',[string]$WarmupBatches)
    if($VerifyInstanceUpdates){$taskArgs+=@('--verify-instance-updates','1')}
    if($ColdStart){$taskArgs+=@('--cold-start','1')}
    if($LazyProbes){$taskArgs+=@('--lazy-probes','1')}
    if($StartupToggle){$taskArgs+=@('--startup-toggle','1')}
    if($InitializeProbeCache){$taskArgs+=@('--initialize-probe-cache','1')}
    if($CompareWorldModes){$taskArgs+=@('--compare-world-modes','1','--ab-rounds',[string]$ABRounds,'--ab-warmup',[string]$ABWarmup)}
    if($LazySettle){$taskArgs+=@('--lazy-settle','1','--steady-samples',[string]$SteadySamples,'--settle-max-batches',[string]$SettleMaxBatches)}
    if($GIOnly){$taskArgs+=@('--gi-only','1')}
    if($RayRoutingStats){$taskArgs+=@('--ray-routing-stats','1')}
    if($ProbePaddingLayers -ge 0){$taskArgs+=@('--probe-padding-layers',[string]$ProbePaddingLayers)}
    if($GpuValidation){$taskArgs+=@('--gpu-validation','1')}
    if($Camera -ge 0){$taskArgs+=@('--camera',[string]$Camera)}
    if($AllCameras){if($Camera -ge 0){throw '-AllCameras and -Camera are mutually exclusive.'};$taskArgs+=@('--all-cameras','1')}
    if($Mode -ne 99){$taskArgs+=@('--mode',[string]$Mode)}
    if($ReuseRaw){$taskArgs+=@('--reuse-raw',$ReuseRaw)}
    if($CameraEye -or $CameraTarget){if(-not $CameraEye -or -not $CameraTarget){throw 'Custom camera requires both -CameraEye and -CameraTarget.'};$taskArgs+=@('--camera-eye',$CameraEye,'--camera-target',$CameraTarget)}
    if($CavityRays){if(-not $ReuseRaw){throw 'Cavity replay requires -ReuseRaw.'};$taskArgs+=@('--cavity-rays',$CavityRays)}
    $taskPreviousPath=$env:PATH
    try {$env:PATH=$taskVcpkgBin+';'+$taskPreviousPath; & $taskExecutablePath @taskArgs; if($LASTEXITCODE -ne 0){throw "Stage 7.5 performance failed: $LASTEXITCODE"}}
    finally {$env:PATH=$taskPreviousPath}
    $taskAfter=@($taskInputPaths | ForEach-Object {$taskHash=Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash}})
    if((Compare-Object $taskBefore $taskAfter -Property path,sha256).Count -ne 0){throw 'Performance inputs changed during run; report is stale.'}
    $taskStem='tmp/VXGIStage75-'+$Label+'-'+$Width+'x'+$Height
    $taskAfter | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath ($taskStem+'-input-hashes.json') -Encoding utf8
    $taskReport=Get-Content -LiteralPath ($taskStem+'-report.json') -Raw | ConvertFrom-Json
    if($ProbePaddingLayers -ge 0 -and $taskReport.probe_allocation.padding_layers -ne $ProbePaddingLayers){throw 'Native probe padding layers differ from requested control.'}
    if($taskReport.debug_shader_policy -ne [bool]$DebugShaderPolicy){throw 'Native report shader policy differs from requested build policy.'}
    if($null -eq $taskReport.exact_fallback -or $taskReport.exact_fallback -ne [bool]$ExactFallback){throw 'Native report fallback policy differs from requested build policy.'}
    if($taskReport.native_debug -ne [bool]$NativeDebug -or $taskReport.native_configuration -ne $taskNativeConfiguration){throw 'Native report CPU configuration differs from requested build policy.'}
    if($taskReport.lazy_probes -ne [bool]$LazyProbes -or $taskReport.lazy_settle -ne [bool]$LazySettle){throw 'Native report lazy cache policy differs from requested policy.'}
    if($taskReport.compare_world_modes -ne [bool]$CompareWorldModes){throw 'Native report world A/B policy differs from requested policy.'}
    if($taskReport.startup_toggle -ne [bool]$StartupToggle){throw 'Native report startup toggle policy differs from requested policy.'}
    if($taskReport.initialize_probe_cache -ne [bool]$InitializeProbeCache){throw 'Native report ResourceManager probe initialization policy differs from requested policy.'}
    $taskReport | Add-Member -NotePropertyName native_build_policy -NotePropertyValue $taskBuildPolicy -Force
    $taskRawHash=Get-FileHash -LiteralPath $taskReport.raw_snapshot -Algorithm SHA256
    $taskReport | Add-Member -NotePropertyName raw_snapshot_sha256 -NotePropertyValue $taskRawHash.Hash -Force
    $taskReport | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath ($taskStem+'-report.json') -Encoding utf8
}
finally {Pop-Location}
