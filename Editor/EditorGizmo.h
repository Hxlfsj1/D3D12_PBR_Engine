#pragma once

#include "imgui.h"
#include "EditorSelection.h"
#include "EditorTransform.h"
#include "EditorHistory.h"
#include "SceneObject.h"
#include <algorithm>
#include <cmath>

// Geometry is projected into client coordinates and drawn by the existing ImGui DX12
// backend. No new shader, GPU picking buffer, descriptor heap or scene material.
class EditorGizmo
{
public:
    enum class Mode { Translate, Rotate, Scale };
    struct CameraFrame
    {
        DirectX::XMFLOAT4X4 viewProjectionGpu, viewGpu, projectionGpu;
    };
    bool IsDragging() const { return m_axis >= 0; }

    bool Draw(std::vector<ModelInstance>& instances, EditorSelection& selection,
        const CameraFrame& camera, ImVec2 clipMin, ImVec2 clipMax, EditorHistory& history)
    {
        using namespace DirectX;
        ImGuiIO& io = ImGui::GetIO();
        ModelInstance* object = Find(instances, selection.SelectedId());
        const bool wasDragging = IsDragging();
        const ImVec2 screenOrigin = ImGui::GetMainViewport()->Pos;
        const bool invalidViewport = io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0 ||
            clipMax.x <= clipMin.x || clipMax.y <= clipMin.y;
        if (wasDragging && (!object || object->editorId != m_objectId || io.AppFocusLost || invalidViewport ||
            m_dragScreenSize.x != io.DisplaySize.x || m_dragScreenSize.y != io.DisplaySize.y ||
            m_dragScreenOrigin.x != screenOrigin.x || m_dragScreenOrigin.y != screenOrigin.y))
        {
            Cancel(instances);
            return true;
        }
        if (!object || invalidViewport) return wasDragging;
        if (!wasDragging && !io.WantCaptureKeyboard && !ImGui::IsAnyItemActive() &&
            !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_mode = Mode::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_mode = Mode::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_mode = Mode::Scale;
        }

        const auto vp = XMMatrixTranspose(XMLoadFloat4x4(&camera.viewProjectionGpu));
        const auto view = XMMatrixTranspose(XMLoadFloat4x4(&camera.viewGpu));
        const auto projection = XMMatrixTranspose(XMLoadFloat4x4(&camera.projectionGpu));
        const XMVECTOR origin = XMLoadFloat3(&object->translation);
        ImVec2 centre;
        if (!Project(origin, vp, screenOrigin, io.DisplaySize, centre))
        {
            if (wasDragging) Cancel(instances);
            return wasDragging;
        }
        const float depth = XMVectorGetZ(XMVector3TransformCoord(origin, view));
        const float scale = 2.0f * depth / (XMVectorGetY(projection.r[1]) * io.DisplaySize.y);
        const float radius = scale * 85.0f;
        if (!std::isfinite(radius) || radius <= 0) return wasDragging;
        if (m_mode == Mode::Scale)
            return DrawScale(instances, *object, selection, vp, screenOrigin, centre, radius, clipMin, clipMax, history);

        constexpr int segments = 96;
        ImVec2 rings[3][segments + 1]{};
        bool valid[3][segments + 1]{};
        ImVec2 ends[3]{};
        bool axisValid[3]{};
        float bestDistance = 9.0f;
        int hovered = -1;
        float hitAngle = 0;
        const bool inScene = io.MousePos.x >= clipMin.x && io.MousePos.x < clipMax.x &&
            io.MousePos.y >= clipMin.y && io.MousePos.y < clipMax.y;
        const bool canStart = inScene && !io.WantCaptureMouse && !ImGui::IsAnyItemActive() &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.AppFocusLost;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (m_mode == Mode::Translate)
            {
                axisValid[axis] = Project(origin + Axis(axis) * radius, vp, screenOrigin, io.DisplaySize, ends[axis]);
                // An axis pointing straight at the eye has no useful screen-space drag.
                axisValid[axis] &= Length(Sub(ends[axis], centre)) > 16.0f;
                float t;
                const float distance = DistanceToSegment(io.MousePos, centre, ends[axis], t);
                if (canStart && axisValid[axis] && t > 0.16f && distance < bestDistance)
                {
                    hovered = axis;
                    bestDistance = distance;
                }
            }
            else
            {
                for (int i = 0; i <= segments; ++i)
                {
                    const float angle = XM_2PI * i / segments;
                    valid[axis][i] = Project(origin + RingVector(axis, angle) * radius,
                        vp, screenOrigin, io.DisplaySize, rings[axis][i]);
                    if (i == 0 || !valid[axis][i - 1] || !valid[axis][i]) continue;
                    float t;
                    const float distance = DistanceToSegment(io.MousePos, rings[axis][i - 1], rings[axis][i], t);
                    if (canStart && distance < bestDistance)
                    {
                        hovered = axis;
                        hitAngle = XM_2PI * (i - 1 + t) / segments;
                        bestDistance = distance;
                    }
                }
            }
        }

