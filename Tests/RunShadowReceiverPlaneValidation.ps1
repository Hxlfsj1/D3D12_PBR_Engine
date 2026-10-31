param([string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stage=Join-Path $repo 'tmp/shadow-receiver-plane/runtime'
New-Item -ItemType Directory -Force "$stage/Models","$stage/Settings","$stage/Tests"|Out-Null
foreach($folder in @('Shaders','ThirdParty')) {Copy-Item -LiteralPath (Join-Path $repo $folder) -Destination $stage -Recurse -Force}
Copy-Item "$repo/Tests/ShadowReceiverPlaneTests.hlsli" "$stage/Tests" -Force
foreach($name in @('Cornell_Box.glb','Plain.glb')) {Copy-Item "$repo/Models/$name" "$stage/Models" -Force}
foreach($name in @('Pipeline.json','Lighting.json')) {Copy-Item "$repo/Settings/$name" "$stage/Settings" -Force}
@{skybox_path='../../../HDRs/citrus_orchard_road_puresky_4k.hdr';instances=@(@{model_path='Models/Cornell_Box.glb';pos=@(0,.01,0);rot=@(0,0,0);scale=@(1,1,1);is_transparent=$false;is_cutout=$true},@{model_path='Models/Plain.glb';pos=@(0,0,0);rot=@(0,0,0);scale=@(2,2,2);is_transparent=$false;is_cutout=$false})}|ConvertTo-Json -Depth 9|Set-Content "$stage/Settings/Scene.json" -Encoding utf8
$pipeline=Get-Content "$stage/Settings/Pipeline.json" -Raw|ConvertFrom-Json
$pipeline.vxgi.enabled=$false;$pipeline.ssgi.enabled=$false;$pipeline.hbao.enabled=$false;$pipeline.'DLSS SR'.anti_aliasing='None'
$camera=Get-Content "$repo/Tests/Fixtures/ShadowCornerCamera.json" -Raw|ConvertFrom-Json
$shader=Get-Content "$repo/Shaders/Shaders_For_Deferred.hlsl" -Raw
$common=Get-Content "$repo/Shaders/LightingCommon.hlsli" -Raw
$legacy=@'
float2 CalculateLegacyReceiverDepthGradient(float3 dx, float3 dy)
{
    float3 n = cross(dx,dy);
    float d = max(max(abs(n.z),length(n)*0.0872665f),1e-6f);
    return -n.xy / (n.z < 0 ? -d : d);
}
'@
$cases=@(@{name='legacy';width=1280;height=800},@{name='fixed';width=1280;height=800},@{name='fixed-native-resolution';width=2560;height=1600},@{name='forward';width=1280;height=800},@{name='old-pose';width=1280;height=800},@{name='near-corner';width=1280;height=800},@{name='analytic';width=1280;height=800})
$results=@()
$hashes=@{};foreach($name in @('Shaders/LightingCommon.hlsli','Shaders/ShadowReceiverPlane.hlsli','Tests/ShadowReceiverPlaneTests.hlsli')) {$hashes[$name]=(Get-FileHash "$repo/$name").Hash}
try {
 foreach($case in $cases) {
  $pipeline.rendering.use_deferred=$case.name -ne 'forward'
  $pipeline|ConvertTo-Json -Depth 12|Set-Content "$stage/Settings/Pipeline.json" -Encoding utf8
  @{width=$case.width;height=$case.height;fullscreen=$false;title='Shadow receiver plane validation'}|ConvertTo-Json|Set-Content "$stage/Settings/Window.json" -Encoding utf8
  $c=$camera
  if($case.name -eq 'old-pose') {
   $eye=@(-.1158,3.2589,7.0816);$p=40*[Math]::PI/180;$y=50.25*[Math]::PI/180
   $c=@{eye=$eye;target=@(($eye[0]+[Math]::Sin($y)*[Math]::Cos($p)),($eye[1]-[Math]::Sin($p)),($eye[2]+[Math]::Cos($y)*[Math]::Cos($p)));up=@(0,1,0);vertical_fov_degrees=45}
  }
  if($case.name -eq 'near-corner') {$c=@{eye=@(1.7,3.65,-1.7);target=@(2,3.95,-2.025);up=@(0,1,0);vertical_fov_degrees=45}}
  $c|ConvertTo-Json -Depth 6|Set-Content "$stage/camera.json" -Encoding utf8
  $s=$shader;$h=$common
  if($case.name -eq 'legacy') {$h=$legacy+"`n"+$common.Replace('CalculateShadowReceiverDepthGradient(projCoordsDDX, projCoordsDDY)','CalculateLegacyReceiverDepthGradient(projCoordsDDX, projCoordsDDY)')}
  if($case.name -eq 'analytic') {
   $s=$shader.Replace('PS_OUTPUT PSMain(VS_OUTPUT input)',"#include `"../Tests/ShadowReceiverPlaneTests.hlsli`"`nPS_OUTPUT PSMain(VS_OUTPUT input)")
   $s=$s.Replace('PS_OUTPUT output = (PS_OUTPUT)0;', 'PS_OUTPUT output = (PS_OUTPUT)0; output.sceneColor = TestShadowReceiverPlane(((uint)input.pos.x) % 32); return output;')
  }
  [IO.File]::WriteAllText("$stage/Shaders/Shaders_For_Deferred.hlsl",$s)
  [IO.File]::WriteAllText("$stage/Shaders/LightingCommon.hlsli",$h)
  $pass=if($case.name -eq 'forward'){'pbr-opaque'}else{'deferred-lighting'}
  $started=[DateTime]::UtcNow
  $process=Start-Process -FilePath "$repo/x64/$Configuration/LearnDirectX.exe" -WorkingDirectory $stage -WindowStyle Hidden -PassThru -ArgumentList @('--hidden','--frames=8','--capture-frame=6',"--pass-view=$pass",'--camera-preset=camera.json')
  $watch=[Diagnostics.Stopwatch]::StartNew();while(-not $process.WaitForExit(1000)){if($watch.Elapsed.TotalSeconds -gt 120){Stop-Process -InputObject $process;throw 'Timeout'}}
  if((Get-Item "$stage/PassViewRun.json").LastWriteTimeUtc -lt $started){throw 'Stale report'}
  $r=Get-Content "$stage/PassViewRun.json" -Raw|ConvertFrom-Json
  if($process.ExitCode -ne 0 -or -not $r.passed -or $r.d3d12_errors -ne 0 -or $r.d3d12_warnings -ne 0){throw "Failed $($case.name)"}
  if($Configuration -eq 'Debug' -and -not $r.d3d12_validation_available){throw 'Debug layer missing'}
  $results+=@{name=$case.name;capture=$r.capture;debug_available=$r.d3d12_validation_available;warnings=$r.d3d12_warnings;errors=$r.d3d12_errors}
  $results|ConvertTo-Json -Depth 5|Set-Content "$stage/runs.json" -Encoding utf8
  Write-Output "$($case.name) $($r.capture)"
 }
 foreach($name in $hashes.Keys){if((Get-FileHash "$repo/$name").Hash -ne $hashes[$name]){throw "Source changed during validation: $name"}}
 @{configuration=$Configuration;sourceHashes=$hashes;exeSha256=(Get-FileHash "$repo/x64/$Configuration/LearnDirectX.exe").Hash}|ConvertTo-Json -Depth 5|Set-Content "$stage/metadata.json" -Encoding utf8
}finally{Copy-Item -LiteralPath "$repo/Shaders" -Destination $stage -Recurse -Force}
