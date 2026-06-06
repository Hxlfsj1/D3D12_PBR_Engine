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
            OutputDebugStringA(("Warning: Failed to open " + filepath + "\n").c_str());
            return {};
        }

        nlohmann::json j;
        file >> j;

        if (j.is_object())
        {
            bool isStressTest = j.value("stress_test", false);
            std::vector<InstanceDesc> instances = j.value("instances", nlohmann::json::array()).get<std::vector<InstanceDesc>>();

            if (isStressTest && !instances.empty())
            {
                return GeneratePerformanceTestScene(instances[0].modelPath);
            }

            return instances;
        }
        else if (j.is_array())
        {
            return j.get<std::vector<InstanceDesc>>();
        }

        return {};
    }

    static std::vector<InstanceDesc> GeneratePerformanceTestScene(const std::string& modelPath)
    {
        std::vector<InstanceDesc> scene;
        scene.reserve(8000);

        float spacing = 3.0f;
        float offset = (10.0f * spacing) / 2.0f;

        for (int x = 0; x < 20; ++x)
        {
            for (int y = 0; y < 20; ++y)
            {
                for (int z = 0; z < 20; ++z)
                {
                    std::string name = "Test_Model_" + std::to_string(x) + "_" + std::to_string(y) + "_" + std::to_string(z);

                    InstanceDesc desc;
                    desc.name = name;
                    desc.modelPath = modelPath;
                    desc.pos = { (x * spacing) - offset, (y * spacing) - offset, (z * spacing) - offset };
                    desc.rot = { 0.0f, 0.0f, 0.0f };
                    desc.scale = { 1.0f, 1.0f, 1.0f };
                    desc.isTransparent = false;
                    desc.isCutout = false;
                    desc.materialOverrideIndex = 0xFFFFFFFF;

                    scene.push_back(desc);
                }
            }
        }
        return scene;
    }
};

#endif