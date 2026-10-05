#pragma once

#include "RDG.h"
#include "ResourceManager.h"
#include "SSGIPipeline.h"
#include "SSGIConstants.h"
#include "Settings_Manager.h"

class SSGIPass
{
public:
    struct Input
    {
        RDGTextureHandle depth, normal, orm, bounceSource;
    };
    struct Output
    {
        RDGTextureHandle rawGI;
        RDGPassHandle tracePass;
        RDGTextureHandle reconstructedGI;
        RDGPassHandle reconstructionPass;
    };

    static Output AddToGraph(RDGBuilder& graph, ResourceManager* resources,
        SSGIPipeline* pipeline, const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& proj, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, UINT sampleFrame,
        const SSGIConfig& config, const Input& input)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        auto depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        auto normal = graph.CreateTextureSRVView(input.normal);
        auto orm = graph.CreateTextureSRVView(input.orm);
        auto source = graph.CreateTextureSRVView(input.bounceSource);
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !source.IsValid()) return {};

        auto noiseTexture = graph.RegisterExternalTexture(resources->GetBlueNoiseTexture(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "BlueNoise.STBN.Vector2");
        D3D12_SHADER_RESOURCE_VIEW_DESC noiseDesc = {};
        noiseDesc.Format = DXGI_FORMAT_R8G8_UINT;
        noiseDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        noiseDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        noiseDesc.Texture2DArray.MipLevels = 1;
        noiseDesc.Texture2DArray.ArraySize = STBN_FRAMES;
        auto noise = graph.CreateTextureSRVView(noiseTexture, &noiseDesc);
        if (!noise.IsValid()) return {};

        RDGPassParameters traceParams;
        traceParams.ReadComputeSRV(depth);
        traceParams.ReadComputeSRV(normal);
        traceParams.ReadComputeSRV(orm);
        traceParams.ReadComputeSRV(source);
        traceParams.ReadComputeSRV(noise);
        SSGI::TraceConstants trace = {};
        trace.view = view; trace.proj = proj; trace.invProj = invProj;
        trace.width = width; trace.height = height;
        trace.outputWidth = (width + 1) / 2; trace.outputHeight = (height + 1) / 2;
        trace.depthIndex = depth.descriptorIndex; trace.normalIndex = normal.descriptorIndex;
        trace.ormIndex = orm.descriptorIndex; trace.sourceIndex = source.descriptorIndex;
        trace.frameIndex = sampleFrame;
        trace.blueNoiseIndex = noise.descriptorIndex;
        constexpr float TraceThickness = 0.15f;
        constexpr float TraceNormalBias = 0.02f;
        trace.radius = config.radius; trace.thickness = TraceThickness; trace.normalBias = TraceNormalBias;
        // Keep UE SSGI ray counts. Hi-Z budgets count visited cells/descents, not fixed steps.
        trace.rayCount = 4u << (config.quality - 1);
        trace.maxIterations = config.quality == 4 ? 96u : 64u;

        auto previousDepth = depth;
        UINT sourceWidth = width, sourceHeight = height;
        for (UINT level = 0; level < SSGI::MaxLevels; ++level)
        {
            UINT w = (sourceWidth + 1) / 2, h = (sourceHeight + 1) / 2;
            std::string suffix = std::to_string(level);
            auto depthTexture = CreateTexture(graph, w, h, DXGI_FORMAT_R32G32_FLOAT, "SSGI.DepthBounds." + suffix);
            auto depthUav = graph.CreateTextureUAVView(depthTexture);
            auto depthSrv = graph.CreateTextureSRVView(depthTexture);
            if (!depthUav.IsValid() || !depthSrv.IsValid()) return {};

            SSGI::PyramidConstants constants = {};
            constants.invProj = invProj;
            constants.sourceWidth = sourceWidth; constants.sourceHeight = sourceHeight;
            constants.outputWidth = w; constants.outputHeight = h;
            constants.depthIndex = previousDepth.descriptorIndex;
            constants.outputDepthIndex = depthUav.descriptorIndex;
            constants.firstLevel = level == 0;
            auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
            if (!allocation) return {};
            memcpy(allocation.cpuAddress, &constants, sizeof(constants));
            auto address = allocation.gpuAddress;
            RDGPassParameters params;
            params.ReadComputeSRV(previousDepth);
            params.WriteUAV(depthUav);
            graph.AddPass(("SSGI.Pyramid." + suffix).c_str(), ERDGPassFlags::Compute, params,
                [=](ID3D12GraphicsCommandList* cmd) {
                    Dispatch(cmd, resources, pipeline, pipeline->Pyramid(), address, w, h);
                });
            traceParams.ReadComputeSRV(depthSrv);
            trace.levels[level] = { depthSrv.descriptorIndex, 0, 0, 0 };
            ++trace.levelCount;
            previousDepth = depthSrv;
            sourceWidth = w; sourceHeight = h;
            if (w == 1 && h == 1) break;
        }

        auto raw = CreateTexture(graph, trace.outputWidth, trace.outputHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, "SSGI.RawGI");
        auto rawUav = graph.CreateTextureUAVView(raw);
        if (!rawUav.IsValid()) return {};
        trace.outputIndex = rawUav.descriptorIndex;
        auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        traceParams.WriteUAV(rawUav);
        auto pass = graph.AddPass("SSGI.Trace", ERDGPassFlags::Compute, traceParams,
            [=](ID3D12GraphicsCommandList* cmd) {
                Dispatch(cmd, resources, pipeline, pipeline->Trace(), address, trace.outputWidth, trace.outputHeight);
            });
        auto reconstructed = CreateTexture(graph, trace.outputWidth, trace.outputHeight,
            DXGI_FORMAT_R16G16B16A16_FLOAT, "SSGI.ReconstructedGI");
        auto reconstructedUav = graph.CreateTextureUAVView(reconstructed);
        auto rawSrv = graph.CreateTextureSRVView(raw);
        if (!reconstructedUav.IsValid() || !rawSrv.IsValid()) return {};
        SSGI::ReconstructConstants reconstruction = {};
        reconstruction.view = view; reconstruction.invProj = invProj;
        reconstruction.width = width; reconstruction.height = height;
        reconstruction.outputWidth = trace.outputWidth; reconstruction.outputHeight = trace.outputHeight;
        reconstruction.depthIndex = depth.descriptorIndex;
        reconstruction.normalIndex = normal.descriptorIndex;
        reconstruction.ormIndex = orm.descriptorIndex;
        reconstruction.rawIndex = rawSrv.descriptorIndex;

        reconstruction.outputIndex = reconstructedUav.descriptorIndex;
        auto reconstructionAllocation = resources->AllocatePassConstants(frameIndex, sizeof(reconstruction));
        if (!reconstructionAllocation) return {};
        memcpy(reconstructionAllocation.cpuAddress, &reconstruction, sizeof(reconstruction));
        auto reconstructionAddress = reconstructionAllocation.gpuAddress;
        RDGPassParameters reconstructionParams;
        reconstructionParams.ReadComputeSRV(depth);
        reconstructionParams.ReadComputeSRV(normal);
        reconstructionParams.ReadComputeSRV(orm);
        reconstructionParams.ReadComputeSRV(rawSrv);
        reconstructionParams.WriteUAV(reconstructedUav);
        auto reconstructionPass = graph.AddPass("SSGI.Reconstruct", ERDGPassFlags::Compute, reconstructionParams,
            [=](ID3D12GraphicsCommandList* cmd) {
                Dispatch(cmd, resources, pipeline, pipeline->Reconstruct(), reconstructionAddress,
                    reconstruction.outputWidth, reconstruction.outputHeight);
            });
        return { raw, pass, reconstructed, reconstructionPass };
    }

private:
    static RDGTextureHandle CreateTexture(RDGBuilder& graph, UINT w, UINT h, DXGI_FORMAT format, const std::string& name)
    {
        RDGTextureDesc desc;
        desc.width = w; desc.height = h; desc.format = format;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name.c_str());
    }

    static void Dispatch(ID3D12GraphicsCommandList* cmd, ResourceManager* resources,
        SSGIPipeline* pipeline, ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS constants, UINT w, UINT h)
    {
        ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(pipeline->Root());
        cmd->SetPipelineState(pso);
        cmd->SetComputeRootConstantBufferView(0, constants);
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
};
