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
`MainMenu` owns the File, Edit and Debug menus. Hovering **Debug > Pass** exposes
four mutually exclusive views: **默认**, **关闭 VXGI**, **仅 SSGI** and **仅 GI**.
The active view is marked with a check; all three alternate views are disabled
when GI is unavailable. Default displays the normal composition with full GI;
VXGI-off retains the normal material, direct-light and IBL composition with SSGI
but disables world-space probe fallback. SSGI-only shows the screen
contribution and ends on screen misses, while GI-only shows screen plus world
probe indirect light. The separate GI views omit receiver albedo. This menu
replaces the former T shortcut and Details source selector.
`DebugPassMenu` is reused by the main menu and the F10 template's own menu bar.
The F10 **Debug pass** section can toggle availability to inspect the disabled
state. View selection is runtime state supplied by the caller, independent of
render quality configuration; selecting a menu item does not edit JSON.
Hovering **Edit > 相机** opens a temporary
submenu using `CameraTransformContent`, also demonstrated in the F10 **Camera
transform** section. Its read-only property rows show the live world transform,
vertical FOV, clipping planes and viewport size. **Copy camera JSON** exports the
current view at float round-trip precision, including eye/target/up/forward so
rendering tests can reproduce the view. Copying keeps the submenu open; clicking
outside dismisses it. Popup width is constrained to the viewport at every DPI;
values wrap and short viewports scroll without reducing the font size. The camera
has no transform scale control; its displayed/exported scale is `(1, 1, 1)`.
Camera values use the renderer's native left-handed, Y-up axes and degree Pitch /
Yaw / Roll rotation, explicitly labelled in the popup. They are not remapped to
the model Details panel's Z-up editor axes, so pasted values directly reproduce
the engine camera without an implicit coordinate conversion.
`Progress` displays a clamped completion fraction with a numeric percentage using
the shared blue accent. `LoadingScreen` places it in a centered card over the
editor background, followed by one wrapped status message. The F10 **Loading**
section uses the same content and offers a three-second full-screen preview.
Startup reports actual completed work through this API; the control does not
animate or invent intermediate progress. At 320 / 640 pixel window widths and
100% / 150% DPI, only the card width and status wrapping change, never font size.
`IntegerProperty` uses the shared property grid, wrapped labels and single-click
integer text editing. It is demonstrated by **Sample count** in F10 Properties;
business bindings validate integer ranges without drawing their own controls.
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
