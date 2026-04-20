#ifndef ASSETS_H
#define ASSETS_H

#include "SceneObject.h"
#include <vector>
#include <string>

struct InstanceDesc
{
    std::string name;
    std::string modelPath;
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 rot;
    DirectX::XMFLOAT3 scale;
};

class Assets
{
public:
    static const char* GetSkyboxPath()
    {
        return "HDRs/citrus_orchard_road_puresky_4k.hdr";
    }

    static std::vector<InstanceDesc> GetSniperAlleyScene()
    {
        return
        {
            {
                "First",
                "Models/Sn_Tashkent_of_Azure_Lane.glb",
                { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f },
                { 1.0f, 1.0f, 1.0f }
            },
        };
    }

    // Stress test
    static std::vector<InstanceDesc> GetPerformanceTestScene()
    {
        std::vector<InstanceDesc> scene;
        scene.reserve(1000);

        float spacing = 3.0f;
        float offset = (10.0f * spacing) / 2.0f;

        for (int x = 0; x < 20; ++x)
        {
            for (int y = 0; y < 20; ++y)
            {
                for (int z = 0; z < 20; ++z)
                {
                    std::string name = "Test_MP5_" + std::to_string(x) + "_" + std::to_string(y) + "_" + std::to_string(z);

                    scene.push_back({
                        name,
                        "Models/MP5.glb",
                        { (x * spacing) - offset, (y * spacing) - offset, (z * spacing) - offset },
                        { 0.0f, 0.0f, 0.0f },
                        { 1.0f, 1.0f, 1.0f }
                        });
                }
            }
        }
        return scene;
    }
};

#endif