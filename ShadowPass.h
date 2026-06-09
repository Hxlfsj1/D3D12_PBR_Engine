#ifndef SHADOW_PASS_H
#define SHADOW_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include <vector>

class ShadowPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::vector<ModelInstance*>& shadowVisibleInstances,
        size_t visibleInstancesSize)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_RESOURCE_BARRIER toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);

        cmdList->ResourceBarrier(1, &toDepthWrite);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetShadowPSO());

        D3D12_VIEWPORT shadowViewport = { 0.0f, 0.0f, 4096.0f, 4096.0f, 0.0f, 1.0f };
        D3D12_RECT shadowScissor = { 0, 0, 4096, 4096 };

        cmdList->RSSetViewports(1, &shadowViewport);
        cmdList->RSSetScissorRects(1, &shadowScissor);

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = resourceManager->GetShadowDsvHandle();

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
        UINT instanceStartOffset = 0;
        UINT currentInstanceCount = 0;

        for (size_t i = 0; i <= shadowVisibleInstances.size(); ++i)
        {
            bool isEnd = (i == shadowVisibleInstances.size());
            Model* thisModel = isEnd ? nullptr : shadowVisibleInstances[i]->pModel;
            int thisLod = isEnd ? -1 : shadowVisibleInstances[i]->currentLodLevel;
            bool thisIsCutout = isEnd ? false : shadowVisibleInstances[i]->isCutout;

            if ((isEnd || thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout) && currentInstanceCount > 0 && currentModel != nullptr)
            {
                D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + ((visibleInstancesSize + instanceStartOffset) * sizeof(InstanceData));
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
                    if (thisIsCutout != currentIsCutout)
                    {
                        if (thisIsCutout)
                        {
                            cmdList->SetPipelineState(pipelineManager->GetShadowCutoutPSO());
                        }
                        else
                        {
                            cmdList->SetPipelineState(pipelineManager->GetShadowPSO());
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

        CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmdList->ResourceBarrier(1, &toSrv);
    }
};

#endif
