#ifndef SSGI_PASS_H
#define SSGI_PASS_H

#include "stdafx.h"
#include "RDG.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Settings_Manager.h"
#include "VXGIGPUScene.h"
#include "VXGIGlobalSDF.h"
#include "VXGIGlobalSDFRuntime.h"
#include "../Shaders/SSGIMeshTracePolicy.hlsli"

// Keep these layouts in sync with the SSGI shader cbuffers.
constexpr uint32_t SSGIMaxLevels = 7; // Reduced mips 1..7; original depth is mip 0.

struct SSGIPyramidConstants
{
    DirectX::XMFLOAT4X4 invProj;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t depthIndex;
    uint32_t outputDepthIndex;
    uint32_t firstLevel;
    uint32_t padding;
};
static_assert(sizeof(SSGIPyramidConstants) == 96);

struct SSGIHybridParameters
{
    DirectX::XMFLOAT3 gridOrigin; float cellSize;
    DirectX::XMUINT3 cellDimensions; uint32_t triangleCount;
    DirectX::XMFLOAT3 boundsMaximum; uint32_t materialCount;
    // Reserved to preserve the shared shader constant-buffer layout.
    // World fallback reads cached radiance; it has no per-hit light/range inputs.
    DirectX::XMFLOAT3 reservedLighting0; float materialMipLevel;
    DirectX::XMFLOAT3 reservedLighting1; float worldMeshMaxTraceDistance; // y = Mesh ray minimum distance.
    float worldMaxTraceDistance; float voxelSize; uint32_t probeCount; uint32_t worldMode;
    uint32_t nodeCount, assetCount, instanceCount, bvhNodeCount;
};
static_assert(sizeof(SSGIHybridParameters) == 112);

struct SSGIGlobalParameters
{
    uint32_t enabled = 0, textureIndex = 0;
    float maxTraceDistance = 0;
    uint32_t maxSteps = 0;
    VXGIGlobalSDF::Constants constants;
};
static_assert(sizeof(SSGIGlobalParameters) == 192 && offsetof(SSGIGlobalParameters, constants) == 16);

struct SSGITraceConstants
{
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 invProj;
    uint32_t width;
    uint32_t height;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t ormIndex;
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t levelCount;
    uint32_t frameIndex;
    uint32_t blueNoiseIndex;
    float radius;
    float thickness;
    float normalBias;
    float padding2;
    uint32_t rayCount;
    uint32_t maxIterations;
    uint32_t albedoIndex;
    uint32_t traceDownsampleFactor;
    // uint arrays in HLSL cbuffers have 16-byte stride. Use uint4 explicitly.
    // x = depth SRV. Reserved yzw slots also carry screen/world-policy inputs;
    // see their assignments after pyramid creation. This preserves the 1024B ABI.
    DirectX::XMUINT4 levels[SSGIMaxLevels];
    DirectX::XMFLOAT4X4 invView;
    DirectX::XMFLOAT4X4 invViewProj;
    SSGIHybridParameters hybrid;
    // Appended to preserve the worldMode SRV alias at byte 604.
    DirectX::XMFLOAT4X4 previousViewProj;
    DirectX::XMFLOAT4X4 previousInvViewProj;
    uint32_t previousGIIndex;
    uint32_t previousDepthIndex;
    uint32_t previousNormalIndex;
    uint32_t historyValid;
    uint32_t probeNeighborsIndex; // Optional bindless six-neighbor topology SRV.
    uint32_t previousFastIndex;
    uint32_t reprojectedGIIndex;
    uint32_t reprojectedFastIndex;
    SSGIGlobalParameters global;
    // Separate SRV/UAV descriptors are supplied to the sparse and full-rate passes.
    uint32_t worldSharingEnabled;
    uint32_t sharedMeshLightingIndex;
    uint32_t sharedMeshReceiverIndex;
    uint32_t sharedGlobalLightingIndex;
    uint32_t sharedGlobalReceiverIndex;
    uint32_t sharedRepairQueueIndex;
    uint32_t sharedRepairCountIndex;
    uint32_t sharedMeshSpan;
    uint32_t previousMeshBandIndex;
    uint32_t previousGlobalBandIndex;
    uint32_t currentMeshBandIndex;
    uint32_t currentGlobalBandIndex;
};
static_assert(offsetof(SSGITraceConstants, levels) == 272);
static_assert(offsetof(SSGITraceConstants, levels) + offsetof(DirectX::XMUINT4, y) == 276);
static_assert(offsetof(SSGITraceConstants, levels) + sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, w) == 300);
static_assert(offsetof(SSGITraceConstants, levels) + 2 * sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, y) == 308);
static_assert(offsetof(SSGITraceConstants, levels) + 2 * sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, w) == 316);
static_assert(offsetof(SSGITraceConstants, levels) + 3 * sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, y) == 324);
static_assert(offsetof(SSGITraceConstants, levels) + 3 * sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, z) == 328);
static_assert(offsetof(SSGITraceConstants, levels) + 6 * sizeof(DirectX::XMUINT4) + offsetof(DirectX::XMUINT4, z) == 376);
static_assert(offsetof(SSGITraceConstants, albedoIndex) == 264);
static_assert(offsetof(SSGITraceConstants, traceDownsampleFactor) == 268);
static_assert(offsetof(SSGITraceConstants, invView) == 384);
static_assert(offsetof(SSGITraceConstants, hybrid) == 512);
static_assert(offsetof(SSGITraceConstants, hybrid) + offsetof(SSGIHybridParameters, worldMode) == 604);
static_assert(offsetof(SSGITraceConstants, previousViewProj) == 624);
static_assert(offsetof(SSGITraceConstants, previousGIIndex) == 752);
static_assert(offsetof(SSGITraceConstants, probeNeighborsIndex) == 768);
static_assert(offsetof(SSGITraceConstants, reprojectedGIIndex) == 776);
static_assert(offsetof(SSGITraceConstants, global) == 784);
static_assert(offsetof(SSGITraceConstants, worldSharingEnabled) == 976);
static_assert(offsetof(SSGITraceConstants, previousMeshBandIndex) == 1008);
static_assert(sizeof(SSGITraceConstants) == 1024);

struct SSGIReconstructConstants
{
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 invProj;
    uint32_t width;
    uint32_t height;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t ormIndex;
    uint32_t rawIndex;
    uint32_t outputIndex;
    uint32_t traceDownsampleFactor;
    uint32_t padding[2];
    uint32_t worldSpatialStep;
    uint32_t reserved[3];
};
static_assert(sizeof(SSGIReconstructConstants) == 192);
static_assert(offsetof(SSGIReconstructConstants, worldSpatialStep) == 176);
static_assert(offsetof(SSGIReconstructConstants, traceDownsampleFactor) == 164);

struct SSGITemporalConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t ormIndex;
    uint32_t currentIndex;
    uint32_t reprojectedGIIndex;
    uint32_t reprojectedFastIndex;
    uint32_t outputGIIndex;
    uint32_t outputDepthIndex;
    uint32_t outputNormalIndex;
    uint32_t outputFastIndex;
    float maxHistoryFrames;
    float maxFastAccumulatedFrames;
    uint32_t traceDownsampleFactor;
    uint32_t padding[3];
};
static_assert(sizeof(SSGITemporalConstants) == 80);
static_assert(offsetof(SSGITemporalConstants, reprojectedGIIndex) == 32);
static_assert(offsetof(SSGITemporalConstants, outputGIIndex) == 40);
static_assert(offsetof(SSGITemporalConstants, traceDownsampleFactor) == 64);

struct SSGIHistoryClampConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t slowIndex;
    uint32_t fastIndex;
    uint32_t noisyIndex;
    uint32_t outputGIIndex;
    uint32_t outputFastIndex;
    float sigmaScale;
    float accelerationAmount;
    float resetAmount;
    float spatialSigmaScale;
    float temporalSigmaScale;
    uint32_t outputMomentsIndex;
    uint32_t padding[3];
};
static_assert(sizeof(SSGIHistoryClampConstants) == 64);

struct SSGIAtrousConstants
{
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 invProj;
    uint32_t width;
    uint32_t height;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t giIndex;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t historyIndex;
    uint32_t outputIndex;
    uint32_t step;
    float phiLuminance;
    float depthThreshold;
    uint32_t traceDownsampleFactor;
    uint32_t padding[3];
};
static_assert(sizeof(SSGIAtrousConstants) == 192);
static_assert(offsetof(SSGIAtrousConstants, traceDownsampleFactor) == 176);

