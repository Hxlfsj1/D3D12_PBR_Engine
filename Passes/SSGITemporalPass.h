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
    struct Output { RDGTextureHandle gi; RDGPassHandle pass; };

    static Output AddToGraph(RDGBuilder& graph, ResourceManager* resources, SSGIPipeline* pipeline,
        const SSGIHistory& history, const DirectX::XMFLOAT4X4& invViewProj, int frameIndex, const Input& input)
    {
        auto import = [&](ID3D12Resource* texture, const char* name) {
            return graph.RegisterExternalTexture(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
        };
        const auto& previous = history.Previous();
        const auto& output = history.Current();
        auto previousGI = graph.CreateTextureSRVView(import(previous.gi.Get(), "SSGI.PreviousGI"));
        auto previousDepth = graph.CreateTextureSRVView(import(previous.depth.Get(), "SSGI.PreviousDepth"));
        auto previousNormal = graph.CreateTextureSRVView(import(previous.normal.Get(), "SSGI.PreviousNormal"));
        auto outputGITexture = import(output.gi.Get(), "SSGI.TemporalGI");
        auto outputDepthTexture = import(output.depth.Get(), "SSGI.HistoryDepth");
        auto outputNormalTexture = import(output.normal.Get(), "SSGI.HistoryNormal");
        auto outputGI = graph.CreateTextureUAVView(outputGITexture);
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
            !depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !current.IsValid()) return {};

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
        auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
        RDGPassParameters params;
        params.ReadComputeSRV(depth); params.ReadComputeSRV(normal); params.ReadComputeSRV(orm);
        params.ReadComputeSRV(current);
        params.ReadComputeSRV(previousGI); params.ReadComputeSRV(previousDepth); params.ReadComputeSRV(previousNormal);
        params.WriteUAV(outputGI); params.WriteUAV(outputDepth); params.WriteUAV(outputNormal);
        auto pass = graph.AddPass("SSGI.Temporal", ERDGPassFlags::Compute, params,
            [=](ID3D12GraphicsCommandList* cmd) {
                ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetComputeRootSignature(pipeline->Root());
                cmd->SetPipelineState(pipeline->Temporal());
                cmd->SetComputeRootConstantBufferView(0, address);
                cmd->Dispatch((constants.outputWidth + 7) / 8, (constants.outputHeight + 7) / 8, 1);
            });
        graph.MarkTextureAsOutput(outputGITexture);
        graph.MarkTextureAsOutput(outputDepthTexture);
        graph.MarkTextureAsOutput(outputNormalTexture);
        return { outputGITexture, pass };
    }
};
