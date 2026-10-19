param([switch]$BuildOnly, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
if ($BuildOnly -and $SkipBuild) { throw 'BuildOnly and SkipBuild cannot be combined.' }
$taskProjectRoot = (Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    $taskOutput = 'tmp/editor-camera-menu-render'
    New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
    $taskInputs = @(
        'Tests/EditorCameraMenuRenderTests.cpp', 'Tests/RunEditorCameraMenuRenderTests.ps1',
        'Editor/EditorCameraSnapshot.h', 'Logic/Camera.h', 'Editor/EditorUICenter.h', 'Editor/EditorTheme.h',
        'ThirdParty/imgui/imgui.h', 'ThirdParty/imgui/imgui_internal.h',
        'ThirdParty/imgui/imgui.cpp', 'ThirdParty/imgui/imgui_draw.cpp',
        'ThirdParty/imgui/imgui_tables.cpp', 'ThirdParty/imgui/imgui_widgets.cpp',
        'ThirdParty/imgui/backends/imgui_impl_dx12.h', 'ThirdParty/imgui/backends/imgui_impl_dx12.cpp'
    )
    $taskBefore = @($taskInputs | ForEach-Object {
        $taskHash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
        [pscustomobject]@{path=$_;sha256=$taskHash.Hash}
    })
    $taskManifest = "$taskOutput/build-input-hashes.json"
    if ($SkipBuild) {
        if (!(Test-Path -LiteralPath $taskManifest)) { throw 'No camera menu render acceptance build manifest exists.' }
        $taskBuilt = @(Get-Content -LiteralPath $taskManifest -Raw | ConvertFrom-Json)
        if (@(Compare-Object $taskBuilt $taskBefore -Property path,sha256).Count -ne 0) {
            throw 'Camera menu acceptance sources changed since compilation; rebuild first.'
        }
    } else {
        $taskCompile = 'call "D:\VsStudio\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /MD /DIMGUI_ENABLE_TEST_ENGINE /I Editor /I Logic /external:I ThirdParty\imgui /external:I ThirdParty\imgui\backends /external:W0 /Fo:tmp\editor-camera-menu-render\ /Fd:tmp\editor-camera-menu-render\camera-menu.pdb /Fe:tmp\editor-camera-menu-render\EditorCameraMenuRenderTests.exe Tests\EditorCameraMenuRenderTests.cpp ThirdParty\imgui\imgui.cpp ThirdParty\imgui\imgui_draw.cpp ThirdParty\imgui\imgui_tables.cpp ThirdParty\imgui\imgui_widgets.cpp ThirdParty\imgui\backends\imgui_impl_dx12.cpp /link d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib user32.lib ole32.lib windowscodecs.lib'
        & cmd.exe /d /c $taskCompile
        if ($LASTEXITCODE -ne 0) { throw "Camera menu render acceptance build failed: $LASTEXITCODE" }
        $taskAfterBuild = @($taskInputs | ForEach-Object {
            $taskHash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
            [pscustomobject]@{path=$_;sha256=$taskHash.Hash}
        })
        if (@(Compare-Object $taskBefore $taskAfterBuild -Property path,sha256).Count -ne 0) {
            throw 'Camera menu inputs changed during compilation; rebuild first.'
        }
        $taskBefore | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $taskManifest -Encoding utf8
    }
    if ($BuildOnly) { return }
    & '.\tmp\editor-camera-menu-render\EditorCameraMenuRenderTests.exe'
    if ($LASTEXITCODE -ne 0) { throw "Camera menu render acceptance failed: $LASTEXITCODE" }
    $taskAfter = @($taskInputs | ForEach-Object {
        $taskHash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
        [pscustomobject]@{path=$_;sha256=$taskHash.Hash}
    })
    if (@(Compare-Object $taskBefore $taskAfter -Property path,sha256).Count -ne 0) {
        throw 'Camera menu component inputs changed during acceptance; report is stale.'
    }
    $taskAfter | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$taskOutput/input-hashes.json" -Encoding utf8
}
finally { Pop-Location }