struct SSGICompositeConstants
{
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 invProj;
    uint32_t width;
    uint32_t height;
    uint32_t halfWidth;
    uint32_t halfHeight;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t ormIndex;
    uint32_t albedoIndex;
    uint32_t giIndex;
    float intensity;
    uint32_t traceDownsampleFactor;
    uint32_t padding;
};
static_assert(sizeof(SSGICompositeConstants) == 176);
static_assert(offsetof(SSGICompositeConstants, traceDownsampleFactor) == 168);

class SSGIPass
{
public:
    struct WorldInput
    {
        const VXGIGPUScene* scene = nullptr;
        ID3D12Resource* rawRecords = nullptr;
        ID3D12Resource* distanceCache = nullptr;
        ID3D12Resource* materialBuffer = nullptr;
        uint32_t probeCount = 0;
        float voxelSize = 1.0f;
        uint32_t mode = 0;
        const VXGIGlobalSDFRuntime* globalSDF = nullptr;
        float globalTraceDistance = 128.0f;
        uint32_t globalMaxSteps = 256;
    };
    static uint32_t EffectiveWorldMode(const WorldInput* world, int frameIndex)
    {
        if (!world || world->mode == 0 || world->mode > 2 || !world->scene ||
            !world->scene->IsMeshBackend(frameIndex) || !world->scene->GetGrid(frameIndex).valid ||
            !world->scene->GetDistanceNodeCount(frameIndex) || !world->scene->GetAssetCount(frameIndex) ||
            !world->scene->GetInstanceCount(frameIndex) || !world->scene->GetInstanceBVHNodeCount(frameIndex)) return 0;
        for (const UINT i : {0u, 7u, 8u, 10u}) if (!world->scene->Get(frameIndex, i)) return 0;
        if (!world->rawRecords || !world->probeCount ||
            !world->scene->Get(frameIndex, 5)) return 0;
        return world->mode;
    }
    struct Input
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
        RDGTextureHandle normal;
        RDGTextureHandle orm;
        RDGTextureHandle albedo;
        RDGTextureHandle bounceSource;
        RDGTextureHandle emissive;
    };
    struct Output
    {
        RDGTextureHandle sceneColor;
        RDGPassHandle pass;
        RDGTextureHandle indirectLighting; // Final filtered E/pi, before receiver material response.
        RDGTextureHandle rawLighting;
        RDGTextureHandle reconstructedLighting;
        RDGTextureHandle worldDisplay;
        RDGTextureHandle worldOwners;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
    };

    struct TextureViews
    {
        RDGTextureSRVHandle depth;
        RDGTextureSRVHandle normal;
        RDGTextureSRVHandle orm;
        RDGTextureSRVHandle albedo;
    };

    static Output AddToGraph(RDGBuilder& graph, ResourceManager* resourceManager, PipelineManager* pipelineManager,
        const SSGIHistoryState& history, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input,
        const WorldInput* world = nullptr)
    {
        auto lighting = AddLightingToGraph(graph, resourceManager, pipelineManager, history, view, proj,
            invProj, invViewProj, width, height, frameIndex, sampleFrame, config, input, world, false);
        if (!lighting.gi.IsValid() || !lighting.pass.IsValid()) return {};
        const auto factor = config.screenProbes ? 1u : static_cast<uint32_t>(config.traceDownsampleFactor);
        auto composite = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, factor, lighting.views, {input.sceneColor, lighting.gi}, config.intensity, config.giOnly, config.screenProbes ? RDGTextureHandle{} : lighting.receiverGuide);
        if (!composite.IsValid()) return {};
        return {input.sceneColor, composite, lighting.gi, lighting.raw, lighting.reconstructed, lighting.worldDisplay, lighting.worldOwners, lighting.receiverGuide,lighting.probePosition,lighting.probeNormal,lighting.probeFiltered};
    }

    // Diagnostic output only. The normal mixed GI and its SceneColor/history
    // continue unchanged; this branch uses a separate temporal history pair.
    // EnsureSSGIPreviewHistoryResources must have succeeded before recording it.
    static RDGTextureHandle AddLightingPreviewToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input,
        const WorldInput* world = nullptr)
    {
        auto lighting = AddLightingToGraph(graph, resourceManager, pipelineManager, history, view, proj,
            invProj, invViewProj, width, height, frameIndex, sampleFrame, config, input, world, true);
        return lighting.pass.IsValid() ? lighting.gi : RDGTextureHandle{};
    }

    struct IrradiancePreviewOutput
    {
        RDGTextureHandle texture;
        RDGPassHandle pass;
    };

    // Use the production composite's geometry-guided upsample and intensity for
    // pure GI. Its full-resolution HDR output can then follow the configured AA
    // and tone mapping without changing normal SceneColor or GI histories.
    static IrradiancePreviewOutput AddIrradiancePreviewToGraph(RDGBuilder& graph,
        ResourceManager* resourceManager, PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor,
        const Input& input, RDGTextureHandle filteredGI, float intensity, RDGTextureHandle receiverGuide = {})
    {
        if (!resourceManager || !pipelineManager || !width || !height || !filteredGI.IsValid() ||
            !SSGIConfig::IsValidTraceDownsampleFactor(static_cast<int>(traceDownsampleFactor))) return {};
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = RenderFormats::DepthSRV;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        TextureViews views;
        views.depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        views.normal = graph.CreateTextureSRVView(input.normal);
        views.orm = graph.CreateTextureSRVView(input.orm);
        views.albedo = graph.CreateTextureSRVView(input.albedo);
        if (!views.depth.IsValid() || !views.normal.IsValid() || !views.orm.IsValid() || !views.albedo.IsValid())
            return {};
        RDGTextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = RenderFormats::SceneColor;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto target = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON, "SSGI.IrradiancePreview");
        if (!target.IsValid()) return {};
        const auto pass = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj,
            width, height, frameIndex, traceDownsampleFactor, views, {target, filteredGI}, intensity, true, receiverGuide);
        return pass.IsValid() ? IrradiancePreviewOutput{target, pass} : IrradiancePreviewOutput{};
    }

