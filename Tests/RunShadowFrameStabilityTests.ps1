param([switch]$Debug,[string]$OutputDirectory='tmp/shadow-frame-tests')
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if(-not $output.StartsWith($repo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Test output must remain in the workspace'}
$output=Join-Path $output $(if($Debug){'debug'}else{'release'})
New-Item -ItemType Directory -Force -Path $output | Out-Null
$relative=[IO.Path]::GetRelativePath($repo,$output)
$hashes=@{}
foreach($p in @('Passes/ShadowPass.h','Passes/RenderStructs.h','Resources/Settings_Manager.h','Logic/Camera.h','Tests/ShadowFrameStabilityTests.cpp','Tests/RunShadowFrameStabilityTests.ps1')){$hashes[$p]=(Get-FileHash (Join-Path $repo $p)).Hash}
$flags=if($Debug){'/Od /Zi /MDd'}else{'/O2 /MD /DNDEBUG'}
$command=@"
@echo off
call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl.exe /nologo /std:c++20 $flags /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "." /I "Core" /I "Graphics" /I "Resources" /I "Logic" /I "ThirdParty" /I "ThirdParty\imgui" /I "Passes" /I "RDG" /I "Editor" /I "ThirdParty\NVIDIA\DLSS\include" /I "C:\Users\28499\vcpkg\installed\x64-windows\include" /I "packages\directxtk12_desktop_win10.2026.4.1.1\include" /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include" /I "packages\Microsoft.Direct3D.D3D12.1.619.1\build\native\include\d3dx12" /Fo"$relative\ShadowFrameStabilityTests.obj" /Fd"$relative\ShadowFrameStabilityTests.pdb" /Fe"$relative\ShadowFrameStabilityTests.exe" "Tests\ShadowFrameStabilityTests.cpp" /link /OPT:REF /LIBPATH:"ThirdParty\NVIDIA\DLSS\lib\Windows_x86_64\x64" /LIBPATH:"C:\Users\28499\vcpkg\installed\x64-windows\lib" /LIBPATH:"packages\directxtk12_desktop_win10.2026.4.1.1\native\lib\x64\Release" assimp-vc145-mt.lib DirectXTK12.lib windowscodecs.lib ole32.lib d3d12.lib dxgi.lib
exit /b %errorlevel%
"@
$build=Join-Path $output 'build.cmd'
[IO.File]::WriteAllText($build,($command -replace "`r?`n","`r`n")+"`r`n",[Text.Encoding]::ASCII)
Push-Location $repo
try {
 & $env:ComSpec /d /c ([IO.Path]::GetRelativePath($repo,$build))
 if($LASTEXITCODE -ne 0){throw 'Shadow frame CPU test build failed'}
 & (Join-Path $output 'ShadowFrameStabilityTests.exe') (Join-Path $relative 'report.json')
 if($LASTEXITCODE -ne 0){throw 'Shadow frame CPU regression failed'}
 foreach($p in $hashes.Keys){if((Get-FileHash $p).Hash -ne $hashes[$p]){throw "Source changed during test: $p"}}
 @{sourceHashes=$hashes;exeSha256=(Get-FileHash (Join-Path $output 'ShadowFrameStabilityTests.exe')).Hash;sourcesUnchangedDuringBuildAndRun=$true}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $output 'metadata.json') -Encoding utf8
} finally {Pop-Location}
