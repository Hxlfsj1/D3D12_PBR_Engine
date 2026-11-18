#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "SceneObject.h"
#include "DLSSQuality.h"
#include "../Core/ErrorLog.h"
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <limits>
#include <array>
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
    float shadowCascadeTransitionFraction = 0.1f;
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
    bool lumen = true; // Lumen: world tracing; SSGI: screen queries, shared Surface Cache.
    bool enabled = false;
    bool giOnly = false; // Runtime presentation; Debug > Pass controls the editor view.
    bool screenProbes = false; // Explicit mode selection; missing key retains legacy fixtures.
    int screenRaysPerProbe = 64; // Actual screen traces per fresh probe; evenly distributed over 8x8 directions.
    int screenProbeHistoryUpdates = 4;
    float screenProbeJitterWidthFraction = 0.125f; // Total screen-tile jitter width; zero disables.
    bool screenProbeAdaptiveEnabled = false; // Missing object retains uniform/legacy fixtures.
    float screenProbeAdaptiveMaxFraction = 0.25f; // Capacity relative to the uniform screen grid.
    int emissiveSamplesPerProbe = 0; // Extra world-emitter samples; zero keeps emission in ordinary rays.
    int maxTraceIterations = 96; // Shared Hi-Z cell visits/descents per ray, independent of sample counts.
    float screenTraceDistance = 2.0f;
    bool cameraFadeEnabled = true;
    float cameraFadeStart = 16.0f; // Receiver-to-camera distance, independent of ray length.
    float cameraFadeEnd = 24.0f;
    float intensity = 1.0f;
    int traceDownsampleFactor = 2;
    int denoiseDownsampleFactor = 2; // Screen-probe pixel reconstruction/history/filtering, independent of probe spacing.
    static constexpr bool IsValidDenoiseDownsampleFactor(int factor)
    {
        return factor == 1 || factor == 2;
    }
    uint32_t DenoiseFactor() const
    {
        return static_cast<uint32_t>(screenProbes ? denoiseDownsampleFactor : traceDownsampleFactor);
    }
    static constexpr bool IsValidTraceDownsampleFactor(int factor)
    {
        return factor >= 1;
    }
    bool HasValidScreenProbeSamples() const
    {
        const bool regular = screenRaysPerProbe >= 64 && screenRaysPerProbe % 64 == 0;
        return regular && emissiveSamplesPerProbe >= 0 && emissiveSamplesPerProbe <= 64;
    }
    bool HasValidScreenProbeJitter() const
    {
        return std::isfinite(screenProbeJitterWidthFraction) &&
            screenProbeJitterWidthFraction >= 0.0f && screenProbeJitterWidthFraction <= 1.0f;
    }

    bool HasValidTraceIterations() const { return maxTraceIterations > 0; }

    bool HasValidScreenProbeAdaptive() const
    {
        return std::isfinite(screenProbeAdaptiveMaxFraction) &&
            screenProbeAdaptiveMaxFraction >= 0.0f && screenProbeAdaptiveMaxFraction <= 1.0f;
    }

    bool HasValidCameraFade() const
    {
        return std::isfinite(cameraFadeStart) && std::isfinite(cameraFadeEnd) &&
            cameraFadeStart >= 0.0f && cameraFadeEnd > cameraFadeStart;
    }

    bool HasValidTraceDistances() const
    {
        return std::isfinite(screenTraceDistance) && screenTraceDistance > 0;
    }

};

struct VXGIConfig
{
    bool enabled = false;
    float voxelSize = 0.5f;
    // Geometry only. Screen GI remains independent until Surface Cache is connected.
    int debugView = 0;
    float sdfCellSize = 1.0f;
    float meshTraceDistance = 2.0f;
    int sdfSliceAxis = 2;
    float sdfSlicePosition = 0.0f;
    bool globalSDFEnabled = true;
    int globalSDFCells = 64, globalSDFClipmaps = 3;
    std::array<uint32_t, 3> globalSDFLayerCells{};
    float globalSDFCellSize = 0.5f;
    float globalSDFTraceDistance = 128.0f;
    int globalSDFMaxSteps = 256;
    float surfaceMaterialMipLevel = 0.0f;
    float surfaceRayMinDistance = 0.0001f;
    float surfaceMaxRayDistance = 1000.0f;

};

