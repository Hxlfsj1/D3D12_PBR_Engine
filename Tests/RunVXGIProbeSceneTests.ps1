param([switch]$DebugBuild)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    $taskCompilerMode=if($DebugBuild){'/Od /Zi'}else{'/O2'}
    $taskSuffix=if($DebugBuild){'Debug'}else{'O2'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /I Logic '+$taskCompilerMode+
        ' /Fo:tmp\VXGIProbeSceneTests'+$taskSuffix+'.obj /Fd:tmp\VXGIProbeSceneTests'+$taskSuffix+'.pdb /Fe:tmp\VXGIProbeSceneTests'+$taskSuffix+'.exe Tests\VXGIProbeSceneTests.cpp'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Probe scene CPU test build failed: $LASTEXITCODE"}
    & ('.\tmp\VXGIProbeSceneTests'+$taskSuffix+'.exe')
    if($LASTEXITCODE -ne 0){throw "Probe scene CPU test failed: $LASTEXITCODE"}
}
finally{Pop-Location}
