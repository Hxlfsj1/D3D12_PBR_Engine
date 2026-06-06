#ifndef ASSETS_H
#define ASSETS_H

#include "SceneObject.h"
#include <vector>
#include <string>
#include <fstream>
#include "json.hpp"

namespace DirectX
{
    inline void from_json(const nlohmann::json& j, XMFLOAT3& p)
    {
        p.x = j[0].get<float>();
        p.y = j[1].get<float>();
        p.z = j[2].get<float>();
    }
}

struct InstanceDesc
{
    std::string name;
    std::string modelPath;
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 rot;
    DirectX::XMFLOAT3 scale;
    bool isTransparent = false;
    bool isCutout = false;

    UINT materialOverrideIndex = 0xFFFFFFFF;
};

inline void from_json(const nlohmann::json& j, InstanceDesc& desc)
{
    desc.name = j.value("name", "");
    desc.modelPath = j.value("model_path", "");

    if (j.contains("pos")) desc.pos = j["pos"].get<DirectX::XMFLOAT3>();
    if (j.contains("rot")) desc.rot = j["rot"].get<DirectX::XMFLOAT3>();
    if (j.contains("scale")) desc.scale = j["scale"].get<DirectX::XMFLOAT3>();

    desc.isTransparent = j.value("is_transparent", false);
    desc.isCutout = j.value("is_cutout", false);
    desc.materialOverrideIndex = j.value("material_override_index", 0xFFFFFFFF);
}

class Assets
{
public:
    static const char* GetSkyboxPath()
    {
        return "HDRs/citrus_orchard_road_puresky_4k.hdr";
    }

    static std::vector<InstanceDesc> LoadSceneFromJson(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            OutputDebugStringA(("Warning: Failed to open " + filepath + ", falling back to hardcoded scene.\n").c_str());
        }

        nlohmann::json j;
        file >> j;

        return j.get<std::vector<InstanceDesc>>();
    }
};

#endif