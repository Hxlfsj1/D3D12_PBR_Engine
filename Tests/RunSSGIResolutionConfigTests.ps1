param([switch]$DebugBuild)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp' -Force | Out-Null
    $taskCompilerMode=if($DebugBuild){'/Od /Zi'}else{'/O2'}
    $taskSuffix=if($DebugBuild){'Debug'}else{'O2'}
    $taskBuildCommand='call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /I Logic /I Graphics /I ThirdParty '+$taskCompilerMode+
        ' /Fo:tmp\SSGIResolutionConfigTests'+$taskSuffix+'.obj /Fd:tmp\SSGIResolutionConfigTests'+$taskSuffix+'.pdb /Fe:tmp\SSGIResolutionConfigTests'+$taskSuffix+'.exe Tests\SSGIResolutionConfigTests.cpp'
    & cmd.exe /d /c $taskBuildCommand
    if($LASTEXITCODE -ne 0){throw "SSGI factor config CPU test build failed: $LASTEXITCODE"}
    & ('.\tmp\SSGIResolutionConfigTests'+$taskSuffix+'.exe') ('tmp\SSGIResolutionConfig-'+$taskSuffix+'-fixture')
    if($LASTEXITCODE -ne 0){throw "SSGI factor config CPU test failed: $LASTEXITCODE"}
}
finally{Pop-Location}
