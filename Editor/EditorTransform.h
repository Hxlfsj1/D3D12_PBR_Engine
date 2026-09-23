#pragma once

#include "SceneObject.h"
#include <cmath>

// The renderer/assets remain Y-up. Editor world axes are X forward, Y right, Z up:
// (editor X,Y,Z) = (engine Z,X,Y). This is a fixed basis, never the camera basis.
namespace EditorTransform
{
    inline DirectX::XMFLOAT3 ToEditorAxes(const DirectX::XMFLOAT3& value)
    {
        return { value.z, value.x, value.y };
    }

    inline DirectX::XMFLOAT3 ToEngineAxes(const DirectX::XMFLOAT3& value)
    {
        return { value.y, value.z, value.x };
    }

    inline bool IsFinite(const DirectX::XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline DirectX::XMFLOAT3 RotationDegrees(const ModelInstance& instance)
    {
        // UE Rotator convention: X=Roll, Y=Pitch (positive nose-up), Z=Yaw.
        // Pitch/roll have the opposite sign to DirectX's positive axis rotations.
        // Engine roll(Z), pitch(X), yaw(Y) order maps to editor X, Y, Z order.
        return { -DirectX::XMConvertToDegrees(instance.rotation.z),
                 -DirectX::XMConvertToDegrees(instance.rotation.x),
                  DirectX::XMConvertToDegrees(instance.rotation.y) };
    }

    inline bool SetLocation(ModelInstance& instance, const DirectX::XMFLOAT3& value)
    {
        if (!IsFinite(value)) return false;
        const auto engine = ToEngineAxes(value);
        instance.SetTranslation(engine.x, engine.y, engine.z);
        return true;
    }

    inline bool SetRotation(ModelInstance& instance, const DirectX::XMFLOAT3& degrees)
    {
        if (!IsFinite(degrees)) return false;
        instance.SetRotation(-DirectX::XMConvertToRadians(degrees.y),
            DirectX::XMConvertToRadians(degrees.z), -DirectX::XMConvertToRadians(degrees.x));
        return true;
    }

    inline bool SetScale(ModelInstance& instance, const DirectX::XMFLOAT3& value)
    {
        // Zero scale makes the inverse-transpose normal matrix singular. Keep signed
        // scale for mirroring, but don't allow a collapsed axis into the renderer.
        constexpr float minimumMagnitude = 0.0001f;
        if (!IsFinite(value) || std::fabs(value.x) < minimumMagnitude ||
            std::fabs(value.y) < minimumMagnitude || std::fabs(value.z) < minimumMagnitude)
            return false;
        const auto engine = ToEngineAxes(value);
        instance.SetScale(engine.x, engine.y, engine.z);
        return true;
    }
}
