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
    DirectX::XMFLOAT3 reservedLighting1; float reservedLighting2;
    float reservedWorldRange; float voxelSize; uint32_t probeCount; uint32_t worldMode;
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
    DirectX::XMUINT4 levels[SSGIMaxLevels]; // depth SRV, padding, padding, padding
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
    uint32_t checkerboardEnabled;
    uint32_t previousFastIndex;
    uint32_t reprojectedGIIndex;
    uint32_t reprojectedFastIndex;
    SSGIGlobalParameters global;
};
static_assert(offsetof(SSGITraceConstants, albedoIndex) == 264);
static_assert(offsetof(SSGITraceConstants, traceDownsampleFactor) == 268);
static_assert(offsetof(SSGITraceConstants, invView) == 384);
static_assert(offsetof(SSGITraceConstants, hybrid) == 512);
static_assert(offsetof(SSGITraceConstants, hybrid) + offsetof(SSGIHybridParameters, worldMode) == 604);
static_assert(offsetof(SSGITraceConstants, previousViewProj) == 624);
static_assert(offsetof(SSGITraceConstants, previousGIIndex) == 752);
static_assert(offsetof(SSGITraceConstants, checkerboardEnabled) == 768);
static_assert(offsetof(SSGITraceConstants, reprojectedGIIndex) == 776);
static_assert(offsetof(SSGITraceConstants, global) == 784);
static_assert(sizeof(SSGITraceConstants) == 976);

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
    };
    struct Output
    {
        RDGTextureHandle sceneColor;
        RDGPassHandle pass;
        RDGTextureHandle indirectLighting; // Final filtered E/pi, before receiver material response.
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
        const auto factor = static_cast<uint32_t>(config.traceDownsampleFactor);
        auto composite = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, factor, lighting.views, {input.sceneColor, lighting.gi}, config.intensity, config.giOnly);
        if (!composite.IsValid()) return {};
        return {input.sceneColor, composite, lighting.gi};
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
        const Input& input, RDGTextureHandle filteredGI, float intensity)
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
            width, height, frameIndex, traceDownsampleFactor, views, {target, filteredGI}, intensity, true);
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
    };
    static LightingOutput AddLightingToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj, UINT width, UINT height,
        int frameIndex, UINT sampleFrame, const SSGIConfig& config, const Input& input,
        const WorldInput* world, bool previewHistory)
    {
        if (!resourceManager || !pipelineManager || width == 0 || height == 0 ||
            !SSGIConfig::IsValidTraceDownsampleFactor(config.traceDownsampleFactor)) return {};
        const uint32_t traceDownsampleFactor = static_cast<uint32_t>(config.traceDownsampleFactor);
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
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != traceWidth || desc.Height != traceHeight)
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
            frameIndex, sampleFrame, config, views, {input.bounceSource}, world, history, previewHistory);
        if (!trace.reconstructedGI.IsValid() || !trace.reconstructionPass.IsValid())
        {
            return {};
        }
        auto temporal = AddTemporalToGraph(graph, resourceManager, pipelineManager, width, height,
            frameIndex, traceDownsampleFactor, views,
            {trace.reconstructedGI, trace.reprojectedGI, trace.reprojectedFast}, previewHistory);
        if (!temporal.gi.IsValid() || !temporal.pass.IsValid())
        {
            return {};
        }
        auto spatial = AddAtrousToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, traceDownsampleFactor, views, {temporal.moments, temporal.gi});
        if (!spatial.gi.IsValid() || !spatial.pass.IsValid())
        {
            return {};
        }
        return {spatial.gi, spatial.pass, views};
    }

    struct TraceInput
    {
        RDGTextureHandle bounceSource;
    };
    struct TraceOutput
    {
        RDGTextureHandle rawGI;
        RDGPassHandle tracePass;
        RDGTextureHandle reconstructedGI;
        RDGPassHandle reconstructionPass;
        RDGTextureHandle reprojectedGI;
        RDGTextureHandle reprojectedFast;
    };

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
        const auto importHistory = [&](ID3D12Resource* texture, const char* name) {
            return graph.CreateTextureSRVView(graph.RegisterExternalTexture(
                texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name));
        };
        auto previousGI = importHistory(previous.gi.Get(), "SSGI.PreviousGI");
        auto previousFast = importHistory(previous.fastGI.Get(), "SSGI.PreviousFastGI");
        auto previousHistoryDepth = importHistory(previous.depth.Get(), "SSGI.PreviousDepth");
        auto previousNormal = importHistory(previous.normal.Get(), "SSGI.PreviousNormal");
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !source.IsValid() ||
            !previousGI.IsValid() || !previousFast.IsValid() || !previousHistoryDepth.IsValid() || !previousNormal.IsValid())
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
        trace.checkerboardEnabled = config.checkerboard;
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
        trace.radius = config.radius;
        trace.thickness = TraceThickness;
        trace.normalBias = TraceNormalBias;
        // Keep UE SSGI ray counts. Hi-Z budgets count visited cells/descents, not fixed steps.
        trace.rayCount = 4u << (config.quality - 1);
        trace.maxIterations = config.quality == 4 ? 96u : 64u;

        WorldAddresses worldAddresses = {};
        // Bind legal addresses even for unused resources in screen-only shaders.
        ID3D12Resource* fallbackBuffer = world && world->materialBuffer ? world->materialBuffer : resourceManager->GetMaterialBuffer();
        worldAddresses.fill(fallbackBuffer ? fallbackBuffer->GetGPUVirtualAddress() : resourceManager->GetCBVGPUAddress(frameIndex));
        trace.hybrid.worldMode = EffectiveWorldMode(world, frameIndex);
        if (trace.hybrid.worldMode)
        {
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
                trace.global.maxTraceDistance = world->globalTraceDistance;
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
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        auto* tracePipeline = pipelineManager->GetSSGITracePSO();
        traceParams.WriteUAV(rawUav);
        traceParams.WriteUAV(reprojectedGIUav);
        traceParams.WriteUAV(reprojectedFastUav);
        auto pass =
            graph.AddPass("SSGI.Trace", ERDGPassFlags::Compute, traceParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, tracePipeline,
                address, trace.outputWidth, trace.outputHeight, &worldAddresses);
        });
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
        return {raw, pass, reconstructed, reconstructionPass, reprojectedGI, reprojectedFast};
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
        const TemporalInput& input, bool previewHistory)
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
        UINT width, UINT height, int frameIndex, uint32_t traceDownsampleFactor, const TextureViews& views, const AtrousInput& input)
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
        float intensity, bool giOnly)
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
