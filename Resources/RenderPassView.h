#pragma once

#include <array>
#include <cstdint>

// Logical viewport stages, rather than every internal RDG dispatch. Feature
// settings remain in Pipeline.json; selecting a view never enables a feature.
enum class RenderPassId : uint8_t
{
    Default,
    Shadow,
    ZPrepass,
    GBuffer,
    MotionVector,
    HBAO,
    DeferredLighting,
    PBROpaque,
    GI,
    Skybox,
    Transparent,
    AntiAliasing,
    PostProcess
};

enum class RenderPassResult : uint8_t
{
    PassOutput,
    Accumulated
};

enum class RenderPassGISource : uint8_t
{
    Default,
    SSGIOnly,
    SSGIAndMeshSDF
};

inline constexpr const char* RenderPassGISourceId(RenderPassGISource source) noexcept
{
    switch (source)
    {
    case RenderPassGISource::SSGIOnly: return "ssgi";
    case RenderPassGISource::SSGIAndMeshSDF: return "mesh-sdf";
    default: return "default";
    }
}

// Multiple attachments of the same GBuffer pass, rather than extra stages.
enum class RenderPassChannel : uint8_t
{
    Albedo,
    Normal,
    ORM,
    Emissive,
    Depth
};

struct RenderPassChannelInfo
{
    RenderPassChannel channel;
    const char* id;
    const char* label;
};

inline constexpr std::array<RenderPassChannelInfo, 5> RenderPassChannels = {{
    {RenderPassChannel::Albedo, "albedo", "Albedo"},
    {RenderPassChannel::Normal, "normal", "Normal"},
    {RenderPassChannel::ORM, "orm", "ORM"},
    {RenderPassChannel::Emissive, "emissive", "Emissive"},
    {RenderPassChannel::Depth, "depth", "Depth"}
}};

struct RenderPassInfo
{
    RenderPassId pass;
    const char* id;
    const char* label;
    bool passOutput;
    bool accumulated;
};

// AA algorithms share one stage. Tone mapping belongs to PostProcess. Their
// output is the processed scene, so neither offers an isolated contribution.
inline constexpr std::array<RenderPassInfo, 13> RenderPassCatalog = {{
    {RenderPassId::Default, "default", "\xE9\xBB\x98\xE8\xAE\xA4", false, true},
    {RenderPassId::Shadow, "shadow", "Shadow", true, false},
    {RenderPassId::ZPrepass, "z-prepass", "ZPrepass", true, false},
    {RenderPassId::GBuffer, "gbuffer", "GBuffer", true, false},
    {RenderPassId::MotionVector, "motion-vector", "MotionVector", true, false},
    {RenderPassId::HBAO, "hbao", "HBAO", true, false},
    {RenderPassId::DeferredLighting, "deferred-lighting", "DeferredLighting", true, true},
    {RenderPassId::PBROpaque, "pbr-opaque", "PBR Opaque", true, true},
    {RenderPassId::GI, "gi", "GI", true, true},
    {RenderPassId::Skybox, "skybox", "Skybox", true, true},
    {RenderPassId::Transparent, "transparent", "Transparent", true, true},
    {RenderPassId::AntiAliasing, "anti-aliasing", "\xE6\x8A\x97\xE9\x94\xAF\xE9\xBD\xBF", false, true},
    {RenderPassId::PostProcess, "post-process", "\xE5\x90\x8E\xE5\xA4\x84\xE7\x90\x86", false, true}
}};

struct RenderPassAvailability
{
    bool deferred = true;
    bool zPrepass = false;
    bool hbao = false;
    bool ssgi = false;
    bool vxgi = false;
    bool antiAliasing = false;
    bool motionVectors = false;
    bool transparent = false;
    bool globalSDF = false;
};

struct RenderPassView
{
    RenderPassId pass = RenderPassId::Default;
    RenderPassResult result = RenderPassResult::Accumulated;
    RenderPassChannel channel = RenderPassChannel::Albedo;
    RenderPassGISource giSource = RenderPassGISource::Default;
};