private:
    using WorldAddresses = std::array<D3D12_GPU_VIRTUAL_ADDRESS, 6>;
    static UINT TraceExtent(UINT dimension, uint32_t factor)
    {
        return dimension / factor + (dimension % factor != 0 ? 1u : 0u);
    }
    struct LightingOutput
    {
        RDGTextureHandle gi;
        RDGPassHandle pass;
        TextureViews views;
        RDGTextureHandle raw, reconstructed, worldDisplay, worldOwners;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
    };
    static LightingOutput AddLightingToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input,
        const WorldInput* world, bool previewHistory)
    {
        if (!resourceManager || !pipelineManager || width == 0 || height == 0 ||
            !SSGIConfig::IsValidTraceDownsampleFactor(config.traceDownsampleFactor) ||
            !SSGIConfig::IsValidMeshMinHitDistance(config.worldMeshMinHitDistance) ||
            !config.HasValidTraceDistances()) return {};
        if (config.screenProbes && (!config.HasValidScreenProbeRayCounts() || !config.HasValidScreenProbeJitter() ||
            config.screenProbeHistoryUpdates < 1 || config.screenProbeHistoryUpdates > 32)) return {};
        const uint32_t traceDownsampleFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
        const UINT traceWidth = TraceExtent(width, traceDownsampleFactor), traceHeight = TraceExtent(height, traceDownsampleFactor);
        // A resolution/factor change must recreate both history sets before any
        // pass is recorded. Refuse mismatched resources rather than writing OOB.
        for (int index = 0; index < 2; ++index)
        {
            const auto& textures = resourceManager->GetSSGIHistoryTextures(index, previewHistory);
            for (ID3D12Resource* texture : {textures.gi.Get(), textures.fastGI.Get(),
                textures.screenBand.Get(), textures.meshBand.Get(), textures.globalBand.Get(),
                textures.worldPolicy.Get(), textures.depth.Get(), textures.normal.Get()})
            {
                if (!texture) return {};
                const auto desc = texture->GetDesc();
                const bool fullHistory = config.screenProbes && (texture == textures.gi.Get() || texture == textures.fastGI.Get() ||
                    texture == textures.depth.Get() || texture == textures.normal.Get());
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    desc.Width != (fullHistory ? width : traceWidth) || desc.Height != (fullHistory ? height : traceHeight))
                    return {};
            }
            for (ID3D12Resource* texture : {textures.globalOwnerRaw.Get(), textures.globalOwnerState.Get(),
                textures.globalOwnerReceiver.Get()})
            {
                if (!texture) return {};
                const auto desc = texture->GetDesc();
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    desc.Width != 2u * ((traceWidth + 1u) / 2u) || desc.Height != (traceHeight + 1u) / 2u) return {};
            }
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = RenderFormats::DepthSRV;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        TextureViews views;
        views.depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        views.normal = graph.CreateTextureSRVView(input.normal);
        views.orm = graph.CreateTextureSRVView(input.orm);
        views.albedo = graph.CreateTextureSRVView(input.albedo);
        if (!views.depth.IsValid() || !views.normal.IsValid() || !views.orm.IsValid() || !views.albedo.IsValid())
        {
            return {};
        }
        auto trace = AddTraceToGraph(graph, resourceManager, pipelineManager, view, proj, invProj, invViewProj, width, height,
            frameIndex, sampleFrame, config, views, {input.bounceSource, input.emissive}, world, history, previewHistory);
        if (!trace.reconstructedGI.IsValid() || !trace.reconstructionPass.IsValid())
        {
            return {};
        }
        auto temporal = AddTemporalToGraph(graph, resourceManager, pipelineManager, width, height,
            frameIndex, config.screenProbes ? 1u : traceDownsampleFactor, views,
            {trace.reconstructedGI, trace.reprojectedGI, trace.reprojectedFast}, previewHistory, config.screenProbes ? RDGTextureHandle{} : trace.receiverGuide,
            config.screenProbes ? traceDownsampleFactor : 1u);
        if (!temporal.gi.IsValid() || !temporal.pass.IsValid())
        {
            return {};
        }
        auto spatial = AddAtrousToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, config.screenProbes ? 1u : traceDownsampleFactor, views, {temporal.moments, temporal.gi}, config.screenProbes ? RDGTextureHandle{} : trace.receiverGuide);
        if (!spatial.gi.IsValid() || !spatial.pass.IsValid())
        {
            return {};
        }
        return {spatial.gi, spatial.pass, views, trace.rawGI, trace.reconstructedGI, trace.worldDisplay, trace.worldOwners,trace.receiverGuide,trace.probePosition,trace.probeNormal,trace.probeFiltered};
    }

    struct TraceInput
    {
        RDGTextureHandle bounceSource;
        RDGTextureHandle emissive;
    };
    struct TraceOutput
    {
        RDGTextureHandle rawGI;
        RDGPassHandle tracePass;
        RDGTextureHandle reconstructedGI;
        RDGPassHandle reconstructionPass;
        RDGTextureHandle reprojectedGI;
        RDGTextureHandle reprojectedFast;
        RDGTextureHandle worldDisplay;
        RDGTextureHandle worldOwners;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
    };

