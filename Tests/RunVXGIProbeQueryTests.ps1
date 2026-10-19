param([switch]$DebugBuild)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    $taskCompilerMode=if($DebugBuild){'/Od /Zi'}else{'/O2'}
    $taskSuffix=if($DebugBuild){'Debug'}else{'O2'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX '+$taskCompilerMode+
        ' /Fo:tmp\VXGIProbeQueryTests'+$taskSuffix+'.obj /Fd:tmp\VXGIProbeQueryTests'+$taskSuffix+'.pdb /Fe:tmp\VXGIProbeQueryTests'+$taskSuffix+'.exe Tests\VXGIProbeQueryTests.cpp'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Probe query CPU test build failed: $LASTEXITCODE"}
    & ('.\tmp\VXGIProbeQueryTests'+$taskSuffix+'.exe')
    if($LASTEXITCODE -ne 0){throw "Probe query CPU test failed: $LASTEXITCODE"}
}
finally{Pop-Location}
