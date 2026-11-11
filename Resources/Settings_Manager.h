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
    bool enabled = false;
    bool giOnly = false; // Runtime presentation; Debug > Pass controls the editor view.
    bool screenProbes = false; // Explicit mode selection; missing key retains legacy fixtures.
    int raysPerScreenProbe = 128; // Total directional budget, including world fallback.
    int screenRaysPerProbe = 128; // Missing JSON key preserves the original full screen budget.
    int screenProbeHistoryUpdates = 4;
    float screenProbeJitterWidthFraction = 0.125f; // Total screen-tile jitter width; zero disables.
    int emissiveSamplesPerProbe = 0; // Extra screen-emitter samples; zero disables.
    float worldMeshMinHitDistance = 0.0f; // No blind interval; near-origin hits use triangle confirmation.
    float screenTraceDistance = 2.0f;
    float worldMeshMaxTraceDistance = 3.0f;
    float worldMaxTraceDistance = 4.0f; // Total surface-origin range; Global continues after Mesh.
    float intensity = 1.0f;
    int traceDownsampleFactor = 2;
    static constexpr bool IsValidTraceDownsampleFactor(int factor)
    {
        return factor >= 1;
    }
    bool HasValidScreenProbeRayCounts() const
    {
        const bool regular = raysPerScreenProbe >= 64 && raysPerScreenProbe % 64 == 0 &&
            screenRaysPerProbe >= 64 && screenRaysPerProbe % 64 == 0 &&
            screenRaysPerProbe <= raysPerScreenProbe && raysPerScreenProbe % screenRaysPerProbe == 0;
        return regular && emissiveSamplesPerProbe >= 0 && emissiveSamplesPerProbe <= 64;
    }
    bool HasValidScreenProbeJitter() const
    {
        return std::isfinite(screenProbeJitterWidthFraction) &&
            screenProbeJitterWidthFraction >= 0.0f && screenProbeJitterWidthFraction <= 1.0f;
    }
    static bool IsValidWorldMaxTraceDistance(float distance)
    {
        return std::isfinite(distance) && distance >= 2.0f && distance <= 16.0f;
    }
    bool HasValidTraceDistances() const
    {
        return IsValidWorldMaxTraceDistance(worldMaxTraceDistance) &&
            std::isfinite(screenTraceDistance) && screenTraceDistance > 0 && screenTraceDistance <= worldMaxTraceDistance &&
            std::isfinite(worldMeshMaxTraceDistance) && worldMeshMaxTraceDistance > 0 && worldMeshMaxTraceDistance <= worldMaxTraceDistance;
    }
    static bool IsValidMeshMinHitDistance(float distance)
    {
        return std::isfinite(distance) && distance >= 0;
    }
};

struct VXGIConfig
{
    bool enabled = false;
    float voxelSize = 0.5f;
    // 0=off, 1..3=allocation, 4..7=SDF, 8..12=world surface, 13..15=GPU probes.
    int debugView = 0;
    float sdfCellSize = 1.0f;
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
    // Extra fixed-size probe layers around surface cells; geometry stays unchanged.
    int probePaddingLayers = 1;
    int probeProbesPerFrame = 256;
    int probeClassifyRays = 64;
    int probeRaysPerUpdate = 64;
    int probeMaxHistoryUpdates = 32;
    float probeBackfaceThreshold = 0.25f;
    float probeFilterNeighborWeight = 0.25f;
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

