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
        RDGPassHandle pass;
    };

    struct RtvHandles
    {
        D3D12_CPU_DESCRIPTOR_HANDLE albedo;
        D3D12_CPU_DESCRIPTOR_HANDLE normal;
        D3D12_CPU_DESCRIPTOR_HANDLE orm;
        D3D12_CPU_DESCRIPTOR_HANDLE emissive;
    };

    static size_t ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        const RtvHandles& rtvHandlesInput,
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        bool useZPrepass = false)
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
        cmdList->OMSetRenderTargets(4, rtvHandles, FALSE, &dsvHandle);

        const float clearColorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(rtvHandles[0], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[1], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[2], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[3], clearColorBlack, 0, nullptr);
        if (!useZPrepass)
        {
            cmdList->ClearDepthStencilView(
                dsvHandle,
                D3D12_CLEAR_FLAG_DEPTH,
                1.0f,
                0,
                0,
                nullptr);
        }

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
                            {
                                ID3D12PipelineState* pso = useZPrepass
                                    ? pipelineManager->GetGBufferCutoutAfterZPrepass_PSO(thisLod)
                                    : pipelineManager->GetGBufferCutout_PSO(thisLod);
                                cmdList->SetPipelineState(pso);
                            }
                            else
                            {
                                ID3D12PipelineState* pso = useZPrepass
                                    ? pipelineManager->GetGBufferAfterZPrepass_PSO(thisLod)
                                    : pipelineManager->GetGBuffer_PSO(thisLod);
                                cmdList->SetPipelineState(pso);
                            }
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
        size_t& transparentStartIndex,
        bool useZPrepass = false,
        RDGTextureHandle inputDepth = {})
    {
        RDGTextureHandle depth = inputDepth;
        bool consumeZPrepassDepth = useZPrepass && depth.IsValid();
        if (!depth.IsValid())
        {
            ID3D12Resource* deviceDepth = deviceContext->GetDepthStencilBuffer();
            const D3D12_RESOURCE_DESC deviceDepthDesc = deviceDepth->GetDesc();
            const UINT sceneWidth = static_cast<UINT>(viewport.Width);
            const UINT sceneHeight = static_cast<UINT>(viewport.Height);

            if (deviceDepthDesc.Width == sceneWidth && deviceDepthDesc.Height == sceneHeight)
            {
                depth = graph.RegisterExternalTexture(
                    deviceDepth,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    "SceneDepth");
            }
            else
            {
                RDGTextureDesc depthTextureDesc;
                depthTextureDesc.width = sceneWidth;
                depthTextureDesc.height = sceneHeight;
                depthTextureDesc.format = DXGI_FORMAT_R32_TYPELESS;
                depthTextureDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                depthTextureDesc.hasClearValue = true;
                depthTextureDesc.clearValue.Format = DXGI_FORMAT_D32_FLOAT;
                depthTextureDesc.clearValue.DepthStencil.Depth = 1.0f;
                depthTextureDesc.clearValue.DepthStencil.Stencil = 0;

                depth = graph.CreateTexture(
                    depthTextureDesc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    "SceneDepth");
            }
        }

        const RDGTextureDesc* depthDesc = graph.GetTextureDesc(depth);
        if (depthDesc == nullptr)
        {
            return {};
        }

        auto createGBufferTexture = [&](DXGI_FORMAT format, const char* name)
            {
                RDGTextureDesc desc;
                desc.width = depthDesc->width;
                desc.height = depthDesc->height;
                desc.format = format;
                desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                desc.hasClearValue = true;
                desc.clearValue.Format = format;

                return graph.CreateTexture(
                    desc,
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COMMON,
                    name);
            };

        RDGTextureHandle gbufferAlbedo = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferAlbedo");

        RDGTextureHandle gbufferNormal = createGBufferTexture(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            "GBufferNormal");

        RDGTextureHandle gbufferORM = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferORM");

        RDGTextureHandle gbufferEmissive = createGBufferTexture(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            "GBufferEmissive");

        if (!gbufferAlbedo.IsValid() ||
            !gbufferNormal.IsValid() ||
            !gbufferORM.IsValid() ||
            !gbufferEmissive.IsValid())
        {
            return {};
        }

        RDGTextureRTVHandle albedoRtv = graph.CreateTextureRTVView(gbufferAlbedo);
        RDGTextureRTVHandle normalRtv = graph.CreateTextureRTVView(gbufferNormal);
        RDGTextureRTVHandle ormRtv = graph.CreateTextureRTVView(gbufferORM);
        RDGTextureRTVHandle emissiveRtv = graph.CreateTextureRTVView(gbufferEmissive);
        RDGTextureDSVHandle depthDsv = graph.CreateTextureDSVView(depth);

        if (!albedoRtv.IsValid() ||
            !normalRtv.IsValid() ||
            !ormRtv.IsValid() ||
            !emissiveRtv.IsValid() ||
            !depthDsv.IsValid())
        {
            return {};
        }

        RtvHandles rtvHandles =
        {
            albedoRtv.cpuHandle,
            normalRtv.cpuHandle,
            ormRtv.cpuHandle,
            emissiveRtv.cpuHandle
        };

        RDGPassParameters params;
        params.WriteRTV(albedoRtv);
        params.WriteRTV(normalRtv);
        params.WriteRTV(ormRtv);
        params.WriteRTV(emissiveRtv);
        params.WriteDSV(depthDsv);

        RDGPassHandle pass = graph.AddPass(
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
                    rtvHandles,
                    depthDsv.cpuHandle,
                    consumeZPrepassDepth);
            });

        return { gbufferAlbedo, gbufferNormal, gbufferORM, gbufferEmissive, depth, pass };
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
        graph.SetTransientResourceAllocator(
            [deviceContext, resourceManager, frameIndex](
                const D3D12_RESOURCE_DESC& resourceDesc,
                D3D12_RESOURCE_STATES initialState,
                D3D12_RESOURCE_STATES finalState,
                const D3D12_CLEAR_VALUE* clearValue,
                RDGTransientResourceLease* outResource)
            {
                return resourceManager->AllocateRDGTransientResource(
                    deviceContext,
                    frameIndex,
                    resourceDesc,
                    initialState,
                    finalState,
                    clearValue,
                    outResource);
            });

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
