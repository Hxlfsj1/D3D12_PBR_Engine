param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = (Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    New-Item -ItemType Directory -Path 'tmp\editor-loading-render' -Force | Out-Null
    $taskInputs = @('Tests/EditorLoadingRenderTests.cpp', 'Tests/RunEditorLoadingRenderTests.ps1', 'Editor/EditorUICenter.h', 'Editor/EditorTheme.h')
    $taskBefore = @($taskInputs | ForEach-Object { $taskHash = Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash} })
    $taskCompile = 'call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MD /I Editor /external:I ThirdParty\imgui /external:I ThirdParty\imgui\backends /external:W0 /Fo:tmp\editor-loading-render\ /Fe:tmp\editor-loading-render\EditorLoadingRenderTests.exe Tests\EditorLoadingRenderTests.cpp ThirdParty\imgui\imgui.cpp ThirdParty\imgui\imgui_draw.cpp ThirdParty\imgui\imgui_tables.cpp ThirdParty\imgui\imgui_widgets.cpp ThirdParty\imgui\backends\imgui_impl_dx12.cpp /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib user32.lib ole32.lib windowscodecs.lib'
    & cmd.exe /d /c $taskCompile
    if ($LASTEXITCODE -ne 0) { throw "Loading UI render acceptance build failed: $LASTEXITCODE" }
    if ($BuildOnly) { return }
    & '.\tmp\editor-loading-render\EditorLoadingRenderTests.exe'
    if ($LASTEXITCODE -ne 0) { throw "Loading UI render acceptance failed: $LASTEXITCODE" }
    $taskAfter = @($taskInputs | ForEach-Object { $taskHash = Get-FileHash -LiteralPath $_ -Algorithm SHA256; [pscustomobject]@{path=$_;sha256=$taskHash.Hash} })
    if ((Compare-Object $taskBefore $taskAfter -Property path,sha256).Count -ne 0) { throw 'UI component inputs changed during acceptance; report is stale.' }
    $taskAfter | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath 'tmp/editor-loading-render/input-hashes.json' -Encoding utf8
}
finally { Pop-Location }
