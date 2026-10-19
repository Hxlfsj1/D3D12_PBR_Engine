param([switch]$DebugBuild)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    $taskCompilerMode=if($DebugBuild){'/Od /Zi'}else{'/O2'}
    $taskSuffix=if($DebugBuild){'Debug'}else{'O2'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX '+$taskCompilerMode+
        ' /Fo:tmp\VXGIInstanceBVHTests'+$taskSuffix+'.obj /Fd:tmp\VXGIInstanceBVHTests'+$taskSuffix+'.pdb /Fe:tmp\VXGIInstanceBVHTests'+$taskSuffix+'.exe Tests\VXGIInstanceBVHTests.cpp'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "Instance BVH CPU test build failed: $LASTEXITCODE"}
    & ('.\tmp\VXGIInstanceBVHTests'+$taskSuffix+'.exe')
    if($LASTEXITCODE -ne 0){throw "Instance BVH CPU test failed: $LASTEXITCODE"}
}
finally{Pop-Location}
