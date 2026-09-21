#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include "imgui.h"
#include "ResourceManager.h"
#include "ErrorLog.h"
#include "EditorSelection.h"

// Palette lifted from Unreal Engine 5.4.4 itself:
//   Engine/Source/Runtime/SlateCore/Private/Styling/StyleColors.cpp
//   USlateThemeManager::InitalizeDefaults()  (the shipped dark editor theme)
// Hex values are authored in sRGB and used as-is, matching how they look on screen.
namespace UEStyle
{
    inline ImVec4 Hex(unsigned int rgb, float alpha = 1.0f)
    {
        return ImVec4(
            static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f,
            alpha);
    }

    // Surfaces
    inline const ImVec4 Title = Hex(0x151515);         // window / panel header base
    inline const ImVec4 Background = Hex(0x151515);    // editor backdrop, scroll regions
    inline const ImVec4 WindowBorder = Hex(0x0F0F0F);
    inline const ImVec4 Input = Hex(0x0F0F0F);         // text fields, recessed wells
    inline const ImVec4 InputOutline = Hex(0x383838);
    inline const ImVec4 Recessed = Hex(0x1A1A1A);
    inline const ImVec4 Panel = Hex(0x242424);         // panel body
    inline const ImVec4 Header = Hex(0x2F2F2F);
    inline const ImVec4 Dropdown = Hex(0x383838);
    inline const ImVec4 DropdownOutline = Hex(0x4C4C4C);
    inline const ImVec4 Hover = Hex(0x575757);
    inline const ImVec4 Hover2 = Hex(0x808080);

    // Accents and text
    inline const ImVec4 Primary = Hex(0x0070E0);       // UE "Highlight"
    inline const ImVec4 PrimaryHover = Hex(0x0E86FF);
    inline const ImVec4 PrimaryPress = Hex(0x0050A0);
    inline const ImVec4 Secondary = Hex(0x383838);
    inline const ImVec4 Foreground = Hex(0xC0C0C0);
    inline const ImVec4 ForegroundHeader = Hex(0xC8C8C8);
    inline const ImVec4 ForegroundHover = Hex(0xFFFFFF);
    inline const ImVec4 SelectInactive = Hex(0x40576F); // list selection, UE "SelectInactive"
    inline const ImVec4 Error = Hex(0xEF3535);
    inline const ImVec4 Warning = Hex(0xFFB800);
}

// Fixed three-zone editor shell - deliberately NOT dockable:
//   Outliner (left column) | Detail (right column) | Console (bottom row)
// The panels are plain windows pinned to computed rects every frame, so the layout can
// never be dragged around. Each seam is a 24px grab band CENTRED ON THE PANEL BORDER:
// half of it lies inside the panel, half over the 4px gap, so the resize target sits
// exactly where the border appears to be. It resizes the adjacent pane and paints a
// UE-blue line under the cursor.
// The remaining centre area is intentionally left empty: with no window there, the app
// keeps receiving mouse and keyboard input for the camera.

class EditorUI
{
public:
    // One-time setup right after ImGui::CreateContext(): derive the UI scale from the
    // initial viewport width, then build the font atlas and the theme at that scale.
    static void Initialize(float viewportWidth)
    {
        ApplyScale(viewportWidth);
        InitializeLayoutDefaults();
        RebuildFont();
        ApplyUEStyle();
    }

    // Call once per frame BEFORE ImGui::NewFrame(). Re-derives the scale whenever the
    // viewport width changed (window resized); the font atlas is re-baked only when the
    // resulting size actually differs. Kept outside the frame because it rebuilds the atlas.
    static void UpdateScaleForViewport(float viewportWidth)
    {
        if (!ApplyScale(viewportWidth))
        {
            return;
        }
        RebuildFont();
        ApplyUEStyle();
    }