struct SurfaceCacheConfig
{
    static constexpr uint32_t MaxTextureDimension = 16384; // D3D12 Texture2D limit.
    float cardCellSize = 0.1f;
    uint32_t maxCardsPerMesh = 12;
    float worldTexelSize = 0.1f;
    float maxDistance = 32.0f;
    uint32_t minCardResolution = 16, maxCardResolution = 512, atlasSize = 2048;
    bool operator==(const SurfaceCacheConfig&)const=default;
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
    VXGIConfig vxgi;
    SurfaceCacheConfig surfaceCache;

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

                return j.value("instances", nlohmann::json::array()).get<std::vector<InstanceDesc>>();
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
                const auto shadows = j.value("shadows", nlohmann::json::object());
                const auto cascades = shadows.value("cascades", nlohmann::json::object());
                const float transition = cascades.value("transition_fraction", 0.1f);
                if (!std::isfinite(transition) || transition < 0 || transition > 0.3f)
                    throw std::runtime_error("Shadow cascade transition_fraction must be between 0 and 0.3.");
                pipeline.shadowCascadeTransitionFraction = transition;

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

                if (j.contains("surface_cache"))
                {
                    const auto& config = j.at("surface_cache");
                    surfaceCache.cardCellSize = config.value("card_cell_size", 0.1f);
                    surfaceCache.maxCardsPerMesh = config.value("max_cards_per_mesh", 12u);
                    surfaceCache.worldTexelSize = config.value("world_texel_size", 0.1f);
                    surfaceCache.maxDistance = config.value("max_distance", 32.0f);
                    const auto resolution=[&](const char* key,uint32_t fallback)
                    {
                        if(!config.contains(key))return fallback;
                        const auto& value=config.at(key);
                        // Validate before narrowing: large/negative JSON integers
                        // must not wrap into an otherwise valid uint32_t size.
                        if(!value.is_number_integer() || value.get<double>()<8 ||
                            value.get<double>()>SurfaceCacheConfig::MaxTextureDimension)
                            throw std::runtime_error("Surface Cache resolutions must be integers in [8,16384].");
                        return value.get<uint32_t>();
                    };
                    surfaceCache.minCardResolution = resolution("min_card_resolution", 16u);
                    surfaceCache.maxCardResolution = resolution("max_card_resolution", 512u);
                    surfaceCache.atlasSize = resolution("atlas_size", 2048u);
                    if (!std::isfinite(surfaceCache.cardCellSize) || surfaceCache.cardCellSize <= 0 ||
                        surfaceCache.maxCardsPerMesh < 1 || surfaceCache.maxCardsPerMesh > 32)
                        throw std::runtime_error("Surface Cache requires a positive card_cell_size and 1..32 max_cards_per_mesh.");
                    const auto powerOfTwo=[](uint32_t value){return value>=8 && (value&(value-1))==0;};
                    if (!std::isfinite(surfaceCache.worldTexelSize) || surfaceCache.worldTexelSize<=0 ||
                        !std::isfinite(surfaceCache.maxDistance) || surfaceCache.maxDistance<=0 ||
                        !powerOfTwo(surfaceCache.minCardResolution) || !powerOfTwo(surfaceCache.maxCardResolution) ||
                        !powerOfTwo(surfaceCache.atlasSize) || surfaceCache.minCardResolution>surfaceCache.maxCardResolution ||
                        surfaceCache.maxCardResolution>surfaceCache.atlasSize)
                        throw std::runtime_error("Surface Cache requires positive world_texel_size/max_distance and power-of-two resolutions >= 8 with min <= max <= atlas_size.");
                }

