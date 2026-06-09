#ifndef GBUFFER_PASS_H
#define GBUFFER_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include <vector>

class GBufferPass
{
public:
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

        CD3DX12_RESOURCE_BARRIER barriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferAlbedo(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferNormal(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferORM(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmdList->ResourceBarrier(3, barriers);

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandles[3] =
        {
            resourceManager->GetGBufferAlbedoRtvHandle(),
            resourceManager->GetGBufferNormalRtvHandle(),
            resourceManager->GetGBufferORMRtvHandle()
        };
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle = deviceContext->GetDSVHandle();
        cmdList->OMSetRenderTargets(3, rtvHandles, FALSE, &dsvHandle);

        const float clearColorBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(rtvHandles[0], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[1], clearColorBlack, 0, nullptr);
        cmdList->ClearRenderTargetView(rtvHandles[2], clearColorBlack, 0, nullptr);

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

        CD3DX12_RESOURCE_BARRIER revertBarriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferAlbedo(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferNormal(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetGBufferORM(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(3, revertBarriers);

        return transparentStartIndex;
    }
};

#endif
