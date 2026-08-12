#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "SceneObject.h"
#include <vector>
#include <string>
#include <fstream>
#include <DirectXMath.h>
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

    if (j.contains("pos"))
    {
        desc.pos = j["pos"].get<DirectX::XMFLOAT3>();
    }
    if (j.contains("rot"))
    {
        desc.rot = j["rot"].get<DirectX::XMFLOAT3>();
    }
    if (j.contains("scale"))
    {
        desc.scale = j["scale"].get<DirectX::XMFLOAT3>();
    }

    desc.isTransparent = j.value("is_transparent", false);
    desc.isCutout = j.value("is_cutout", false);
    desc.materialOverrideIndex = j.value("material_override_index", 0xFFFFFFFF);
}

struct WindowConfig
{
    int width = 2240;
    int height = 1400;
    bool fullScreen = false;
    float tsrUpscaleFactor = 2.0f;
    std::string title = "PBR IBL Model Viewer";
};

enum class AntiAliasingMode
{
    None,
    TAA,
    TSR,
    DLSS
};

struct PipelineConfig
{
    bool useDeferred = true;
    bool useZPrepass = false;
    AntiAliasingMode antiAliasing = AntiAliasingMode::None;
};

struct LightingConfig
{
    DirectX::XMFLOAT3 lightDir = { -0.5f, -1.0f, 0.5f };
    DirectX::XMFLOAT3 lightColor = { 5.0f, 5.0f, 5.0f };
    float sunAngularRadiusDegrees = 0.266f;
};

class SettingsManager
{
public:
    WindowConfig window;
    PipelineConfig pipeline;
    LightingConfig lighting;

    inline static std::string s_skyboxPath = "HDRs/citrus_orchard_road_puresky_4k.hdr";

    static const char* GetSkyboxPathFromJson()
    {
        return s_skyboxPath.c_str();
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
        try
        {
            file >> j;
        }
        catch (const nlohmann::json::parse_error& e)
        {
            OutputDebugStringA(("Error: JSON Parse failed in " + filepath + "\nDetail: " + std::string(e.what()) + "\n").c_str());
            return {};
        }

        if (j.is_object())
        {
            s_skyboxPath = j.value("skybox_path", "HDRs/citrus_orchard_road_puresky_4k.hdr");

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

    void LoadAllSettingsFromJson()
    {
        LoadWindowConfigFromJson("Settings/Window.json");
        LoadPipelineConfigFromJson("Settings/Pipeline.json");
        LoadLightingConfigFromJson("Settings/Lighting.json");
    }

private:
    void LoadWindowConfigFromJson(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (file.is_open())
        {
            nlohmann::json j;
            try
            {
                file >> j;
            }
            catch (const nlohmann::json::parse_error& e)
            {
                OutputDebugStringA(("Error: Window Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            window.width = j.value("width", window.width);
            window.height = j.value("height", window.height);
            window.fullScreen = j.value("fullscreen", window.fullScreen);
            window.tsrUpscaleFactor = j.value("tsr_upscale_factor", window.tsrUpscaleFactor);
            window.title = j.value("title", window.title);
        }
        else
        {
            OutputDebugStringA(("Warning: Failed to open " + filepath + "\n").c_str());
        }
    }

    void LoadPipelineConfigFromJson(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (file.is_open())
        {
            nlohmann::json j;
            try
            {
                file >> j;
            }
            catch (const nlohmann::json::parse_error& e)
            {
                OutputDebugStringA(("Error: Pipeline Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            pipeline.useDeferred = j.value("use_deferred", pipeline.useDeferred);
            pipeline.useZPrepass = j.value("use_z_prepass", pipeline.useZPrepass);

            const std::string antiAliasing = j.value("anti_aliasing", std::string("None"));
            if (antiAliasing == "TAA")
            {
                pipeline.antiAliasing = AntiAliasingMode::TAA;
            }
            else if (antiAliasing == "TSR")
            {
                pipeline.antiAliasing = AntiAliasingMode::TSR;
            }
            else if (antiAliasing == "DLSS")
            {
                pipeline.antiAliasing = AntiAliasingMode::DLSS;
            }
            else
            {
                pipeline.antiAliasing = AntiAliasingMode::None;
                if (antiAliasing != "None")
                {
                    OutputDebugStringA(("Warning: Unknown anti_aliasing value '" + antiAliasing + "'; using None.\n").c_str());
                }
            }
        }
        else
        {
            OutputDebugStringA(("Warning: Failed to open " + filepath + "\n").c_str());
        }
    }

    void LoadLightingConfigFromJson(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (file.is_open())
        {
            nlohmann::json j;
            try
            {
                file >> j;
            }
            catch (const nlohmann::json::parse_error& e)
            {
                OutputDebugStringA(("Error: Lighting Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            if (j.contains("light_dir"))
            {
                lighting.lightDir = j["light_dir"].get<DirectX::XMFLOAT3>();
            }
            if (j.contains("light_color"))
            {
                lighting.lightColor = j["light_color"].get<DirectX::XMFLOAT3>();
            }
            lighting.sunAngularRadiusDegrees = j.value(
                "sun_angular_radius_degrees",
                lighting.sunAngularRadiusDegrees);
        }
        else
        {
            OutputDebugStringA(("Warning: Failed to open " + filepath + "\n").c_str());
        }
    }
};

#endif
