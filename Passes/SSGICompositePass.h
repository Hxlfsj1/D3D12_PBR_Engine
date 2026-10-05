#pragma once

#include "RDG.h"
#include "ResourceManager.h"
#include "SSGIPipeline.h"
#include "SSGIConstants.h"

class SSGICompositePass
{
public:
    struct Input { RDGTextureHandle sceneColor, gi, depth, normal, orm, albedo; };

    static RDGPassHandle AddToGraph(RDGBuilder& graph, ResourceManager* resources, SSGIPipeline* pipeline,
        const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& invProj,
        UINT width, UINT height, int frameIndex, const Input& input, float intensity = 1.0f)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDesc.Texture2D.MipLevels = 1;
        auto depth = graph.CreateTextureSRVView(input.depth, &depthDesc);
        auto normal = graph.CreateTextureSRVView(input.normal);
        auto orm = graph.CreateTextureSRVView(input.orm);
        auto albedo = graph.CreateTextureSRVView(input.albedo);
        auto gi = graph.CreateTextureSRVView(input.gi);
        auto target = graph.CreateTextureRTVView(input.sceneColor);
        if (!depth.IsValid() || !normal.IsValid() || !orm.IsValid() || !albedo.IsValid() || !gi.IsValid() || !target.IsValid()) return {};
        SSGI::CompositeConstants constants = {};
        constants.view = view; constants.invProj = invProj;
        constants.width = width; constants.height = height;
        constants.halfWidth = (width + 1) / 2; constants.halfHeight = (height + 1) / 2;
        constants.depthIndex = depth.descriptorIndex; constants.normalIndex = normal.descriptorIndex;
        constants.ormIndex = orm.descriptorIndex; constants.albedoIndex = albedo.descriptorIndex;
        constants.giIndex = gi.descriptorIndex;
        constants.intensity = intensity;
        auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        auto address = allocation.gpuAddress;
        RDGPassParameters params;
        params.ReadSRV(depth); params.ReadSRV(normal); params.ReadSRV(orm); params.ReadSRV(albedo); params.ReadSRV(gi);
        // RTV writes are ordered after previous writers by RDG; blending loads existing color.
        params.WriteRTV(target);
        return graph.AddPass("SSGI.Composite", ERDGPassFlags::Graphics, params,
            [=](ID3D12GraphicsCommandList* cmd) {
                ID3D12DescriptorHeap* heaps[] = { resources->GetMainDescriptorHeap() };
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetGraphicsRootSignature(pipeline->Root());
                cmd->SetPipelineState(pipeline->Composite());
                cmd->SetGraphicsRootConstantBufferView(0, address);
                cmd->OMSetRenderTargets(1, &target.cpuHandle, FALSE, nullptr);
                D3D12_VIEWPORT viewport = { 0, 0, float(width), float(height), 0, 1 };
                D3D12_RECT rect = { 0, 0, LONG(width), LONG(height) };
                cmd->RSSetViewports(1, &viewport); cmd->RSSetScissorRects(1, &rect);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->DrawInstanced(3, 1, 0, 0);
            });
    }
};
