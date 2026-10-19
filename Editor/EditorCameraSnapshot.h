#pragma once

#include "../Logic/Camera.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

// A read-only copy of the live camera, shared by the editor menu and capture
// tooling. Rotation uses Camera's own degree convention: X=Pitch, Y=Yaw, Z=0.
// Eye/forward/up preserve the actual left-handed view independently of Euler
// conventions, and forward avoids cancellation when eye + forward is rounded.
struct EditorCameraSnapshot
{
    std::array<float, 3> position{};
    std::array<float, 3> rotationDegrees{};
    std::array<float, 3> scale{ 1.0f, 1.0f, 1.0f };
    std::array<float, 3> target{};
    std::array<float, 3> up{ 0.0f, 1.0f, 0.0f };
    std::array<float, 3> forward{ 0.0f, 0.0f, 1.0f };
    float verticalFovDegrees = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;

    static EditorCameraSnapshot Capture(const Camera& camera, uint32_t width, uint32_t height,
        float nearDistance = 0.1f, float farDistance = 1000.0f)
    {
        EditorCameraSnapshot snapshot;
        snapshot.position = { camera.Position.x, camera.Position.y, camera.Position.z };
        snapshot.rotationDegrees = { camera.Pitch, camera.Yaw, 0.0f };
        snapshot.forward = { camera.Front.x, camera.Front.y, camera.Front.z };
        snapshot.target = { camera.Position.x + camera.Front.x,
            camera.Position.y + camera.Front.y, camera.Position.z + camera.Front.z };
        snapshot.up = { camera.Up.x, camera.Up.y, camera.Up.z };
        snapshot.verticalFovDegrees = camera.Zoom;
        snapshot.nearPlane = nearDistance;
        snapshot.farPlane = farDistance;
        snapshot.viewportWidth = width;
        snapshot.viewportHeight = height;
        return snapshot;
    }

    std::string ToJson() const
    {
        std::ostringstream output;
        // A user's decimal locale must not turn a JSON number into "1,25".
        // max_digits10 makes each exported float round-trip without losing bits.
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        const auto vector = [&output](const char* name, const std::array<float, 3>& value)
        {
            output << "  \"" << name << "\": [" << value[0] << ", " << value[1]
                << ", " << value[2] << "],\n";
        };
        output << "{\n  \"coordinate_system\": \"left_handed_y_up\",\n";
        vector("eye", position);
        vector("target", target);
        vector("up", up);
        vector("forward", forward);
        vector("rotation_degrees_xyz", rotationDegrees);
        vector("scale", scale);
        output << "  \"vertical_fov_degrees\": " << verticalFovDegrees
            << ",\n  \"near_plane\": " << nearPlane
            << ",\n  \"far_plane\": " << farPlane
            << ",\n  \"viewport_size\": [" << viewportWidth << ", " << viewportHeight << "]\n}\n";
        return output.str();
    }
};
