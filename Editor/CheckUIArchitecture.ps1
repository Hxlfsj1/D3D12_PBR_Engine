param([string]$Root = (Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\','/')
$allowed = @('Editor/EditorTheme.h', 'Editor/EditorUICenter.h')
$excludedFolders = @('ThirdParty','packages','tmp','x64','x86','arm64','Debug','Release','obj','bin','Models','HDRs','Scene_Assets')
function Get-ProjectSources([string]$Directory) {
    foreach ($entry in (Get-ChildItem -LiteralPath $Directory)) {
        if ($entry.PSIsContainer) {
            if (!$entry.Name.StartsWith('.') -and $entry.Name -notin $excludedFolders) {
                Get-ProjectSources $entry.FullName
            }
        } elseif ($entry.Extension -in '.h','.hpp','.cpp','.cc','.cxx') {
            $entry
        }
    }
}
$files = @(Get-ProjectSources $Root)
# Frame/backend setup, input queries and stable ID scopes are allowed outside the
# center. Widget construction, style changes and panel drawing belong to the center.
$queryOrLifecycle = '^(GetCurrentContext|CreateContext|DestroyContext|GetIO|GetMainViewport|GetDrawData|NewFrame|Render|EndFrame|PushID|PopID|GetID|GetActiveID|IsAnyItemActive|IsKeyPressed|IsKeyDown|IsKeyReleased|IsMouseDown|IsMouseClicked|IsMouseDoubleClicked|IsMouseReleased|IsMouseDragging|GetMouseDragDelta|ResetMouseDragDelta|SetNextFrameWantCaptureKeyboard|SetMouseCursor)$'
$failures = @()
foreach ($file in $files) {
    $relative = $file.FullName.Substring($Root.Length).TrimStart('\','/').Replace('\','/')
    if ($relative -in $allowed) { continue }
    $line = 0
    foreach ($text in (Get-Content -LiteralPath $file.FullName)) {
        ++$line
        if ($text.TrimStart().StartsWith('//')) { continue }
        foreach ($call in [regex]::Matches($text, 'ImGui::(\w+)\s*\(')) {
            $name = $call.Groups[1].Value
            if ($name -match $queryOrLifecycle) { continue }
            if ($relative -eq 'Editor/EditorGizmo.h' -and $name -eq 'GetBackgroundDrawList') { continue }
            $failures += "$($file.FullName):$line - create UI through EditorUICenter ($name)"
        }
    }
}
if ($failures.Count) { $failures | ForEach-Object { Write-Output $_ }; exit 1 }
Write-Output 'Editor UI architecture: all controls use EditorUICenter.'
