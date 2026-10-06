#pragma once
#include "RDG.h"
#include "ResourceManager.h"
#include "SSGIPipeline.h"
#include "SSGIConstants.h"

class SSGIAtrousPass
{
public:
    struct Input { RDGTextureHandle moments, history, depth, normal; };
    struct Output { RDGTextureHandle gi; RDGPassHandle pass; };
    static Output AddToGraph(RDGBuilder& graph, ResourceManager* resources, SSGIPipeline* pipeline,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, const Input& input)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        auto depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        auto normal = graph.CreateTextureSRVView(input.normal);
        auto history = graph.CreateTextureSRVView(input.history);
        if (!depth.IsValid() || !normal.IsValid() || !history.IsValid()) return {};
        RDGTextureDesc desc;
        desc.width = (width + 1) / 2; desc.height = (height + 1) / 2;
        desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto source = input.moments;
        RDGPassHandle lastPass;
        for (UINT iteration = 0; iteration < 2; ++iteration)
        {
            const char* name = iteration == 0 ? "SSGI.Atrous1" : "SSGI.Atrous2";
            auto target = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
            auto read = graph.CreateTextureSRVView(source);
            auto write = graph.CreateTextureUAVView(target);
            if (!read.IsValid() || !write.IsValid()) return {};
            SSGI::AtrousConstants constants = {};
            constants.view = view; constants.invProj = invProj;
            constants.width = width; constants.height = height;
            constants.outputWidth = desc.width; constants.outputHeight = desc.height;
            constants.giIndex = read.descriptorIndex; constants.depthIndex = depth.descriptorIndex;
            constants.normalIndex = normal.descriptorIndex; constants.historyIndex = history.descriptorIndex;
            constants.outputIndex = write.descriptorIndex; constants.step = 1u << iteration;
            constants.phiLuminance = 2.0f; constants.depthThreshold = 0.003f;
            auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
            if (!allocation) return {};
            memcpy(allocation.cpuAddress, &constants, sizeof(constants));
            auto address = allocation.gpuAddress;
            auto pso = pipeline->Atrous(iteration == 0);
            RDGPassParameters params;
            params.ReadComputeSRV(read); params.ReadComputeSRV(depth); params.ReadComputeSRV(normal);
            params.ReadComputeSRV(history); params.WriteUAV(write);
            lastPass = graph.AddPass(name, ERDGPassFlags::Compute, params,
                [=](ID3D12GraphicsCommandList* cmd) {
                    ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
                    cmd->SetDescriptorHeaps(1, heaps);
                    cmd->SetComputeRootSignature(pipeline->Root());
                    cmd->SetPipelineState(pso);
                    cmd->SetComputeRootConstantBufferView(0, address);
                    cmd->Dispatch((constants.outputWidth + 7) / 8, (constants.outputHeight + 7) / 8, 1);
                });
            if (!lastPass.IsValid()) return {};
            source = target;
        }
        // Spatial output is consumed by composite only, never fed back into temporal history.
        return { source, lastPass };
    }
};
