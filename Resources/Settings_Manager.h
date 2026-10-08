#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "SceneObject.h"
#include "DLSSQuality.h"
#include "../Core/ErrorLog.h"
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <stdexcept>
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
    SMAA,
    DLSS
};

struct PipelineConfig
{
    bool useDeferred = true;
    bool useZPrepass = false;
    AntiAliasingMode antiAliasing = AntiAliasingMode::None;
    DLSSQualityMode dlssQuality = DLSSQualityMode::Quality;
};

struct HBAOConfig
{
    bool enabled = true;
    float intensity = 2.0f;
    float radius = 1.0f;
    int quality = 1;
};

struct SSGIConfig
{
    bool enabled = false;
    float intensity = 1.0f;
    int quality = 1;
    float radius = 2.0f;
};

struct LightingConfig
{
    float environmentIntensity = 1.0f;
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
    SSGIConfig ssgi;
    HBAOConfig hbao;

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
            ErrorLog::Write("Settings: scene file could not be opened: " + filepath);
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
            ErrorLog::Write(
                "Settings: scene JSON parse failed. File: " + filepath +
                "\nReason: " + e.what());
            OutputDebugStringA(("Error: JSON Parse failed in " + filepath + "\nDetail: " + std::string(e.what()) + "\n").c_str());
            return {};
        }

        try
        {
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

            ErrorLog::Write("Settings: scene JSON root must be an object or array. File: " + filepath);
        }
        catch (const nlohmann::json::exception& e)
        {
            ErrorLog::Write(
                "Settings: scene JSON contains an invalid field. File: " + filepath +
                "\nReason: " + e.what());
            throw;
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
                ErrorLog::Write(
                    "Settings: window JSON parse failed. File: " + filepath +
                    "\nReason: " + e.what());
                OutputDebugStringA(("Error: Window Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            try
            {
                window.width = j.value("width", window.width);
                window.height = j.value("height", window.height);
                window.fullScreen = j.value("fullscreen", window.fullScreen);
                window.tsrUpscaleFactor = j.value("tsr_upscale_factor", window.tsrUpscaleFactor);
                window.title = j.value("title", window.title);
            }
            catch (const nlohmann::json::exception& e)
            {
                ErrorLog::Write(
                    "Settings: window JSON contains an invalid field. File: " + filepath +
                    "\nReason: " + e.what());
                throw;
            }
        }
        else
        {
            ErrorLog::Write("Settings: window file could not be opened: " + filepath);
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
                ErrorLog::Write(
                    "Settings: pipeline JSON parse failed. File: " + filepath +
                    "\nReason: " + e.what());
                OutputDebugStringA(("Error: Pipeline Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            try
            {
                const auto rendering = j.value("rendering", nlohmann::json::object());
                const auto reconstruction = j.value("DLSS SR", nlohmann::json::object());
                pipeline.useDeferred = rendering.value("use_deferred", pipeline.useDeferred);
                pipeline.useZPrepass = rendering.value("use_z_prepass", pipeline.useZPrepass);

                const std::string antiAliasing = reconstruction.value("anti_aliasing", std::string("None"));
                if (antiAliasing == "TAA")
                {
                    pipeline.antiAliasing = AntiAliasingMode::TAA;
                }
                else if (antiAliasing == "TSR")
                {
                    pipeline.antiAliasing = AntiAliasingMode::TSR;
                }
                else if (antiAliasing == "SMAA")
                {
                    pipeline.antiAliasing = AntiAliasingMode::SMAA;
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

                const std::string dlssQuality = reconstruction.value("dlss_quality", std::string("Quality"));
                if (!TryParseDLSSQualityMode(dlssQuality, &pipeline.dlssQuality))
                {
                    pipeline.dlssQuality = DLSSQualityMode::Quality;
                    OutputDebugStringA(
                        ("Warning: Unknown dlss_quality value '" + dlssQuality + "'; using Quality.\n").c_str());
                }

                if (j.contains("hbao"))
                {
                    const auto& config = j.at("hbao");
                    hbao.enabled = config.value("enabled", hbao.enabled);
                    hbao.intensity = config.value("intensity", hbao.intensity);
                    if (config.contains("quality") &&
                        (!config.at("quality").is_number_integer() ||
                            config.at("quality") < 1 || config.at("quality") > 4))
                        throw std::runtime_error("HBAO quality must be an integer from 1 to 4.");
                    hbao.quality = config.value("quality", hbao.quality);
                    hbao.radius = config.value("radius", hbao.radius);
                    if (!std::isfinite(hbao.intensity) || hbao.intensity < 0 ||
                        !std::isfinite(hbao.radius) || hbao.radius <= 0)
                    {
                        throw std::runtime_error("HBAO requires finite intensity >= 0 and finite radius > 0.");
                    }
                }

                if (j.contains("ssgi"))
                {
                    const auto& config = j.at("ssgi");
                    ssgi.enabled = config.value("enabled", ssgi.enabled);
                    ssgi.intensity = config.value("intensity", ssgi.intensity);
                    if (config.contains("quality") && !config.at("quality").is_number_integer())
                        throw std::runtime_error("SSGI quality must be an integer from 1 to 4.");
                    ssgi.quality = config.value("quality", ssgi.quality);
                    ssgi.radius = config.value("radius", ssgi.radius);
                    if (!(std::isfinite(ssgi.radius) && ssgi.radius > 0 &&
                        std::isfinite(ssgi.intensity) && ssgi.intensity >= 0 &&
                        ssgi.quality >= 1 && ssgi.quality <= 4))
                        throw std::runtime_error("SSGI requires finite radius > 0, finite intensity >= 0 and quality from 1 to 4.");
                }
            }
            catch (const std::exception& e)
            {
                ErrorLog::Write(
                    "Settings: pipeline JSON contains an invalid field. File: " + filepath +
                    "\nReason: " + e.what());
                throw;
            }
        }
        else
        {
            ErrorLog::Write("Settings: pipeline file could not be opened: " + filepath);
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
                ErrorLog::Write(
                    "Settings: lighting JSON parse failed. File: " + filepath +
                    "\nReason: " + e.what());
                OutputDebugStringA(("Error: Lighting Config JSON Parse failed in " + filepath + "\n").c_str());
                return;
            }

            try
            {
                if (j.contains("environment"))
                {
                    lighting.environmentIntensity = j.at("environment").value("intensity", 1.0f);
                    if (!std::isfinite(lighting.environmentIntensity) || lighting.environmentIntensity < 0)
                        throw std::runtime_error("Environment intensity must be finite and nonnegative.");
                }

                const auto directLight = j.value("direct light", nlohmann::json::object());
                if (directLight.contains("light_dir"))
                {
                    lighting.lightDir = directLight["light_dir"].get<DirectX::XMFLOAT3>();
                }
                if (directLight.contains("light_color"))
                {
                    lighting.lightColor = directLight["light_color"].get<DirectX::XMFLOAT3>();
                }
                lighting.sunAngularRadiusDegrees = directLight.value(
                    "sun_angular_radius_degrees",
                    lighting.sunAngularRadiusDegrees);
            }
            catch (const std::exception& e)
            {
                ErrorLog::Write(
                    "Settings: lighting JSON contains an invalid field. File: " + filepath +
                    "\nReason: " + e.what());
                throw;
            }
        }
        else
        {
            ErrorLog::Write("Settings: lighting file could not be opened: " + filepath);
            OutputDebugStringA(("Warning: Failed to open " + filepath + "\n").c_str());
        }
    }
};

#endif
