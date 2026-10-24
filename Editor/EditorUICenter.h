#pragma once

#include "EditorTheme.h"
#include "EditorCameraSnapshot.h"
#include "../Resources/RenderDebugPass.h"
#include "imgui_internal.h"
#include <string>
#include <vector>
#include <cstring>

// The only creation/drawing layer for editor panels and controls. Functional panels
// supply IDs, values and callbacks; they must not supply colors or layout metrics.
namespace EditorUICenter
{
    using EditorTheme::Px;
    using EditorTheme::Color;
    enum class Icon { Outliner, Details, Console, Mesh, Search };
    struct Panel { const char* id; const char* title; Icon icon; };
    struct Edit { ImGuiID id; bool changed, activated, deactivated, submitted, cancelled; };
    struct Row { bool clicked, rename; };
    struct LogState { std::string text; ImGuiID windowId = 0; bool lineClick = false; };

    inline void RowBackground(ImDrawList* draw, const ImRect& rect, int row)
    { draw->AddRectFilled(rect.Min,rect.Max,Color(row%2 ? EditorColors::RowOdd : EditorColors::RowEven)); }
    inline void ListRowBackground(int row, float height)
    {
        const auto p=ImGui::GetCursorScreenPos(); const float halfGap=ImGui::GetStyle().ItemSpacing.y*.5f;
        RowBackground(ImGui::GetWindowDrawList(),ImRect(ImVec2(p.x,p.y-halfGap),
            ImVec2(p.x+ImGui::GetContentRegionAvail().x,p.y+height+halfGap)),row);
    }

