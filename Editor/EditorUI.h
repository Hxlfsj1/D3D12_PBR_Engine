#ifndef EDITOR_UI_H
#define EDITOR_UI_H

#include "imgui.h"
#include "imgui_internal.h" // Input ownership and case-insensitive filtering.
#include "ResourceManager.h"
#include "ErrorLog.h"
#include "EditorSelection.h"
#include "EditorGizmo.h"
#include "EditorTransform.h"

#include "EditorUICenter.h"

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
    static void Initialize(float /*viewportWidth*/, float dpiScale = 1.0f)
    {
        EditorTheme::Apply(dpiScale);
        s_leftWidth = EditorTheme::Px(EditorTheme::LeftWidth);
        s_rightWidth = EditorTheme::Px(EditorTheme::RightWidth);
        s_bottomHeight = EditorTheme::Px(EditorTheme::BottomHeight);
    }
    static void UpdateScale(float dpiScale)
    {
        dpiScale = (std::clamp)(dpiScale, .75f, 3.0f);
        if (std::fabs(dpiScale - EditorTheme::Scale) < .01f) return;
        const float previous = EditorTheme::Scale;
        EditorTheme::Apply(dpiScale);
        const float ratio = EditorTheme::Scale / previous;
        s_leftWidth *= ratio; s_rightWidth *= ratio; s_bottomHeight *= ratio;
    }

    // Returns a quit request for the application to handle after completing the frame.
    static bool Draw(ResourceManager& resourceManager, EditorSelection& selection,
        EditorGizmo& gizmo, const EditorGizmo::CameraFrame& camera, EditorHistory& history)
    {
        auto& instances = resourceManager.GetSceneInstances();
        history.CommitInactiveTransform(instances, ImGui::GetActiveID());
        const auto& input = ImGui::GetIO();
        // InputText owns its native undo stack while editing (including the console).
        // Do not replay scene history during a drag or another active UI operation.
        if (input.KeyCtrl && !input.KeyAlt && !input.KeySuper && !input.WantTextInput &&
            !ImGui::IsAnyItemActive() && !gizmo.IsDragging() && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            const bool z = ImGui::IsKeyPressed(ImGuiKey_Z, false);
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (z && input.KeyShift))
                history.Redo(instances, selection);
            else if (z) history.Undo(instances, selection);
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!input.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F10, false)) s_showStyleGuide = !s_showStyleGuide;
        float menuHeight = 0.0f;
        const bool quitRequested = EditorUICenter::FileMenu(menuHeight);
        // WorkPos incorporates main-menu height on the next frame. Use this frame's
        // measured height so first display and font/viewport resizing don't overlap.
        const ImVec2 origin(viewport->Pos.x, viewport->Pos.y + menuHeight);
        const ImVec2 total(viewport->Size.x, MaxF(0.0f, viewport->Size.y - menuHeight));
        const float kSplitterThickness = EditorTheme::Px(EditorTheme::SplitterThickness);
        const float kSplitterGrabSize = EditorTheme::Px(EditorTheme::SplitterGrabSize);

        // Clamping keeps the centre (the game view) alive however far a seam is dragged
        const float maxSideWidth = MaxF(
            EditorTheme::Px(EditorTheme::MinPanelWidth),
            total.x - EditorTheme::Px(EditorTheme::MinPanelWidth) - EditorTheme::Px(EditorTheme::MinViewportWidth) - 2.0f * kSplitterThickness);
        s_leftWidth = ClampF(s_leftWidth, EditorTheme::Px(EditorTheme::MinPanelWidth), maxSideWidth);
        s_rightWidth = ClampF(s_rightWidth, EditorTheme::Px(EditorTheme::MinPanelWidth), maxSideWidth);
        s_bottomHeight = ClampF(
            s_bottomHeight,
            EditorTheme::Px(EditorTheme::MinPanelHeight),
            MaxF(EditorTheme::Px(EditorTheme::MinPanelHeight), total.y - EditorTheme::Px(EditorTheme::MinViewportWidth) - kSplitterThickness));

        const float columnsHeight = MaxF(EditorTheme::Px(EditorTheme::MinPanelHeight), total.y - s_bottomHeight - kSplitterThickness);
        const float rightX = origin.x + total.x - s_rightWidth;

        EditorUICenter::CreatePanel({"Outliner", "Outliner", EditorUICenter::Icon::Outliner}, origin, ImVec2(s_leftWidth, columnsHeight),
            [&] { DrawOutlinerBody(resourceManager, selection, history); });
        EditorUICenter::CreatePanel({"Detail", "Details", EditorUICenter::Icon::Details}, ImVec2(rightX, origin.y), ImVec2(s_rightWidth, columnsHeight),
            [&] { DrawDetailBody(resourceManager, selection, history); });
        EditorUICenter::CreatePanel({"Console", "Output Log", EditorUICenter::Icon::Console}, ImVec2(origin.x, origin.y + columnsHeight + kSplitterThickness),
            ImVec2(total.x, s_bottomHeight),
            [&] { DrawConsoleBody(); });

        // Splitter windows can rise above the fixed panels when focused.
        // Submission order alone does not control top-level window stacking in ImGui.
        // Bands are described by their CENTRE, anchored on the border the user can see:
        // the Outliner's right edge, the Detail's left edge, and the Console's top edge.
        const ImVec2 leftCentre(origin.x + s_leftWidth, origin.y + columnsHeight * 0.5f);
        const ImVec2 rightCentre(rightX, origin.y + columnsHeight * 0.5f);
        const ImVec2 bottomCentre(
            origin.x + total.x * 0.5f,
            origin.y + columnsHeight + kSplitterThickness);

        EditorUICenter::Splitter("##SplitLeft", leftCentre,
            ImVec2(kSplitterGrabSize, columnsHeight), true, s_leftWidth, 1.0f);
        EditorUICenter::Splitter("##SplitRight", rightCentre,
            ImVec2(kSplitterGrabSize, columnsHeight), true, s_rightWidth, -1.0f);
        EditorUICenter::Splitter("##SplitBottom", bottomCentre,
            ImVec2(total.x, kSplitterGrabSize), false, s_bottomHeight, -1.0f);

        EditorUICenter::StyleGuide(s_showStyleGuide);

        // Only a click that starts and ends in exposed scene space may pick a model.
        // The scene still fills the entire client area underneath these overlay panels.
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        const float grabHalf = kSplitterGrabSize * 0.5f;
        history.CommitInactiveTransform(instances, ImGui::GetActiveID());
        const bool gizmoConsumesMouse = gizmo.Draw(resourceManager.GetSceneInstances(), selection, camera,
            ImVec2(leftCentre.x + grabHalf, origin.y),
            ImVec2(rightCentre.x - grabHalf, bottomCentre.y - grabHalf), history);
        const bool inScene = mouse.x > leftCentre.x + grabHalf &&
            mouse.x < rightCentre.x - grabHalf && mouse.y >= origin.y &&
            mouse.y < bottomCentre.y - grabHalf;
        const bool canPick = !s_showStyleGuide && !gizmoConsumesMouse && inScene && !io.WantCaptureMouse &&
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
        return quitRequested;
    }