                if (j.contains("world_geometry"))
                {
                    const auto& config = j.at("world_geometry");
                    vxgi.enabled = config.value("enabled", false);
                    vxgi.sdfCellSize = config.value("mesh_cell_size", 1.0f);
                    vxgi.meshTraceDistance = config.value("mesh_trace_distance", 2.0f);
                    vxgi.globalSDFTraceDistance = config.value("trace_distance", 128.0f);
                    if (config.contains("global_sdf"))
                    {
                        const auto& global = config.at("global_sdf");
                        vxgi.globalSDFEnabled = global.value("enabled", true);
                        vxgi.globalSDFCells = global.value("cells", 64);
                        vxgi.globalSDFClipmaps = global.value("clipmaps", 3);
                        vxgi.globalSDFCellSize = global.value("cell_size", 0.5f);
                    }
                    if (!std::isfinite(vxgi.sdfCellSize) || !std::isfinite(vxgi.meshTraceDistance) ||
                        !std::isfinite(vxgi.globalSDFTraceDistance) || !std::isfinite(vxgi.globalSDFCellSize) ||
                        !(vxgi.sdfCellSize > 0 && vxgi.meshTraceDistance > 0 &&
                        vxgi.globalSDFTraceDistance >= vxgi.meshTraceDistance && vxgi.globalSDFCellSize > 0) ||
                        vxgi.globalSDFCells < 2 || vxgi.globalSDFCells > 256 || (vxgi.globalSDFCells & 1) ||
                        vxgi.globalSDFClipmaps < 1 || vxgi.globalSDFClipmaps > 3)
                        throw std::runtime_error("World geometry requires finite positive cell sizes, finite 0 < mesh_trace_distance <= trace_distance, even Global SDF cells in [2,256], and 1..3 clipmaps.");
                }

