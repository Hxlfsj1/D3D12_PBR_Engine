# UI creation center — UE 5.4.4 Starship reference

The fixed Outliner / Details / Output Log layout uses one theme and one component
creation API. Press **F10** in the application to open the live component template.
The template calls the same components as production panels; it is not a mock image.
Outliner and Output Log additionally use Unity-inspired alternating row backgrounds.
`RowEven` / `RowOdd` are shared theme tokens; they are a project choice, not UE or
Unity pixel-exact color claims. The F10 template demonstrates both list styles.

## Source reference

Inspected locally under `D:/UE544/Engine/Source/`:

| UE source | Adopted contract |
| --- | --- |
| `Runtime/SlateCore/Private/Styling/StyleColors.cpp`, `InitializeDefaults` | sRGB semantic palette: Background #151515, Panel #242424, Header #2F2F2F, Input #0F0F0F, outline #383838, Primary #0070E0 |
| `Runtime/SlateCore/Private/Styling/StarshipCoreStyle.cpp`, `SetupTextStyles`, spin-box defaults | Input background remains dark; 1-unit border changes from outline to hover to primary; compact rounded input boxes |
| Same file, `Docking.Tab` | Active tab surface above a darker tab strip, restrained active indicator |
| `Editor/EditorStyle/Private/StarshipStyle.cpp`, `DetailsView.*` | Header-colored category, small bold title, recessed property-grid separators |
| `Editor/PropertyEditor/Private/SDetailCategoryTableRow.cpp` | Vertically centered title, shared indentation/padding and category style |
| `Engine/Content/Slate/Fonts/Roboto-Regular.ttf`, `Roboto-Bold.ttf` | Same Roboto font files, bundled with their Apache license in `Assets/Fonts/LICENSE.txt` |

Official API context: [FAppStyle](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/SlateCore/FAppStyle),
[Slate architecture](https://dev.epicgames.com/documentation/unreal-engine/understanding-the-slate-ui-architecture-in-unreal-engine).

This is an ImGui implementation of the inspected style, not a copy of the Slate
engine. Palette/font inputs are matched. Icon geometry, font rasterization,
property sizing and fixed panel placement are adaptations; whole-window pixel
identity with Unreal Editor is not claimed. No UE engine source or proprietary
icon set is redistributed. Icons are drawn locally by the creation center.

## Ownership

- **EditorTheme.h**: semantic colors, fonts, metrics, DPI scale and ImGui defaults.
- **EditorUICenter.h**: the only panel/control drawing API. Owns panel headers,
  menus, rows, search, text editing, sections, property grids, buttons, log view,
  splitters and the live style guide. Focus/hover/disabled styles are built in.
- **EditorUI.h**: layout and model/history binding. No raw widget creation or
  panel-specific visual overrides. Searches filter real data; no decorative fake
  toolbar buttons are added.
- **EditorGizmo.h**: world-space overlay geometry and interaction, separate from
  panel creation. It uses the theme for overlay colors.

## Create a new panel

```cpp
EditorUICenter::CreatePanel(
    {"MaterialPanel", "Material", EditorUICenter::Icon::Details}, position, size, [&] {
        if (EditorUICenter::Section("Surface")) {
            EditorUICenter::FloatProperty("Roughness", roughness);
            EditorUICenter::BoolProperty("Visible", visible);
        }
    });
```

For history-aware numeric editing, use `VectorProperty`'s `Edit` callback as in
`EditorUI::DrawTransformRow`: capture before applying values, apply on `changed`,
commit on `deactivated`. Text editors own their text-level Ctrl+Z.

`ObjectRow` and `Rename` receive the visible row index (after filtering).
`LogView` owns row backgrounds and single-click whole-line selection; mouse dragging,
Shift selection and Ctrl+A retain the text widget's native behavior. Line selection
includes the newline and retains the frozen copy snapshot while the log has focus.

New control types must be added to the center and demonstrated in `StyleGuide`.
Do not duplicate ImGui style pushes, custom headers or per-panel sizes. The
MSBuild target `CheckEditorUIArchitecture` rejects raw widgets outside the center.

## Visual acceptance

Compare F10 template and production panels using the same DPI. Check normal,
hovered, focused, selected and disabled states; long names; narrow Details; Chinese
names; selected console text; menu clipping; and splitter hit positions. Body text
is 13 logical pixels, rows 24, panel headers 28; these are this port's metrics,
not an assertion that every UE widget uses those exact dimensions.

The build copies fonts to `EditorAssets/Fonts` alongside the executable. Fonts
also resolve relative to the project for development. CJK uses Windows Microsoft
YaHei when installed. Resizing a window reflows content but never rescales fonts.
