#ifndef PBR_PASS_H
#define PBR_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include <vector>

class PBRPass
{
public:
    static size_t ExecuteOpaque(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances)
    {
        auto cmdList = deviceContext->GetCommandList();

        // ====================================================================================================
        // Viewport and scissor setup
        // ====================================================================================================

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = resourceManager->GetPostProcessRtvHandle();
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = deviceContext->GetDSVHandle();

        // ====================================================================================================
        // Global State Setup (Needed for both Models and Skybox)
        // ====================================================================================================

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);
        cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);

        cmdList->SetGraphicsRootConstantBufferView(2, resourceManager->GetSHBufferGPUAddress());

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ====================================================================================================
        // Opaque Objects Rendering (Only if visible)
        // ====================================================================================================

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

            auto DrawQueue = [=](size_t startIdx, size_t endIdx, bool isPBR)
                {
                    if (startIdx >= endIdx) return;

                    Model* currentModel = nullptr;
                    int currentLod = -1;
                    UINT instanceStartOffset = 0;
                    UINT currentInstanceCount = 0;

                    for (size_t i = startIdx; i <= endIdx; ++i)
                    {
                        bool isEnd = (i == endIdx);
                        Model* thisModel = isEnd ? nullptr : visibleInstances[i]->pModel;
                        int thisLod = isEnd ? -1 : visibleInstances[i]->currentLodLevel;

                        if ((isEnd || thisModel != currentModel || thisLod != currentLod) && currentInstanceCount > 0 && currentModel != nullptr)
                        {
                            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (instanceStartOffset * sizeof(InstanceData));
                            cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                            for (auto& mesh : currentModel->meshes)
                            {
                                UINT srvIdx[4] = { resourceManager->GetDummyAlbedoIdx(), resourceManager->GetDummyNormalIdx(), resourceManager->GetDummyORMIdx(), resourceManager->GetDummyEmissiveIdx() };

                                for (auto& tex : mesh.textures)
                                {
                                    switch (tex.type)
                                    {
                                    case TextureType::Albedo:
                                        srvIdx[0] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        break;
                                    case TextureType::Normal:
                                        srvIdx[1] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        break;
                                    case TextureType::ORM:
                                        srvIdx[2] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        break;
                                    case TextureType::Emissive:
                                        srvIdx[3] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        break;
                                    default:
                                        break;
                                    }
                                }

                                if (isPBR)
                                {
                                    UINT32 matIndices[5] = { srvIdx[0], srvIdx[1], srvIdx[2], srvIdx[3], (UINT32)mesh.isUnlit };
                                    cmdList->SetGraphicsRoot32BitConstants(3, 5, matIndices, 0);
                                }

                                mesh.Draw(cmdList, currentInstanceCount, currentLod);
                            }
                        }

                        if (!isEnd)
                        {
                            if (thisModel != currentModel || thisLod != currentLod)
                            {
                                if (thisLod != currentLod)
                                {
                                    if (isPBR)
                                    {
                                        cmdList->SetPipelineState(pipelineManager->GetPBR_PSO(thisLod));
                                    }
                                    else
                                    {
                                        cmdList->SetPipelineState(pipelineManager->GetZPrepass_PSO());
                                    }
                                }

                                currentModel = thisModel;
                                currentLod = thisLod;
                                instanceStartOffset = i;
                                currentInstanceCount = 1;
                            }
                            else
                            {
                                currentInstanceCount++;
                            }
                        }
                    }
                };

            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            DrawQueue(0, transparentStartIndex, false);

            cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            DrawQueue(0, transparentStartIndex, true);
        }
        else
        {
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }

        return transparentStartIndex;
    }

    static void ExecuteTransparent(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex)
    {
        // ====================================================================================================
        // Transparent Object Pass (Only if visible)
        // ====================================================================================================

        if (visibleInstances.empty() || transparentStartIndex >= visibleInstances.size())
        {
            return;
        }

        auto cmdList = deviceContext->GetCommandList();
        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);

        for (size_t i = transparentStartIndex; i < visibleInstances.size(); ++i)
        {
            ModelInstance* instance = visibleInstances[i];
            if (!instance->pModel) continue;

            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (i * sizeof(InstanceData));

            auto BindAndDrawSingleInstance = [&](ID3D12PipelineState* pso, bool isColorPass)
                {
                    cmdList->SetPipelineState(pso);
                    cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                    for (auto& mesh : instance->pModel->meshes)
                    {
                        UINT srvIdx[4] = { resourceManager->GetDummyAlbedoIdx(), resourceManager->GetDummyNormalIdx(), resourceManager->GetDummyORMIdx(), resourceManager->GetDummyEmissiveIdx() };

                        for (auto& tex : mesh.textures)
                        {
                            switch (tex.type)
                            {
                            case TextureType::Albedo:
                                srvIdx[0] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                break;
                            case TextureType::Normal:
                                srvIdx[1] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                break;
                            case TextureType::ORM:
                                srvIdx[2] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                break;
                            case TextureType::Emissive:
                                srvIdx[3] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                break;
                            default:
                                break;
                            }
                        }

                        if (isColorPass)
                        {
                            UINT32 matIndices[5] = { srvIdx[0], srvIdx[1], srvIdx[2], srvIdx[3], (UINT32)mesh.isUnlit };
                            cmdList->SetGraphicsRoot32BitConstants(3, 5, matIndices, 0);
                        }

                        mesh.Draw(cmdList, 1, instance->currentLodLevel);
                    }
                };

            BindAndDrawSingleInstance(pipelineManager->GetTransparentPSO_DepthOnly(), false);
            BindAndDrawSingleInstance(pipelineManager->GetTransparentPSO_Color(), true);
        }
    }
};

#endif