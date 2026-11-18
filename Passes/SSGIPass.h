#ifndef SSGI_PASS_H
#define SSGI_PASS_H

#include "stdafx.h"
#include "RDG.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Settings_Manager.h"
#include "SurfaceCacheGIData.h"

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
    uint32_t emissiveSourceIndex;
    uint32_t outputIndex;
    uint32_t levelCount;
    uint32_t frameIndex;
    uint32_t blueNoiseIndex;
    float radius;
    float thickness;
    float normalBias;
    uint32_t maxIterations;
    uint32_t traceDownsampleFactor;
    uint32_t denoiseDownsampleFactor;
    uint32_t probeScreenRays;
    uint32_t emissiveSamples;
    // uint arrays in HLSL cbuffers have 16-byte stride. Use uint4 explicitly.
    // x = depth SRV; yzw provide the required cbuffer array stride.
    DirectX::XMUINT4 levels[SSGIMaxLevels];
    DirectX::XMFLOAT4X4 invView;
    DirectX::XMFLOAT4X4 invViewProj;
    // Screen-space temporal history.
    DirectX::XMFLOAT4X4 previousViewProj;
    DirectX::XMFLOAT4X4 previousInvViewProj;
    uint32_t previousGIIndex;
    uint32_t previousDepthIndex;
    uint32_t previousNormalIndex;
    uint32_t historyValid;
    uint32_t previousFastIndex;
    uint32_t reprojectedGIIndex;
    uint32_t reprojectedFastIndex;
    uint32_t probePositionIndex;
    uint32_t probeNormalIndex;
    uint32_t probeReceiverIndex;
    uint32_t previousProbePositionIndex;
    uint32_t previousProbeNormalIndex;
    uint32_t previousProbeRadianceIndex;
    uint32_t probeRadianceUavIndex;
    uint32_t probeRadianceIndex;
    uint32_t probeFilteredUavIndex;
    uint32_t probeFilteredIndex;
    uint32_t probeHistoryLimit;
    uint32_t probeSHUavIndex;
    uint32_t probeSHIndex;
    uint32_t probePositionUavIndex;
    uint32_t probeNormalUavIndex;
    uint32_t probeReceiverUavIndex;
    float probeJitterWidth;
    uint32_t probeAnchorUavIndex;
    uint32_t previousProbeAnchorIndex;
    uint32_t probeDispatchBase;
    uint32_t emissiveListIndex;
    DirectX::XMUINT2 probeAtlasSize;
    uint32_t adaptiveCapacity;
    uint32_t adaptiveCountIndex;
    uint32_t adaptiveCountUavIndex;
    uint32_t adaptiveHeaderIndex;
    uint32_t adaptiveHeaderUavIndex;
    uint32_t adaptiveIndicesIndex;
    uint32_t adaptiveIndicesUavIndex;
    uint32_t adaptiveMaskIndex;
    uint32_t adaptiveMaskUavIndex;
    uint32_t adaptiveArgsUavIndex;
    uint32_t previousAdaptiveHeaderIndex;
    uint32_t previousAdaptiveIndicesIndex;
    uint32_t previousAdaptiveCountIndex;
    uint32_t probeHistoryLinkIndex;
    uint32_t probeHistoryLinkUavIndex;
    uint32_t probeSeedIndex;
    uint32_t probeSeedUavIndex;
    uint32_t previousProbeSeedIndex;
    float probeCameraFadeEnd;
    uint32_t probeCameraFadeEnabled;
    uint32_t probeDiagnosticsIndexPlusOne;
    uint32_t emissiveCountIndex;
    uint32_t emissiveListUavIndex,emissiveCountUavIndex;
    DirectX::XMUINT2 screenEmissivePadding;
};
static_assert(offsetof(SSGITraceConstants, maxIterations) == 252);
static_assert(offsetof(SSGITraceConstants, probeScreenRays) == 264);
static_assert(offsetof(SSGITraceConstants, emissiveSamples) == 268);
static_assert(offsetof(SSGITraceConstants, levels) == 272);
static_assert(offsetof(SSGITraceConstants, previousViewProj) == 512);
static_assert(offsetof(SSGITraceConstants, probePositionIndex) == 668);
static_assert(sizeof(SSGITraceConstants) == 864);
static_assert(offsetof(SSGITraceConstants, probeDispatchBase) == 744);
static_assert(offsetof(SSGITraceConstants, probeAtlasSize) == 752);
static_assert(offsetof(SSGITraceConstants, probeHistoryLinkIndex) == 812);

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
};
static_assert(sizeof(SSGIReconstructConstants) == 176);
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
    float cameraFadeStart;
    float cameraFadeEnd;
    uint32_t cameraFadeEnabled;
    uint32_t cameraFadePadding;
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMUINT2 probeUniformSize;
    DirectX::XMUINT2 probeAtlasSize;
    uint32_t probeFactor;
    uint32_t probePositionIndex;
    uint32_t probeNormalIndex;
    uint32_t probeReceiverIndex;
    uint32_t probeSHIndex;
    uint32_t adaptiveCapacity;
    uint32_t adaptiveCountIndex;
    uint32_t adaptiveHeaderIndex;
    uint32_t adaptiveIndicesIndex;
    uint32_t validityIndexPlusOne;
    uint32_t probeDiagnosticsIndexPlusOne;
    uint32_t probeCompositePadding;
};
static_assert(sizeof(SSGICompositeConstants) == 320);
static_assert(offsetof(SSGICompositeConstants, traceDownsampleFactor) == 168);
static_assert(offsetof(SSGICompositeConstants, cameraFadeStart) == 176);
static_assert(offsetof(SSGICompositeConstants, cameraFadeEnabled) == 184);
static_assert(offsetof(SSGICompositeConstants, probeUniformSize) == 256);
static_assert(offsetof(SSGICompositeConstants, validityIndexPlusOne) == 308);

