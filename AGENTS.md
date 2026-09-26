# Editor UI

- Read `Editor/STYLE_REFERENCE.md` before creating or changing editor UI.
- Every editor panel and control must be created through `Editor/EditorUICenter.h`.
  Business panels may query input and use stable ID scopes, but may not directly
  create ImGui widgets, draw panel chrome or override colors/fonts/spacing.
- Reuse a component first. Add missing components to the center and its F10 style
  guide before using them in a panel. Theme tokens live in `Editor/EditorTheme.h`.
- Keep the source reference at UE 5.4.4 Starship. Window width must not change font
  size. Use DPI/UI scale, and check narrow layouts and 100%/150% scale.
- Outliner and Console use shared Unity-inspired alternating row backgrounds.
  Keep row colors in the theme and row selection/copy behavior in the UI center.
- Preserve single-click numeric editing, selectable/copyable logs, model selection,
  W/E/R gizmos, and the shared 100-entry undo history.
- `Editor/CheckUIArchitecture.ps1` runs during MSBuild. Do not bypass it to add UI.
  The world-space gizmo and rendering backend are not panel widgets; their input,
  projection and GPU work remain in their existing modules.
- Visual changes require inspecting the actual rendered components, not only a
  successful build. Do not describe an approximation as a pixel-identical UE clone.
