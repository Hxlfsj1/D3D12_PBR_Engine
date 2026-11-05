param(
    [ValidateSet('A','B0','B1','B2','B1Visible','B1Interior','B1Endpoints','B1Sun','B2Sun','B1SunWitness')][string]$Case='B1',
    [ValidateSet('overview','inside','corner','outside')][string]$Camera='inside',
    [ValidateSet('default','mesh-sdf','ssgi')][string]$Source='default',
    [int]$Frame=256,
    [ValidateSet('gi','default','gbuffer','deferred-lighting')][string]$Pass='gi',
    [string]$Configuration='Debug',
    [switch]$Prepare,
    [string]$ExecutablePath='',
    [string]$OutputDirectory='tmp/vxgi-cornell-leak'
)
$ErrorActionPreference='Stop'
if($Case -eq 'B1Endpoints'){throw 'Exact contributor visibility was reverted for performance; endpoint captures are historical only.'}
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if(-not $output.StartsWith((Join-Path $repo 'tmp')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Output must remain inside workspace tmp'}
$env:VXGI_CORNELL_OUTPUT=$OutputDirectory
if($Prepare){ & python (Join-Path $PSScriptRoot 'PrepareVXGICornellLeak.py'); if($LASTEXITCODE){throw 'Fixture generation failed'} }
$stage=Join-Path $output $Case
foreach($folder in @('Shaders','ThirdParty')){
    $link=Join-Path $stage $folder
    if(-not(Test-Path -LiteralPath $link)){New-Item -ItemType Junction -Path $link -Target (Join-Path $output "common/$folder")|Out-Null}
}
$exe=if($ExecutablePath){[IO.Path]::GetFullPath((Join-Path $repo $ExecutablePath))}else{Join-Path $repo "x64/$Configuration/LearnDirectX.exe"}
$shaderHashes=@{}
foreach($name in @('VXGIProbeRadianceCommon.hlsli','VXGIProbeVisibilityCommon.hlsli','SSGIHybridCommon.hlsli','Shaders_For_SSGI.hlsl',
    'VXGIProbeDistanceCommon.hlsli','Shaders_For_VXGI_Probe_Update.hlsl','VXGISDFCommon.hlsli','VXGIMeshSDFTraceBody.hlsli','VXGISDFTraceBody.hlsli','VXGISurfaceCommon.hlsli')){
    $path=Join-Path $stage "Shaders/$name"
    if(Test-Path -LiteralPath $path){$shaderHashes[$name]=(Get-FileHash -LiteralPath $path).Hash}
}
$started=[DateTime]::UtcNow
$arguments=@('--hidden',"--frames=$($Frame+3)","--capture-frame=$Frame","--pass-view=$Pass","--camera-preset=camera-$Camera.json")
if($Pass -eq 'gi'){$arguments+=@('--pass-result=output',"--pass-gi-source=$Source")}
$process=Start-Process -FilePath $exe -WorkingDirectory $stage -WindowStyle Hidden -PassThru -ArgumentList $arguments
$watch=[Diagnostics.Stopwatch]::StartNew()
while(-not $process.WaitForExit(1000)){if($watch.Elapsed.TotalSeconds -gt 300){Stop-Process -InputObject $process;throw 'Cornell test timed out'}}
$report=Join-Path $stage 'PassViewRun.json'
if(-not(Test-Path -LiteralPath $report)){throw "No run report: process exit=$($process.ExitCode), case=$Case"}
if((Get-Item -LiteralPath $report).LastWriteTimeUtc -lt $started){throw 'Stale run report'}
$result=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
$runs=Join-Path $output 'runs';New-Item -ItemType Directory -Force -Path $runs|Out-Null
$label="$Case-$Camera-$Source-$Pass-$Frame"
if($Configuration -ne 'Debug'){$label+="-$Configuration"}
$result|Add-Member test_case $Case;$result|Add-Member test_camera $Camera
$result|Add-Member test_source $Source;$result|Add-Member elapsed_seconds $watch.Elapsed.TotalSeconds
$result|Add-Member configuration $Configuration
$result|Add-Member executable_sha256 (Get-FileHash -LiteralPath $exe).Hash
$result|Add-Member shader_sha256 $shaderHashes
$result|Add-Member pipeline_sha256 (Get-FileHash -LiteralPath (Join-Path $stage 'Settings/Pipeline.json')).Hash
$result|Add-Member exit_code $process.ExitCode
$result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $runs "$label.json") -Encoding utf8
if($process.ExitCode -ne 0 -or -not $result.passed -or $result.d3d12_errors -ne 0 -or $result.d3d12_warnings -ne 0){throw "Failed $label"}
if($Configuration -eq 'Debug' -and -not $result.d3d12_validation_available){throw "Debug validation unavailable: $label"}
if($result.vxgi_probes.unclassified -ne 0 -or $result.vxgi_probes.valid -eq 0 -or
   $result.vxgi_probes.total_updates -ne ($result.vxgi_probes.valid * $result.vxgi_probes.target_updates)){
    throw "Incomplete probe fill: $label"
}
Write-Output "$label $($result.capture) seconds=$([Math]::Round($watch.Elapsed.TotalSeconds,2)) warnings=$($result.d3d12_warnings)"
