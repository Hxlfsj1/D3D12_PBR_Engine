param([string]$Configuration='Release')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stage=Join-Path $repo 'tmp/shadow-cascade-blend/runtime'
$sourceHashes=@{}
foreach($name in @('Passes/ShadowPass.h','Passes/RenderStructs.h','PBR_Render.cpp','Resources/Settings_Manager.h','Shaders/LightingCommon.hlsli','Shaders/Shaders_For_Deferred.hlsl','Shaders/Shaders_For_PBR.hlsl','Shaders/Shaders_For_Shadow.hlsl')){$sourceHashes[$name]=(Get-FileHash (Join-Path $repo $name)).Hash}
New-Item -ItemType Directory -Force "$stage/Models","$stage/Settings"|Out-Null
foreach($directory in @('Shaders','ThirdParty')){Copy-Item -LiteralPath (Join-Path $repo $directory) -Destination $stage -Recurse -Force}
foreach($name in @('Cornell_Box.glb','Plain.glb')){Copy-Item (Join-Path $repo "Models/$name") "$stage/Models/$name" -Force}
foreach($name in @('Pipeline.json','Lighting.json')){Copy-Item (Join-Path $repo "Settings/$name") "$stage/Settings/$name" -Force}
@{width=1280;height=800;fullscreen=$false;title='Cascade transition validation'}|ConvertTo-Json|Set-Content "$stage/Settings/Window.json" -Encoding utf8
@{skybox_path='../../../HDRs/citrus_orchard_road_puresky_4k.hdr';instances=@(@{model_path='Models/Cornell_Box.glb';pos=@(0,.01,0);rot=@(0,0,0);scale=@(1,1,1);is_transparent=$false;is_cutout=$true},@{model_path='Models/Plain.glb';pos=@(0,0,0);rot=@(0,0,0);scale=@(2,2,2);is_transparent=$false;is_cutout=$false})}|ConvertTo-Json -Depth 9|Set-Content "$stage/Settings/Scene.json" -Encoding utf8
$pipeline=Get-Content "$stage/Settings/Pipeline.json" -Raw|ConvertFrom-Json
$pipeline.vxgi.enabled=$false;$pipeline.ssgi.enabled=$false;$pipeline.hbao.enabled=$false;$pipeline.'DLSS SR'.anti_aliasing='None'
if(-not $pipeline.PSObject.Properties['shadows']){$pipeline|Add-Member shadows ([pscustomobject]@{cascades=[pscustomobject]@{transition_fraction=.1}})}
$eye=@(-.1158,3.2589,7.0816);$pitch=40*[Math]::PI/180;$yaw=50.25*[Math]::PI/180
$f=@(([Math]::Sin($yaw)*[Math]::Cos($pitch)),(-[Math]::Sin($pitch)),([Math]::Cos($yaw)*[Math]::Cos($pitch)))
$source=Get-Content (Join-Path $repo 'Shaders/Shaders_For_Deferred.hlsl') -Raw
$diagnostic=@'
uint diagnosticIndex = SelectCascadeIndex(viewDepth);
bool diagnosticCurrentValid;
float diagnosticCurrent = SampleCascadeShadow(diagnosticIndex, worldPos, worldPosDDX, worldPosDDY, diagnosticCurrentValid);
bool diagnosticPreviousValid = diagnosticCurrentValid;
float diagnosticPrevious = diagnosticCurrent;
if (diagnosticIndex > 0) diagnosticPrevious = SampleCascadeShadow(diagnosticIndex-1, worldPos, worldPosDDX, worldPosDDY, diagnosticPreviousValid);
output.sceneColor = float4(shadow, diagnosticPrevious, diagnosticCurrent, diagnosticCurrentValid && diagnosticPreviousValid ? 1 : 0);
'@
$diag=$source.Replace('output.sceneColor = float4(color, 1.0f);',$diagnostic)
$cases=@(@{name='no-blend';fraction=0;offset=0},@{name='blend-base';fraction=.1;offset=0})
foreach($offset in @(-1,-.5,-.1,.1,.5,1)){$cases+=@{name="blend-move-$offset";fraction=.1;offset=$offset}}
foreach($offset in @(-.1,0,.1,.5,-10,-45)){$cases+=@{name="diagnostic-$offset";fraction=.1;offset=$offset;diagnostic=$true}}
$cases+=@{name='forward';fraction=.1;offset=0;forward=$true},@{name='near';fraction=.1;near=$true;offset=0}
$results=@()
try {
 foreach($case in $cases){
  $pipeline.rendering.use_deferred=-not [bool]$case.forward;$pipeline.shadows.cascades.transition_fraction=$case.fraction
  $pipeline|ConvertTo-Json -Depth 12|Set-Content "$stage/Settings/Pipeline.json" -Encoding utf8
  [IO.File]::WriteAllText("$stage/Shaders/Shaders_For_Deferred.hlsl",$(if($case.diagnostic){$diag}else{$source}))
  $offset=$case.offset;$position=@(($eye[0]+$f[0]*$offset),($eye[1]+$f[1]*$offset),($eye[2]+$f[2]*$offset));$target=@(($position[0]+$f[0]),($position[1]+$f[1]),($position[2]+$f[2]))
  if($case.near){$position=@(1.7,3.65,-1.7);$target=@(2,3.95,-2.025)}
  @{eye=$position;target=$target;up=@(0,1,0);vertical_fov_degrees=45}|ConvertTo-Json|Set-Content "$stage/camera.json" -Encoding utf8
  $pass=if($case.forward){'pbr-opaque'}else{'deferred-lighting'};$started=[DateTime]::UtcNow
  $process=Start-Process -FilePath "$repo/x64/$Configuration/LearnDirectX.exe" -WorkingDirectory $stage -WindowStyle Hidden -PassThru -ArgumentList @('--hidden','--frames=8','--capture-frame=6',"--pass-view=$pass",'--camera-preset=camera.json')
  $watch=[Diagnostics.Stopwatch]::StartNew();while(-not $process.WaitForExit(1000)){if($watch.Elapsed.TotalSeconds -gt 120){Stop-Process -InputObject $process;throw 'Timeout'}}
  if((Get-Item "$stage/PassViewRun.json").LastWriteTimeUtc -lt $started){throw 'Stale report'}
  $r=Get-Content "$stage/PassViewRun.json" -Raw|ConvertFrom-Json
  if($process.ExitCode -ne 0 -or -not $r.passed -or $r.d3d12_errors -ne 0 -or $r.d3d12_warnings -ne 0){throw "Failed $($case.name)"}
  if($Configuration -eq 'Debug' -and -not $r.d3d12_validation_available){throw 'Debug layer was not available'}
  $results+=@{name=$case.name;fraction=$case.fraction;offset=$offset;diagnostic=[bool]$case.diagnostic;capture=$r.capture;errors=$r.d3d12_errors;warnings=$r.d3d12_warnings;debug_available=$r.d3d12_validation_available}
  $results|ConvertTo-Json -Depth 6|Set-Content "$stage/runs.json" -Encoding utf8;Write-Output "$($case.name) $($r.capture)"
 }
 foreach($name in $sourceHashes.Keys){if((Get-FileHash (Join-Path $repo $name)).Hash -ne $sourceHashes[$name]){throw "Source changed during rendering: $name"}}
 @{configuration=$Configuration;sourceHashes=$sourceHashes;exeSha256=(Get-FileHash "$repo/x64/$Configuration/LearnDirectX.exe").Hash;sourcesUnchangedDuringRun=$true}|ConvertTo-Json -Depth 5|Set-Content "$stage/metadata.json" -Encoding utf8
}finally{Copy-Item -LiteralPath (Join-Path $repo 'Shaders') -Destination $stage -Recurse -Force}