        bool consumed = wasDragging || hovered >= 0;
        if (!wasDragging && hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_axis = hovered;
            m_objectId = object->editorId;
            m_startTranslation = object->translation;
            m_startRotation = object->rotation;
            m_startScale = object->scale;
            m_startMouse = io.MousePos;
            m_lastAngle = m_accumulatedAngle = 0;
            XMStoreFloat4x4(&m_dragInverseVp, XMMatrixInverse(nullptr, vp));
            m_dragScreenOrigin = screenOrigin;
            m_dragScreenSize = io.DisplaySize;
            // Keep a fixed screen derivative for axes/rings seen nearly edge-on.
            if (m_mode == Mode::Translate)
            {
                m_screenDirection = Sub(ends[hovered], centre);
                m_unitsPerPixel = radius / Length(m_screenDirection);
                m_screenDirection = Mul(m_screenDirection, 1.0f / Length(m_screenDirection));
            }
            else
            {
                ImVec2 p0 = centre, p1 = centre;
                Project(origin + RingVector(hovered, hitAngle - 0.02f) * radius, vp, screenOrigin, io.DisplaySize, p0);
                Project(origin + RingVector(hovered, hitAngle + 0.02f) * radius, vp, screenOrigin, io.DisplaySize, p1);
                m_screenDirection = Sub(p1, p0);
                const float length = Length(m_screenDirection);
                m_screenDirection = length > 0.01f ? Mul(m_screenDirection, 1.0f / length) : ImVec2(1, 0);
                m_unitsPerPixel = 1.0f / 85.0f;
            }
            XMVECTOR rayOrigin, rayDirection;
            Ray(io.MousePos, rayOrigin, rayDirection);
            const XMVECTOR normal = m_mode == Mode::Translate ?
                XMVector3Normalize(rayDirection - Axis(m_axis) * XMVector3Dot(rayDirection, Axis(m_axis))) : Axis(m_axis);
            XMStoreFloat3(&m_planeNormal, normal);
            XMVECTOR hit;
            m_usePlane = std::fabs(XMVectorGetX(XMVector3Dot(rayDirection, normal))) > 0.1f &&
                PlaneHit(rayOrigin, rayDirection, origin, normal, hit);
            if (m_usePlane)
            {
                XMStoreFloat3(&m_startHit, hit);
                if (m_mode == Mode::Rotate)
                {
                    m_usePlane = XMVectorGetX(XMVector3LengthSq(hit - origin)) > 1e-10f;
                    if (m_usePlane) XMStoreFloat3(&m_startRingVector, XMVector3Normalize(hit - origin));
                }
            }
            selection.Select(object->editorId); // Invalidate older asynchronous scene picks.
        }