    // Draw the three panels; call every frame between ImGui::NewFrame() and ImGui::Render()
    static void Draw(ResourceManager& resourceManager, EditorSelection& selection)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 origin = viewport->WorkPos;
        const ImVec2 total = viewport->WorkSize;

        // Clamping keeps the centre (the game view) alive however far a seam is dragged
        const float maxSideWidth = MaxF(
            s_minPanelWidth,
            total.x - s_minPanelWidth - s_minViewportWidth - 2.0f * kSplitterThickness);
        s_leftWidth = ClampF(s_leftWidth, s_minPanelWidth, maxSideWidth);
        s_rightWidth = ClampF(s_rightWidth, s_minPanelWidth, maxSideWidth);
        s_bottomHeight = ClampF(
            s_bottomHeight,
            s_minPanelHeight,
            MaxF(s_minPanelHeight, total.y - s_minViewportWidth - kSplitterThickness));

        const float columnsHeight = MaxF(s_minPanelHeight, total.y - s_bottomHeight - kSplitterThickness);
        const float rightX = origin.x + total.x - s_rightWidth;

        DrawPanel("Outliner", origin, ImVec2(s_leftWidth, columnsHeight),
            [&] { DrawOutlinerBody(resourceManager, selection); });
        DrawPanel("Detail", ImVec2(rightX, origin.y), ImVec2(s_rightWidth, columnsHeight),
            [&] { DrawDetailBody(resourceManager, selection); });
        DrawPanel("Console", ImVec2(origin.x, origin.y + columnsHeight + kSplitterThickness),
            ImVec2(total.x, s_bottomHeight),
            [&] { DrawConsoleBody(); });

        // Splitter windows are created above the panels (see kSplitterWindowFlags).
        // Submission order alone does not control top-level window stacking in ImGui.
        // Bands are described by their CENTRE, anchored on the border the user can see:
        // the Outliner's right edge, the Detail's left edge, and the Console's top edge.
        const ImVec2 leftCentre(origin.x + s_leftWidth, origin.y + columnsHeight * 0.5f);
        const ImVec2 rightCentre(rightX, origin.y + columnsHeight * 0.5f);
        const ImVec2 bottomCentre(
            origin.x + total.x * 0.5f,
            origin.y + columnsHeight + kSplitterThickness);

        DrawSplitter("##SplitLeft", leftCentre,
            ImVec2(kSplitterGrabSize, columnsHeight), true, s_leftWidth, 1.0f);
        DrawSplitter("##SplitRight", rightCentre,
            ImVec2(kSplitterGrabSize, columnsHeight), true, s_rightWidth, -1.0f);
        DrawSplitter("##SplitBottom", bottomCentre,
            ImVec2(total.x, kSplitterGrabSize), false, s_bottomHeight, -1.0f);

        // Only a click that starts and ends in exposed scene space may pick a model.
        // The scene still fills the entire client area underneath these overlay panels.
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        const float grabHalf = kSplitterGrabSize * 0.5f;
        const bool inScene = mouse.x > leftCentre.x + grabHalf &&
            mouse.x < rightCentre.x - grabHalf && mouse.y >= origin.y &&
            mouse.y < bottomCentre.y - grabHalf;
        const bool canPick = inScene && !io.WantCaptureMouse &&
            !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            s_sceneClickStarted = canPick;
            s_sceneClickStart = mouse;
        }
        const float dx = mouse.x - s_sceneClickStart.x;
        const float dy = mouse.y - s_sceneClickStart.y;
        if (!canPick || dx * dx + dy * dy > 16.0f)
            s_sceneClickStarted = false;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (s_sceneClickStarted && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
                selection.RequestPick((mouse.x - viewport->Pos.x) / io.DisplaySize.x,
                    (mouse.y - viewport->Pos.y) / io.DisplaySize.y);
            s_sceneClickStarted = false;
        }
    }