                if (j.contains("ssgi"))
                {
                    const auto& config = j.at("ssgi");
                    ssgi.enabled = config.value("enabled", ssgi.enabled);
                    const auto mode=config.value("mode",std::string("Lumen"));
                    if(mode!="SSGI" && mode!="Lumen")throw std::runtime_error("ssgi.mode must be SSGI or Lumen.");
                    ssgi.lumen=mode=="Lumen";
                    const auto samplingMode = config.value("sampling_mode", std::string("legacy"));
                    if (samplingMode != "legacy" && samplingMode != "screen_probes")
                        throw std::runtime_error("SSGI sampling_mode must be legacy or screen_probes.");
                    ssgi.screenProbes = samplingMode == "screen_probes";
                    ssgi.screenTraceDistance = config.value("screen_trace_distance", ssgi.screenTraceDistance);
                    if (config.contains("max_trace_iterations") &&
                        (!config.at("max_trace_iterations").is_number_integer() ||
                            config.at("max_trace_iterations") < 1 ||
                            config.at("max_trace_iterations") > (std::numeric_limits<int>::max)()))
                        throw std::runtime_error("SSGI max_trace_iterations must be a positive integer representable as int.");
                    ssgi.maxTraceIterations = config.value("max_trace_iterations", ssgi.maxTraceIterations);
                    if (config.contains("camera_fade"))
                    {
                        const auto& fade = config.at("camera_fade");
                        if (!fade.is_object()) throw std::runtime_error("SSGI camera_fade must be an object.");
                        for (const auto& item : fade.items())
                            if (item.key() != "enabled" && item.key() != "start" && item.key() != "end")
                                throw std::runtime_error("Unknown SSGI camera_fade field: " + item.key());
                        for (const char* field : {"start", "end"})
                            if (fade.contains(field) && !fade.at(field).is_number())
                                throw std::runtime_error(std::string("SSGI camera_fade.") + field + " must be numeric.");
                        ssgi.cameraFadeEnabled = fade.value("enabled", ssgi.cameraFadeEnabled);
                        ssgi.cameraFadeStart = fade.value("start", ssgi.cameraFadeStart);
                        ssgi.cameraFadeEnd = fade.value("end", ssgi.cameraFadeEnd);
                    }
                    if (!ssgi.HasValidCameraFade())
                        throw std::runtime_error("SSGI camera_fade requires finite 0 <= start < end, in meters.");
                    if (config.contains("screen_probes"))
                    {
                        const auto& probes = config.at("screen_probes");
                        if (!probes.is_object()) throw std::runtime_error("SSGI screen_probes must be an object.");
                        for (const auto& item : probes.items())
                            if (item.key() != "screen_rays_per_probe" && item.key() != "emissive_samples_per_probe" &&
                                item.key() != "history_updates" && item.key() != "jitter_width_fraction" &&
                                item.key() != "adaptive")
                                throw std::runtime_error("Unknown SSGI screen_probes field: " + item.key());
                        for (const char* field : {"screen_rays_per_probe", "history_updates"})
                            if (probes.contains(field) && (!probes.at(field).is_number_integer() ||
                                probes.at(field) < 1 || probes.at(field) > (std::numeric_limits<int>::max)()))
                                throw std::runtime_error(std::string("SSGI screen_probes.") + field + " must be a positive integer representable as int.");
                        const auto screenRays = probes.value("screen_rays_per_probe", 64);
                        const auto updates = probes.value("history_updates", 4);
                        if (updates < 1 || updates > 32)
                            throw std::runtime_error("SSGI screen probes require history_updates in 1..32.");
                        ssgi.screenRaysPerProbe = screenRays;
                        if (probes.contains("emissive_samples_per_probe") &&
                            (!probes.at("emissive_samples_per_probe").is_number_integer() ||
                            probes.at("emissive_samples_per_probe") < 0 || probes.at("emissive_samples_per_probe") > 64))
                            throw std::runtime_error("SSGI emissive_samples_per_probe must be an integer in 0..64.");
                        ssgi.emissiveSamplesPerProbe = probes.value("emissive_samples_per_probe", 0);
                        if (!ssgi.HasValidScreenProbeSamples())
                            throw std::runtime_error("SSGI screen_rays_per_probe must be >=64 and divisible by64 (one or more traces per 8x8 direction cell).");
                        ssgi.screenProbeHistoryUpdates = updates;
                        if (probes.contains("jitter_width_fraction") && !probes.at("jitter_width_fraction").is_number())
                            throw std::runtime_error("SSGI jitter_width_fraction must be numeric.");
                        ssgi.screenProbeJitterWidthFraction = probes.value("jitter_width_fraction", 0.125f);
                        if (!ssgi.HasValidScreenProbeJitter())
                            throw std::runtime_error("SSGI jitter_width_fraction must be finite in 0..1 (total tile-width fraction).");
                        if (probes.contains("adaptive"))
                        {
                            const auto& adaptive = probes.at("adaptive");
                            if (!adaptive.is_object()) throw std::runtime_error("SSGI screen_probes.adaptive must be an object.");
                            for (const auto& item : adaptive.items())
                                if (item.key() != "enabled" && item.key() != "max_fraction")
                                    throw std::runtime_error("Unknown SSGI adaptive field: " + item.key());
                            if (adaptive.contains("max_fraction") && !adaptive.at("max_fraction").is_number())
                                throw std::runtime_error("SSGI adaptive.max_fraction must be numeric.");
                            ssgi.screenProbeAdaptiveEnabled = adaptive.value("enabled", false);
                            ssgi.screenProbeAdaptiveMaxFraction = adaptive.value("max_fraction", 0.25f);
                        }
                        if (!ssgi.HasValidScreenProbeAdaptive())
                            throw std::runtime_error("SSGI adaptive.max_fraction must be finite in 0..1.");
                    }

                    if (!ssgi.HasValidTraceDistances())
                        throw std::runtime_error("SSGI screen_trace_distance must be finite and > 0.");
                    ssgi.intensity = config.value("intensity", ssgi.intensity);
                    if (config.contains("trace_downsample_factor"))
                    {
                        const auto& factor = config.at("trace_downsample_factor");
                        if (!factor.is_number_integer() || factor < 1 ||
                            factor > (std::numeric_limits<int>::max)())
                            throw std::runtime_error("SSGI trace_downsample_factor must be a positive integer representable as int.");
                    }
                    ssgi.traceDownsampleFactor = config.value("trace_downsample_factor", ssgi.traceDownsampleFactor);
                    if (config.contains("denoise_downsample_factor"))
                    {
                        const auto& factor = config.at("denoise_downsample_factor");
                        if (!factor.is_number_integer() || (factor != 1 && factor != 2))
                            throw std::runtime_error("SSGI denoise_downsample_factor must be integer 1 or 2.");
                    }
                    ssgi.denoiseDownsampleFactor = config.value("denoise_downsample_factor", ssgi.denoiseDownsampleFactor);
                    if (!(std::isfinite(ssgi.intensity) && ssgi.intensity >= 0 &&
                        SSGIConfig::IsValidTraceDownsampleFactor(ssgi.traceDownsampleFactor)))
                        throw std::runtime_error("SSGI requires finite intensity >= 0 and positive integer trace_downsample_factor.");
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
