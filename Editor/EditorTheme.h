#pragma once
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <filesystem>

// UE 5.4.4 Starship reference: see STYLE_REFERENCE.md for source locations.
// Colors are sRGB bytes, as expected by the current ImGui UNORM render target.
namespace EditorColors
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
    inline const ImVec4 RowEven = Hex(0x1D1D1D);
    inline const ImVec4 RowOdd = Hex(0x242424);
    inline const ImVec4 Error = Hex(0xEF3535);
    inline const ImVec4 Warning = Hex(0xFFB800);
}


namespace EditorTheme
{
    inline float Scale = 1.0f;
    inline ImFont* Regular = nullptr;
    inline ImFont* Bold = nullptr;
    inline ImFont* Mono = nullptr;
    inline float Px(float value) { return std::round(value * Scale); }
    inline constexpr float FontSize = 13.0f;
    inline constexpr float PanelHeader = 28.0f;
    inline constexpr float RowHeight = 24.0f;
    inline constexpr float Spacing = 4.0f;
    inline constexpr float Padding = 8.0f;
    inline constexpr float LabelWidth = 86.0f;
    inline constexpr float LeftWidth = 250.0f, RightWidth = 390.0f, BottomHeight = 190.0f;
    inline constexpr float MinPanelWidth = 180.0f, MinPanelHeight = 100.0f, MinViewportWidth = 240.0f;
    inline constexpr float SplitterThickness = 4.0f, SplitterGrabSize = 24.0f;
    inline constexpr ImU32 GizmoSelected = IM_COL32(255,215,70,255);
    inline constexpr ImU32 GizmoCenter = IM_COL32(225,225,225,255);
    inline constexpr ImU32 GizmoOutline = IM_COL32(40,40,40,255);
    inline ImU32 GizmoAxis(int axis)
    {
        const ImU32 colors[] = { IM_COL32(235,70,70,255), IM_COL32(85,205,85,255), IM_COL32(75,145,255,255) };
        return colors[axis];
    }
    inline ImU32 GizmoPlane(bool selected) { return selected ? IM_COL32(255,215,70,65) : IM_COL32(180,180,180,25); }
    inline ImU32 Color(const ImVec4& color) { return ImGui::GetColorU32(color); }
    inline ImVec4 Axis(int axis)
    {
        const ImVec4 colors[] = { EditorColors::Hex(0xB83C3C), EditorColors::Hex(0x438A43), EditorColors::Hex(0x326FA8) };
        return colors[axis];
    }
    inline std::string FontPath(const char* filename)
    {
        std::filesystem::path path = std::filesystem::path("Editor/Assets/Fonts") / filename;
        if (!std::filesystem::exists(path))
        {
            wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            path = std::filesystem::path(exe).parent_path() / "EditorAssets/Fonts" / filename;
        }
        if (!std::filesystem::exists(path)) return {};
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }
    inline ImFont* LoadFont(const char* filename, bool mergeCjk)
    {
        auto& io = ImGui::GetIO();
        const auto path = FontPath(filename);
        ImFont* font = nullptr;
        if (!path.empty())
            font = io.Fonts->AddFontFromFileTTF(path.c_str(), Px(FontSize));
        if (!font) { ImFontConfig config; config.SizePixels = Px(FontSize); font = io.Fonts->AddFontDefault(&config); }
        if (mergeCjk && GetFileAttributesA("C:/Windows/Fonts/msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig config; config.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", Px(FontSize), &config, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        return font;
    }
    // Call before NewFrame. Window width never changes typography or UI density.
    inline void Apply(float dpiScale = 1.0f)
    {
        Scale = (std::clamp)(dpiScale, 0.75f, 3.0f);
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        ImVec4* c = style.Colors;

        // Surfaces
        c[ImGuiCol_WindowBg] = EditorColors::Panel;
        c[ImGuiCol_ChildBg] = EditorColors::Background;
        c[ImGuiCol_PopupBg] = EditorColors::Panel;
        c[ImGuiCol_MenuBarBg] = EditorColors::Title;
        c[ImGuiCol_Border] = EditorColors::WindowBorder;
        c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

        // Recessed fields (text input, sliders, checkboxes)
        c[ImGuiCol_FrameBg] = EditorColors::Input;
        c[ImGuiCol_FrameBgHovered] = EditorColors::Recessed;
        c[ImGuiCol_FrameBgActive] = EditorColors::Header;

        // Title bars (panels draw their own header strip; these cover future popups)
        c[ImGuiCol_TitleBg] = EditorColors::Title;
        c[ImGuiCol_TitleBgActive] = EditorColors::Recessed;
        c[ImGuiCol_TitleBgCollapsed] = EditorColors::Title;

        // Scrollbars: thin and dark, with no bright track
        c[ImGuiCol_ScrollbarBg] = EditorColors::Background;
        c[ImGuiCol_ScrollbarGrab] = EditorColors::Secondary;
        c[ImGuiCol_ScrollbarGrabHovered] = EditorColors::Hover;
        c[ImGuiCol_ScrollbarGrabActive] = EditorColors::Hover2;

        // Text
        c[ImGuiCol_Text] = EditorColors::Foreground;
        c[ImGuiCol_TextDisabled] = EditorColors::Hover2;
        c[ImGuiCol_TextLink] = EditorColors::Primary;
        c[ImGuiCol_TextSelectedBg] = ImVec4(EditorColors::Primary.x, EditorColors::Primary.y, EditorColors::Primary.z, 0.35f);
        c[ImGuiCol_InputTextCursor] = EditorColors::ForegroundHover;

        // List rows / tree nodes / selectables: transparent when idle,
        // UE Hover on hover, UE SelectInactive when selected
        c[ImGuiCol_Header] = EditorColors::SelectInactive;
        c[ImGuiCol_HeaderHovered] = EditorColors::Hover;
        c[ImGuiCol_HeaderActive] = EditorColors::PrimaryPress;
        c[ImGuiCol_TreeLines] = EditorColors::InputOutline;

        // Buttons (UE secondary style)
        c[ImGuiCol_Button] = EditorColors::Secondary;
        c[ImGuiCol_ButtonHovered] = EditorColors::Hover;
        c[ImGuiCol_ButtonActive] = EditorColors::PrimaryPress;

        // Selection widgets
        c[ImGuiCol_CheckMark] = EditorColors::Primary;
        c[ImGuiCol_CheckboxSelectedBg] = EditorColors::Primary;
        c[ImGuiCol_SliderGrab] = EditorColors::Hover2;
        c[ImGuiCol_SliderGrabActive] = EditorColors::PrimaryHover;

        // Tabs (unused while dockless; kept consistent in case tab bars return)
        c[ImGuiCol_Tab] = EditorColors::Title;
        c[ImGuiCol_TabHovered] = EditorColors::Header;
        c[ImGuiCol_TabSelected] = EditorColors::Panel;

        // Separators / seams
        c[ImGuiCol_Separator] = EditorColors::WindowBorder;
        c[ImGuiCol_SeparatorHovered] = EditorColors::PrimaryHover;
        c[ImGuiCol_SeparatorActive] = EditorColors::Primary;

        // Tables
        c[ImGuiCol_TableHeaderBg] = EditorColors::Header;
        c[ImGuiCol_TableBorderStrong] = EditorColors::WindowBorder;
        c[ImGuiCol_TableBorderLight] = EditorColors::Recessed;
        c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);

        // Misc
        c[ImGuiCol_NavCursor] = EditorColors::Primary;
        c[ImGuiCol_DragDropTarget] = EditorColors::Primary;
        c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // panels are not grip-resizable
        c[ImGuiCol_ResizeGripHovered] = EditorColors::Hover;
        c[ImGuiCol_ResizeGripActive] = EditorColors::Primary;
        c[ImGuiCol_UnsavedMarker] = EditorColors::Warning;

        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.PopupRounding = 4.0f;
        style.FrameRounding = 2.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 0.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;

        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(7.0f, 3.0f);
        style.CellPadding = ImVec2(4.0f, 2.0f);
        style.ItemSpacing = ImVec2(4.0f, 2.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
        style.IndentSpacing = 18.0f;
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 10.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.AntiAliasedLines = true;
        style.AntiAliasedFill = true;


        c[ImGuiCol_Border] = EditorColors::InputOutline;
        c[ImGuiCol_FrameBgHovered] = EditorColors::Input;
        c[ImGuiCol_FrameBgActive] = EditorColors::Input;
        c[ImGuiCol_Header] = EditorColors::Header;
        style.ScaleAllSizes(Scale);
        auto& io = ImGui::GetIO();
        io.FontDefault = nullptr;
        io.Fonts->ClearFonts();
        Regular = LoadFont("Roboto-Regular.ttf", true);
        Bold = LoadFont("Roboto-Bold.ttf", true);
        Mono = Regular;
        if (GetFileAttributesA("C:/Windows/Fonts/consola.ttf") != INVALID_FILE_ATTRIBUTES)
            Mono = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", Px(FontSize));
        io.FontDefault = Regular;
    }
}
