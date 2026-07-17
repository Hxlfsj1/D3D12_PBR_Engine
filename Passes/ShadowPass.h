#ifndef SHADOW_PASS_H
#define SHADOW_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include "RDG.h"
#include <array>
#include <vector>

class ShadowPass
{
public:
    struct Output
    {
        RDGTextureHandle shadowMap;
        RDGTextureSRVHandle shadowMapSrv;
        RDGPassHandle pass;
    };

    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        auto cmdList = deviceContext->GetCommandList();

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES> shadowDsvHandles = {};
        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            shadowDsvHandles[cascadeIdx] = resourceManager->GetShadowDsvHandle(cascadeIdx);
        }

        CD3DX12_RESOURCE_BARRIER toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &toDepthWrite);

        ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            shadowVisibleInstancesByCascade,
            shadowInstanceOffsets,
            visibleInstancesSize,
            shadowDsvHandles);

        CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toSrv);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize,
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES>& shadowDsvHandles)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetShadowPSO());

        D3D12_VIEWPORT shadowViewport = { 0.0f, 0.0f, 4096.0f, 4096.0f, 0.0f, 1.0f };
        D3D12_RECT shadowScissor = { 0, 0, 4096, 4096 };

        cmdList->RSSetViewports(1, &shadowViewport);
        cmdList->RSSetScissorRects(1, &shadowScissor);

        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            const auto& shadowVisibleInstances = shadowVisibleInstancesByCascade[cascadeIdx];
            size_t cascadeInstanceOffset = shadowInstanceOffsets[cascadeIdx];

            D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHandles[cascadeIdx];
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
            cmdList->SetDescriptorHeaps(1, heaps);

            D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);
            cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);
            cmdList->SetGraphicsRootShaderResourceView(3, resourceManager->GetMaterialBufferGPUAddress());

            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            Model* currentModel = nullptr;
            int currentLod = -1;
            bool currentIsCutout = false;
            size_t instanceStartOffset = 0;
            UINT currentInstanceCount = 0;

            for (size_t i = 0; i <= shadowVisibleInstances.size(); ++i)
            {
                bool isEnd = (i == shadowVisibleInstances.size());
                Model* thisModel = isEnd ? nullptr : shadowVisibleInstances[i]->pModel;
                int thisLod = isEnd ? -1 : shadowVisibleInstances[i]->currentLodLevel;
                bool thisIsCutout = isEnd ? false : shadowVisibleInstances[i]->isCutout;

                if ((isEnd || thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout) && currentInstanceCount > 0 && currentModel != nullptr)
                {
                    D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + ((visibleInstancesSize + cascadeInstanceOffset + instanceStartOffset) * sizeof(InstanceData));
                    cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                    for (auto& mesh : currentModel->meshes)
                    {
                        UINT constants[2] = { mesh.materialID, cascadeIdx };
                        cmdList->SetGraphicsRoot32BitConstants(4, 2, constants, 0);
                        mesh.Draw(cmdList, currentInstanceCount, currentLod);
                    }
                }

                if (!isEnd)
                {
                    if (thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout)
                    {
                        if (currentModel == nullptr || thisIsCutout != currentIsCutout)
                        {
                            if (thisIsCutout)
                                cmdList->SetPipelineState(pipelineManager->GetShadowCutoutPSO());
                            else
                                cmdList->SetPipelineState(pipelineManager->GetShadowPSO());
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
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "ShadowMap");
        graph.MarkTextureAsOutput(shadowMap);

        D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDesc = {};
        shadowDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        shadowDsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        shadowDsvDesc.Texture2DArray.MipSlice = 0;
        shadowDsvDesc.Texture2DArray.ArraySize = 1;

        std::array<RDGTextureDSVHandle, NUM_CASCADES> shadowDsvViews = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES> shadowDsvHandles = {};
        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            shadowDsvDesc.Texture2DArray.FirstArraySlice = cascadeIdx;
            shadowDsvViews[cascadeIdx] = graph.CreateTextureDSVView(shadowMap, &shadowDsvDesc);
            if (!shadowDsvViews[cascadeIdx].IsValid())
            {
                return {};
            }

            shadowDsvHandles[cascadeIdx] = shadowDsvViews[cascadeIdx].cpuHandle;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
        shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2DArray.MipLevels = 1;
        shadowSrvDesc.Texture2DArray.ArraySize = NUM_CASCADES;

        RDGTextureSRVHandle shadowMapSrv = graph.CreateTextureSRVView(shadowMap, &shadowSrvDesc);
        if (!shadowMapSrv.IsValid())
        {
            return {};
        }

        RDGPassParameters params;
        params.WriteDSV(shadowDsvViews[0]);

        RDGPassHandle pass = graph.AddPass(
            "ShadowMap",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    shadowVisibleInstancesByCascade,
                    shadowInstanceOffsets,
                    visibleInstancesSize,
                    shadowDsvHandles);
            });

        return { shadowMap, shadowMapSrv, pass };
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        RDGBuilder graph(deviceContext, "ShadowGraph");
        graph.SetTransientResourceAllocator(
            [deviceContext, resourceManager, frameIndex](
                const D3D12_RESOURCE_DESC& resourceDesc,
                D3D12_RESOURCE_STATES initialState,
                D3D12_RESOURCE_STATES finalState,
                const D3D12_CLEAR_VALUE* clearValue,
                Microsoft::WRL::ComPtr<ID3D12Resource>* outResource)
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
        graph.SetTransientSrvUavDescriptorAllocator(
            [resourceManager](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return resourceManager->AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

        AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            shadowVisibleInstancesByCascade,
            shadowInstanceOffsets,
            visibleInstancesSize);

        graph.Execute(deviceContext->GetCommandList());
    }
};

#endif