class SSGIPass
{
public:
    struct Input
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
        RDGTextureHandle normal;
        RDGTextureHandle orm;
        RDGTextureHandle albedo;
        RDGTextureHandle emissive;
        SurfaceCacheGIData::Bindings world;
        bool diagnostics = false; // Capture-only counters; absent in ordinary rendering.
    };
    struct ProbeData
    {
        RDGTextureHandle position, normal, receiver, sh;
        RDGTextureHandle adaptiveCount, tileHeader, tileIndices, diagnostics, validity;
    };
    struct Output
    {
        RDGTextureHandle sceneColor;
        RDGPassHandle pass;
        RDGTextureHandle indirectLighting; // Final filtered E/pi, before receiver material response.
        RDGTextureHandle rawLighting;
        RDGTextureHandle reconstructedLighting;
        RDGTextureHandle probeDirections;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
        ProbeData probes;
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
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input)
    {
        auto lighting = AddLightingToGraph(graph, resourceManager, pipelineManager, history, view, proj,
            invProj, invViewProj, width, height, frameIndex, sampleFrame, config, input, false);
        if (!lighting.gi.IsValid() || !lighting.pass.IsValid()) return {};
        const auto factor = config.DenoiseFactor();
        auto composite = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj, invViewProj, width, height,
            frameIndex, factor, lighting.views, {input.sceneColor, lighting.gi}, config, config.giOnly,
            config.screenProbes ? RDGTextureHandle{} : lighting.receiverGuide, lighting.probes);
        if (!composite.IsValid()) return {};
        return {input.sceneColor, composite, lighting.gi, lighting.raw, lighting.reconstructed, lighting.probeDirections, lighting.receiverGuide,lighting.probePosition,lighting.probeNormal,lighting.probeFiltered,lighting.probes};
    }

    // Diagnostic output only. The normal mixed GI and its SceneColor/history
    // continue unchanged; this branch uses a separate temporal history pair.
    // EnsureSSGIPreviewHistoryResources must have succeeded before recording it.
    static RDGTextureHandle AddLightingPreviewToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input)
    {
        auto lighting = AddLightingToGraph(graph, resourceManager, pipelineManager, history, view, proj,
            invProj, invViewProj, width, height, frameIndex, sampleFrame, config, input, true);
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
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj,
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor,
        const Input& input, RDGTextureHandle filteredGI, const SSGIConfig& config, RDGTextureHandle receiverGuide,
        const ProbeData& probes)
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
        auto previewProbes = probes;
        previewProbes.diagnostics = {}; // The ordinary composite already counted these pixels.
        const auto pass = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj, invViewProj,
            width, height, frameIndex, traceDownsampleFactor, views, {target, filteredGI}, config, true, receiverGuide, previewProbes);
        return pass.IsValid() ? IrradiancePreviewOutput{target, pass} : IrradiancePreviewOutput{};
    }