        if (IsDragging())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                Cancel(instances);
            else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                float amount = Dot(Sub(io.MousePos, m_startMouse), m_screenDirection) * m_unitsPerPixel;
                bool canUpdate = true;
                if (m_usePlane)
                {
                    XMVECTOR rayOrigin, rayDirection, hit;
                    Ray(io.MousePos, rayOrigin, rayDirection);
                    canUpdate = PlaneHit(rayOrigin, rayDirection, XMLoadFloat3(&m_startTranslation),
                        XMLoadFloat3(&m_planeNormal), hit);
                    if (canUpdate && m_mode == Mode::Translate)
                        amount = XMVectorGetX(XMVector3Dot(hit - XMLoadFloat3(&m_startHit), Axis(m_axis)));
                    else if (canUpdate)
                    {
                        const auto offset = hit - XMLoadFloat3(&m_startTranslation);
                        canUpdate = XMVectorGetX(XMVector3LengthSq(offset)) > 1e-10f;
                        if (canUpdate)
                        {
                            const auto start = XMLoadFloat3(&m_startRingVector);
                            const auto current = XMVector3Normalize(offset);
                            const float angle = std::atan2(XMVectorGetX(XMVector3Dot(Axis(m_axis), XMVector3Cross(start, current))),
                                XMVectorGetX(XMVector3Dot(start, current)));
                            m_accumulatedAngle += std::remainder(angle - m_lastAngle, XM_2PI);
                            m_lastAngle = angle;
                            amount = m_accumulatedAngle;
                        }
                    }
                }
                if (canUpdate && std::isfinite(amount))
                {
                    if (m_mode == Mode::Translate)
                    {
                        XMFLOAT3 position;
                        XMStoreFloat3(&position, XMLoadFloat3(&m_startTranslation) + Axis(m_axis) * amount);
                        if (position.x != object->translation.x || position.y != object->translation.y || position.z != object->translation.z)
                            object->SetTranslation(position.x, position.y, position.z);
                    }
                    else
                    {
                        const auto rotation = XMMatrixRotationRollPitchYaw(m_startRotation.x, m_startRotation.y, m_startRotation.z) *
                            XMMatrixRotationAxis(Axis(m_axis), amount);
                        // Preserve the exact original values for an unchanged click;
                        // a matrix/Euler round trip alone must not create undo history.
                        const auto angles = amount == 0 ? m_startRotation : EulerNearest(rotation, object->rotation);
                        if (angles.x != object->rotation.x || angles.y != object->rotation.y || angles.z != object->rotation.z)
                            object->SetRotation(angles.x, angles.y, angles.z);
                    }
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    history.RecordTransform(object->editorId, { m_startTranslation, m_startRotation, m_startScale },
                        EditorHistory::Transform::Capture(*object));
                    m_axis = -1;
                }
            }
            else
                Cancel(instances); // Lost mouse-up/focus: never leave an unfinished drag.
            consumed = true;
        }
        if (hovered >= 0 || IsDragging()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (IsDragging()) ImGui::SetNextFrameWantCaptureKeyboard(true);

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        draw->PushClipRect(clipMin, clipMax, true);
        const ImU32 colors[] = { IM_COL32(235, 70, 70, 255), IM_COL32(85, 205, 85, 255), IM_COL32(75, 145, 255, 255) };
        for (int axis = 0; axis < 3; ++axis)
        {
            const bool highlighted = axis == (IsDragging() ? m_axis : hovered);
            const ImU32 color = highlighted ? IM_COL32(255, 215, 70, 255) : colors[axis];
            if (m_mode == Mode::Translate && axisValid[axis])
            {
                const auto direction = Mul(Sub(ends[axis], centre), 1.0f / Length(Sub(ends[axis], centre)));
                const auto base = Sub(ends[axis], Mul(direction, 12.0f));
                const ImVec2 side(-direction.y * 5.0f, direction.x * 5.0f);
                draw->AddLine(centre, base, color, highlighted ? 4.0f : 3.0f);
                draw->AddTriangleFilled(ends[axis], Add(base, side), Sub(base, side), color);
            }
            else if (m_mode == Mode::Translate)
                draw->AddCircle(centre, 5.0f, color, 16, 2.0f);
            else if (m_mode == Mode::Rotate)
                for (int i = 1; i <= segments; ++i)
                    if (valid[axis][i - 1] && valid[axis][i])
                        draw->AddLine(rings[axis][i - 1], rings[axis][i], color, highlighted ? 3.5f : 2.0f);
        }
        if (m_mode == Mode::Translate) draw->AddCircleFilled(centre, 3.0f, IM_COL32_WHITE);
        draw->PopClipRect();
        return consumed;
    }

