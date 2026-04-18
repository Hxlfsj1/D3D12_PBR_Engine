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
                "Sniper_MP5_1",
                "Models/MP5.glb",
                { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f },
                { 1.0f, 1.0f, 1.0f }
            },
            {
                "Sniper_MP5_2",
                "Models/MP5.glb",
                { 5.0f, 0.0f, -2.0f },
                { 0.0f, 1.5708f, 0.0f },
                { 1.2f, 1.2f, 1.2f }
            }
        };
    }
};

#endif