    inline void DrawIcon(Icon icon, ImVec2 p, ImU32 color)
    {
        auto* d = ImGui::GetWindowDrawList();
        auto at = [&](float x, float y) { return ImVec2(p.x+Px(x), p.y+Px(y)); };
        if (icon == Icon::Mesh)
        {
            ImVec2 pts[] = { at(2,4), at(8,1), at(14,4), at(14,11), at(8,15), at(2,11) };
            d->AddPolyline(pts, 6, color, ImDrawFlags_Closed, Px(1));
            d->AddLine(pts[0], at(8,8), color); d->AddLine(pts[2], at(8,8), color); d->AddLine(at(8,8), pts[4], color);
        }
        else if (icon == Icon::Search)
        { d->AddCircle(at(6,6), Px(4), color, 16); d->AddLine(at(9,9), at(14,14), color, Px(1)); }
        else if (icon == Icon::Console)
        { d->AddLine(at(2,4),at(6,8),color); d->AddLine(at(6,8),at(2,12),color); d->AddLine(at(8,12),at(14,12),color); }
        else
            for (int i=0;i<3;++i)
            {
                const float y = 3.0f + static_cast<float>(i)*5.0f;
                d->AddRectFilled(at(2,y),at(4,y+2),color);
                d->AddLine(at(icon==Icon::Details?7.0f:6.0f,y+1),at(14,y+1),color);
            }
    }
    inline void Text(const char* text) { ImGui::TextUnformatted(text); }
    inline void Muted(const char* text,bool wrap=false)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Hover2);
        if(wrap)ImGui::TextWrapped("%s",text);else Text(text);
        ImGui::PopStyleColor();
    }
    inline void Error(const char* text)
    { ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::Error); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor(); }
    inline void Progress(float fraction, const char* overlay = nullptr)
    {
        fraction = std::isfinite(fraction) ? (std::clamp)(fraction, 0.0f, 1.0f) : 0.0f;
        char percent[16];
        ImFormatString(percent, IM_ARRAYSIZE(percent), "%d%%", static_cast<int>(fraction * 100.0f));
        ImGui::ProgressBar(fraction, ImVec2(-1, Px(EditorTheme::ProgressHeight)), overlay ? overlay : percent);
    }
    inline void LoadingContent(float fraction, const char* status)
    {
        ImGui::PushFont(EditorTheme::Bold); Text("Loading"); ImGui::PopFont();
        Progress(fraction);
        Muted(status ? status : "", true);
    }
    // Loading uses the same fonts and logical metrics as the editor. Only the card
    // width changes with the viewport; status text wraps rather than scaling down.
    inline void LoadingScreen(float fraction, const char* status)
    {
        const auto* viewport = ImGui::GetMainViewport();
        constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowPos(viewport->Pos); ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorColors::Background);
        ImGui::Begin("##StartupLoadingBackdrop", nullptr, flags);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(3);

        const float width = (std::min)(Px(EditorTheme::LoadingWidth),
            (std::max)(1.0f, viewport->Size.x - 2 * Px(EditorTheme::LoadingMargin)));
        const float contentWidth = (std::max)(1.0f, width - 2 * Px(EditorTheme::LoadingPadding));
        ImGui::PushFont(EditorTheme::Bold);
        const float titleHeight = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const float statusHeight = (std::max)(ImGui::GetTextLineHeight(),
            ImGui::CalcTextSize(status ? status : "", nullptr, false, contentWidth).y);
        // Explicit measurement keeps the first startup frame visible. An
        // auto-resizing window hides its initial frame while measuring contents.
        const float height = 2 * Px(EditorTheme::LoadingPadding) + titleHeight +
            Px(EditorTheme::ProgressHeight) + statusHeight + 2 * Px(EditorTheme::LoadingSpacing);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * .5f,
            viewport->Pos.y + viewport->Size.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(Px(EditorTheme::LoadingPadding), Px(EditorTheme::LoadingPadding)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(Px(EditorTheme::LoadingSpacing), Px(EditorTheme::LoadingSpacing)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(EditorTheme::LoadingRounding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, Px(1));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorColors::Panel);
        ImGui::PushStyleColor(ImGuiCol_Border, EditorColors::InputOutline);
        const bool visible = ImGui::Begin("##StartupLoadingCard", nullptr, flags);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (visible)
            LoadingContent(fraction, status);
        ImGui::End(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(4);
    }
    inline void Hint(const char* text) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",text); }
    inline void InputOutline()
    {
        const auto color = ImGui::IsItemActive() ? EditorColors::Primary : ImGui::IsItemHovered() ? EditorColors::Hover : EditorColors::InputOutline;
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),Color(color),Px(2),0,Px(1));
    }
    inline void PanelHeader(const Panel& panel, float width)
    {
        const auto p = ImGui::GetCursorScreenPos(); auto* d = ImGui::GetWindowDrawList();
        const float height = Px(EditorTheme::PanelHeader);
        d->AddRectFilled(p,ImVec2(p.x+width,p.y+height),Color(EditorColors::Title));
        const float tabWidth = (std::min)(width, ImGui::CalcTextSize(panel.title).x+Px(54));
        d->AddRectFilled(ImVec2(p.x+Px(4),p.y+Px(3)),ImVec2(p.x+tabWidth,p.y+height),Color(EditorColors::Panel),Px(3),ImDrawFlags_RoundCornersTop);
        DrawIcon(panel.icon,ImVec2(p.x+Px(12),p.y+Px(8)),Color(EditorColors::Foreground));
        d->AddText(ImVec2(p.x+Px(34),p.y+(height-ImGui::GetTextLineHeight())*.5f),Color(EditorColors::ForegroundHeader),panel.title);
        ImGui::Dummy(ImVec2(width,height));
    }
    template<class Body> void CreatePanel(const Panel& panel, ImVec2 position, ImVec2 size, Body body)
    {
        ImGui::SetNextWindowPos(position); ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,0));
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin(panel.id,nullptr,flags);
        PanelHeader(panel,size.x);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(Px(8),Px(6)));
        ImGui::BeginChild("##body",ImVec2(0,0),ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(Px(4),Px(4)));
        body();
        ImGui::PopStyleVar(); ImGui::EndChild(); ImGui::End(); ImGui::PopStyleVar(2);
    }
    inline bool Section(const char* title)
    {
        ImGui::PushFont(EditorTheme::Bold);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(Px(4),Px(5)));
        const bool open=ImGui::CollapsingHeader(title,ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleVar(); ImGui::PopFont(); return open;
    }
    inline void Search(const char* id, char* buffer, size_t size, const char* hint)
    {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search",hint,buffer,size);
        InputOutline(); ImGui::PopID();
    }
    inline void ListHeader(const char* label)
    {
        const auto p=ImGui::GetCursorScreenPos(); const float w=ImGui::GetContentRegionAvail().x;
        auto* d=ImGui::GetWindowDrawList();
        d->AddRectFilled(p,ImVec2(p.x+w,p.y+Px(24)),Color(EditorColors::Header));
        d->AddText(ImVec2(p.x+Px(26),p.y+Px(5)),Color(EditorColors::ForegroundHeader),label);
        ImGui::Dummy(ImVec2(w,Px(24)));
    }
    inline Row ObjectRow(const char* name, bool selected, int row)
    {
        ListRowBackground(row,Px(22));
        ImGui::PushStyleColor(ImGuiCol_Header,ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)?EditorColors::Primary:EditorColors::SelectInactive);
        const auto p=ImGui::GetCursorScreenPos(); const float width=ImGui::GetContentRegionAvail().x;
        const bool clicked=ImGui::Selectable("##instance",selected,ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0,Px(22)));
        const bool rename=ImGui::IsItemHovered()&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        DrawIcon(Icon::Mesh,ImVec2(p.x+Px(4),p.y+Px(3)),Color(EditorColors::Foreground));
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),ImVec2(p.x+Px(26),p.y+Px(4)),ImVec2(p.x+width,p.y+Px(22)),p.x+width,name,nullptr,nullptr);
        Hint(name); ImGui::PopStyleColor(); return {clicked,rename};
    }
    inline int ResizeText(ImGuiInputTextCallbackData* data)
    {
        auto& text=*static_cast<std::vector<char>*>(data->UserData);
        text.resize(static_cast<size_t>(data->BufSize));data->Buf=text.data();return 0;
    }
    inline Edit Rename(std::vector<char>& buffer, bool& focus, int row)
    {
        ListRowBackground(row,ImGui::GetFrameHeight());
        const auto id=ImGui::GetID("##rename");
        const bool cancel=ImGui::GetActiveID()==id&&ImGui::IsKeyPressed(ImGuiKey_Escape,false);
        if (focus) { ImGui::SetKeyboardFocusHere();focus=false; }
        ImGui::SetNextItemWidth(-1);
        const bool submitted=ImGui::InputText("##rename",buffer.data(),buffer.size(),ImGuiInputTextFlags_AutoSelectAll |
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,ResizeText,&buffer);
        Edit result{id,ImGui::IsItemEdited(),ImGui::IsItemActivated(),ImGui::IsItemDeactivated(),submitted,cancel};
        InputOutline(); return result;
    }
    inline void ObjectHeading(const char* name)
    {
        ImGui::PushFont(EditorTheme::Bold);Text(name);ImGui::PopFont();
        Muted("Model instance"); ImGui::Spacing();
    }
    inline bool BeginProperty(const char* label,bool wrapLabel=false)
    {
        ImGui::PushID(label);
        if (!ImGui::BeginTable("##property",2,ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings))
        { ImGui::PopID();return false; }
        ImGui::TableSetupColumn("Label",ImGuiTableColumnFlags_WidthFixed,Px(EditorTheme::LabelWidth));
        ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(0,Px(EditorTheme::RowHeight));ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if(wrapLabel)ImGui::TextWrapped("%s",label);else Text(label);
        ImGui::TableSetColumnIndex(1);return true;
    }
    inline void EndProperty() { ImGui::EndTable();ImGui::PopID(); }
    template<class OnEdit> void VectorProperty(const char* label, float* values, const char* const* hints, OnEdit onEdit)
    {
        if (!BeginProperty(label)) return;
        const float width=ImGui::GetContentRegionAvail().x;
        const bool stacked=width<Px(180);
        const float fieldWidth=stacked?width:(width-Px(8))/3;
        for(int axis=0;axis<3;++axis)
        {
            if(axis&&!stacked)ImGui::SameLine(0,Px(4));
            ImGui::PushID(axis);ImGui::BeginGroup();
            const auto p=ImGui::GetCursorScreenPos();const float h=ImGui::GetFrameHeight();
            const char* labels[]={"X","Y","Z"};auto* d=ImGui::GetWindowDrawList();
            d->AddRectFilled(p,ImVec2(p.x+Px(18),p.y+h),Color(EditorColors::Input),Px(2));
            d->AddRectFilled(p,ImVec2(p.x+Px(3),p.y+h),Color(EditorTheme::Axis(axis)),Px(1));
            d->AddText(ImVec2(p.x+Px(6),p.y+(h-ImGui::GetTextLineHeight())*.5f),Color(EditorColors::Foreground),labels[axis]);
            ImGui::Dummy(ImVec2(Px(18),h));ImGui::SameLine(0,0);
            ImGui::SetNextItemWidth((std::max)(Px(20),fieldWidth-Px(18)));
            const auto id=ImGui::GetID("##value");
            const bool changed=ImGui::InputFloat("##value",&values[axis],0,0,"%.3f",ImGuiInputTextFlags_AutoSelectAll);
            Edit edit{id,changed,ImGui::IsItemActivated(),ImGui::IsItemDeactivated(),false,false};
            InputOutline();if(hints)Hint(hints[axis]);onEdit(edit);
            ImGui::EndGroup();ImGui::PopID();
        }
        EndProperty();
    }
    inline bool FloatProperty(const char* label,float& value)
    {
        if(!BeginProperty(label))return false;ImGui::SetNextItemWidth(-1);
        bool changed=ImGui::InputFloat("##value",&value,0,0,"%.3f",ImGuiInputTextFlags_AutoSelectAll);
        InputOutline();EndProperty();return changed;
    }
    inline bool IntegerProperty(const char* label,int& value)
    {
        if(!BeginProperty(label,true))return false;ImGui::SetNextItemWidth(-1);
        bool changed=ImGui::InputInt("##value",&value,0,0,ImGuiInputTextFlags_AutoSelectAll);
        InputOutline();EndProperty();return changed;
    }
    inline void ReadOnlyProperty(const char* label,const char* value)
    {
        if(!BeginProperty(label,true))return;
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s",value);
        EndProperty();
    }
    inline bool BoolProperty(const char* label,bool& value)
    { if(!BeginProperty(label))return false;bool changed=ImGui::Checkbox("##value",&value);EndProperty();return changed; }
    inline bool ChoiceProperty(const char* label,int& value,const char* const* choices,int count)
    { if(!BeginProperty(label))return false;ImGui::SetNextItemWidth(-1);bool changed=ImGui::Combo("##value",&value,choices,count);InputOutline();if(value>=0&&value<count)Hint(choices[value]);EndProperty();return changed; }
    inline bool Button(const char* label,bool enabled=true)
    { ImGui::BeginDisabled(!enabled);bool clicked=ImGui::Button(label,ImVec2(0,Px(24)));ImGui::EndDisabled();return clicked; }
    inline void CameraTransformContent(const EditorCameraSnapshot& camera)
    {
        ImGui::PushFont(EditorTheme::Bold); Text("Camera transform"); ImGui::PopFont();
        Muted("Engine world (+Y up). Rotation: X pitch, Y yaw, Z roll (degrees).", true);
        const auto vector = [](const char* label, const std::array<float, 3>& value) {
            char text[192];
            ImFormatString(text, IM_ARRAYSIZE(text), "X %.4f   Y %.4f   Z %.4f", value[0], value[1], value[2]);
            ReadOnlyProperty(label, text);
        };
        vector("Location", camera.position);
        vector("Rotation", camera.rotationDegrees);
        vector("Scale", camera.scale);
        char text[96];
        ImFormatString(text, IM_ARRAYSIZE(text), "%.4f degrees", camera.verticalFovDegrees);
        ReadOnlyProperty("Vertical FOV", text);
        ImFormatString(text, IM_ARRAYSIZE(text), "%.4f / %.4f", camera.nearPlane, camera.farPlane);
        ReadOnlyProperty("Near / Far", text);
        ImFormatString(text, IM_ARRAYSIZE(text), "%u x %u", camera.viewportWidth, camera.viewportHeight);
        ReadOnlyProperty("Viewport", text);
        // A button keeps the submenu open while the user copies this live view.
        // Export floats at round-trip precision; the compact display is rounded.
        if (Button("Copy camera JSON##camera-copy-json"))
            ImGui::SetClipboardText(camera.ToJson().c_str());
        Muted("Copy the full camera data to share this view.", true);
    }
    inline bool WrappedMenuItem(const char* text, const char* stableId, bool selected,
        bool enabled = true, const char* shortcut = nullptr)
    {
        const auto& style = ImGui::GetStyle();
        const float statusWidth = ImGui::CalcTextSize("\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8").x;
        const float width = (std::max)(ImGui::GetFontSize(), ImGui::GetMainViewport()->WorkSize.x -
            2 * Px(EditorTheme::Padding) - 2 * style.WindowPadding.x -
            1.2f * ImGui::GetFontSize() - 2 * style.ItemSpacing.x - statusWidth);
        const char* end = text + std::strlen(text);
        std::string label;
        for (const char* begin = text; begin < end;)
        {
            const char* lineEnd = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), begin, end, width);
            if (lineEnd <= begin)
            {
                unsigned int character = 0;
                lineEnd = begin + (std::max)(1, ImTextCharFromUtf8(&character, begin, end));
            }
            label.append(begin, lineEnd);
            begin = lineEnd;
            while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n')) ++begin;
            if (begin < end) label += '\n';
        }
        label += "###";
        label += stableId;
        return ImGui::MenuItem(label.c_str(), shortcut, selected, enabled);
    }
    inline void DebugPassMenu(RenderDebugPass& pass, RenderPassView& view,
        const RenderPassAvailability& available, bool accumulatedReady = false)
    {
        NormalizeRenderPassView(view, available);
        NormalizeRenderDebugPass(pass, available);
        if (view.pass != RenderPassId::Default) pass = RenderDebugPass::Default;
        if (ImGui::BeginMenu("Pass##debug-pass-menu"))
        {
            if (ImGui::MenuItem("\xe9\xbb\x98\xe8\xae\xa4##pass-view-default", nullptr,
                view.pass == RenderPassId::Default && pass == RenderDebugPass::Default))
                { view = {}; pass = RenderDebugPass::Default; }
            ImGui::Separator();
            for (const auto& info : RenderPassCatalog)
            {
                if (info.pass == RenderPassId::Default || (!info.passOutput && !accumulatedReady)) continue;
                const std::string label = std::string(info.label) + "##pass-view-stage-" + info.id;
                if (!IsRenderPassAvailable(info.pass, available))
                {
                    ImGui::MenuItem(label.c_str(), "\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8", false, false);
                    continue;
                }
                if (!ImGui::BeginMenu(label.c_str())) continue;
                const auto select = [&](RenderPassResult result, RenderPassChannel channel = RenderPassChannel::Albedo,
                    RenderPassGISource giSource = RenderPassGISource::Default) {
                    view = {info.pass, result, channel, giSource};
                    pass = RenderDebugPass::Default;
                };
                if (info.pass == RenderPassId::GI)
                {
                    const char* defaultLabel = IsGlobalGIAvailable(available) ?
                        "\xe9\xbb\x98\xe8\xae\xa4\xef\xbc\x88SSGI + Mesh SDF VXGI + Global SDF GI\xef\xbc\x89" : IsWorldGIAvailable(available) ?
                        "\xe9\xbb\x98\xe8\xae\xa4\xef\xbc\x88SSGI + Mesh SDF VXGI\xef\xbc\x89" :
                        "\xe9\xbb\x98\xe8\xae\xa4\xef\xbc\x88SSGI\xef\xbc\x89";
                    if (WrappedMenuItem(defaultLabel, "pass-view-gi-default",
                        view.pass == info.pass && view.result == RenderPassResult::PassOutput &&
                        view.giSource == RenderPassGISource::Default))
                        select(RenderPassResult::PassOutput);
                    if (WrappedMenuItem("SSGI", "pass-view-gi-ssgi",
                        view.pass == info.pass && view.result == RenderPassResult::PassOutput &&
                        view.giSource == RenderPassGISource::SSGIOnly))
                        select(RenderPassResult::PassOutput, RenderPassChannel::Albedo, RenderPassGISource::SSGIOnly);
                    if (WrappedMenuItem("SSGI + Mesh SDF VXGI", "pass-view-gi-mesh",
                        view.pass == info.pass && view.result == RenderPassResult::PassOutput &&
                        view.giSource == RenderPassGISource::SSGIAndMeshSDF, IsWorldGIAvailable(available),
                        IsWorldGIAvailable(available) ? nullptr : "\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8"))
                        select(RenderPassResult::PassOutput, RenderPassChannel::Albedo, RenderPassGISource::SSGIAndMeshSDF);
                    ImGui::Separator();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(160));
                    Muted(available.antiAliasing ?
                        "\xe7\xba\xaf GI \xe8\xbe\x93\xe5\x87\xba\xef\xbc\x9a\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf\xe5\xb7\xb2\xe5\x90\xaf\xe7\x94\xa8" :
                        "\xe7\xba\xaf GI \xe8\xbe\x93\xe5\x87\xba\xef\xbc\x9a\xe6\x8a\x97\xe9\x94\xaf\xe9\xbd\xbf\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8", true);
                    ImGui::PopTextWrapPos();
                }
                else if (info.pass == RenderPassId::GBuffer)
                {
                    Muted("\xe6\x9c\xac\xe6\xad\xa5\xe7\xbb\x93\xe6\x9e\x9c");
                    for (const auto& channel : RenderPassChannels)
                    {
                        const std::string channelLabel = std::string(channel.label) + "##pass-view-channel-" + channel.id;
                        if (ImGui::MenuItem(channelLabel.c_str(), nullptr,
                            view.pass == info.pass && view.channel == channel.channel))
                            select(RenderPassResult::PassOutput, channel.channel);
                    }
                }
                else if (info.passOutput && ImGui::MenuItem(
                    "\xe6\x9c\xac\xe6\xad\xa5\xe7\xbb\x93\xe6\x9e\x9c##pass-view-output", nullptr,
                    view.pass == info.pass && view.result == RenderPassResult::PassOutput))
                    select(RenderPassResult::PassOutput);
                if (info.pass != RenderPassId::GI && accumulatedReady && info.accumulated && ImGui::MenuItem(
                    "\xe6\x88\xaa\xe8\x87\xb3\xe6\x9c\xac\xe6\xad\xa5##pass-view-accumulated", nullptr,
                    view.pass == info.pass && view.result == RenderPassResult::Accumulated))
                    select(RenderPassResult::Accumulated);
                if (info.pass == RenderPassId::Skybox || info.pass == RenderPassId::Transparent)
                {
                    ImGui::Separator();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(160));
                    Muted("\xe6\x9c\xac\xe6\xad\xa5\xe9\xa2\x9c\xe8\x89\xb2\xe5\x8f\x98\xe5\x8c\x96\xef\xbc\x9a\xe6\xa9\x99\xe8\x89\xb2\xe4\xb8\xba\xe5\x8f\x98\xe4\xba\xae\xef\xbc\x8c\xe8\x93\x9d\xe8\x89\xb2\xe4\xb8\xba\xe5\x8f\x98\xe6\x9a\x97\xe3\x80\x82", true);
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
    }
    inline bool MainMenu(float& height, const EditorCameraSnapshot& camera,
        RenderDebugPass& pass, RenderPassView& view, const RenderPassAvailability& available,
        bool accumulatedReady = false)
    {
        NormalizeRenderPassView(view, available);
        NormalizeRenderDebugPass(pass, available);
        if (view.pass != RenderPassId::Default) pass = RenderDebugPass::Default;
        bool quit = false; height = 0;
        if (ImGui::BeginMainMenuBar())
        {
            height = ImGui::GetWindowSize().y;
            if (ImGui::BeginMenu("File")) { quit = ImGui::MenuItem("quit"); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Edit"))
            {
                const auto* viewport = ImGui::GetMainViewport();
                const float width = (std::min)(Px(EditorTheme::CameraPopupWidth),
                    (std::max)(1.0f, viewport->WorkSize.x - 2 * Px(EditorTheme::CameraPopupMargin)));
                const float heightLimit = (std::max)(1.0f,
                    viewport->WorkSize.y - 2 * Px(EditorTheme::CameraPopupMargin));
                ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, heightLimit));
                if (ImGui::BeginMenu("\xe7\x9b\xb8\xe6\x9c\xba##camera-transform-menu"))
                {
                    CameraTransformContent(camera);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug"))
            {
                DebugPassMenu(pass, view, available, accumulatedReady);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        return quit;
    }
    inline bool MainMenu(float& height, const EditorCameraSnapshot& camera)
    {
        RenderDebugPass pass = RenderDebugPass::Default;
        RenderPassView view;
        return MainMenu(height, camera, pass, view, RenderPassAvailability{});
    }
    inline void Splitter(const char* id, ImVec2 centre,ImVec2 size,bool vertical,float& value,float direction)
    {
        const ImVec2 p(centre.x-size.x*.5f,centre.y-size.y*.5f);
        ImGui::SetNextWindowPos(p);ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,ImVec2(1,1));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::Begin(id,nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoNavFocus);
        ImGui::PopStyleVar(2);ImGui::InvisibleButton("##grab",size);
        if(ImGui::IsItemHovered()||ImGui::IsItemActive())
        {
            ImGui::SetMouseCursor(vertical?ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsItemActive())value+=(vertical?ImGui::GetIO().MouseDelta.x:ImGui::GetIO().MouseDelta.y)*direction;
            auto* d=ImGui::GetWindowDrawList();
            d->AddLine(vertical?ImVec2(centre.x,p.y):ImVec2(p.x,centre.y),vertical?ImVec2(centre.x,p.y+size.y):ImVec2(p.x+size.x,centre.y),Color(EditorColors::Primary),Px(2));
        }
        ImGui::End();
    }
    inline int SelectLogLine(ImGuiInputTextCallbackData* data)
    {
        auto& state=*static_cast<LogState*>(data->UserData);
        const auto& io=ImGui::GetIO();
        const bool hovered=ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        // Commit line selection on release so ordinary text dragging still works.
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            state.lineClick=hovered && io.MouseClickedCount[0]==1 && !io.KeyShift && !io.KeyCtrl;
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if(state.lineClick && hovered &&
                io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold*io.MouseDragThreshold)
            {
                int start=data->CursorPos, end=start;
                while(start>0 && data->Buf[start-1]!='\n')--start;
                while(end<data->BufTextLen && data->Buf[end]!='\n')++end;
                if(end<data->BufTextLen)++end; // Include the line break when copying.
                data->SelectionStart=start; data->SelectionEnd=end;
                // Keep the cursor at the clicked position to avoid horizontal scroll jumps.
            }
            state.lineClick=false;
        }
        return 0;
    }
    inline void LogView(LogState& state,const std::string& source,float height=0)
    {
        const auto id=ImGui::GetID("##log");auto* old=ImGui::FindWindowByID(state.windowId);
        const bool bottom=!old||old->Scroll.y>=old->ScrollMax.y-1;bool changed=false;
        if(ImGui::GetActiveID()!=id){changed=state.text!=source;if(changed)state.text=source;state.lineClick=false;}
        auto* parent=ImGui::GetCurrentWindow();int children=parent->DC.ChildWindows.Size;
        ImGui::PushFont(EditorTheme::Mono);ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0,0,0,0));
        const float lineHeight=ImGui::GetTextLineHeight();
        auto size=ImGui::GetContentRegionAvail();if(height>0)size.y=height;
        ImGui::InputTextMultiline("##log",state.text.data(),state.text.size()+1,size,
            ImGuiInputTextFlags_ReadOnly|ImGuiInputTextFlags_CallbackAlways,SelectLogLine,&state);
        ImGui::PopStyleColor();ImGui::PopFont();
        if(parent->DC.ChildWindows.Size>children){auto* child=parent->DC.ChildWindows.back();state.windowId=child->ID;
            // Parent draw commands sit behind the text child's selection/text commands.
            // Use this frame's child position and scroll, including wheel and scrollbar moves.
            auto* draw=parent->DrawList;const auto clip=child->InnerClipRect;
            const float top=child->Pos.y+child->WindowPadding.y+child->DecoOuterSizeY1-child->Scroll.y+ImGui::GetStyle().FramePadding.y;
            const int first=(std::max)(0,static_cast<int>(std::floor((clip.Min.y-top)/lineHeight)));
            const int lineCount=static_cast<int>(std::count(state.text.begin(),state.text.end(),'\n'))+
                (!state.text.empty()&&state.text.back()!='\n'?1:0);
            draw->PushClipRect(clip.Min,clip.Max,true);
            for(int line=first;line<lineCount && top+line*lineHeight<clip.Max.y;++line)
                RowBackground(draw,ImRect(ImVec2(clip.Min.x,top+line*lineHeight),
                    ImVec2(clip.Max.x,top+(line+1)*lineHeight)),line);
            draw->PopClipRect();
            if(changed&&bottom&&!ImGui::IsItemActive())ImGui::SetScrollY(child,child->DC.CursorMaxPos.y-child->DC.CursorStartPos.y);}
    }
    inline void StyleGuide(bool& open)
    {
        if(!open)return;
        static double loadingPreviewUntil = 0;
        static RenderDebugPass debugPass = RenderDebugPass::Default;
        static RenderPassView passView;
        static RenderPassAvailability available = [] {
            RenderPassAvailability value;
            value.deferred = true;
            value.ssgi = true;
            value.vxgi = true;
            value.globalSDF = true;
            value.zPrepass = true;
            value.hbao = true;
            value.antiAliasing = true;
            value.motionVectors = true;
            value.transparent = true;
            return value;
        }();
        const auto* viewport = ImGui::GetMainViewport();
        const float margin = Px(EditorTheme::Padding);
        const ImVec2 limit((std::max)(1.0f, viewport->WorkSize.x - 2 * margin),
            (std::max)(1.0f, viewport->WorkSize.y - 2 * margin));
        const auto* guide = ImGui::FindWindowByName("UI Creation Center - UE 5.4.4");
        const ImVec2 size((std::min)(guide ? guide->Size.x : Px(540), limit.x),
            (std::min)(guide ? guide->Size.y : Px(640), limit.y));
        const ImVec2 minimum(viewport->WorkPos.x + margin, viewport->WorkPos.y + margin);
        const ImVec2 position(guide ? guide->Pos.x : minimum.x, guide ? guide->Pos.y : minimum.y);
        ImGui::SetNextWindowPos(ImVec2(
            (std::max)(minimum.x, (std::min)(position.x, viewport->WorkPos.x + viewport->WorkSize.x - size.x - margin)),
            (std::max)(minimum.y, (std::min)(position.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y - margin))));
        ImGui::SetNextWindowSizeConstraints(ImVec2(1, 1), limit);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        if(ImGui::Begin("UI Creation Center - UE 5.4.4",&open,ImGuiWindowFlags_MenuBar))
        {
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("Debug"))
                {
                    DebugPassMenu(debugPass, passView, available, true);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            ObjectHeading("Editor component template");
            Muted("The same components used by every editor panel.", true);
            static char query[128]{};Search("guide",query,sizeof(query),"Search field");
            if(Section("Outliner")){ListHeader("Item Label");ImGui::PushID("example1");ObjectRow("StaticMesh",false,0);ImGui::PopID();
                ImGui::PushID("example2");ObjectRow("SelectedMesh",true,1);ImGui::PopID();
                ImGui::PushID("example3");ObjectRow("AnotherMesh",false,2);ImGui::PopID();}
            if(Section("Output Log")){static LogState log;LogView(log,"Click a line to select it.\nCtrl+C copies the selection.\nDrag to select part of the text.\n",Px(90));}
            if(Section("Transform")){static float t[3]{},r[3]{},s[3]{1,1,1};auto edit=[](Edit){};
                VectorProperty("Location",t,nullptr,edit);VectorProperty("Rotation",r,nullptr,edit);VectorProperty("Scale",s,nullptr,edit);}
            if(Section("Camera transform"))
            {
                const auto sample = EditorCameraSnapshot::Capture(
                    Camera({-.3f, 1.05f, -1.1f}, {0, 1, 0}, 116.565f, 68.542f), 1707, 1067);
                CameraTransformContent(sample);
            }
            if(Section("Debug pass"))
            {
                BoolProperty("SSGI enabled", available.ssgi);
                BoolProperty("VXGI enabled", available.vxgi);
                BoolProperty("GDF enabled", available.globalSDF);
                BoolProperty("Deferred enabled", available.deferred);
                BoolProperty("HBAO enabled", available.hbao);
                BoolProperty("AA enabled", available.antiAliasing);
                NormalizeRenderDebugPass(debugPass, available);
                NormalizeRenderPassView(passView, available);
                Muted("Use Debug > Pass above to inspect selected and disabled menu items.", true);
            }
            if(Section("Properties")){static float roughness=.5f;static bool visible=true;static int mode=0,sampleCount=256;
                const char* modes[]={"Opaque","Masked","Translucent"};FloatProperty("Roughness",roughness);BoolProperty("Visible",visible);ChoiceProperty("Blend Mode",mode,modes,3);
                IntegerProperty("Sample count",sampleCount);
                ReadOnlyProperty("Status","Read-only values wrap at narrow widths.");
                Muted("Wrapped muted text stays readable in narrow panels.",true);
                Button("Apply");ImGui::SameLine();Button("Disabled",false);}
            if(Section("Loading"))
            {
                LoadingContent(.63f, "Preparing VXGI probes: 63 / 100 batches");
                if(Button("Preview startup screen")) loadingPreviewUntil = ImGui::GetTime() + 3.0;
                Muted("Preview lasts three seconds. Long loading details wrap at narrow widths.",true);
            }
            Muted("F10 closes this template. Hover / focus controls to inspect states.", true);
        }
        ImGui::End();
        if(ImGui::GetTime() < loadingPreviewUntil)
            LoadingScreen(.63f, "Preparing VXGI probes: 63 / 100 batches");
    }
}