#include "ScreenProbeGather.inl"

    static TraceOutput AddTraceToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj,
        UINT width, UINT height, int frameIndex, UINT sampleFrame,
        const SSGIConfig& config, const TextureViews& views, const TraceInput& input, const WorldInput* world,
        const SSGIHistoryState& history, bool previewHistory)
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto orm = views.orm;
        auto source = graph.CreateTextureSRVView(input.bounceSource);
        const int historyIndex = resourceManager->GetSSGICurrentHistoryIdx(previewHistory);
        const auto& previous = resourceManager->GetSSGIHistoryTextures(1 - historyIndex, previewHistory);
        const auto& current = resourceManager->GetSSGIHistoryTextures(historyIndex, previewHistory);
        const auto importHistory = [&](ID3D12Resource* texture, const char* name) {
            return graph.CreateTextureSRVView(graph.RegisterExternalTexture(
                texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name));
        };
        auto previousGI = importHistory(previous.gi.Get(), "SSGI.PreviousGI");
        auto previousFast = importHistory(previous.fastGI.Get(), "SSGI.PreviousFastGI");
        auto previousHistoryDepth = importHistory(previous.depth.Get(), "SSGI.PreviousDepth");
        auto previousNormal = importHistory(previous.normal.Get(), "SSGI.PreviousNormal");
        auto previousScreenBand = importHistory(previous.screenBand.Get(), "SSGI.PreviousScreenBand");
        auto previousWorldPolicy = importHistory(previous.worldPolicy.Get(), "SSGI.PreviousWorldPolicy");
        auto previousMeshBand = importHistory(previous.meshBand.Get(), "SSGI.PreviousMeshBand");
        auto previousGlobalBand = importHistory(previous.globalBand.Get(), "SSGI.PreviousGlobalBand");
        auto currentScreenBand = graph.CreateTextureUAVView(graph.RegisterExternalTexture(
            current.screenBand.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.CurrentScreenBand"));
        auto currentWorldPolicy = graph.CreateTextureUAVView(graph.RegisterExternalTexture(
            current.worldPolicy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.CurrentWorldPolicy"));
        auto currentMeshBand = graph.CreateTextureUAVView(graph.RegisterExternalTexture(
            current.meshBand.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.CurrentMeshBand"));
        auto currentGlobalBand = graph.CreateTextureUAVView(graph.RegisterExternalTexture(
            current.globalBand.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.CurrentGlobalBand"));
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !source.IsValid() ||
            !previousGI.IsValid() || !previousFast.IsValid() || !previousHistoryDepth.IsValid() || !previousNormal.IsValid() ||
            !previousScreenBand.IsValid() || !previousWorldPolicy.IsValid() ||
            !previousMeshBand.IsValid() || !previousGlobalBand.IsValid() ||
            !currentScreenBand.IsValid() || !currentWorldPolicy.IsValid() ||
            !currentMeshBand.IsValid() || !currentGlobalBand.IsValid())
        {
            return {};
        }

        auto noiseTexture = graph.RegisterExternalTexture(resourceManager->GetBlueNoiseTexture(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "BlueNoise.STBN.Vector2");
        D3D12_SHADER_RESOURCE_VIEW_DESC noiseDesc = {};
        noiseDesc.Format = DXGI_FORMAT_R8G8_UINT;
        noiseDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        noiseDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        noiseDesc.Texture2DArray.MipLevels = 1;
        noiseDesc.Texture2DArray.ArraySize = STBN_FRAMES;
        auto noise = graph.CreateTextureSRVView(noiseTexture, &noiseDesc);
        if (!noise.IsValid())
        {
            return {};
        }

        RDGPassParameters traceParams;
        traceParams.ReadComputeSRV(depth);
        traceParams.ReadComputeSRV(normal);
        traceParams.ReadComputeSRV(orm);
        traceParams.ReadComputeSRV(views.albedo);
        traceParams.ReadComputeSRV(source);
        traceParams.ReadComputeSRV(noise);
        traceParams.ReadComputeSRV(previousGI);
        traceParams.ReadComputeSRV(previousFast);
        traceParams.ReadComputeSRV(previousHistoryDepth);
        traceParams.ReadComputeSRV(previousNormal);
        traceParams.ReadComputeSRV(previousScreenBand);
        traceParams.ReadComputeSRV(previousWorldPolicy);
        // SharedWorld produces quality/provenance; Trace and Repair read/update
        // it through the same R32_UINT UAV. RDG orders their UAV accesses.
        traceParams.WriteUAV(currentWorldPolicy);
        traceParams.ReadComputeSRV(previousMeshBand);
        traceParams.ReadComputeSRV(previousGlobalBand);
        SSGITraceConstants trace = {};
        trace.view = view;
        trace.proj = proj;
        trace.invProj = invProj;
        DirectX::XMStoreFloat4x4(&trace.invView, DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&view)));
        trace.invViewProj = invViewProj;
        trace.previousViewProj = history.previousViewProj;
        trace.previousInvViewProj = history.previousInvViewProj;
        trace.previousGIIndex = previousGI.descriptorIndex;
        trace.previousFastIndex = previousFast.descriptorIndex;
        trace.previousDepthIndex = previousHistoryDepth.descriptorIndex;
        trace.previousNormalIndex = previousNormal.descriptorIndex;
        trace.historyValid = history.valid;
        trace.probeNeighborsIndex = UINT_MAX; // Missing topology uses the original lookup.
        trace.previousMeshBandIndex = previousMeshBand.descriptorIndex;
        trace.previousGlobalBandIndex = previousGlobalBand.descriptorIndex;
        trace.currentMeshBandIndex = currentMeshBand.descriptorIndex;
        trace.currentGlobalBandIndex = currentGlobalBand.descriptorIndex;
        trace.width = width;
        trace.height = height;
        trace.traceDownsampleFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
        trace.outputWidth = TraceExtent(width, trace.traceDownsampleFactor);
        trace.outputHeight = TraceExtent(height, trace.traceDownsampleFactor);
        trace.depthIndex = depth.descriptorIndex;
        trace.normalIndex = normal.descriptorIndex;
        trace.ormIndex = orm.descriptorIndex;
        trace.albedoIndex = views.albedo.descriptorIndex;
        trace.sourceIndex = source.descriptorIndex;
        trace.frameIndex = sampleFrame;
        trace.blueNoiseIndex = noise.descriptorIndex;
        constexpr float TraceThickness = 0.15f;
        constexpr float TraceNormalBias = 0.02f;
        trace.radius = config.screenTraceDistance;
        trace.thickness = TraceThickness;
        trace.normalBias = TraceNormalBias;
        // Screen remains four member observations per 2x2. Each world tile uses
        // its configured near/far count at the camera-selected sharing span.
        // rayCount remains reserved for the constant ABI.
        // Hi-Z budgets count visited cells/descents, not sample directions.
        trace.rayCount = 1;
        trace.maxIterations = 64;

        WorldAddresses worldAddresses = {};
        // Bind legal addresses even for unused resources in screen-only shaders.
        ID3D12Resource* fallbackBuffer = world && world->materialBuffer ? world->materialBuffer : resourceManager->GetMaterialBuffer();
        worldAddresses.fill(fallbackBuffer ? fallbackBuffer->GetGPUVirtualAddress() : resourceManager->GetCBVGPUAddress(frameIndex));
        trace.hybrid.worldMeshMaxTraceDistance = config.worldMeshMaxTraceDistance;
        trace.hybrid.worldMaxTraceDistance = config.worldMaxTraceDistance;
        trace.hybrid.worldMode = EffectiveWorldMode(world, frameIndex);
        if (trace.hybrid.worldMode)
        {
            if (!world->distanceCache)
            {
                ErrorLog::Write("SSGI: world probe visibility cache is required.");
                return {};
            }
            const auto& grid = world->scene->GetGrid(frameIndex);
            trace.hybrid.gridOrigin = grid.origin;
            trace.hybrid.cellSize = grid.cellSize;
            trace.hybrid.cellDimensions = {grid.cellDimensions[0], grid.cellDimensions[1], grid.cellDimensions[2]};
            trace.hybrid.boundsMaximum = grid.boundsMaximum;
            trace.hybrid.voxelSize = world->voxelSize;
            trace.hybrid.probeCount = world->probeCount;
            trace.hybrid.nodeCount = world->scene->GetDistanceNodeCount(frameIndex);
            trace.hybrid.assetCount = world->scene->GetAssetCount(frameIndex);
            trace.hybrid.instanceCount = world->scene->GetInstanceCount(frameIndex);
            trace.hybrid.bvhNodeCount = world->scene->GetInstanceBVHNodeCount(frameIndex);
            trace.hybrid.reservedLighting1.y = config.worldMeshMinHitDistance;
            const auto bind = [&](UINT binding, ID3D12Resource* buffer, const char* name) {
                const auto handle = graph.RegisterExternalBuffer(buffer, VXGIGPUScene::ReadState,
                    VXGIGPUScene::ReadState, name);
                traceParams.ReadSRV(handle);
                worldAddresses[binding] = buffer->GetGPUVirtualAddress();
            };
            bind(0, world->scene->Get(frameIndex, 0), "SSGI.SDFNodes");
            bind(1, world->scene->Get(frameIndex, 5), "SSGI.ProbeLookup");
            bind(2, world->rawRecords, "SSGI.RawProbeSH");
            bind(3, world->scene->Get(frameIndex, 7), "SSGI.MeshSDFAssets");
            bind(4, world->scene->Get(frameIndex, 8), "SSGI.MeshSDFInstances");
            bind(5, world->scene->Get(frameIndex, 10), "SSGI.InstanceBVH");
#if SSGI_MESH_ORIGIN_CONFIRMATION_ENABLED
            // Reuse the existing cell lists and triangles for near-origin hit confirmation.
            // Descriptor indices occupy reserved constants; the root layout is unchanged.
            const char* geometryNames[] = {"SSGI.MeshCellOffsets", "SSGI.MeshTriangleReferences", "SSGI.MeshTriangles"};
            for (UINT i = 0; i < 3; ++i)
            {
                auto* resource = world->scene->Get(frameIndex, i + 1);
                const auto buffer = graph.RegisterExternalBuffer(resource, VXGIGPUScene::ReadState,
                    VXGIGPUScene::ReadState, geometryNames[i]);
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.StructureByteStride = i == 2 ? sizeof(VXGIMeshSDFGPUData::Triangle) : sizeof(uint32_t);
                desc.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / desc.Buffer.StructureByteStride);
                const auto view = graph.CreateBufferSRVView(buffer, &desc);
                if (!view.IsValid()) return {};
                traceParams.ReadSRV(view);
                float* index = i == 0 ? &trace.hybrid.reservedLighting0.y
                    : i == 1 ? &trace.hybrid.reservedLighting0.z : &trace.hybrid.reservedLighting1.x;
                memcpy(index, &view.descriptorIndex, sizeof(uint32_t));
            }
#endif
            if(world->probeCount)
            {
                const auto buffer=graph.RegisterExternalBuffer(world->distanceCache,VXGIGPUScene::ReadState,
                    VXGIGPUScene::ReadState,"SSGI.ProbeDistance");
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_UNKNOWN;
                desc.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.NumElements=world->probeCount*64;desc.Buffer.StructureByteStride=8;
                const auto distanceView=graph.CreateBufferSRVView(buffer,&desc);
                if(!distanceView.IsValid() || distanceView.descriptorIndex == 0u) return {};
                traceParams.ReadSRV(distanceView);
                memcpy(&trace.hybrid.reservedLighting0.x,&distanceView.descriptorIndex,sizeof(uint32_t));
            }
            if (auto* neighbors = world->scene->Get(frameIndex, 6); neighbors && world->probeCount)
            {
                const auto buffer = graph.RegisterExternalBuffer(neighbors, VXGIGPUScene::ReadState,
                    VXGIGPUScene::ReadState, "SSGI.ProbeNeighbors");
                D3D12_SHADER_RESOURCE_VIEW_DESC neighborDesc{};
                neighborDesc.Format = DXGI_FORMAT_UNKNOWN;
                neighborDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                neighborDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                neighborDesc.Buffer.NumElements = world->probeCount;
                neighborDesc.Buffer.StructureByteStride = static_cast<UINT>(sizeof(VXGIProbeTopology::NeighborRecord));
                const auto neighborView = graph.CreateBufferSRVView(buffer, &neighborDesc);
                if (!neighborView.IsValid()) return {};
                traceParams.ReadSRV(neighborView);
                trace.probeNeighborsIndex = neighborView.descriptorIndex;
            }
            if (world->globalSDF && world->globalSDF->GetPass().GetTexture() &&
                (world->globalSDF->GetState().HasPendingPlan() || world->globalSDF->GetState().IsInitialized()))
            {
                const auto texture = graph.RegisterExternalTexture(world->globalSDF->GetPass().GetTexture(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    "SSGI.GlobalSDF");
                D3D12_SHADER_RESOURCE_VIEW_DESC globalDesc{};
                globalDesc.Format = DXGI_FORMAT_R32_FLOAT;
                globalDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                globalDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                globalDesc.Texture3D.MipLevels = 1;
                const auto globalView = graph.CreateTextureSRVView(texture, &globalDesc);
                if (!globalView.IsValid() || !std::isfinite(world->globalTraceDistance) ||
                    world->globalTraceDistance <= 0 || !world->globalMaxSteps) return {};
                traceParams.ReadComputeSRV(globalView);
                trace.global.enabled = 1;
                trace.global.textureIndex = globalView.descriptorIndex;
                trace.global.maxTraceDistance = config.worldMaxTraceDistance;
                trace.global.maxSteps = world->globalMaxSteps;
                const auto& state = world->globalSDF->GetState();
                trace.global.constants = state.HasPendingPlan() ?
                    state.GetPendingPlan().layout.constants : state.GetLayout().constants;
            }
        }

        auto previousDepth = depth;
        UINT sourceWidth = width, sourceHeight = height;
        for (UINT level = 0; level < SSGIMaxLevels; ++level)
        {
            UINT w = (sourceWidth + 1) / 2, h = (sourceHeight + 1) / 2;
            std::string suffix = std::to_string(level);
            auto depthTexture = CreateUAVTexture(graph, w, h, RenderFormats::DepthBounds, "SSGI.DepthBounds." + suffix);
            auto depthUav = graph.CreateTextureUAVView(depthTexture);
            auto depthSrv = graph.CreateTextureSRVView(depthTexture);
            if (!depthUav.IsValid() || !depthSrv.IsValid())
            {
                return {};
            }

            SSGIPyramidConstants constants = {};
            constants.invProj = invProj;
            constants.sourceWidth = sourceWidth;
            constants.sourceHeight = sourceHeight;
            constants.outputWidth = w;
            constants.outputHeight = h;
            constants.depthIndex = previousDepth.descriptorIndex;
            constants.outputDepthIndex = depthUav.descriptorIndex;
            constants.firstLevel = level == 0;
            auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(constants));
            if (!allocation)
            {
                return {};
            }
            memcpy(allocation.cpuAddress, &constants, sizeof(constants));
            auto address = allocation.gpuAddress;
            RDGPassParameters params;
            params.ReadComputeSRV(previousDepth);
            params.WriteUAV(depthUav);
            graph.AddPass(("SSGI.Pyramid." + suffix).c_str(), ERDGPassFlags::Compute, params,
                [=](ID3D12GraphicsCommandList* cmdList) {
                ExecuteComputeNoBarrier(
                    cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGIPyramidPSO(), address, w, h);
            });
            traceParams.ReadComputeSRV(depthSrv);
            trace.levels[level] = {depthSrv.descriptorIndex, 0, 0, 0};
            ++trace.levelCount;
            previousDepth = depthSrv;
            sourceWidth = w;
            sourceHeight = h;
            if (w == 1 && h == 1)
            {
                break;
            }
        }

        if (config.screenProbes)
            return AddScreenProbesToGraph(graph, resourceManager, pipelineManager, frameIndex,
                trace, traceParams, worldAddresses, config, previewHistory, input.emissive);

        // Fixed compatibility values for the inactive legacy path. These are
        // not settings and do not participate in screen-probe history identity.
        constexpr bool LegacyWorldSharing = true, LegacySpatialFilter = false;
        constexpr float LegacyNearDistance = 2.0f, LegacyNearExitDistance = 2.2f;
        constexpr uint32_t LegacyNearRayCount = 8, LegacyFarRayCount = 8;
        constexpr uint32_t LegacyHistoryUpdates = 1, LegacySpatialStep = 2;

        // Preserve every mip's x descriptor while reusing its formerly reserved
        // yzw channels. Keep this after the pyramid's uint4 initializations.
        trace.levels[0].y = previousScreenBand.descriptorIndex;
        trace.levels[0].z = currentScreenBand.descriptorIndex;
        trace.levels[0].w = UINT_MAX; // Sparse screen lighting, supplied below.
        trace.levels[1].y = UINT_MAX; // Sparse screen receiver, supplied below.
        trace.levels[1].z = previousWorldPolicy.descriptorIndex;
        trace.levels[1].w = currentWorldPolicy.descriptorIndex;
        const float nearDistanceSquared = LegacyNearDistance * LegacyNearDistance;
        const float nearExitDistanceSquared = LegacyNearExitDistance * LegacyNearExitDistance;
        memcpy(&trace.levels[2].y, &nearDistanceSquared, sizeof(nearDistanceSquared));
        memcpy(&trace.levels[2].z, &nearExitDistanceSquared, sizeof(nearExitDistanceSquared));
        trace.levels[2].w = static_cast<uint32_t>(LegacyNearRayCount);
        trace.levels[3].y = static_cast<uint32_t>(LegacyFarRayCount);
        // Owner caching uses only reserved channels; all depth-pyramid x
        // descriptors and the 1024-byte trace constant ABI remain unchanged.
        trace.levels[3].z = trace.levels[3].w = UINT_MAX;
        trace.levels[4].y = trace.levels[4].z = UINT_MAX;
        trace.levels[5].y = UINT_MAX;
        trace.levels[5].z = static_cast<uint32_t>(LegacyHistoryUpdates);
        trace.levels[6].z = 0u;

        auto raw = CreateUAVTexture(
            graph, trace.outputWidth, trace.outputHeight, RenderFormats::IndirectLighting, "SSGI.RawGI");
        auto rawUav = graph.CreateTextureUAVView(raw);
        // Reprojection is computed once in Trace and consumed by Temporal.
        // Float32 preserves its interpolated colors, moments and sample count.
        auto reprojectedGI = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight,
            DXGI_FORMAT_R32G32B32A32_FLOAT, "SSGI.ReprojectedGI");
        auto reprojectedFast = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight,
            DXGI_FORMAT_R32G32B32A32_FLOAT, "SSGI.ReprojectedFastGI");
        auto reprojectedGIUav = graph.CreateTextureUAVView(reprojectedGI);
        auto reprojectedFastUav = graph.CreateTextureUAVView(reprojectedFast);
        if (!rawUav.IsValid() || !reprojectedGIUav.IsValid() || !reprojectedFastUav.IsValid())
        {
            return {};
        }
        trace.outputIndex = rawUav.descriptorIndex;
        trace.reprojectedGIIndex = reprojectedGIUav.descriptorIndex;
        trace.reprojectedFastIndex = reprojectedFastUav.descriptorIndex;
        RDGTextureUAVHandle repairQueueUav{}, repairCountUav{}, worldDisplayUav{};
        RDGTextureSRVHandle worldDisplaySrv{};
        RDGTextureHandle worldDisplay{};
        RDGTextureHandle worldOwners{};
        if (LegacyWorldSharing)
        {
            // Screen uses fixed 2x2. World estimates choose 2x2 or 4x4;
            // coarse estimates are replicated into four fine-grid atlas slots.
            // Separate screen / Mesh / Global histories retain their own ages.
            trace.sharedMeshSpan = 2u;
            const UINT meshSpan = trace.sharedMeshSpan;
            const UINT globalSpan = meshSpan;
            const UINT meshWidth = (trace.outputWidth + meshSpan - 1u) / meshSpan;
            const UINT meshHeight = (trace.outputHeight + meshSpan - 1u) / meshSpan;
            const UINT globalWidth = (trace.outputWidth + globalSpan - 1u) / globalSpan;
            const UINT globalHeight = (trace.outputHeight + globalSpan - 1u) / globalSpan;
            const auto screenLighting = CreateUAVTexture(graph, meshWidth, meshHeight,
                RenderFormats::IndirectLighting, "SSGI.SharedScreenLighting");
            const auto screenReceiver = CreateUAVTexture(graph, meshWidth, meshHeight,
                DXGI_FORMAT_R32_UINT, "SSGI.SharedScreenReceiver");
            const bool cacheWorldOwners = trace.hybrid.worldMode != 0u;
            const auto globalLighting = cacheWorldOwners ? graph.RegisterExternalTexture(
                current.globalOwnerRaw.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                "SSGI.CurrentWorldOwnerRaw") : CreateUAVTexture(graph, globalWidth, globalHeight,
                RenderFormats::IndirectLighting, "SSGI.SharedGlobalLighting");
            const auto globalReceiver = cacheWorldOwners ? graph.RegisterExternalTexture(
                current.globalOwnerReceiver.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                "SSGI.CurrentWorldOwnerReceiver") : CreateUAVTexture(graph, globalWidth, globalHeight,
                DXGI_FORMAT_R32_UINT, "SSGI.SharedGlobalReceiver");
            const auto meshLighting = cacheWorldOwners ? globalLighting : CreateUAVTexture(graph, meshWidth, meshHeight,
                RenderFormats::IndirectLighting, "SSGI.SharedMeshLighting");
            const auto meshReceiver = cacheWorldOwners ? globalReceiver : CreateUAVTexture(graph, meshWidth, meshHeight,
                DXGI_FORMAT_R32_UINT, "SSGI.SharedMeshReceiver");
            if (cacheWorldOwners) worldOwners = globalLighting;
            RDGTextureSRVHandle previousOwnerRaw{}, previousOwnerState{}, previousOwnerReceiver{};
            RDGTextureUAVHandle ownerStateUav{};
            if (cacheWorldOwners)
            {
                previousOwnerRaw = importHistory(previous.globalOwnerRaw.Get(), "SSGI.PreviousWorldOwnerRaw");
                previousOwnerState = importHistory(previous.globalOwnerState.Get(), "SSGI.PreviousWorldOwnerState");
                previousOwnerReceiver = importHistory(previous.globalOwnerReceiver.Get(), "SSGI.PreviousWorldOwnerReceiver");
                const auto ownerState = graph.RegisterExternalTexture(current.globalOwnerState.Get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.CurrentWorldOwnerState");
                ownerStateUav = graph.CreateTextureUAVView(ownerState);
                worldDisplay = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight,
                    RenderFormats::IndirectLighting, "SSGI.WorldDisplay");
                worldDisplayUav = graph.CreateTextureUAVView(worldDisplay);
                worldDisplaySrv = graph.CreateTextureSRVView(worldDisplay);
                if (!previousOwnerRaw.IsValid() || !previousOwnerState.IsValid() || !previousOwnerReceiver.IsValid() ||
                    !ownerStateUav.IsValid() ||
                    !worldDisplayUav.IsValid() || !worldDisplaySrv.IsValid()) return {};
                trace.levels[3].z = previousOwnerRaw.descriptorIndex;
                trace.levels[3].w = previousOwnerState.descriptorIndex;
                trace.levels[4].y = previousOwnerReceiver.descriptorIndex;
                trace.levels[5].y = worldDisplayUav.descriptorIndex;
                trace.levels[6].z = 1u;
            }
            const auto screenLightingUav = graph.CreateTextureUAVView(screenLighting);
            const auto screenReceiverUav = graph.CreateTextureUAVView(screenReceiver);
            const auto screenLightingSrv = graph.CreateTextureSRVView(screenLighting);
            const auto screenReceiverSrv = graph.CreateTextureSRVView(screenReceiver);
            const auto globalLightingUav = graph.CreateTextureUAVView(globalLighting);
            const auto globalReceiverUav = graph.CreateTextureUAVView(globalReceiver);
            const auto globalLightingSrv = graph.CreateTextureSRVView(globalLighting);
            const auto globalReceiverSrv = graph.CreateTextureSRVView(globalReceiver);
            const auto meshLightingUav = cacheWorldOwners ? globalLightingUav : graph.CreateTextureUAVView(meshLighting);
            const auto meshReceiverUav = cacheWorldOwners ? globalReceiverUav : graph.CreateTextureUAVView(meshReceiver);
            const auto meshLightingSrv = cacheWorldOwners ? globalLightingSrv : graph.CreateTextureSRVView(meshLighting);
            const auto meshReceiverSrv = cacheWorldOwners ? globalReceiverSrv : graph.CreateTextureSRVView(meshReceiver);
            if (!screenLightingUav.IsValid() || !screenReceiverUav.IsValid() ||
                !screenLightingSrv.IsValid() || !screenReceiverSrv.IsValid() ||
                !meshLightingUav.IsValid() || !meshReceiverUav.IsValid() ||
                !globalLightingUav.IsValid() || !globalReceiverUav.IsValid() ||
                !meshLightingSrv.IsValid() || !meshReceiverSrv.IsValid() ||
                !globalLightingSrv.IsValid() || !globalReceiverSrv.IsValid()) return {};
            auto shared = trace;
            shared.worldSharingEnabled = 1;
            shared.levels[0].w = screenLightingUav.descriptorIndex;
            shared.levels[1].y = screenReceiverUav.descriptorIndex;
            shared.sharedMeshLightingIndex = meshLightingUav.descriptorIndex;
            shared.sharedMeshReceiverIndex = meshReceiverUav.descriptorIndex;
            shared.sharedGlobalLightingIndex = globalLightingUav.descriptorIndex;
            shared.sharedGlobalReceiverIndex = globalReceiverUav.descriptorIndex;
            if (cacheWorldOwners) shared.levels[4].z = ownerStateUav.descriptorIndex;
            const auto sharedAllocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(shared));
            if (!sharedAllocation) return {};
            memcpy(sharedAllocation.cpuAddress, &shared, sizeof(shared));
            const auto sharedAddress = sharedAllocation.gpuAddress;
            auto sharedParams = traceParams;
            sharedParams.WriteUAV(screenLightingUav);
            sharedParams.WriteUAV(screenReceiverUav);
            sharedParams.WriteUAV(meshLightingUav);
            sharedParams.WriteUAV(meshReceiverUav);
            if (!cacheWorldOwners)
            {
                sharedParams.WriteUAV(globalLightingUav);
                sharedParams.WriteUAV(globalReceiverUav);
            }
            if (cacheWorldOwners)
            {
                sharedParams.ReadComputeSRV(previousOwnerRaw);
                sharedParams.ReadComputeSRV(previousOwnerState);
                sharedParams.ReadComputeSRV(previousOwnerReceiver);
                sharedParams.WriteUAV(ownerStateUav);
            }
            auto* sharedPipeline = pipelineManager->GetSSGISharedWorldPSO();
            graph.AddPass("SSGI.SharedWorld", ERDGPassFlags::Compute, sharedParams,
                [=](ID3D12GraphicsCommandList* cmdList) {
                    ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, sharedPipeline,
                        sharedAddress, meshWidth * 2u, meshHeight * 2u, &worldAddresses);
                });
            trace.worldSharingEnabled = 1;
            trace.levels[0].w = screenLightingSrv.descriptorIndex;
            trace.levels[1].y = screenReceiverSrv.descriptorIndex;
            trace.sharedMeshLightingIndex = meshLightingSrv.descriptorIndex;
            trace.sharedMeshReceiverIndex = meshReceiverSrv.descriptorIndex;
            trace.sharedGlobalLightingIndex = globalLightingSrv.descriptorIndex;
            trace.sharedGlobalReceiverIndex = globalReceiverSrv.descriptorIndex;
            if (cacheWorldOwners) traceParams.WriteUAV(worldDisplayUav);
            const auto repairQueue = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight,
                DXGI_FORMAT_R32_UINT, "SSGI.SharedRepairQueue");
            const auto repairCount = CreateUAVTexture(graph, 1, 1,
                DXGI_FORMAT_R32_UINT, "SSGI.SharedRepairCount");
            repairQueueUav = graph.CreateTextureUAVView(repairQueue);
            repairCountUav = graph.CreateTextureUAVView(repairCount);
            if (!repairQueueUav.IsValid() || !repairCountUav.IsValid()) return {};
            trace.sharedRepairQueueIndex = repairQueueUav.descriptorIndex;
            trace.sharedRepairCountIndex = repairCountUav.descriptorIndex;
            traceParams.ReadComputeSRV(screenLightingSrv);
            traceParams.ReadComputeSRV(screenReceiverSrv);
            traceParams.ReadComputeSRV(meshLightingSrv);
            traceParams.ReadComputeSRV(meshReceiverSrv);
            if (!cacheWorldOwners)
            {
                traceParams.ReadComputeSRV(globalLightingSrv);
                traceParams.ReadComputeSRV(globalReceiverSrv);
            }
            traceParams.WriteUAV(repairQueueUav);
            traceParams.WriteUAV(repairCountUav);
        }
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        if (trace.worldSharingEnabled)
        {
            RDGPassParameters clearParams;
            clearParams.WriteUAV(repairCountUav);
            graph.AddPass("SSGI.ClearRepairs", ERDGPassFlags::Compute, clearParams,
                [=](ID3D12GraphicsCommandList* cmdList) {
                    ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager,
                        pipelineManager->GetSSGISharedClearPSO(), address, 1, 1, &worldAddresses);
                });
        }
        auto* tracePipeline = trace.worldSharingEnabled ? pipelineManager->GetSSGISharedTracePSO() :
            pipelineManager->GetSSGITracePSO();
        traceParams.WriteUAV(rawUav);
        traceParams.WriteUAV(reprojectedGIUav);
        traceParams.WriteUAV(reprojectedFastUav);
        traceParams.WriteUAV(currentScreenBand);
        traceParams.WriteUAV(currentMeshBand);
        traceParams.WriteUAV(currentGlobalBand);
        auto pass =
            graph.AddPass("SSGI.Trace", ERDGPassFlags::Compute, traceParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, tracePipeline,
                address, trace.outputWidth, trace.outputHeight, &worldAddresses);
        });
        if (trace.worldSharingEnabled)
        {
            // The fixed upper-bound dispatch has only a compact prefix doing
            // ray work; empty groups return after one count load. This avoids
            // scattered edge repairs keeping every screen wave in world tracing.
            pass = graph.AddPass("SSGI.RepairWorld", ERDGPassFlags::Compute, traceParams,
                [=](ID3D12GraphicsCommandList* cmdList) {
                    ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager,
                        pipelineManager->GetSSGISharedRepairPSO(), address,
                        trace.outputWidth * 2u, trace.outputHeight * 2u, &worldAddresses);
                });
        }
        auto reconstructed = CreateUAVTexture(
            graph, trace.outputWidth, trace.outputHeight, RenderFormats::IndirectLighting, "SSGI.ReconstructedGI");
        auto reconstructedUav = graph.CreateTextureUAVView(reconstructed);
        auto rawSrv = graph.CreateTextureSRVView(raw);
        if (!reconstructedUav.IsValid() || !rawSrv.IsValid())
        {
            return {};
        }
        SSGIReconstructConstants reconstruction = {};
        reconstruction.view = view;
        reconstruction.invProj = invProj;
        reconstruction.width = width;
        reconstruction.height = height;
        reconstruction.outputWidth = trace.outputWidth;
        reconstruction.outputHeight = trace.outputHeight;
        reconstruction.traceDownsampleFactor = trace.traceDownsampleFactor;
        reconstruction.depthIndex = depth.descriptorIndex;
        reconstruction.normalIndex = normal.descriptorIndex;
        reconstruction.ormIndex = orm.descriptorIndex;
        reconstruction.rawIndex = rawSrv.descriptorIndex;
        reconstruction.padding[0] = worldDisplaySrv.IsValid() ? worldDisplaySrv.descriptorIndex : UINT_MAX;
        reconstruction.padding[1] = trace.levels[6].z;
        reconstruction.worldSpatialStep = LegacySpatialFilter ? static_cast<uint32_t>(LegacySpatialStep) : 0u;

        reconstruction.outputIndex = reconstructedUav.descriptorIndex;
        auto reconstructionAllocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(reconstruction));
        if (!reconstructionAllocation)
        {
            return {};
        }
        memcpy(reconstructionAllocation.cpuAddress, &reconstruction, sizeof(reconstruction));
        auto reconstructionAddress = reconstructionAllocation.gpuAddress;
        RDGPassParameters reconstructionParams;
        reconstructionParams.ReadComputeSRV(depth);
        reconstructionParams.ReadComputeSRV(normal);
        reconstructionParams.ReadComputeSRV(orm);
        reconstructionParams.ReadComputeSRV(rawSrv);
        if (worldDisplaySrv.IsValid()) reconstructionParams.ReadComputeSRV(worldDisplaySrv);
        reconstructionParams.WriteUAV(reconstructedUav);
        auto reconstructionPass = graph.AddPass(
            "SSGI.Reconstruct", ERDGPassFlags::Compute, reconstructionParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGIReconstructPSO(),
                reconstructionAddress, reconstruction.outputWidth, reconstruction.outputHeight);
        });
        return {raw, pass, reconstructed, reconstructionPass, reprojectedGI, reprojectedFast, worldDisplay, worldOwners};
    }

    static RDGTextureHandle CreateUAVTexture(
        RDGBuilder& graph, UINT w, UINT h, DXGI_FORMAT format, const std::string& name)
    {
        RDGTextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = format;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name.c_str());
    }

    static void ExecuteComputeNoBarrier(ID3D12GraphicsCommandList* cmdList, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS constants, UINT w, UINT h,
        const WorldAddresses* worldAddresses = nullptr)
    {
        ID3D12DescriptorHeap* heaps[] = {resourceManager->GetMainDescriptorHeap()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(pipelineManager->GetSSGIRootSignature());
        cmdList->SetPipelineState(pso);
        cmdList->SetComputeRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, constants);
        if (worldAddresses)
        {
            for (UINT i = 0; i < worldAddresses->size(); ++i) cmdList->SetComputeRootShaderResourceView(i + 1, (*worldAddresses)[i]);
            // Temporary worldMode SRV alias; see SSGIWorldMode() for the performance workaround rationale.
            cmdList->SetComputeRootShaderResourceView(7, constants);
        }
        cmdList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    struct TemporalInput
    {
        RDGTextureHandle reconstructedGI;
        RDGTextureHandle reprojectedGI;
        RDGTextureHandle reprojectedFast;
    };
    struct TemporalOutput
    {
        RDGTextureHandle gi;
        RDGPassHandle pass;
        RDGTextureHandle moments;
    };

    static TemporalOutput AddTemporalToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, UINT width, UINT height, int frameIndex,
        uint32_t traceDownsampleFactor, const TextureViews& views,
        const TemporalInput& input, bool previewHistory, RDGTextureHandle receiverGuide = {}, uint32_t statisticsStep = 1u)
    {
        auto import = [&](ID3D12Resource* texture, const char* name) {
            return graph.RegisterExternalTexture(
                texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
        };
        const int historyIndex = resourceManager->GetSSGICurrentHistoryIdx(previewHistory);
        const auto& output = resourceManager->GetSSGIHistoryTextures(historyIndex, previewHistory);
        auto reprojectedGI = graph.CreateTextureSRVView(input.reprojectedGI);
        auto reprojectedFast = graph.CreateTextureSRVView(input.reprojectedFast);
        auto outputGITexture = import(output.gi.Get(), "SSGI.TemporalGI");
        auto outputFastTexture = import(output.fastGI.Get(), "SSGI.FastGI");
        auto outputDepthTexture = import(output.depth.Get(), "SSGI.HistoryDepth");
        auto outputNormalTexture = import(output.normal.Get(), "SSGI.HistoryNormal");
        const UINT halfWidth = TraceExtent(width, traceDownsampleFactor);
        const UINT halfHeight = TraceExtent(height, traceDownsampleFactor);
        auto slowTexture =
            CreateUAVTexture(graph, halfWidth, halfHeight, RenderFormats::IndirectLighting, "SSGI.AccumulatedSlow");
        auto fastTexture =
            CreateUAVTexture(graph, halfWidth, halfHeight, RenderFormats::IndirectLighting, "SSGI.AccumulatedFast");
        auto outputGI = graph.CreateTextureUAVView(slowTexture);
        auto outputFast = graph.CreateTextureUAVView(fastTexture);
        auto outputDepth = graph.CreateTextureUAVView(outputDepthTexture);
        auto outputNormal = graph.CreateTextureUAVView(outputNormalTexture);
        auto depth = views.depth;
        auto normal = views.normal;
        auto orm = views.orm;
        auto current = graph.CreateTextureSRVView(input.reconstructedGI);
        if (!reprojectedGI.IsValid() || !reprojectedFast.IsValid() || !outputGI.IsValid() ||
            !outputDepth.IsValid() || !outputNormal.IsValid() || !outputFast.IsValid() ||
            !depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !current.IsValid())
        {
            return {};
        }

        SSGITemporalConstants constants = {};
        constants.width = width;
        constants.height = height;
        constants.outputWidth = halfWidth;
        constants.outputHeight = halfHeight;
        constants.traceDownsampleFactor = traceDownsampleFactor;
        constants.depthIndex = depth.descriptorIndex;
        constants.normalIndex = normal.descriptorIndex;
        constants.ormIndex = orm.descriptorIndex;
        constants.currentIndex = current.descriptorIndex;
        constants.reprojectedGIIndex = reprojectedGI.descriptorIndex;
        constants.reprojectedFastIndex = reprojectedFast.descriptorIndex;
        constants.outputGIIndex = outputGI.descriptorIndex;
        constants.outputDepthIndex = outputDepth.descriptorIndex;
        constants.outputNormalIndex = outputNormal.descriptorIndex;
        constants.outputFastIndex = outputFast.descriptorIndex;
        // Keep the project-tested 16-sample main cap; NRD fast weight remains 1/7.
        constants.maxHistoryFrames = 16.0f;
        constants.maxFastAccumulatedFrames = 6.0f;
        auto guide = receiverGuide.IsValid() ? graph.CreateTextureSRVView(receiverGuide) : RDGTextureSRVHandle{};
        constants.padding[0] = guide.IsValid() ? guide.descriptorIndex + 1u : 0u;
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
        RDGPassParameters params;
        params.ReadComputeSRV(depth);
        params.ReadComputeSRV(normal);
        params.ReadComputeSRV(orm);
        params.ReadComputeSRV(current);
        if (guide.IsValid()) params.ReadComputeSRV(guide);
        params.ReadComputeSRV(reprojectedGI);
        params.ReadComputeSRV(reprojectedFast);
        params.WriteUAV(outputGI);
        params.WriteUAV(outputDepth);
        params.WriteUAV(outputNormal);
        params.WriteUAV(outputFast);
        auto pass =
            graph.AddPass("SSGI.Temporal", ERDGPassFlags::Compute, params, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGITemporalPSO(),
                address, constants.outputWidth, constants.outputHeight);
        });
        auto slow = graph.CreateTextureSRVView(slowTexture);
        auto fast = graph.CreateTextureSRVView(fastTexture);
        auto finalGI = graph.CreateTextureUAVView(outputGITexture);
        auto finalFast = graph.CreateTextureUAVView(outputFastTexture);
        auto momentTexture =
            CreateUAVTexture(graph, halfWidth, halfHeight, RenderFormats::LuminanceMoments, "SSGI.ColorMoments");
        auto moments = graph.CreateTextureUAVView(momentTexture);
        if (!pass.IsValid() || !slow.IsValid() || !fast.IsValid() || !finalGI.IsValid() || !finalFast.IsValid() ||
            !moments.IsValid())
        {
            return {};
        }
        SSGIHistoryClampConstants clamp = {};
        clamp.width = halfWidth;
        clamp.height = halfHeight;
        clamp.slowIndex = slow.descriptorIndex;
        clamp.fastIndex = fast.descriptorIndex;
        clamp.noisyIndex = current.descriptorIndex;
        clamp.outputMomentsIndex = moments.descriptorIndex;
        clamp.outputGIIndex = finalGI.descriptorIndex;
        clamp.outputFastIndex = finalFast.descriptorIndex;
        // RELAX defaults; these are implementation constants, not additional artist settings.
        clamp.sigmaScale = 2.0f;
        clamp.accelerationAmount = 0.3f;
        clamp.resetAmount = 0.5f;
        clamp.spatialSigmaScale = 4.5f;
        clamp.temporalSigmaScale = 0.5f;
        clamp.padding[0] = statisticsStep;
        auto clampAllocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(clamp));
        if (!clampAllocation)
        {
            return {};
        }
        memcpy(clampAllocation.cpuAddress, &clamp, sizeof(clamp));
        auto clampAddress = clampAllocation.gpuAddress;
        RDGPassParameters clampParams;
        clampParams.ReadComputeSRV(slow);
        clampParams.ReadComputeSRV(fast);
        clampParams.ReadComputeSRV(current);
        clampParams.WriteUAV(finalGI);
        clampParams.WriteUAV(finalFast);
        clampParams.WriteUAV(moments);
        auto clampPass = graph.AddPass(
            "SSGI.HistoryClamp", ERDGPassFlags::Compute, clampParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager,
                pipelineManager->GetSSGIHistoryClampPSO(), clampAddress, clamp.width, clamp.height);
        });
        graph.MarkTextureAsOutput(outputGITexture);
        graph.MarkTextureAsOutput(outputFastTexture);
        graph.MarkTextureAsOutput(outputDepthTexture);
        graph.MarkTextureAsOutput(outputNormalTexture);
        return {outputGITexture, clampPass, momentTexture};
    }

    struct AtrousInput
    {
        RDGTextureHandle moments;
        RDGTextureHandle history;
    };
    struct AtrousOutput
    {
        RDGTextureHandle gi;
        RDGPassHandle pass;
    };
    static AtrousOutput AddAtrousToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor, const TextureViews& views, const AtrousInput& input, RDGTextureHandle receiverGuide = {})
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto history = graph.CreateTextureSRVView(input.history);
        if (!depth.IsValid() || !normal.IsValid() || !history.IsValid())
        {
            return {};
        }
        const UINT halfWidth = TraceExtent(width, traceDownsampleFactor);
        const UINT halfHeight = TraceExtent(height, traceDownsampleFactor);
        auto source = input.moments;
        RDGPassHandle lastPass;
        for (UINT iteration = 0; iteration < 2; ++iteration)
        {
            const char* name = iteration == 0 ? "SSGI.Atrous1" : "SSGI.Atrous2";
            auto target = CreateUAVTexture(graph, halfWidth, halfHeight, RenderFormats::LuminanceMoments, name);
            auto read = graph.CreateTextureSRVView(source);
            auto write = graph.CreateTextureUAVView(target);
            if (!read.IsValid() || !write.IsValid())
            {
                return {};
            }
            SSGIAtrousConstants constants = {};
            constants.view = view;
            constants.invProj = invProj;
            constants.width = width;
            constants.height = height;
            constants.outputWidth = halfWidth;
            constants.outputHeight = halfHeight;
            constants.giIndex = read.descriptorIndex;
            constants.depthIndex = depth.descriptorIndex;
            constants.normalIndex = normal.descriptorIndex;
            constants.historyIndex = history.descriptorIndex;
            constants.outputIndex = write.descriptorIndex;
            auto guide = receiverGuide.IsValid() ? graph.CreateTextureSRVView(receiverGuide) : RDGTextureSRVHandle{};
            constants.padding[0] = guide.IsValid() ? guide.descriptorIndex + 1u : 0u;
            constants.step = 1u << iteration;
            constants.phiLuminance = 2.0f;
            constants.depthThreshold = 0.003f;
            constants.traceDownsampleFactor = traceDownsampleFactor;
            auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(constants));
            if (!allocation)
            {
                return {};
            }
            memcpy(allocation.cpuAddress, &constants, sizeof(constants));
            auto address = allocation.gpuAddress;
            auto pso = pipelineManager->GetSSGIAtrousPSO(iteration == 0);
            RDGPassParameters params;
            params.ReadComputeSRV(read);
            params.ReadComputeSRV(depth);
            params.ReadComputeSRV(normal);
            params.ReadComputeSRV(history);
            if (guide.IsValid()) params.ReadComputeSRV(guide);
            params.WriteUAV(write);
            lastPass = graph.AddPass(name, ERDGPassFlags::Compute, params, [=](ID3D12GraphicsCommandList* cmdList) {
                ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pso, address, constants.outputWidth,
                    constants.outputHeight);
            });
            if (!lastPass.IsValid())
            {
                return {};
            }
            source = target;
        }
        // Spatial output is consumed by composite only, never fed back into temporal history.
        return {source, lastPass};
    }

    struct CompositeInput
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle gi;
    };

    static RDGPassHandle AddCompositeToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor, const TextureViews& views, const CompositeInput& input,
        float intensity, bool giOnly, RDGTextureHandle receiverGuide = {})
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto orm = views.orm;
        auto albedo = views.albedo;
        auto gi = graph.CreateTextureSRVView(input.gi);
        auto target = graph.CreateTextureRTVView(input.sceneColor);
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !albedo.IsValid() || !gi.IsValid() ||
            !target.IsValid())
        {
            return {};
        }
        SSGICompositeConstants constants = {};
        constants.view = view;
        constants.invProj = invProj;
        constants.width = width;
        constants.height = height;
        constants.halfWidth = TraceExtent(width, traceDownsampleFactor);
        constants.halfHeight = TraceExtent(height, traceDownsampleFactor);
        constants.traceDownsampleFactor = traceDownsampleFactor;
        constants.depthIndex = depth.descriptorIndex;
        constants.normalIndex = normal.descriptorIndex;
        constants.ormIndex = orm.descriptorIndex;
        constants.albedoIndex = albedo.descriptorIndex;
        constants.giIndex = gi.descriptorIndex;
        constants.intensity = intensity;
        auto guide = receiverGuide.IsValid() ? graph.CreateTextureSRVView(receiverGuide) : RDGTextureSRVHandle{};
        constants.padding = guide.IsValid() ? guide.descriptorIndex + 1u : 0u;
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
        RDGPassParameters params;
        params.ReadSRV(depth);
        params.ReadSRV(normal);
        params.ReadSRV(orm);
        params.ReadSRV(albedo);
        params.ReadSRV(gi);
        if (guide.IsValid()) params.ReadSRV(guide);
        // GI-only displays the completed lighting signal; the normal PSO applies the receiver response.
        params.WriteRTV(target);
        return graph.AddPass(
            "SSGI.Composite", ERDGPassFlags::Graphics, params, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteCompositeNoBarrier(
                cmdList, resourceManager, pipelineManager, address, target.cpuHandle, width, height, giOnly);
        });
    }

    static void ExecuteCompositeNoBarrier(ID3D12GraphicsCommandList* cmdList, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, D3D12_GPU_VIRTUAL_ADDRESS address, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
        UINT width, UINT height, bool giOnly = false)
    {
        ID3D12DescriptorHeap* heaps[] = {resourceManager->GetMainDescriptorHeap()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(pipelineManager->GetSSGIRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetSSGICompositePSO(giOnly));
        cmdList->SetGraphicsRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, address);
        cmdList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
        D3D12_RECT rect = {0, 0, LONG(width), LONG(height)};
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &rect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
};

#endif
