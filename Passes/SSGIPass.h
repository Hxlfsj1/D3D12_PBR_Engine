#ifndef SSGI_PASS_H
#define SSGI_PASS_H

#include "stdafx.h"
#include "RDG.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Settings_Manager.h"

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
    uint32_t padding3[2];
    // uint arrays in HLSL cbuffers have 16-byte stride. Use uint4 explicitly.
    DirectX::XMUINT4 levels[SSGIMaxLevels]; // depth SRV, padding, padding, padding
};
static_assert(sizeof(SSGITraceConstants) == 384);

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
    uint32_t padding[3];
};
static_assert(sizeof(SSGIReconstructConstants) == 176);

struct SSGITemporalConstants
{
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT4X4 previousViewProj;
    DirectX::XMFLOAT4X4 previousInvViewProj;
    uint32_t width;
    uint32_t height;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t ormIndex;
    uint32_t currentIndex;
    uint32_t previousGIIndex;
    uint32_t previousDepthIndex;
    uint32_t previousNormalIndex;
    uint32_t historyValid;
    uint32_t outputGIIndex;
    uint32_t outputDepthIndex;
    uint32_t outputNormalIndex;
    uint32_t previousFastIndex;
    uint32_t outputFastIndex;
    float maxHistoryFrames;
    float maxFastAccumulatedFrames;
    uint32_t padding;
};
static_assert(sizeof(SSGITemporalConstants) == 272);

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
};
static_assert(sizeof(SSGIAtrousConstants) == 176);

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
    uint32_t padding[2];
};
static_assert(sizeof(SSGICompositeConstants) == 176);

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
        RDGTextureHandle bounceSource;
    };
    struct Output
    {
        RDGTextureHandle sceneColor;
        RDGPassHandle pass;
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
        auto trace = AddTraceToGraph(graph, resourceManager, pipelineManager, view, proj, invProj, width, height,
            frameIndex, sampleFrame, config, views, {input.bounceSource});
        if (!trace.reconstructedGI.IsValid() || !trace.reconstructionPass.IsValid())
        {
            return {};
        }
        auto temporal = AddTemporalToGraph(graph, resourceManager, pipelineManager, history, invViewProj, width, height,
            frameIndex, views, {trace.reconstructedGI});
        if (!temporal.gi.IsValid() || !temporal.pass.IsValid())
        {
            return {};
        }
        auto spatial = AddAtrousToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, views, {temporal.moments, temporal.gi});
        if (!spatial.gi.IsValid() || !spatial.pass.IsValid())
        {
            return {};
        }
        auto composite = AddCompositeToGraph(graph, resourceManager, pipelineManager, view, invProj, width, height,
            frameIndex, views, {input.sceneColor, spatial.gi}, config.intensity);
        if (!composite.IsValid())
        {
            return {};
        }
        return {input.sceneColor, composite};
    }