private:
    // windows.h defines min/max as macros (the project does not set NOMINMAX), so the
    // std:: equivalents cannot be used here without breaking the parse
    static constexpr float MaxF(float a, float b) { return a > b ? a : b; }
    static constexpr float ClampF(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    // ---- UI scale ------------------------------------------------------------
    // The UI is sized from the VIEWPORT WIDTH, not from a fixed pixel value:
    //     reference ratio: a 2560px-wide window uses a 24px font
    // Every metric in this file was authored against a 15px font, so one factor
    // (fontSize / 15) drives the whole layout: ApplyUEStyle() hands it to
    // style.ScaleAllSizes(), and the hand-placed offsets below derive from it too.
    static constexpr float kReferenceWidth = 2560.0f;
    static constexpr float kReferenceFontSize = 24.0f;
    static constexpr float kMetricBaseFontSize = 15.0f; // size the raw numbers were tuned at
    static constexpr float kMinFontSize = 12.0f;        // guard rails against odd window sizes
    static constexpr float kMaxFontSize = 40.0f;

    // Recomputed by ApplyScale() whenever the viewport width changes
    inline static float s_fontSize = kReferenceFontSize;
    inline static float s_uiScale = kReferenceFontSize / kMetricBaseFontSize;
    inline static float s_panelHeaderHeight = 26.0f * s_uiScale;
    inline static float s_panelHeaderTextInset = 8.0f * s_uiScale;
    inline static float s_panelBodyPaddingX = 8.0f * s_uiScale;
    inline static float s_panelBodyPaddingY = 6.0f * s_uiScale;
    inline static float s_minPanelWidth = 140.0f * s_uiScale;
    inline static float s_minPanelHeight = 90.0f * s_uiScale;
    inline static float s_minViewportWidth = 200.0f * s_uiScale;

    // kSplitterThickness is the visible gap between the panes (the game shows through it);
    // kSplitterGrabSize is the invisible grab band, centred on the panel border. Both are
    // mouse-precision values, so they deliberately do NOT scale with the font.
    static constexpr float kSplitterThickness = 4.0f;
    static constexpr float kSplitterGrabSize = 24.0f;

    // Returns true when the derived font size changed. Pure arithmetic plus bookkeeping;
    // the caller decides whether to re-bake the atlas and the style.
    static bool ApplyScale(float viewportWidth)
    {
        const float width = viewportWidth > 0.0f ? viewportWidth : kReferenceWidth;
        // Whole-pixel steps: the size only moves 1px per ~107px of window width, so rounding
        // keeps a window drag from re-baking the atlas (and re-reading the TTF) every frame.
        const float exact = ClampF(
            kReferenceFontSize * width / kReferenceWidth, kMinFontSize, kMaxFontSize);
        const float fontSize = static_cast<float>(static_cast<int>(exact + 0.5f));
        if (fontSize == s_fontSize)
        {
            return false;
        }

        s_fontSize = fontSize;
        s_uiScale = s_fontSize / kMetricBaseFontSize;
        s_panelHeaderHeight = 26.0f * s_uiScale;
        s_panelHeaderTextInset = 8.0f * s_uiScale;
        s_panelBodyPaddingX = 8.0f * s_uiScale;
        s_panelBodyPaddingY = 6.0f * s_uiScale;
        s_minPanelWidth = 140.0f * s_uiScale;
        s_minPanelHeight = 90.0f * s_uiScale;
        s_minViewportWidth = 200.0f * s_uiScale;
        return true;
    }

    // Initial pane sizes grow with the font so the same amount of content still fits. Only
    // applied once, so a later resize never fights a size the user dragged the seam to.
    static void InitializeLayoutDefaults()
    {
        s_leftWidth = 220.0f * s_uiScale;
        s_rightWidth = 300.0f * s_uiScale;
        s_bottomHeight = 180.0f * s_uiScale;
    }

    // Swap the atlas contents for a font of the current size. Only ever called outside a
    // frame: ImFontAtlas::Clear() is documented as "don't call mid-frame!". No manual
    // texture work is needed - the DX12 backend advertises RendererHasTextures and uploads
    // the rebuilt atlas itself - but io.FontDefault must be dropped first, because the
    // ImFont object it points at is destroyed by the clear.
    static void RebuildFont()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.FontDefault = nullptr;
        io.Fonts->ClearFonts();
        LoadEditorFont();
    }

    static constexpr ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    static constexpr ImGuiWindowFlags kSplitterWindowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
        // NoBringToFrontOnFocus would insert new splitters BEHIND the panels, hiding
        // the part of the hit band inside them. Panels themselves retain that flag.
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus;

    // A clean sans-serif at a readable size; falls back to ImGui's built-in font if absent.
    // NOTE: the bundled system fonts carry no CJK glyphs; console lines must stay
    // Latin-only until a CJK font atlas is added.
    static void LoadEditorFont()
    {
        ImGuiIO& io = ImGui::GetIO();
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf", // Segoe UI: closest to UE's Roboto/Inter look
            "C:\\Windows\\Fonts\\tahoma.ttf",
        };

        for (const char* path : candidates)
        {
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            {
                continue;
            }
            if (io.Fonts->AddFontFromFileTTF(path, s_fontSize) != nullptr)
            {
                return;
            }
        }

        io.Fonts->AddFontDefault();
    }

    // UE5 editor palette (see UEStyle above) mapped onto ImGuiStyle, plus UE-ish metrics:
    // square panels, 4px rounding on interactive elements, tight spacing, thin seams.
    static void ApplyUEStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Re-applying (after a resize changed the scale) must not compound the previous
        // scaling: restore the untouched 1.0x metrics captured on the first call, then
        // scale from that baseline. The assignments below are absolute, so this keeps the
        // result bit-identical no matter how many times the window is resized.
        if (s_hasBaseStyle)
        {
            style = s_baseStyle;
        }

        ImVec4* c = style.Colors;

        // Surfaces
        c[ImGuiCol_WindowBg] = UEStyle::Panel;
        c[ImGuiCol_ChildBg] = UEStyle::Background;
        c[ImGuiCol_PopupBg] = UEStyle::Panel;
        c[ImGuiCol_MenuBarBg] = UEStyle::Title;
        c[ImGuiCol_Border] = UEStyle::WindowBorder;
        c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

        // Recessed fields (text input, sliders, checkboxes)
        c[ImGuiCol_FrameBg] = UEStyle::Input;
        c[ImGuiCol_FrameBgHovered] = UEStyle::Recessed;
        c[ImGuiCol_FrameBgActive] = UEStyle::Header;

        // Title bars (panels draw their own header strip; these cover future popups)
        c[ImGuiCol_TitleBg] = UEStyle::Title;
        c[ImGuiCol_TitleBgActive] = UEStyle::Recessed;
        c[ImGuiCol_TitleBgCollapsed] = UEStyle::Title;

        // Scrollbars: thin and dark, with no bright track
        c[ImGuiCol_ScrollbarBg] = UEStyle::Background;
        c[ImGuiCol_ScrollbarGrab] = UEStyle::Secondary;
        c[ImGuiCol_ScrollbarGrabHovered] = UEStyle::Hover;
        c[ImGuiCol_ScrollbarGrabActive] = UEStyle::Hover2;

        // Text
        c[ImGuiCol_Text] = UEStyle::Foreground;
        c[ImGuiCol_TextDisabled] = UEStyle::Hover2;
        c[ImGuiCol_TextLink] = UEStyle::Primary;
        c[ImGuiCol_TextSelectedBg] = ImVec4(UEStyle::Primary.x, UEStyle::Primary.y, UEStyle::Primary.z, 0.35f);
        c[ImGuiCol_InputTextCursor] = UEStyle::ForegroundHover;

        // List rows / tree nodes / selectables: transparent when idle,
        // UE Hover on hover, UE SelectInactive when selected
        c[ImGuiCol_Header] = UEStyle::SelectInactive;
        c[ImGuiCol_HeaderHovered] = UEStyle::Hover;
        c[ImGuiCol_HeaderActive] = UEStyle::PrimaryPress;
        c[ImGuiCol_TreeLines] = UEStyle::InputOutline;

        // Buttons (UE secondary style)
        c[ImGuiCol_Button] = UEStyle::Secondary;
        c[ImGuiCol_ButtonHovered] = UEStyle::Hover;
        c[ImGuiCol_ButtonActive] = UEStyle::PrimaryPress;

        // Selection widgets
        c[ImGuiCol_CheckMark] = UEStyle::Primary;
        c[ImGuiCol_CheckboxSelectedBg] = UEStyle::Primary;
        c[ImGuiCol_SliderGrab] = UEStyle::Hover2;
        c[ImGuiCol_SliderGrabActive] = UEStyle::PrimaryHover;

        // Tabs (unused while dockless; kept consistent in case tab bars return)
        c[ImGuiCol_Tab] = UEStyle::Title;
        c[ImGuiCol_TabHovered] = UEStyle::Header;
        c[ImGuiCol_TabSelected] = UEStyle::Panel;

        // Separators / seams
        c[ImGuiCol_Separator] = UEStyle::WindowBorder;
        c[ImGuiCol_SeparatorHovered] = UEStyle::PrimaryHover;
        c[ImGuiCol_SeparatorActive] = UEStyle::Primary;

        // Tables
        c[ImGuiCol_TableHeaderBg] = UEStyle::Header;
        c[ImGuiCol_TableBorderStrong] = UEStyle::WindowBorder;
        c[ImGuiCol_TableBorderLight] = UEStyle::Recessed;
        c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);

        // Misc
        c[ImGuiCol_NavCursor] = UEStyle::Primary;
        c[ImGuiCol_DragDropTarget] = UEStyle::Primary;
        c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // panels are not grip-resizable
        c[ImGuiCol_ResizeGripHovered] = UEStyle::Hover;
        c[ImGuiCol_ResizeGripActive] = UEStyle::Primary;
        c[ImGuiCol_UnsavedMarker] = UEStyle::Warning;

        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.PopupRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 0.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;

        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.CellPadding = ImVec2(4.0f, 2.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
        style.IndentSpacing = 18.0f;
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 10.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.AntiAliasedLines = true;
        style.AntiAliasedFill = true;

        // Capture the unscaled baseline once, then scale the whole metric set (padding,
        // spacing, scrollbars, rounding...) by the font ratio. Without the scaling a larger
        // font ends up cramped inside spacing meant for a smaller one.
        if (!s_hasBaseStyle)
        {
            s_baseStyle = style;
            s_hasBaseStyle = true;
        }
        style.ScaleAllSizes(s_uiScale);
    }

    // A panel = pinned window + flush header strip + scrollable padded body
    template <typename BodyFn>
    static void DrawPanel(const char* name, ImVec2 pos, ImVec2 size, const BodyFn& body)
    {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);

        // Zero padding so the header strip can sit flush against the panel edges
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(name, nullptr, kPanelFlags);
        ImGui::PopStyleVar();

        DrawPanelHeader(name);

        // Child windows ignore WindowPadding unless explicitly asked to use it
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s_panelBodyPaddingX, s_panelBodyPaddingY));
        ImGui::BeginChild("##body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        body();
        ImGui::EndChild();

        ImGui::End();
    }

    static void DrawPanelHeader(const char* name)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(
            pos,
            ImVec2(pos.x + width, pos.y + s_panelHeaderHeight),
            ImGui::GetColorU32(UEStyle::Title));
        drawList->AddLine(
            ImVec2(pos.x, pos.y + s_panelHeaderHeight - 1.0f),
            ImVec2(pos.x + width, pos.y + s_panelHeaderHeight - 1.0f),
            ImGui::GetColorU32(UEStyle::WindowBorder));

        const ImVec2 textPos(pos.x + s_panelHeaderTextInset, pos.y + (s_panelHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f);
        drawList->AddText(textPos, ImGui::GetColorU32(UEStyle::ForegroundHeader), name);

        ImGui::Dummy(ImVec2(width, s_panelHeaderHeight));
    }

    // Drag band between two panes: `centre` is the panel border the band straddles,
    // `target` is the adjacent pane size the drag mutates.
    static void DrawSplitter(const char* id, ImVec2 centre, ImVec2 size, bool vertical, float& target, float sign)
    {
        const ImVec2 pos(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f);
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);

        // ImGui clamps EVERY top-level window up to style.WindowMinSize (32x32 by default,
        // imgui.cpp CalcWindowMinSize + CalcWindowSizeAfterConstraint - note the final
        // ImMax is applied even when a size constraint was supplied). Without this override
        // these thin strips silently become 32px windows and steal hover and mouse capture
        // from the neighbouring panel far beyond the band the user is aiming at.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(id, nullptr, kSplitterWindowFlags);
        ImGui::PopStyleVar(2);

        ImGui::InvisibleButton("##grab", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        if (hovered || active)
        {
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }
        if (active)
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            target += (vertical ? delta.x : delta.y) * sign;
        }

        // UE paints the seam blue under the cursor; the line runs through the band centre,
        // i.e. exactly along the panel border the user is dragging
        if (hovered || active)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(active ? UEStyle::Primary : UEStyle::PrimaryHover);
            if (vertical)
            {
                drawList->AddLine(ImVec2(centre.x, pos.y), ImVec2(centre.x, pos.y + size.y), color, 2.0f);
            }
            else
            {
                drawList->AddLine(ImVec2(pos.x, centre.y), ImVec2(pos.x + size.x, centre.y), color, 2.0f);
            }
        }

        ImGui::End();
    }

    static void DrawOutlinerBody(ResourceManager& resourceManager, EditorSelection& selection)
    {
        std::vector<ModelInstance>& instances = resourceManager.GetSceneInstances();
        for (int i = 0; i < static_cast<int>(instances.size()); ++i)
        {
            const ModelInstance& instance = instances[static_cast<size_t>(i)];
            ImGui::PushID(static_cast<int>(instance.editorId));
            if (ImGui::Selectable(instance.name.c_str(), selection.SelectedId() == instance.editorId))
            {
                selection.Select(instance.editorId);
            }
            ImGui::PopID();
        }
    }

    static void DrawDetailBody(ResourceManager& resourceManager, const EditorSelection& selection)
    {
        std::vector<ModelInstance>& instances = resourceManager.GetSceneInstances();
        for (const ModelInstance& instance : instances)
        {
            if (instance.editorId == selection.SelectedId())
            {
                ImGui::TextUnformatted(instance.name.c_str());
                return;
            }
        }
        ImGui::TextUnformatted("(no selection)");
    }

    static void DrawConsoleBody()
    {
        ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& entry : ErrorLog::RecentEntries())
        {
            ImGui::TextUnformatted(entry.c_str());
        }
        // Stick to the newest line when the user is already at the bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    // Layout state in pixels, mutated by the seams. The starting sizes below are placeholders:
    // InitializeLayoutDefaults() derives them from s_uiScale at startup.
    inline static float s_leftWidth = 220.0f;
    inline static float s_rightWidth = 300.0f;
    inline static float s_bottomHeight = 180.0f;
    inline static bool s_sceneClickStarted = false;
    inline static ImVec2 s_sceneClickStart;

    // Pristine 1.0x metrics, captured on the first ApplyUEStyle() so a later re-apply can
    // reset to them instead of scaling already-scaled values.
    inline static ImGuiStyle s_baseStyle;
    inline static bool s_hasBaseStyle = false;
};

#endif