                if (j.contains("ssgi"))
                {
                    const auto& config = j.at("ssgi");
                    ssgi.enabled = config.value("enabled", ssgi.enabled);
                    const auto samplingMode = config.value("sampling_mode", std::string("legacy"));
                    if (samplingMode != "legacy" && samplingMode != "screen_probes")
                        throw std::runtime_error("SSGI sampling_mode must be legacy or screen_probes.");
                    ssgi.screenProbes = samplingMode == "screen_probes";
                    ssgi.screenTraceDistance = config.value("screen_trace_distance", ssgi.screenTraceDistance);
                    if (config.contains("screen_probes"))
                    {
                        const auto& probes = config.at("screen_probes");
                        if (!probes.is_object()) throw std::runtime_error("SSGI screen_probes must be an object.");
                        for (const char* field : {"rays_per_probe", "screen_rays_per_probe", "history_updates"})
                            if (probes.contains(field) && (!probes.at(field).is_number_integer() ||
                                probes.at(field) < 1 || probes.at(field) > (std::numeric_limits<int>::max)()))
                                throw std::runtime_error(std::string("SSGI screen_probes.") + field + " must be a positive integer representable as int.");
                        const auto rays = probes.value("rays_per_probe", 128);
                        const auto screenRays = probes.value("screen_rays_per_probe", rays);
                        const auto updates = probes.value("history_updates", 4);
                        if ((rays < 64 || rays % 64 != 0) || updates < 1 || updates > 32)
                            throw std::runtime_error("SSGI screen probes require rays_per_probe >=64 divisible by 64, and history_updates in 1..32.");
                        ssgi.raysPerScreenProbe = rays;
                        ssgi.screenRaysPerProbe = screenRays;
                        if (probes.contains("emissive_samples_per_probe") &&
                            (!probes.at("emissive_samples_per_probe").is_number_integer() ||
                            probes.at("emissive_samples_per_probe") < 0 || probes.at("emissive_samples_per_probe") > 64))
                            throw std::runtime_error("SSGI emissive_samples_per_probe must be an integer in 0..64.");
                        ssgi.emissiveSamplesPerProbe = probes.value("emissive_samples_per_probe", 0);
                        if (!ssgi.HasValidScreenProbeRayCounts())
                            throw std::runtime_error("SSGI requires screen_rays_per_probe >=64 divisible by64 and dividing rays_per_probe.");
                        ssgi.screenProbeHistoryUpdates = updates;
                        if (probes.contains("jitter_width_fraction") && !probes.at("jitter_width_fraction").is_number())
                            throw std::runtime_error("SSGI jitter_width_fraction must be numeric.");
                        ssgi.screenProbeJitterWidthFraction = probes.value("jitter_width_fraction", 0.125f);
                        if (!ssgi.HasValidScreenProbeJitter())
                            throw std::runtime_error("SSGI jitter_width_fraction must be finite in 0..1 (total tile-width fraction).");
                    }
                    if (config.contains("world_sampling"))
                    {
                        const auto& sampling = config.at("world_sampling");
                        if (!sampling.is_object())
                            throw std::runtime_error("SSGI world_sampling must be an object.");
                        ssgi.worldMeshMaxTraceDistance = sampling.value("mesh_max_trace_distance", ssgi.worldMeshMaxTraceDistance);
                        ssgi.worldMaxTraceDistance = sampling.value("max_trace_distance", ssgi.worldMaxTraceDistance);
                        if (!SSGIConfig::IsValidWorldMaxTraceDistance(ssgi.worldMaxTraceDistance))
                            throw std::runtime_error("SSGI world_sampling.max_trace_distance must be finite and within [2, 16] meters.");
                        ssgi.worldMeshMinHitDistance = sampling.value("mesh_min_hit_distance", ssgi.worldMeshMinHitDistance);
                        if (!SSGIConfig::IsValidMeshMinHitDistance(ssgi.worldMeshMinHitDistance))
                            throw std::runtime_error("SSGI world_sampling.mesh_min_hit_distance must be finite and >= 0.");
                    }
                    if (!ssgi.HasValidTraceDistances())
                        throw std::runtime_error("SSGI screen_trace_distance and world_sampling.mesh_max_trace_distance must be finite, > 0 and <= world_sampling.max_trace_distance (2..16 meters).");
                    ssgi.intensity = config.value("intensity", ssgi.intensity);
                    if (config.contains("trace_downsample_factor"))
                    {
                        const auto& factor = config.at("trace_downsample_factor");
                        if (!factor.is_number_integer() || factor < 1 ||
                            factor > (std::numeric_limits<int>::max)())
                            throw std::runtime_error("SSGI trace_downsample_factor must be a positive integer representable as int.");
                    }
                    ssgi.traceDownsampleFactor = config.value("trace_downsample_factor", ssgi.traceDownsampleFactor);
                    if (!(std::isfinite(ssgi.intensity) && ssgi.intensity >= 0 &&
                        SSGIConfig::IsValidTraceDownsampleFactor(ssgi.traceDownsampleFactor)))
                        throw std::runtime_error("SSGI requires finite intensity >= 0 and positive integer trace_downsample_factor.");
                }

