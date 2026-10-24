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
`MainMenu` owns the File, Edit and Debug menus. **Debug > Pass** lists the
logical stages in `Resources/RenderPassView.h`. Enabled stages offer their real
pass output and/or accumulated result; GBuffer selects an attachment within the
same stage, and Shadow shows a four-cascade atlas. AA algorithms share one
AntiAliasing entry; tone mapping belongs to PostProcess. Those two stages offer
accumulated scene results only. Inactive stages are disabled and show `未启用`.
The selected result is checked. Invalid selections return to Default.

GI offers exactly three mutually exclusive pure-light views, in order:
**默认（SSGI + Mesh SDF VXGI + Global SDF GI）**, **SSGI**, and
**SSGI + Mesh SDF VXGI**. They compare the cumulative tracing tiers. Without
Global SDF the default becomes **默认（SSGI + Mesh SDF VXGI）**, or **默认（SSGI）**
without VXGI. The Mesh SDF choice is disabled when world GI is unavailable.
`WrappedMenuItem` reuses the normal menu widget with viewport-aware line breaks
and a stable `###` ID; narrow layouts retain full labels and unchanged font size.
The F10 menu demonstrates this same component. Accumulated GI remains available
through CLI capture, but has no entry in this three-item submenu. Pure views display indirect
light before receiver material response. Diagnostic sources use separate output
and history while normal mixed GI keeps running. Switching diagnostic sources
invalidates their shared preview history. There is no isolated Mesh SDF/GDF-only
contribution view. The entire GI menu is disabled when GI is unavailable.
All three pure GI views continue through the configured AA and ToneMap
before display. Accumulated GI remains a snapshot at the GI composition boundary.
The AA choice follows Pipeline.json; None stays off. A wrapped muted footer reads
**纯 GI 输出：抗锯齿已启用** or **纯 GI 输出：抗锯齿未启用**, according to the
caller-supplied availability. The F10 template reuses this same status marker.
Mixed/diagnostic GI histories stay separate; color AA histories restart on view or
source changes instead of reusing colors from a different image. CLI captures of
pure GI store the final full-output-resolution RGBA8 resource after configured
AA and one ToneMap; accumulated GI captures retain the native HDR resource at the
GI boundary. Capture metadata records the output stage and effective AA state.

A stage choice never enables a feature or edits JSON. Normal GI computation and
its history updates continue while the viewer presents the selected result;
pure GI results use the AA/ToneMap display path described above.
Skybox and Transparent pass output
means signed linear after-minus-before color change (orange brighter, blue darker),
explicitly described in their menus; their accumulated result is normal scene color.

`DebugPassMenu` is shared by production and the F10 template. The template exposes
independent availability controls, including Global SDF, to inspect disabled stages, forward/deferred
paths and AA's accumulated-only result. It stays inside the viewport at narrow
widths; long text wraps and short windows scroll without changing font size.

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