private:
    // Scale is stored before rotation in S*R*T, so these handles follow the model's
    // rotation. World-aligned nonuniform scaling would require storing shear.
    bool DrawScale(std::vector<ModelInstance>& instances, ModelInstance& object, EditorSelection& selection,
        DirectX::FXMMATRIX vp, ImVec2 screenOrigin, ImVec2 centre, float radius, ImVec2 clipMin, ImVec2 clipMax, EditorHistory& history)
    {
        using namespace DirectX;
        auto& io = ImGui::GetIO();
        const auto origin = XMLoadFloat3(&object.translation);
        const auto rotation = XMMatrixRotationRollPitchYaw(object.rotation.x, object.rotation.y, object.rotation.z);
        XMVECTOR directions[3];
        ImVec2 ends[3]{}, planes[3][4]{};
        bool axisValid[3]{}, planeValid[3]{};
        const bool canStart = io.MousePos.x >= clipMin.x && io.MousePos.x < clipMax.x &&
            io.MousePos.y >= clipMin.y && io.MousePos.y < clipMax.y && !io.WantCaptureMouse &&
            !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.AppFocusLost;
        int hovered = -1;
        float bestDistance = 9.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            directions[axis] = XMVector3TransformNormal(Axis(axis), rotation);
            axisValid[axis] = Project(origin + directions[axis] * radius, vp, screenOrigin, io.DisplaySize, ends[axis]) &&
                Length(Sub(ends[axis], centre)) > 16.0f;
            float t;
            const float distance = DistanceToSegment(io.MousePos, centre, ends[axis], t);
            if (canStart && axisValid[axis] && t > 0.2f && distance < bestDistance)
            {
                hovered = axis;
                bestDistance = distance;
            }
        }
        for (int plane = 0; plane < 3; ++plane)
        {
            const auto a = directions[plane] * radius, b = directions[(plane + 1) % 3] * radius;
            auto& p = planes[plane];
            // One connected L: axis A -> outer corner -> axis B.
            planeValid[plane] = Project(origin, vp, screenOrigin, io.DisplaySize, p[0]) &&
                Project(origin + a*.50f, vp, screenOrigin, io.DisplaySize, p[1]) &&
                Project(origin + a*.50f + b*.50f, vp, screenOrigin, io.DisplaySize, p[2]) &&
                Project(origin + b*.50f, vp, screenOrigin, io.DisplaySize, p[3]);
            // Hide an edge-on plane instead of letting a collapsed patch steal an axis.
            planeValid[plane] &= std::fabs(Cross(Sub(p[1], p[0]), Sub(p[3], p[0]))) > 24.0f &&
                Length(Sub(p[2], centre)) > 18.0f;
            if (!canStart || !planeValid[plane]) continue;
            float t;
            float distance = (std::min)(DistanceToSegment(io.MousePos, p[1], p[2], t),
                DistanceToSegment(io.MousePos, p[2], p[3], t));
            // Overlapping projected patches choose the nearer patch centre, not
            // whichever plane happened to be visited first.
            if (InsideQuad(io.MousePos, p))
                distance = (std::min)(distance, Length(Sub(io.MousePos, Mul(Add(Add(p[0],p[1]),Add(p[2],p[3])), .25f))) * .1f);
            if (distance < bestDistance) { hovered = 3 + plane; bestDistance = distance; }
        }
        // The central square has priority where the three axes meet.
        if (canStart && std::fabs(io.MousePos.x-centre.x) <= 10 && std::fabs(io.MousePos.y-centre.y) <= 10)
            hovered = 6;
        bool consumed = IsDragging() || hovered >= 0;
        if (!IsDragging() && hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_axis = hovered;
            m_objectId = object.editorId;
            m_startTranslation = object.translation;
            m_startRotation = object.rotation;
            m_startScale = object.scale;
            m_startMouse = io.MousePos;
            m_dragScreenOrigin = screenOrigin;
            m_dragScreenSize = io.DisplaySize;
            m_screenDirection = hovered == 6 ? ImVec2(.70710678f, -.70710678f) :
                Sub(hovered < 3 ? ends[hovered] : planes[hovered-3][2], centre);
            m_screenDirection = Mul(m_screenDirection, 1.0f / Length(m_screenDirection));
            selection.Select(object.editorId);
        }
        if (IsDragging())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Cancel(instances);
            else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                const int mask = ScaleMask(m_axis);
                const auto initial = EditorTransform::ToEditorAxes(m_startScale);
                float values[3] = { initial.x, initial.y, initial.z };
                // A fixed pixel sensitivity makes tiny/large models equally usable.
                // A positive factor preserves mirrored axes and never crosses zero.
                float factor = std::exp2((std::clamp)(Dot(Sub(io.MousePos, m_startMouse), m_screenDirection) / 85.0f, -12.0f, 12.0f));
                for (int axis = 0; axis < 3; ++axis)
                    if ((mask & (1 << axis)) && std::fabs(values[axis]) > 0)
                        factor = (std::max)(factor, .0001f / std::fabs(values[axis]));
                for (int axis = 0; axis < 3; ++axis)
                    if (mask & (1 << axis)) values[axis] *= factor;
                const auto current = EditorTransform::ToEditorAxes(object.scale);
                if (values[0] != current.x || values[1] != current.y || values[2] != current.z)
                    EditorTransform::SetScale(object, { values[0], values[1], values[2] });
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    history.RecordTransform(object.editorId, { m_startTranslation, m_startRotation, m_startScale },
                        EditorHistory::Transform::Capture(object));
                    m_axis = -1;
                }
            }
            else Cancel(instances);
            consumed = true;
        }
        if (hovered >= 0 || IsDragging()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (IsDragging()) ImGui::SetNextFrameWantCaptureKeyboard(true);

        auto* draw = ImGui::GetBackgroundDrawList();
        draw->PushClipRect(clipMin, clipMax, true);
        const ImU32 colors[] = { IM_COL32(235,70,70,255), IM_COL32(85,205,85,255), IM_COL32(75,145,255,255) };
        const ImU32 yellow = IM_COL32(255,215,70,255);
        const int active = IsDragging() ? m_axis : hovered;
        const int mask = active >= 0 ? ScaleMask(active) : 0;
        for (int plane = 0; plane < 3; ++plane)
        {
            if (!planeValid[plane]) continue;
            const auto* p = planes[plane];
            const bool selected = active == plane + 3;
            draw->AddQuadFilled(p[0], p[1], p[2], p[3], selected ? IM_COL32(255,215,70,65) : IM_COL32(180,180,180,25));
            draw->AddPolyline(p + 1, 3, selected ? yellow : colors[(plane+2)%3], ImDrawFlags_None, 3.0f);
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!axisValid[axis]) continue;
            const auto color = mask & (1 << axis) ? yellow : colors[axis];
            draw->AddLine(centre, ends[axis], color, 3.0f);
            draw->AddRectFilled(Sub(ends[axis], ImVec2(5,5)), Add(ends[axis], ImVec2(5,5)), color);
        }
        draw->AddRectFilled(Sub(centre, ImVec2(8,8)), Add(centre, ImVec2(8,8)), active == 6 ? yellow : IM_COL32(225,225,225,255));
        draw->AddRect(Sub(centre, ImVec2(8,8)), Add(centre, ImVec2(8,8)), IM_COL32(40,40,40,255));
        draw->PopClipRect();
        return consumed;
    }
    static int ScaleMask(int handle)
    {
        return handle < 3 ? 1 << handle : handle == 6 ? 7 : (1 << (handle-3)) | (1 << ((handle-2)%3));
    }
    static float Cross(ImVec2 a, ImVec2 b) { return a.x*b.y-a.y*b.x; }
    static bool InsideQuad(ImVec2 point, const ImVec2 (&p)[4])
    {
        bool positive = false, negative = false;
        for (int i = 0; i < 4; ++i)
        {
            const float side = Cross(Sub(p[(i+1)%4], p[i]), Sub(point, p[i]));
            positive |= side > 0; negative |= side < 0;
        }
        return !(positive && negative);
    }
    static ModelInstance* Find(std::vector<ModelInstance>& instances, uint32_t id)
    {
        for (auto& instance : instances) if (id != 0 && instance.editorId == id) return &instance;
        return nullptr;
    }
    void Cancel(std::vector<ModelInstance>& instances)
    {
        if (auto* object = Find(instances, m_objectId))
        {
            object->SetTranslation(m_startTranslation.x, m_startTranslation.y, m_startTranslation.z);
            object->SetRotation(m_startRotation.x, m_startRotation.y, m_startRotation.z);
            object->SetScale(m_startScale.x, m_startScale.y, m_startScale.z);
        }
        m_axis = -1;
    }
    static DirectX::XMVECTOR Axis(int axis)
    {
        // Editor X forward = engine Z; editor Y right = engine X; editor Z up = engine Y.
        return axis == 0 ? DirectX::XMVectorSet(0, 0, 1, 0) :
            axis == 1 ? DirectX::XMVectorSet(1, 0, 0, 0) : DirectX::XMVectorSet(0, 1, 0, 0);
    }
    static DirectX::XMVECTOR RingVector(int axis, float angle)
    {
        return Axis((axis + 1) % 3) * std::cos(angle) + Axis((axis + 2) % 3) * std::sin(angle);
    }
    static ImVec2 Add(ImVec2 a, ImVec2 b) { return { a.x + b.x, a.y + b.y }; }
    static ImVec2 Sub(ImVec2 a, ImVec2 b) { return { a.x - b.x, a.y - b.y }; }
    static ImVec2 Mul(ImVec2 a, float b) { return { a.x * b, a.y * b }; }
    static float Dot(ImVec2 a, ImVec2 b) { return a.x * b.x + a.y * b.y; }
    static float Length(ImVec2 a) { return std::sqrt(Dot(a, a)); }
    static float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b, float& t)
    {
        const auto delta = Sub(b, a);
        const float lengthSq = Dot(delta, delta);
        t = lengthSq > 0.0001f ? (std::clamp)(Dot(Sub(p, a), delta) / lengthSq, 0.0f, 1.0f) : 0.0f;
        return Length(Sub(p, Add(a, Mul(delta, t))));
    }
    static bool Project(DirectX::FXMVECTOR world, DirectX::FXMMATRIX vp, ImVec2 origin, ImVec2 size, ImVec2& result)
    {
        DirectX::XMFLOAT4 p;
        DirectX::XMStoreFloat4(&p, DirectX::XMVector4Transform(DirectX::XMVectorSetW(world, 1), vp));
        if (p.w < 0.0001f || p.z < 0 || p.z > p.w) return false;
        result = { origin.x + (p.x / p.w + 1) * size.x * 0.5f, origin.y + (1 - p.y / p.w) * size.y * 0.5f };
        return std::isfinite(result.x) && std::isfinite(result.y);
    }
    void Ray(ImVec2 mouse, DirectX::XMVECTOR& origin, DirectX::XMVECTOR& direction) const
    {
        using namespace DirectX;
        const float x = 2 * (mouse.x - m_dragScreenOrigin.x) / m_dragScreenSize.x - 1;
        const float y = 1 - 2 * (mouse.y - m_dragScreenOrigin.y) / m_dragScreenSize.y;
        const auto inverse = XMLoadFloat4x4(&m_dragInverseVp);
        origin = XMVector3TransformCoord(XMVectorSet(x, y, 0, 1), inverse);
        direction = XMVector3Normalize(XMVector3TransformCoord(XMVectorSet(x, y, 1, 1), inverse) - origin);
    }
    static bool PlaneHit(DirectX::FXMVECTOR origin, DirectX::FXMVECTOR direction,
        DirectX::FXMVECTOR centre, DirectX::GXMVECTOR normal, DirectX::XMVECTOR& hit)
    {
        const float denominator = DirectX::XMVectorGetX(DirectX::XMVector3Dot(direction, normal));
        if (std::fabs(denominator) < 0.001f) return false;
        const float t = DirectX::XMVectorGetX(DirectX::XMVector3Dot(centre - origin, normal)) / denominator;
        if (t < 0 || !std::isfinite(t)) return false;
        hit = origin + direction * t;
        return true;
    }
    static DirectX::XMFLOAT3 EulerNearest(DirectX::FXMMATRIX rotation, const DirectX::XMFLOAT3& reference)
    {
        using namespace DirectX;
        XMFLOAT4X4 m; XMStoreFloat4x4(&m, rotation);
        XMFLOAT3 result;
        // atan2 retains the small cosine near +/-90 degrees; asin loses it to
        // float rounding and can visibly perturb a nearly vertical model.
        const float cosPitch = std::hypot(m._31, m._33);
        result.x = std::atan2(-m._32, cosPitch);
        if (cosPitch > 0.00001f)
        {
            result.y = std::atan2(m._31, m._33);
            result.z = std::atan2(m._12, m._22);
        }
        else
        {
            result.z = reference.z;
            result.y = std::atan2(-m._13, m._11) + (result.x > 0 ? result.z : -result.z);
        }
        auto nearest = [&](XMFLOAT3 angles)
        {
            return XMFLOAT3(reference.x + std::remainder(angles.x - reference.x, XM_2PI),
                reference.y + std::remainder(angles.y - reference.y, XM_2PI),
                reference.z + std::remainder(angles.z - reference.z, XM_2PI));
        };
        const auto alternate = nearest({ XM_PI - result.x, result.y + XM_PI, result.z + XM_PI });
        result = nearest(result);
        auto distance = [&](const XMFLOAT3& a) { return (a.x-reference.x)*(a.x-reference.x) +
            (a.y-reference.y)*(a.y-reference.y) + (a.z-reference.z)*(a.z-reference.z); };
        return distance(alternate) < distance(result) ? alternate : result;
    }

    Mode m_mode = Mode::Translate;
    int m_axis = -1;
    uint32_t m_objectId = 0;
    DirectX::XMFLOAT3 m_startTranslation{}, m_startRotation{}, m_startScale{}, m_startHit{}, m_planeNormal{}, m_startRingVector{};
    DirectX::XMFLOAT4X4 m_dragInverseVp{};
    ImVec2 m_startMouse{}, m_screenDirection{}, m_dragScreenOrigin{}, m_dragScreenSize{};
    float m_unitsPerPixel = 0, m_lastAngle = 0, m_accumulatedAngle = 0;
    bool m_usePlane = false;
};