private:
    // windows.h defines min/max as macros (the project does not set NOMINMAX), so the
    // std:: equivalents cannot be used here without breaking the parse
    static constexpr float MaxF(float a, float b) { return a > b ? a : b; }
    static constexpr float ClampF(float value, float low, float high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    static void FinishRename(std::vector<ModelInstance>& instances, EditorHistory& history, bool cancel = false)
    {
        if (!cancel && s_renameObjectId != 0)
        {
            const std::string name(s_renameBuffer.data());
            const size_t first = name.find_first_not_of(" \t\r\n");
            if (first != std::string::npos)
                for (auto& instance : instances)
                    if (instance.editorId == s_renameObjectId)
                    {
                        history.Rename(instances, instance, name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1));
                        break;
                    }
        }
        s_renameObjectId = 0;
        s_focusRename = false;
    }

    static void DrawOutlinerBody(ResourceManager& resourceManager, EditorSelection& selection, EditorHistory& history)
    {
        auto& instances = resourceManager.GetSceneInstances();
        EditorUICenter::Search("outliner-filter", s_outlinerFilter, sizeof(s_outlinerFilter), "Search Outliner");
        EditorUICenter::ListHeader("Item Label");
        int visibleRow=0;
        for (auto& instance : instances)
        {
            if (s_renameObjectId != instance.editorId && !Matches(instance.name.c_str(), s_outlinerFilter)) continue;
            ImGui::PushID(static_cast<int>(instance.editorId));
            if (s_renameObjectId == instance.editorId)
            {
                const auto edit = EditorUICenter::Rename(s_renameBuffer, s_focusRename, visibleRow);
                if (edit.cancelled || edit.submitted || edit.deactivated) FinishRename(instances, history, edit.cancelled);
            }
            else
            {
                const auto row = EditorUICenter::ObjectRow(instance.name.c_str(), selection.SelectedId() == instance.editorId, visibleRow);
                if (row.clicked || row.rename)
                {
                    FinishRename(instances, history);
                    history.Select(instances, selection, instance.editorId);
                }
                if (row.rename)
                {
                    s_renameObjectId = instance.editorId;
                    s_renameBuffer.assign(instance.name.begin(), instance.name.end());
                    s_renameBuffer.resize((std::max)(s_renameBuffer.size() + 1, size_t{256}), '\0');
                    s_focusRename = true;
                }
            }
            ImGui::PopID();
            ++visibleRow;
        }
    }
    static bool Matches(const char* text, const char* filter)
    {
        return !*filter || ImStristr(text, nullptr, filter, nullptr) != nullptr;
    }

    static void DrawDetailBody(ResourceManager& resourceManager, const EditorSelection& selection, EditorHistory& history)
    {
        std::vector<ModelInstance>& instances = resourceManager.GetSceneInstances();
        for (ModelInstance& instance : instances)
        {
            if (instance.editorId == selection.SelectedId())
            {
                EditorUICenter::ObjectHeading(instance.name.c_str());
                EditorUICenter::Search("details-filter", s_detailsFilter, sizeof(s_detailsFilter), "Search Details");
                ImGui::PushID(static_cast<int>(instance.editorId));
                DrawTransform(instance, instances, history);
                ImGui::PopID();
                return;
            }
        }
        EditorUICenter::Muted("Select an object to view details.");
    }

    static void DrawTransformRow(const char* label, DirectX::XMFLOAT3& value,
        const char* xMeaning, const char* yMeaning, const char* zMeaning,
        ModelInstance& instance, std::vector<ModelInstance>& instances, EditorHistory& history,
        bool (*apply)(ModelInstance&, const DirectX::XMFLOAT3&), const char* error)
    {
        if (!Matches(label, s_detailsFilter) && !Matches("Transform", s_detailsFilter)) return;
        float values[] = { value.x, value.y, value.z };
        const char* hints[] = { xMeaning, yMeaning, zMeaning };
        EditorUICenter::VectorProperty(label, values, hints, [&](const EditorUICenter::Edit& edit)
        {
            if (edit.activated || edit.changed) history.BeginTransform(instances, instance, edit.id);
            if (edit.changed) s_transformError = apply(instance, { values[0], values[1], values[2] }) ? nullptr : error;
            if (edit.deactivated) history.CommitTransform(instances, edit.id);
        });
    }

    static void DrawTransform(ModelInstance& instance, std::vector<ModelInstance>& instances, EditorHistory& history)
    {
        if (s_transformObjectId != instance.editorId)
        {
            s_transformObjectId = instance.editorId;
            s_transformError = nullptr;
        }
        if (!EditorUICenter::Section("Transform")) return;

        auto location = EditorTransform::ToEditorAxes(instance.translation);
        auto rotation = EditorTransform::RotationDegrees(instance);
        auto scale = EditorTransform::ToEditorAxes(instance.scale);
        DrawTransformRow("Location", location, "World X: forward", "World Y: right", "World Z: up",
            instance, instances, history, EditorTransform::SetLocation, "Location must contain finite values.");
        DrawTransformRow("Rotation", rotation, "Roll (degrees)", "Pitch (degrees, positive nose-up)", "Yaw (degrees)",
            instance, instances, history, EditorTransform::SetRotation, "Rotation must contain finite values.");
        DrawTransformRow("Scale", scale, "X scale", "Y scale", "Z scale",
            instance, instances, history, EditorTransform::SetScale, "Scale must be finite, with magnitude >= 0.0001 on each axis.");
        if (s_transformError) EditorUICenter::Error(s_transformError);
    }

    inline static UINT s_renameObjectId = 0;
    inline static bool s_focusRename = false;
    inline static std::vector<char> s_renameBuffer;

    inline static UINT s_transformObjectId = 0;
    inline static const char* s_transformError = nullptr;

    static void DrawConsoleBody()
    {
        EditorUICenter::Search("log-filter", s_logFilter, sizeof(s_logFilter), "Search Output Log");
        std::string text;
        for (const auto& entry : ErrorLog::RecentEntries())
            if (Matches(entry.c_str(), s_logFilter))
            {
                text += entry;
                if (entry.empty() || entry.back() != '\n') text += '\n';
            }
        EditorUICenter::LogView(s_log, text);
    }
    inline static EditorUICenter::LogState s_log;
    inline static char s_outlinerFilter[128]{}, s_detailsFilter[128]{}, s_logFilter[128]{};
    inline static bool s_showStyleGuide = false;
    inline static float s_leftWidth = EditorTheme::LeftWidth, s_rightWidth = EditorTheme::RightWidth, s_bottomHeight = EditorTheme::BottomHeight;
    inline static bool s_sceneClickStarted = false;
    inline static ImVec2 s_sceneClickStart;
};

#endif
