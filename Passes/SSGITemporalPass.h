#pragma once

#include "RDG.h"
#include "ResourceManager.h"
#include "SSGIPipeline.h"
#include "SSGIHistory.h"
#include "SSGIConstants.h"

class SSGITemporalPass
{
public:
    struct Input { RDGTextureHandle reconstructedGI, depth, normal, orm; };
    struct Output { RDGTextureHandle gi; RDGPassHandle pass; RDGTextureHandle moments; };

    static Output AddToGraph(RDGBuilder& graph, ResourceManager* resources, SSGIPipeline* pipeline,
        const SSGIHistory& history, const DirectX::XMFLOAT4X4& invViewProj, int frameIndex, const Input& input)
    {
        auto import = [&](ID3D12Resource* texture, const char* name) {
            return graph.RegisterExternalTexture(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
        };
        const auto& previous = history.Previous();
        const auto& output = history.Current();
        auto previousGI = graph.CreateTextureSRVView(import(previous.gi.Get(), "SSGI.PreviousGI"));
        auto previousFast = graph.CreateTextureSRVView(import(previous.fastGI.Get(), "SSGI.PreviousFastGI"));
        auto previousDepth = graph.CreateTextureSRVView(import(previous.depth.Get(), "SSGI.PreviousDepth"));
        auto previousNormal = graph.CreateTextureSRVView(import(previous.normal.Get(), "SSGI.PreviousNormal"));
        auto outputGITexture = import(output.gi.Get(), "SSGI.TemporalGI");
        auto outputFastTexture = import(output.fastGI.Get(), "SSGI.FastGI");
        auto outputDepthTexture = import(output.depth.Get(), "SSGI.HistoryDepth");
        auto outputNormalTexture = import(output.normal.Get(), "SSGI.HistoryNormal");
        RDGTextureDesc desc;
        desc.width = (history.Width() + 1) / 2; desc.height = (history.Height() + 1) / 2;
        desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto slowTexture = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.AccumulatedSlow");
        auto fastTexture = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.AccumulatedFast");
        auto outputGI = graph.CreateTextureUAVView(slowTexture);
        auto outputFast = graph.CreateTextureUAVView(fastTexture);
        auto outputDepth = graph.CreateTextureUAVView(outputDepthTexture);
        auto outputNormal = graph.CreateTextureUAVView(outputNormalTexture);
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        auto depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        auto normal = graph.CreateTextureSRVView(input.normal);
        auto orm = graph.CreateTextureSRVView(input.orm);
        auto current = graph.CreateTextureSRVView(input.reconstructedGI);
        if (!previousGI.IsValid() || !previousDepth.IsValid() || !previousNormal.IsValid() ||
            !outputGI.IsValid() || !outputDepth.IsValid() || !outputNormal.IsValid() ||
            !previousFast.IsValid() || !outputFast.IsValid() || !depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !current.IsValid()) return {};

        SSGI::TemporalConstants constants = {};
        constants.invViewProj = invViewProj;
        constants.previousViewProj = history.PreviousViewProj();
        constants.previousInvViewProj = history.PreviousInvViewProj();
        constants.width = history.Width(); constants.height = history.Height();
        constants.outputWidth = (constants.width + 1) / 2; constants.outputHeight = (constants.height + 1) / 2;
        constants.depthIndex = depth.descriptorIndex; constants.normalIndex = normal.descriptorIndex;
        constants.ormIndex = orm.descriptorIndex; constants.currentIndex = current.descriptorIndex;
        constants.previousGIIndex = previousGI.descriptorIndex;
        constants.previousDepthIndex = previousDepth.descriptorIndex;
        constants.previousNormalIndex = previousNormal.descriptorIndex;
        constants.historyValid = history.IsValid();
        constants.outputGIIndex = outputGI.descriptorIndex;
        constants.outputDepthIndex = outputDepth.descriptorIndex;
        constants.outputNormalIndex = outputNormal.descriptorIndex;
        constants.previousFastIndex = previousFast.descriptorIndex;
        constants.outputFastIndex = outputFast.descriptorIndex;
        // Keep the project-tested 16-sample main cap; NRD fast weight remains 1/7.
        constants.maxHistoryFrames = 16.0f;
        constants.maxFastAccumulatedFrames = 6.0f;
        auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
        RDGPassParameters params;
        params.ReadComputeSRV(depth); params.ReadComputeSRV(normal); params.ReadComputeSRV(orm);
        params.ReadComputeSRV(current);
        params.ReadComputeSRV(previousGI); params.ReadComputeSRV(previousDepth); params.ReadComputeSRV(previousNormal);
        params.WriteUAV(outputGI); params.WriteUAV(outputDepth); params.WriteUAV(outputNormal);
        params.ReadComputeSRV(previousFast); params.WriteUAV(outputFast);
        auto pass = graph.AddPass("SSGI.Temporal", ERDGPassFlags::Compute, params,
            [=](ID3D12GraphicsCommandList* cmd) {
                ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetComputeRootSignature(pipeline->Root());
                cmd->SetPipelineState(pipeline->Temporal());
                cmd->SetComputeRootConstantBufferView(0, address);
                cmd->Dispatch((constants.outputWidth + 7) / 8, (constants.outputHeight + 7) / 8, 1);
            });
        auto slow = graph.CreateTextureSRVView(slowTexture);
        auto fast = graph.CreateTextureSRVView(fastTexture);
        auto finalGI = graph.CreateTextureUAVView(outputGITexture);
        auto finalFast = graph.CreateTextureUAVView(outputFastTexture);
        auto momentTexture = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "SSGI.ColorMoments");
        auto moments = graph.CreateTextureUAVView(momentTexture);
        if (!pass.IsValid() || !slow.IsValid() || !fast.IsValid() || !finalGI.IsValid() || !finalFast.IsValid() || !moments.IsValid()) return {};
        SSGI::HistoryClampConstants clamp = {};
        clamp.width = desc.width; clamp.height = desc.height;
        clamp.slowIndex = slow.descriptorIndex; clamp.fastIndex = fast.descriptorIndex;
        clamp.noisyIndex = current.descriptorIndex;
        clamp.outputMomentsIndex = moments.descriptorIndex;
        clamp.outputGIIndex = finalGI.descriptorIndex; clamp.outputFastIndex = finalFast.descriptorIndex;
        // RELAX defaults; these are implementation constants, not additional artist settings.
        clamp.sigmaScale = 2.0f; clamp.accelerationAmount = 0.3f; clamp.resetAmount = 0.5f;
        clamp.spatialSigmaScale = 4.5f; clamp.temporalSigmaScale = 0.5f;
        auto clampAllocation = resources->AllocatePassConstants(frameIndex, sizeof(clamp));
        if (!clampAllocation) return {};
        memcpy(clampAllocation.cpuAddress, &clamp, sizeof(clamp));
        auto clampAddress = clampAllocation.gpuAddress;
        RDGPassParameters clampParams;
        clampParams.ReadComputeSRV(slow); clampParams.ReadComputeSRV(fast); clampParams.ReadComputeSRV(current);
        clampParams.WriteUAV(finalGI); clampParams.WriteUAV(finalFast); clampParams.WriteUAV(moments);
        auto clampPass = graph.AddPass("SSGI.HistoryClamp", ERDGPassFlags::Compute, clampParams,
            [=](ID3D12GraphicsCommandList* cmd) {
                ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetComputeRootSignature(pipeline->Root());
                cmd->SetPipelineState(pipeline->HistoryClamp());
                cmd->SetComputeRootConstantBufferView(0, clampAddress);
                cmd->Dispatch((clamp.width + 7) / 8, (clamp.height + 7) / 8, 1);
            });
        graph.MarkTextureAsOutput(outputGITexture);
        graph.MarkTextureAsOutput(outputFastTexture);
        graph.MarkTextureAsOutput(outputDepthTexture);
        graph.MarkTextureAsOutput(outputNormalTexture);
        return { outputGITexture, clampPass, momentTexture };
    }
};