private:
    static UINT TraceExtent(UINT dimension, uint32_t factor)
    {
        return dimension / factor + (dimension % factor != 0 ? 1u : 0u);
    }
    struct LightingOutput
    {
        RDGTextureHandle gi;
        RDGPassHandle pass;
        TextureViews views;
        RDGTextureHandle raw, reconstructed, probeDirections;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
        ProbeData probes;
    };
    static LightingOutput AddLightingToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input,
        bool previewHistory)
    {
        if (!resourceManager || !pipelineManager || width == 0 || height == 0 ||
            !SSGIConfig::IsValidTraceDownsampleFactor(config.traceDownsampleFactor) ||
            !config.HasValidTraceDistances() || !config.HasValidTraceIterations()) return {};
        if (config.screenProbes && (!config.HasValidScreenProbeSamples() || !config.HasValidScreenProbeJitter() ||
            !config.HasValidScreenProbeAdaptive() ||
            config.screenProbeHistoryUpdates < 1 || config.screenProbeHistoryUpdates > 32 ||
            !SSGIConfig::IsValidDenoiseDownsampleFactor(config.denoiseDownsampleFactor))) return {};
        const uint32_t traceDownsampleFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
        const uint32_t denoiseFactor = config.DenoiseFactor();
        const UINT historyWidth = TraceExtent(width, denoiseFactor), historyHeight = TraceExtent(height, denoiseFactor);
        const UINT traceWidth = TraceExtent(width, traceDownsampleFactor), traceHeight = TraceExtent(height, traceDownsampleFactor);
        // A resolution/factor change must recreate both history sets before any
        // pass is recorded. Refuse mismatched resources rather than writing OOB.
        for (int index = 0; index < 2; ++index)
        {
            const auto& textures = resourceManager->GetSSGIHistoryTextures(index, previewHistory);
            for (ID3D12Resource* texture : {textures.gi.Get(), textures.fastGI.Get(), textures.depth.Get(), textures.normal.Get()})
            {
                if (!texture) return {};
                const auto desc = texture->GetDesc();
                const bool pixelHistory = config.screenProbes && (texture == textures.gi.Get() || texture == textures.fastGI.Get() ||
                    texture == textures.depth.Get() || texture == textures.normal.Get());
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    desc.Width != (pixelHistory ? historyWidth : traceWidth) || desc.Height != (pixelHistory ? historyHeight : traceHeight))
                    return {};
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
            frameIndex, sampleFrame, config, views, {input.world, input.emissive, input.diagnostics}, history, previewHistory);
        if (!trace.reconstructedGI.IsValid() || !trace.reconstructionPass.IsValid())
        {
            return {};
        }
        auto temporal = AddTemporalToGraph(graph, resourceManager, pipelineManager, width, height,
            frameIndex, denoiseFactor, views,
            {trace.reconstructedGI, trace.reprojectedGI, trace.reprojectedFast}, previewHistory, config.screenProbes ? RDGTextureHandle{} : trace.receiverGuide,
            config.screenProbes ? (traceDownsampleFactor + denoiseFactor - 1u) / denoiseFactor : 1u);
        if (!temporal.gi.IsValid() || !temporal.pass.IsValid())
        {
            return {};
        }
        auto spatial = AddAtrousToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, denoiseFactor, views, {temporal.moments, temporal.gi}, config.screenProbes ? RDGTextureHandle{} : trace.receiverGuide);
        if (!spatial.gi.IsValid() || !spatial.pass.IsValid())
        {
            return {};
        }
        trace.probes.validity = temporal.gi;
        return {spatial.gi, spatial.pass, views, trace.rawGI, trace.reconstructedGI, trace.probeDirections,trace.receiverGuide,trace.probePosition,trace.probeNormal,trace.probeFiltered,trace.probes};
    }

    struct TraceInput
    {
        SurfaceCacheGIData::Bindings world;
        RDGTextureHandle emissive;
        bool diagnostics = false;
    };
    struct TraceOutput
    {
        RDGTextureHandle rawGI;
        RDGPassHandle tracePass;
        RDGTextureHandle reconstructedGI;
        RDGPassHandle reconstructionPass;
        RDGTextureHandle reprojectedGI;
        RDGTextureHandle reprojectedFast;
        RDGTextureHandle probeDirections;
        RDGTextureHandle receiverGuide,probePosition,probeNormal,probeFiltered;
        ProbeData probes;
    };

