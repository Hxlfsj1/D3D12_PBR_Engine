#ifndef GBUFFER_PASS_H
#define GBUFFER_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include "RDG.h"
#include <vector>

class GBufferPass
{
public:
    struct Output
    {
        RDGTextureHandle albedo;
        RDGTextureHandle normal;
        RDGTextureHandle orm;
        RDGTextureHandle emissive;
        RDGTextureHandle depth;
    };

    struct RtvHandles
    {
        D3D12_CPU_DESCRIPTOR_HANDLE albedo;
        D3D12_CPU_DESCRIPTOR_HANDLE normal;
        D3D12_CPU_DESCRIPTOR_HANDLE orm;
        D3D12_CPU_DESCRIPTOR_HANDLE emissive;
    };

    static size_t Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances)
    {
        auto cmdList = deviceContext->GetCommandList();

        RtvHandles rtvHandles =
        {
            resourceManager->GetGBufferAlbedoRtvHandle(),
            resourceManager->GetGBufferNormalRtvHandle(),
            resourceManager->GetGBufferORMRtvHandle(),
            resourceManager->GetGBufferEmissiveRtvHandle()
        };

        CD3DX12_RESOURCE_BARRIER barriers[4] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferAlbedo(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferNormal(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferORM(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferEmissive(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmdList->ResourceBarrier(4, barriers);

        size_t transparentStartIndex = ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            visibleInstances,
            rtvHandles);

        CD3DX12_RESOURCE_BARRIER revertBarriers[4] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferAlbedo(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferNormal(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferORM(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferEmissive(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(4, revertBarriers);

        return transparentStartIndex;
    }

    static size_t ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        const RtvHandles& rtvHandlesInput)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[4] =
        {
            rtvHandlesInput.albedo,
            rtvHandlesInput.normal,
            rtvHandlesInput.orm,
            rtvHandlesInput.emissive
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle = deviceContext->GetDSVHandle();
        cmdList->OMSetRenderTargets(4, rtvHandles, FALSE, &dsvHandle);

        const float clearColorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(rtvHandles[0], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[1], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[2], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[3], clearColorBlack, 0, nullptr);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());
        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);
        cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);
        cmdList->SetGraphicsRootShaderResourceView(3, resourceManager->GetMaterialBufferGPUAddress());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        size_t transparentStartIndex = visibleInstances.size();

        if (!visibleInstances.empty())
        {
            for (size_t i = 0; i < visibleInstances.size(); ++i)
            {
                if (visibleInstances[i]->isTransparent)
                {
                    transparentStartIndex = i;
                    break;
                }
            }

            Model* currentModel = nullptr;
            int currentLod = -1;
            bool currentIsCutout = false;
            UINT instanceStartOffset = 0;
            UINT currentInstanceCount = 0;

            for (size_t i = 0; i <= transparentStartIndex; ++i)
            {
                bool isEnd = (i == transparentStartIndex);
                Model* thisModel = isEnd ? nullptr : visibleInstances[i]->pModel;
                int thisLod = isEnd ? -1 : visibleInstances[i]->currentLodLevel;
                bool thisIsCutout = isEnd ? false : visibleInstances[i]->isCutout;

                if ((isEnd || thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout) && currentInstanceCount > 0 && currentModel != nullptr)
                {
                    D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + (instanceStartOffset * sizeof(InstanceData));
                    cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                    for (auto& mesh : currentModel->meshes)
                    {
                        cmdList->SetGraphicsRoot32BitConstants(4, 1, &mesh.materialID, 0);
                        mesh.Draw(cmdList, currentInstanceCount, currentLod);
                    }
                }

                if (!isEnd)
                {
                    if (thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout)
                    {
                        if (thisLod != currentLod || thisIsCutout != currentIsCutout)
                        {
                            if (thisIsCutout)
                                cmdList->SetPipelineState(pipelineManager->GetGBufferCutout_PSO(thisLod));
                            else
                                cmdList->SetPipelineState(pipelineManager->GetGBuffer_PSO(thisLod));
                        }
                        currentModel = thisModel;
                        currentLod = thisLod;
                        currentIsCutout = thisIsCutout;
                        instanceStartOffset = i;
                        currentInstanceCount = 1;
                    }
                    else
                    {
                        currentInstanceCount++;
                    }
                }
            }
        }

        return transparentStartIndex;
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t& transparentStartIndex)
    {
        const uint32_t width = viewport.Width > 0.0f ? static_cast<uint32_t>(viewport.Width) : 1;
        const uint32_t height = viewport.Height > 0.0f ? static_cast<uint32_t>(viewport.Height) : 1;

        auto createGBufferTexture = [&](DXGI_FORMAT format, const char* transientName, ID3D12Resource* fallbackResource, const char* fallbackName)
            {
                RDGTextureDesc desc = {};
                desc.width = width;
                desc.height = height;
                desc.format = format;
                desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                desc.hasClearValue = true;
                desc.clearValue.Format = format;

                RDGTextureHandle texture = graph.CreateTexture(
                    desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    transientName);

                if (!texture.IsValid())
                {
                    texture = graph.RegisterExternalTexture(
                        fallbackResource,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        fallbackName);
                }

                return texture;
            };

        RDGTextureHandle gbufferAlbedo = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferAlbedoTransient",
            resourceManager->GetGBufferAlbedo(),
            "GBufferAlbedo");

        RDGTextureHandle gbufferNormal = createGBufferTexture(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            "GBufferNormalTransient",
            resourceManager->GetGBufferNormal(),
            "GBufferNormal");

        RDGTextureHandle gbufferORM = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferORMTransient",
            resourceManager->GetGBufferORM(),
            "GBufferORM");

        RDGTextureHandle gbufferEmissive = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferEmissiveTransient",
            resourceManager->GetGBufferEmissive(),
            "GBufferEmissive");

        RDGTextureHandle depth = graph.RegisterExternalTexture(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");

        RtvHandles rtvHandles =
        {
            resourceManager->GetGBufferAlbedoRtvHandle(),
            resourceManager->GetGBufferNormalRtvHandle(),
            resourceManager->GetGBufferORMRtvHandle(),
            resourceManager->GetGBufferEmissiveRtvHandle()
        };

        D3D12_CPU_DESCRIPTOR_HANDLE transientRtv = {};
        if (graph.CreateTransientTextureRTV(gbufferAlbedo, &transientRtv))
        {
            rtvHandles.albedo = transientRtv;
        }

        if (graph.CreateTransientTextureRTV(gbufferNormal, &transientRtv))
        {
            rtvHandles.normal = transientRtv;
        }

        if (graph.CreateTransientTextureRTV(gbufferORM, &transientRtv))
        {
            rtvHandles.orm = transientRtv;
        }

        if (graph.CreateTransientTextureRTV(gbufferEmissive, &transientRtv))
        {
            rtvHandles.emissive = transientRtv;
        }

        RDGPassParameters params;
        params.WriteRTV(gbufferAlbedo);
        params.WriteRTV(gbufferNormal);
        params.WriteRTV(gbufferORM);
        params.WriteRTV(gbufferEmissive);
        params.WriteDSV(depth);

        graph.AddPass(
            "GBuffer",
            ERDGPassFlags::Graphics,
            params,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                transparentStartIndex = ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    rtvHandles);
            });

        return { gbufferAlbedo, gbufferNormal, gbufferORM, gbufferEmissive, depth };
    }

    static size_t ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances)
    {
        RDGBuilder graph(deviceContext, "GBufferGraph");

        size_t transparentStartIndex = visibleInstances.size();

        AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            visibleInstances,
            transparentStartIndex);

        graph.Execute(deviceContext->GetCommandList());

        return transparentStartIndex;
    }
};

#endif