                if (j.contains("vxgi"))
                {
                    const auto& config = j.at("vxgi");
                    const auto readInteger = [](const nlohmann::json& object, const char* field, int fallback,
                        int minimum, int maximum) {
                        if (!object.contains(field)) return fallback;
                        const auto& value = object.at(field);
                        const std::string error = std::string("VXGI ") + field + " must be an integer from " +
                            std::to_string(minimum) + " to " + std::to_string(maximum) + ".";
                        if (!value.is_number_integer()) throw std::runtime_error(error);
                        if (value.is_number_unsigned())
                        {
                            const auto integer = value.get<uint64_t>();
                            if (integer < static_cast<uint64_t>(minimum) || integer > static_cast<uint64_t>(maximum))
                                throw std::runtime_error(error);
                            return static_cast<int>(integer);
                        }
                        const auto integer = value.get<int64_t>();
                        if (integer < minimum || integer > maximum) throw std::runtime_error(error);
                        return static_cast<int>(integer);
                    };
                    vxgi.enabled = config.value("enabled", vxgi.enabled);
                    vxgi.voxelSize = config.value("voxel_size", vxgi.voxelSize);
                    vxgi.debugView = readInteger(config, "debug_view", vxgi.debugView, 0, 20);
                    if (config.contains("global_sdf"))
                    {
                        const auto& global = config.at("global_sdf");
                        if (!global.is_object()) throw std::runtime_error("VXGI global_sdf must be an object.");
                        vxgi.globalSDFEnabled = global.value("enabled", vxgi.globalSDFEnabled);
                        vxgi.globalSDFClipmaps = readInteger(global, "clipmaps", vxgi.globalSDFClipmaps, 1, 3);
                        if (global.contains("cells_per_axis"))
                        {
                            const auto& cells = global.at("cells_per_axis");
                            vxgi.globalSDFLayerCells = {};
                            if (cells.is_array())
                            {
                                if (cells.size() != size_t(vxgi.globalSDFClipmaps))
                                    throw std::runtime_error("VXGI global_sdf.cells_per_axis needs one count per clipmap.");
                                for (size_t layer = 0; layer < cells.size(); ++layer)
                                {
                                    const nlohmann::json entry = {{"cells_per_axis", cells.at(layer)}};
                                    const int count = readInteger(entry, "cells_per_axis", 0, 2, 256);
                                    if (count % 2)
                                        throw std::runtime_error("VXGI global_sdf cells_per_axis counts must be even.");
                                    vxgi.globalSDFLayerCells[layer] = uint32_t(count);
                                }
                                vxgi.globalSDFCells = int(vxgi.globalSDFLayerCells[0]);
                            }
                            else
                                vxgi.globalSDFCells = readInteger(global, "cells_per_axis", vxgi.globalSDFCells, 2, 256);
                        }
                        vxgi.globalSDFCellSize = global.value("cell_size", vxgi.globalSDFCellSize);
                        vxgi.globalSDFTraceDistance = global.value("trace_distance", vxgi.globalSDFTraceDistance);
                        vxgi.globalSDFMaxSteps = readInteger(global, "max_trace_steps", vxgi.globalSDFMaxSteps, 1, 1024);
                        if (vxgi.globalSDFCells % 2 || !std::isfinite(vxgi.globalSDFCellSize) || vxgi.globalSDFCellSize <= 0)
                            throw std::runtime_error("VXGI global_sdf requires an even cell count and finite positive cell_size.");
                        if (!std::isfinite(vxgi.globalSDFTraceDistance) || vxgi.globalSDFTraceDistance <= 0)
                            throw std::runtime_error("VXGI global_sdf.trace_distance must be finite and positive.");
                    }
                    if (config.contains("probes"))
                    {
                        const auto& probes = config.at("probes");
                        if (!probes.is_object()) throw std::runtime_error("VXGI probes must be an object.");
                        vxgi.probePaddingLayers = readInteger(probes, "padding_layers", vxgi.probePaddingLayers, 0, 2);
                        vxgi.probeProbesPerFrame = readInteger(probes, "probes_per_frame", vxgi.probeProbesPerFrame, 1, 4096);
                        vxgi.probeClassifyRays = readInteger(probes, "classify_rays", vxgi.probeClassifyRays, 8, 256);
                        vxgi.probeRaysPerUpdate = readInteger(probes, "rays_per_update", vxgi.probeRaysPerUpdate, 8, 256);
                        vxgi.probeMaxHistoryUpdates = readInteger(probes, "max_history_updates", vxgi.probeMaxHistoryUpdates, 1, 256);
                        if (probes.contains("backface_threshold") && !probes.at("backface_threshold").is_number())
                            throw std::runtime_error("VXGI probes.backface_threshold must be a finite number from 0 to 1.");
                        vxgi.probeBackfaceThreshold = probes.value("backface_threshold", vxgi.probeBackfaceThreshold);
                        if (probes.contains("filter_neighbor_weight") && !probes.at("filter_neighbor_weight").is_number())
                            throw std::runtime_error("VXGI probes.filter_neighbor_weight must be a finite number from 0 to 1.");
                        vxgi.probeFilterNeighborWeight = probes.value("filter_neighbor_weight", vxgi.probeFilterNeighborWeight);
                    }
                    if (config.contains("surface"))
                    {
                        const auto& surface = config.at("surface");
                        vxgi.surfaceMaterialMipLevel = surface.value("material_mip_level", vxgi.surfaceMaterialMipLevel);
                        vxgi.surfaceRayMinDistance = surface.value("ray_min_distance", vxgi.surfaceRayMinDistance);
                        vxgi.surfaceMaxRayDistance = surface.value("max_ray_distance", vxgi.surfaceMaxRayDistance);
                    }
                    if (config.contains("sdf"))
                    {
                        const auto& sdf = config.at("sdf");
                        vxgi.sdfCellSize = sdf.value("cell_size", vxgi.sdfCellSize);
                        vxgi.sdfSliceAxis = readInteger(sdf, "slice_axis", vxgi.sdfSliceAxis, 0, 2);
                        vxgi.sdfSlicePosition = sdf.value("slice_position", vxgi.sdfSlicePosition);
                    }
                    if (!std::isfinite(vxgi.voxelSize) || vxgi.voxelSize <= 0 ||
                        vxgi.debugView < 0 || vxgi.debugView > 20 ||
                        vxgi.probePaddingLayers < 0 || vxgi.probePaddingLayers > 2 ||
                        vxgi.probeProbesPerFrame < 1 || vxgi.probeProbesPerFrame > 4096 ||
                        vxgi.probeMaxHistoryUpdates < 1 || vxgi.probeMaxHistoryUpdates > 256 ||
                        vxgi.probeClassifyRays < 8 || vxgi.probeClassifyRays > 256 || vxgi.probeClassifyRays % 2 != 0 ||
                        vxgi.probeRaysPerUpdate < 8 || vxgi.probeRaysPerUpdate > 256 || vxgi.probeRaysPerUpdate % 2 != 0 ||
                        !std::isfinite(vxgi.probeBackfaceThreshold) || vxgi.probeBackfaceThreshold < 0 || vxgi.probeBackfaceThreshold > 1 ||
                        !std::isfinite(vxgi.probeFilterNeighborWeight) || vxgi.probeFilterNeighborWeight < 0 || vxgi.probeFilterNeighborWeight > 1 ||
                        !std::isfinite(vxgi.surfaceMaterialMipLevel) || vxgi.surfaceMaterialMipLevel < 0 ||
                        !std::isfinite(vxgi.surfaceRayMinDistance) || vxgi.surfaceRayMinDistance <= 0 ||
                        !std::isfinite(vxgi.surfaceMaxRayDistance) || vxgi.surfaceMaxRayDistance <= vxgi.surfaceRayMinDistance ||
                        !std::isfinite(vxgi.sdfCellSize) || vxgi.sdfCellSize <= 0 ||
                        vxgi.sdfSliceAxis < 0 || vxgi.sdfSliceAxis > 2 || !std::isfinite(vxgi.sdfSlicePosition))
                        throw std::runtime_error("VXGI requires positive voxel/SDF sizes, probes.padding_layers 0..2, probes_per_frame 1..4096, max_history_updates 1..256, debug_view 0..20, even probe ray counts 8..256, finite backface_threshold/filter_neighbor_weight 0..1, valid slices, nonnegative surface mip, and max_ray_distance > ray_min_distance > 0 (all finite).");
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