#include "ScreenProbeGather.inl"

    static TraceOutput AddTraceToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj,
        UINT width, UINT height, int frameIndex, UINT sampleFrame,
        const SSGIConfig& config, const TextureViews& views, const TraceInput& input,
        const SSGIHistoryState& history, bool previewHistory)
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto orm = views.orm;
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
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() ||
            !previousGI.IsValid() || !previousFast.IsValid() || !previousHistoryDepth.IsValid() || !previousNormal.IsValid()) return {};

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
        traceParams.ReadComputeSRV(noise);
        traceParams.ReadComputeSRV(previousGI);
        traceParams.ReadComputeSRV(previousFast);
        traceParams.ReadComputeSRV(previousHistoryDepth);
        traceParams.ReadComputeSRV(previousNormal);
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
        trace.width = width;
        trace.height = height;
        trace.traceDownsampleFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
        trace.denoiseDownsampleFactor = config.DenoiseFactor();
        trace.outputWidth = TraceExtent(width, trace.traceDownsampleFactor);
        trace.outputHeight = TraceExtent(height, trace.traceDownsampleFactor);
        trace.depthIndex = depth.descriptorIndex;
        trace.normalIndex = normal.descriptorIndex;
        trace.ormIndex = orm.descriptorIndex;
        trace.frameIndex = sampleFrame;
        trace.blueNoiseIndex = noise.descriptorIndex;
        constexpr float TraceThickness = 0.15f;
        constexpr float TraceNormalBias = 0.02f;
        trace.radius = config.screenTraceDistance;
        trace.thickness = TraceThickness;
        trace.normalBias = TraceNormalBias;
        // Legacy pixel tracing uses four screen observations; probe budgets are explicit.
        // Hi-Z budgets count visited cells/descents, not sample directions.
        trace.maxIterations = static_cast<uint32_t>(config.maxTraceIterations);

        // Root buffers are immutable for this retired frame slot, prepared before
        // graph recording. Upload heaps retain GENERIC_READ; atlas/field states
        // are declared here and restored for their persistent owners.
        for(UINT i=0;i<5;++i)
        {
            const auto texture=graph.RegisterExternalTexture(input.world.textures[i],SurfaceCacheCapturePass::ReadState,
                SurfaceCacheCapturePass::ReadState,"SSGI.SurfaceCache");
            traceParams.ReadComputeSRV(texture);
        }
        if(input.world.globalEnabled)
        {
            const auto texture=graph.RegisterExternalTexture(input.world.textures[5],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"SSGI.GlobalSDF");
            traceParams.ReadComputeSRV(texture);
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
                trace, traceParams, config, previewHistory, input.world, input.emissive, input.diagnostics);

        const auto feedback=graph.RegisterExternalBuffer(input.world.feedback,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"SurfaceCache.CardFeedback");
        traceParams.WriteUAV(feedback);

        auto raw = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight, RenderFormats::IndirectLighting, "SSGI.RawGI");
        auto reprojectedGI = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, "SSGI.ReprojectedGI");
        auto reprojectedFast = CreateUAVTexture(graph, trace.outputWidth, trace.outputHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, "SSGI.ReprojectedFast");
        auto rawUav = graph.CreateTextureUAVView(raw);
        auto rawSrv = graph.CreateTextureSRVView(raw);
        auto reprojectedGIUav = graph.CreateTextureUAVView(reprojectedGI), reprojectedFastUav = graph.CreateTextureUAVView(reprojectedFast);
        if (!rawUav.IsValid() || !rawSrv.IsValid() || !reprojectedGIUav.IsValid() || !reprojectedFastUav.IsValid()) return {};
        trace.outputIndex = rawUav.descriptorIndex;
        trace.reprojectedGIIndex = reprojectedGIUav.descriptorIndex;
        trace.reprojectedFastIndex = reprojectedFastUav.descriptorIndex;
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        traceParams.WriteUAV(rawUav); traceParams.WriteUAV(reprojectedGIUav); traceParams.WriteUAV(reprojectedFastUav);
        auto pass = graph.AddPass("SSGI.Trace", ERDGPassFlags::Compute, traceParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGITracePSO(), address, trace.outputWidth, trace.outputHeight,&input.world);
        });
        auto reconstructed = CreateUAVTexture(
            graph, trace.outputWidth, trace.outputHeight, RenderFormats::IndirectLighting, "SSGI.ReconstructedGI");
        auto reconstructedUav = graph.CreateTextureUAVView(reconstructed);

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
        reconstructionParams.WriteUAV(reconstructedUav);
        auto reconstructionPass = graph.AddPass(
            "SSGI.Reconstruct", ERDGPassFlags::Compute, reconstructionParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGIReconstructPSO(),
                reconstructionAddress, reconstruction.outputWidth, reconstruction.outputHeight);
        });
        return {raw, pass, reconstructed, reconstructionPass, reprojectedGI, reprojectedFast, {}};
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
        const SurfaceCacheGIData::Bindings* world=nullptr)
    {
        ID3D12DescriptorHeap* heaps[] = {resourceManager->GetMainDescriptorHeap()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(pipelineManager->GetSSGIRootSignature());
        cmdList->SetPipelineState(pso);
        cmdList->SetComputeRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, constants);
        if(world)world->Bind(cmdList);

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
        const DirectX::XMFLOAT4X4& invViewProj,
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor, const TextureViews& views, const CompositeInput& input,
        const SSGIConfig& config, bool giOnly, RDGTextureHandle receiverGuide, const ProbeData& probes)
    {
        if (!config.HasValidCameraFade()) return {};
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
        constants.intensity = config.intensity;
        constants.cameraFadeStart = config.cameraFadeStart;
        constants.cameraFadeEnd = config.cameraFadeEnd;
        constants.cameraFadeEnabled = config.cameraFadeEnabled ? 1u : 0u;
        constants.invViewProj = invViewProj;
        auto guide = receiverGuide.IsValid() ? graph.CreateTextureSRVView(receiverGuide) : RDGTextureSRVHandle{};
        constants.padding = guide.IsValid() ? guide.descriptorIndex + 1u : 0u;
        RDGPassParameters params;
        if (config.screenProbes)
        {
            auto validity = graph.CreateTextureSRVView(probes.validity);
            if (!validity.IsValid()) return {};
            constants.validityIndexPlusOne = validity.descriptorIndex + 1u;
            params.ReadSRV(validity);
            ScreenProbeLayout layout;
            if (!ScreenProbeLayout::Calculate(width, height, static_cast<uint32_t>(config.traceDownsampleFactor),
                config.screenProbeAdaptiveEnabled, config.screenProbeAdaptiveMaxFraction, layout)) return {};
            if (layout.adaptiveCapacity > 0u)
            {
                constants.probeUniformSize = {layout.uniformWidth, layout.uniformHeight};
                constants.probeAtlasSize = {layout.atlasWidth, layout.atlasHeight};
                constants.probeFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
                constants.adaptiveCapacity = layout.adaptiveCapacity;
                auto bind = [&](RDGTextureHandle texture, uint32_t& field) {
                    auto srv = graph.CreateTextureSRVView(texture);
                    if (!srv.IsValid()) return false;
                    field = srv.descriptorIndex; params.ReadSRV(srv); return true;
                };
                if (!bind(probes.position, constants.probePositionIndex) || !bind(probes.normal, constants.probeNormalIndex) ||
                    !bind(probes.receiver, constants.probeReceiverIndex) || !bind(probes.sh, constants.probeSHIndex) ||
                    !bind(probes.adaptiveCount, constants.adaptiveCountIndex) || !bind(probes.tileHeader, constants.adaptiveHeaderIndex) ||
                    !bind(probes.tileIndices, constants.adaptiveIndicesIndex)) return {};
            }
            if (probes.diagnostics.IsValid())
            {
                auto diagnostic = graph.CreateTextureUAVView(probes.diagnostics);
                if (!diagnostic.IsValid()) return {};
                constants.probeDiagnosticsIndexPlusOne = diagnostic.descriptorIndex + 1u;
                params.WriteUAV(diagnostic);
            }
        }
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
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
