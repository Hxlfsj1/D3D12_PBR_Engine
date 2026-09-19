#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include "imgui.h"
#include "ResourceManager.h"
#include "ErrorLog.h"

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
// never be dragged around. The seams between them are 4px drag strips that resize the
// adjacent pane and paint a UE-blue line under the cursor.
// The remaining centre area is intentionally left empty: with no window there, the app
// keeps receiving mouse and keyboard input for the camera.

class EditorUI
{
public:
    // One-time setup right after ImGui::CreateContext(): font atlas + theme
    static void Initialize()
    {
        LoadEditorFont();
        ApplyUEStyle();
    }

    // Draw the three panels; call every frame between ImGui::NewFrame() and ImGui::Render()
    static void Draw(ResourceManager& resourceManager)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 origin = viewport->WorkPos;
        const ImVec2 total = viewport->WorkSize;

        // Clamping keeps the centre (the game view) alive however far a seam is dragged
        const float maxSideWidth = MaxF(
            kMinPanelWidth,
            total.x - kMinPanelWidth - kMinViewportWidth - 2.0f * kSplitterThickness);
        s_leftWidth = ClampF(s_leftWidth, kMinPanelWidth, maxSideWidth);
        s_rightWidth = ClampF(s_rightWidth, kMinPanelWidth, maxSideWidth);
        s_bottomHeight = ClampF(
            s_bottomHeight,
            kMinPanelHeight,
            MaxF(kMinPanelHeight, total.y - kMinViewportWidth - kSplitterThickness));

        const float columnsHeight = MaxF(kMinPanelHeight, total.y - s_bottomHeight - kSplitterThickness);
        const float rightX = origin.x + total.x - s_rightWidth;

        DrawPanel("Outliner", origin, ImVec2(s_leftWidth, columnsHeight),
            [&] { DrawOutlinerBody(resourceManager); });
        DrawPanel("Detail", ImVec2(rightX, origin.y), ImVec2(s_rightWidth, columnsHeight),
            [&] { DrawDetailBody(resourceManager); });
        DrawPanel("Console", ImVec2(origin.x, origin.y + columnsHeight + kSplitterThickness),
            ImVec2(total.x, s_bottomHeight),
            [&] { DrawConsoleBody(); });

        // Seams are submitted last so they sit above the panel edges. Each grab strip is
        // centred on the visible gap and reaches kSplitterGrabPadding into BOTH neighbours,
        // so hovering the panel edge itself is enough to start the drag.
        const float leftSeam = origin.x + s_leftWidth;               // start of the left gap
        const float rightSeam = rightX - kSplitterThickness;         // start of the right gap
        const float bottomSeam = origin.y + columnsHeight;           // start of the bottom gap
        const ImVec2 verticalGrab(
            kSplitterThickness + 2.0f * kSplitterGrabPadding,
            columnsHeight);
        const ImVec2 horizontalGrab(
            total.x + 2.0f * kSplitterGrabPadding,
            kSplitterThickness + 2.0f * kSplitterGrabPadding);

        DrawSplitter("##SplitLeft",
            ImVec2(leftSeam - kSplitterGrabPadding, origin.y), verticalGrab, true, s_leftWidth, 1.0f);
        DrawSplitter("##SplitRight",
            ImVec2(rightSeam - kSplitterGrabPadding, origin.y), verticalGrab, true, s_rightWidth, -1.0f);
        DrawSplitter("##SplitBottom",
            ImVec2(origin.x - kSplitterGrabPadding, bottomSeam - kSplitterGrabPadding), horizontalGrab, false, s_bottomHeight, -1.0f);
    }

private:
    // windows.h defines min/max as macros (the project does not set NOMINMAX), so the
    // std:: equivalents cannot be used here without breaking the parse
    static constexpr float MaxF(float a, float b) { return a > b ? a : b; }
    static constexpr float ClampF(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    // kSplitterThickness is the visible gap between panels (the game shows through it);
    // the grab strip itself is wider and straddles that gap by kSplitterGrabPadding on
    // each side, so the panel border is a valid grab point too.
    static constexpr float kSplitterThickness = 4.0f;
    static constexpr float kSplitterGrabPadding = 4.0f;
    static constexpr float kPanelHeaderHeight = 26.0f;
    static constexpr float kMinPanelWidth = 140.0f;
    static constexpr float kMinPanelHeight = 90.0f;
    static constexpr float kMinViewportWidth = 200.0f;

    static constexpr ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    static constexpr ImGuiWindowFlags kSplitterWindowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    // A clean sans-serif at UE-ish size; falls back to ImGui's built-in font if absent.
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
            if (io.Fonts->AddFontFromFileTTF(path, 15.0f) != nullptr)
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
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
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
            ImVec2(pos.x + width, pos.y + kPanelHeaderHeight),
            ImGui::GetColorU32(UEStyle::Title));
        drawList->AddLine(
            ImVec2(pos.x, pos.y + kPanelHeaderHeight - 1.0f),
            ImVec2(pos.x + width, pos.y + kPanelHeaderHeight - 1.0f),
            ImGui::GetColorU32(UEStyle::WindowBorder));

        const ImVec2 textPos(pos.x + 8.0f, pos.y + (kPanelHeaderHeight - ImGui::GetTextLineHeight()) * 0.5f);
        drawList->AddText(textPos, ImGui::GetColorU32(UEStyle::ForegroundHeader), name);

        ImGui::Dummy(ImVec2(width, kPanelHeaderHeight));
    }

    // Drag strip between two panes; mutates the adjacent pane size
    static void DrawSplitter(const char* id, ImVec2 pos, ImVec2 size, bool vertical, float& target, float sign)
    {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(id, nullptr, kSplitterWindowFlags);
        ImGui::PopStyleVar();

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

        // UE paints the seam blue under the cursor
        if (hovered || active)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(active ? UEStyle::Primary : UEStyle::PrimaryHover);
            if (vertical)
            {
                const float x = pos.x + size.x * 0.5f;
                drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), color, 2.0f);
            }
            else
            {
                const float y = pos.y + size.y * 0.5f;
                drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), color, 2.0f);
            }
        }

        ImGui::End();
    }

    static void DrawOutlinerBody(ResourceManager& resourceManager)
    {
        std::vector<ModelInstance>& instances = resourceManager.GetSceneInstances();
        for (int i = 0; i < static_cast<int>(instances.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(instances[static_cast<size_t>(i)].name.c_str(), s_selectedIndex == i))
            {
                s_selectedIndex = i;
            }
            ImGui::PopID();
        }
    }

    static void DrawDetailBody(ResourceManager& resourceManager)
    {
        std::vector<ModelInstance>& instances = resourceManager.GetSceneInstances();
        if (s_selectedIndex >= 0 && s_selectedIndex < static_cast<int>(instances.size()))
        {
            // Name only, per current scope; properties come later
            ImGui::TextUnformatted(instances[static_cast<size_t>(s_selectedIndex)].name.c_str());
        }
        else
        {
            ImGui::TextUnformatted("(no selection)");
        }
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

    // Layout state in pixels, mutated by the seams
    inline static float s_leftWidth = 220.0f;
    inline static float s_rightWidth = 300.0f;
    inline static float s_bottomHeight = 180.0f;
    inline static int s_selectedIndex = -1;
};

#endif