inline constexpr const RenderPassInfo* GetRenderPassInfo(RenderPassId pass) noexcept
{
    for (const auto& info : RenderPassCatalog)
        if (info.pass == pass)
            return &info;
    return nullptr;
}

inline constexpr bool IsScreenGIAvailable(const RenderPassAvailability& available) noexcept
{
    return available.deferred && available.ssgi;
}

inline constexpr bool IsWorldGIAvailable(const RenderPassAvailability& available) noexcept
{
    // The current world cache supplements the deferred SSGI trace. It is not a
    // separate scene-color pass merely because its resources were initialized.
    return IsScreenGIAvailable(available) && available.vxgi;
}

inline constexpr bool IsGlobalGIAvailable(const RenderPassAvailability& available) noexcept
{
    return IsWorldGIAvailable(available) && available.globalSDF;
}

inline constexpr bool IsRenderPassAvailable(RenderPassId pass,
    const RenderPassAvailability& available) noexcept
{
    switch (pass)
    {
    case RenderPassId::Default:
    case RenderPassId::Shadow:
    case RenderPassId::Skybox:
    case RenderPassId::PostProcess:
        return true;
    case RenderPassId::ZPrepass:
        return available.zPrepass;
    case RenderPassId::GBuffer:
    case RenderPassId::DeferredLighting:
        return available.deferred;
    case RenderPassId::PBROpaque:
        return !available.deferred;
    case RenderPassId::MotionVector:
        return available.motionVectors;
    case RenderPassId::HBAO:
        return available.deferred && available.hbao;
    case RenderPassId::GI:
        return IsScreenGIAvailable(available);
    case RenderPassId::Transparent:
        return available.transparent;
    case RenderPassId::AntiAliasing:
        return available.antiAliasing;
    }
    return false;
}

inline constexpr bool SupportsRenderPassResult(RenderPassId pass, RenderPassResult result) noexcept
{
    const auto* info = GetRenderPassInfo(pass);
    if (!info)
        return false;
    switch (result)
    {
    case RenderPassResult::PassOutput: return info->passOutput;
    case RenderPassResult::Accumulated: return info->accumulated;
    }
    return false;
}

inline constexpr bool CanSelectRenderPassView(const RenderPassView& view,
    const RenderPassAvailability& available) noexcept
{
    return IsRenderPassAvailable(view.pass, available) && SupportsRenderPassResult(view.pass, view.result) &&
        (view.giSource != RenderPassGISource::SSGIAndMeshSDF || IsWorldGIAvailable(available));
}

// Disabled stages return to the normal frame. A supported stage keeps its
// selection when only the result mode needs correcting (e.g. AA -> accumulated).
inline constexpr bool NormalizeRenderPassView(RenderPassView& view,
    const RenderPassAvailability& available) noexcept
{
    const auto before = view;
    if (!IsRenderPassAvailable(view.pass, available))
        view = {};
    else if (!SupportsRenderPassResult(view.pass, view.result))
    {
        const auto* info = GetRenderPassInfo(view.pass);
        view.result = info->passOutput ? RenderPassResult::PassOutput : RenderPassResult::Accumulated;
    }
    if (view.pass != RenderPassId::GBuffer ||
        static_cast<uint8_t>(view.channel) > static_cast<uint8_t>(RenderPassChannel::Depth))
        view.channel = RenderPassChannel::Albedo;
    if (view.pass != RenderPassId::GI || view.result != RenderPassResult::PassOutput ||
        static_cast<uint8_t>(view.giSource) > static_cast<uint8_t>(RenderPassGISource::SSGIAndMeshSDF) ||
        (view.giSource == RenderPassGISource::SSGIAndMeshSDF && !IsWorldGIAvailable(available)))
        view.giSource = RenderPassGISource::Default;
    return before.pass != view.pass || before.result != view.result || before.channel != view.channel ||
        before.giSource != view.giSource;
}