private:
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
    };

    static TraceOutput AddTraceToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT4X4& invProj, UINT width, UINT height, int frameIndex, UINT sampleFrame,
        const SSGIConfig& config, const TextureViews& views, const TraceInput& input)
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto orm = views.orm;
        auto source = graph.CreateTextureSRVView(input.bounceSource);
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !source.IsValid())
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
        traceParams.ReadComputeSRV(source);
        traceParams.ReadComputeSRV(noise);
        SSGITraceConstants trace = {};
        trace.view = view;
        trace.proj = proj;
        trace.invProj = invProj;
        trace.width = width;
        trace.height = height;
        trace.outputWidth = (width + 1) / 2;
        trace.outputHeight = (height + 1) / 2;
        trace.depthIndex = depth.descriptorIndex;
        trace.normalIndex = normal.descriptorIndex;
        trace.ormIndex = orm.descriptorIndex;
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
        if (!rawUav.IsValid())
        {
            return {};
        }
        trace.outputIndex = rawUav.descriptorIndex;
        auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation)
        {
            return {};
        }
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        traceParams.WriteUAV(rawUav);
        auto pass =
            graph.AddPass("SSGI.Trace", ERDGPassFlags::Compute, traceParams, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteComputeNoBarrier(cmdList, resourceManager, pipelineManager, pipelineManager->GetSSGITracePSO(),
                address, trace.outputWidth, trace.outputHeight);
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
        return {raw, pass, reconstructed, reconstructionPass};
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
        PipelineManager* pipelineManager, ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS constants, UINT w, UINT h)
    {
        ID3D12DescriptorHeap* heaps[] = {resourceManager->GetMainDescriptorHeap()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(pipelineManager->GetSSGIRootSignature());
        cmdList->SetPipelineState(pso);
        cmdList->SetComputeRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, constants);
        cmdList->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    struct TemporalInput
    {
        RDGTextureHandle reconstructedGI;
    };
    struct TemporalOutput
    {
        RDGTextureHandle gi;
        RDGPassHandle pass;
        RDGTextureHandle moments;
    };

    static TemporalOutput AddTemporalToGraph(RDGBuilder& graph, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, const SSGIHistoryState& history, const DirectX::XMFLOAT4X4& invViewProj,
        UINT width, UINT height, int frameIndex, const TextureViews& views, const TemporalInput& input)
    {
        auto import = [&](ID3D12Resource* texture, const char* name) {
            return graph.RegisterExternalTexture(
                texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
        };
        const int historyIndex = resourceManager->GetSSGICurrentHistoryIdx();
        const auto& previous = resourceManager->GetSSGIHistoryTextures(1 - historyIndex);
        const auto& output = resourceManager->GetSSGIHistoryTextures(historyIndex);
        auto previousGI = graph.CreateTextureSRVView(import(previous.gi.Get(), "SSGI.PreviousGI"));
        auto previousFast = graph.CreateTextureSRVView(import(previous.fastGI.Get(), "SSGI.PreviousFastGI"));
        auto previousDepth = graph.CreateTextureSRVView(import(previous.depth.Get(), "SSGI.PreviousDepth"));
        auto previousNormal = graph.CreateTextureSRVView(import(previous.normal.Get(), "SSGI.PreviousNormal"));
        auto outputGITexture = import(output.gi.Get(), "SSGI.TemporalGI");
        auto outputFastTexture = import(output.fastGI.Get(), "SSGI.FastGI");
        auto outputDepthTexture = import(output.depth.Get(), "SSGI.HistoryDepth");
        auto outputNormalTexture = import(output.normal.Get(), "SSGI.HistoryNormal");
        const UINT halfWidth = (width + 1) / 2;
        const UINT halfHeight = (height + 1) / 2;
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
        if (!previousGI.IsValid() || !previousDepth.IsValid() || !previousNormal.IsValid() || !outputGI.IsValid() ||
            !outputDepth.IsValid() || !outputNormal.IsValid() || !previousFast.IsValid() || !outputFast.IsValid() ||
            !depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !current.IsValid())
        {
            return {};
        }

        SSGITemporalConstants constants = {};
        constants.invViewProj = invViewProj;
        constants.previousViewProj = history.previousViewProj;
        constants.previousInvViewProj = history.previousInvViewProj;
        constants.width = width;
        constants.height = height;
        constants.outputWidth = (constants.width + 1) / 2;
        constants.outputHeight = (constants.height + 1) / 2;
        constants.depthIndex = depth.descriptorIndex;
        constants.normalIndex = normal.descriptorIndex;
        constants.ormIndex = orm.descriptorIndex;
        constants.currentIndex = current.descriptorIndex;
        constants.previousGIIndex = previousGI.descriptorIndex;
        constants.previousDepthIndex = previousDepth.descriptorIndex;
        constants.previousNormalIndex = previousNormal.descriptorIndex;
        constants.historyValid = history.valid;
        constants.outputGIIndex = outputGI.descriptorIndex;
        constants.outputDepthIndex = outputDepth.descriptorIndex;
        constants.outputNormalIndex = outputNormal.descriptorIndex;
        constants.previousFastIndex = previousFast.descriptorIndex;
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
        params.ReadComputeSRV(previousGI);
        params.ReadComputeSRV(previousDepth);
        params.ReadComputeSRV(previousNormal);
        params.WriteUAV(outputGI);
        params.WriteUAV(outputDepth);
        params.WriteUAV(outputNormal);
        params.ReadComputeSRV(previousFast);
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
        UINT width, UINT height, int frameIndex, const TextureViews& views, const AtrousInput& input)
    {
        auto depth = views.depth;
        auto normal = views.normal;
        auto history = graph.CreateTextureSRVView(input.history);
        if (!depth.IsValid() || !normal.IsValid() || !history.IsValid())
        {
            return {};
        }
        const UINT halfWidth = (width + 1) / 2;
        const UINT halfHeight = (height + 1) / 2;
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
        UINT width, UINT height, int frameIndex, const TextureViews& views, const CompositeInput& input,
        float intensity)
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
        constants.halfWidth = (width + 1) / 2;
        constants.halfHeight = (height + 1) / 2;
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
        // RTV writes are ordered after previous writers by RDG; blending loads existing color.
        params.WriteRTV(target);
        return graph.AddPass(
            "SSGI.Composite", ERDGPassFlags::Graphics, params, [=](ID3D12GraphicsCommandList* cmdList) {
            ExecuteCompositeNoBarrier(
                cmdList, resourceManager, pipelineManager, address, target.cpuHandle, width, height);
        });
    }

    static void ExecuteCompositeNoBarrier(ID3D12GraphicsCommandList* cmdList, ResourceManager* resourceManager,
        PipelineManager* pipelineManager, D3D12_GPU_VIRTUAL_ADDRESS address, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv,
        UINT width, UINT height)
    {
        ID3D12DescriptorHeap* heaps[] = {resourceManager->GetMainDescriptorHeap()};
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(pipelineManager->GetSSGIRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetSSGICompositePSO());
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
