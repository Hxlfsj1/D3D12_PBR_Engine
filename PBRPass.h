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

        cmdList->SetGraphicsRootConstantBufferView(8, resourceManager->GetSHBufferGPUAddress());

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        CD3DX12_GPU_DESCRIPTOR_HANDLE hStart(resourceManager->GetMainDescriptorHeap()->GetGPUDescriptorHandleForHeapStart());
        UINT srvDescSize = resourceManager->GetSrvDescriptorSize();

        cmdList->SetGraphicsRootDescriptorTable(5, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, resourceManager->GetIblPrefilterIdx(), srvDescSize));
        cmdList->SetGraphicsRootDescriptorTable(6, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, resourceManager->GetIblBRDFIdx(), srvDescSize));

        cmdList->SetGraphicsRootDescriptorTable(10, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, resourceManager->GetShadowSrvIdx(), srvDescSize));

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

            auto DrawQueue = [=](size_t startIdx, size_t endIdx, ID3D12PipelineState* pso)
                {
                    if (startIdx >= endIdx) return;

                    cmdList->SetPipelineState(pso);

                    Model* currentModel = nullptr;
                    UINT instanceStartOffset = 0;
                    UINT currentInstanceCount = 0;

                    for (size_t i = startIdx; i <= endIdx; ++i)
                    {
                        bool isEnd = (i == endIdx);
                        Model* thisModel = isEnd ? nullptr : visibleInstances[i]->pModel;

                        if ((isEnd || thisModel != currentModel) && currentInstanceCount > 0 && currentModel != nullptr)
                        {
                            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (instanceStartOffset * sizeof(InstanceData));
                            cmdList->SetGraphicsRootShaderResourceView(9, srvAddress);

                            for (auto& mesh : currentModel->meshes)
                            {
                                UINT srvIdx[4] = { resourceManager->GetDummyAlbedoIdx(), resourceManager->GetDummyNormalIdx(), resourceManager->GetDummyORMIdx(), resourceManager->GetDummyEmissiveIdx() };
                                bool hasMap[4] = { false, false, false, false };

                                for (auto& tex : mesh.textures)
                                {
                                    switch (tex.type)
                                    {
                                    case TextureType::Albedo:
                                        srvIdx[0] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        hasMap[0] = true;
                                        break;
                                    case TextureType::Normal:
                                        srvIdx[1] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        hasMap[1] = true;
                                        break;
                                    case TextureType::ORM:
                                        srvIdx[2] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        hasMap[2] = true;
                                        break;
                                    case TextureType::Emissive:
                                        srvIdx[3] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                        hasMap[3] = true;
                                        break;
                                    default:
                                        break;
                                    }
                                }

                                UINT32 flags[4] = { (UINT32)hasMap[0], (UINT32)hasMap[1], (UINT32)hasMap[2], (UINT32)hasMap[3] };
                                cmdList->SetGraphicsRoot32BitConstants(7, 4, flags, 0);

                                cmdList->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[0], srvDescSize));
                                cmdList->SetGraphicsRootDescriptorTable(2, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[1], srvDescSize));
                                cmdList->SetGraphicsRootDescriptorTable(3, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[2], srvDescSize));
                                cmdList->SetGraphicsRootDescriptorTable(4, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[3], srvDescSize));

                                mesh.Draw(cmdList, currentInstanceCount);
                            }
                        }

                        if (!isEnd)
                        {
                            if (thisModel != currentModel)
                            {
                                currentModel = thisModel;
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
            DrawQueue(0, transparentStartIndex, pipelineManager->GetZPrepass_PSO());

            cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            DrawQueue(0, transparentStartIndex, pipelineManager->GetPBR_PSO());
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
        CD3DX12_GPU_DESCRIPTOR_HANDLE hStart(resourceManager->GetMainDescriptorHeap()->GetGPUDescriptorHandleForHeapStart());
        UINT srvDescSize = resourceManager->GetSrvDescriptorSize();

        for (size_t i = transparentStartIndex; i < visibleInstances.size(); ++i)
        {
            ModelInstance* instance = visibleInstances[i];
            if (!instance->pModel) continue;

            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (i * sizeof(InstanceData));

            auto BindAndDrawSingleInstance = [&](ID3D12PipelineState* pso)
                {
                    cmdList->SetPipelineState(pso);
                    cmdList->SetGraphicsRootShaderResourceView(9, srvAddress);

                    for (auto& mesh : instance->pModel->meshes)
                    {
                        UINT srvIdx[4] = { resourceManager->GetDummyAlbedoIdx(), resourceManager->GetDummyNormalIdx(), resourceManager->GetDummyORMIdx(), resourceManager->GetDummyEmissiveIdx() };
                        bool hasMap[4] = { false, false, false, false };

                        for (auto& tex : mesh.textures)
                        {
                            switch (tex.type)
                            {
                            case TextureType::Albedo:
                                srvIdx[0] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                hasMap[0] = true;
                                break;
                            case TextureType::Normal:
                                srvIdx[1] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                hasMap[1] = true;
                                break;
                            case TextureType::ORM:
                                srvIdx[2] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                hasMap[2] = true;
                                break;
                            case TextureType::Emissive:
                                srvIdx[3] = resourceManager->GetTextureSrvIdx(tex.Resource.Get());
                                hasMap[3] = true;
                                break;
                            default:
                                break;
                            }
                        }

                        UINT32 flags[4] = { (UINT32)hasMap[0], (UINT32)hasMap[1], (UINT32)hasMap[2], (UINT32)hasMap[3] };
                        cmdList->SetGraphicsRoot32BitConstants(7, 4, flags, 0);

                        cmdList->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[0], srvDescSize));
                        cmdList->SetGraphicsRootDescriptorTable(2, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[1], srvDescSize));
                        cmdList->SetGraphicsRootDescriptorTable(3, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[2], srvDescSize));
                        cmdList->SetGraphicsRootDescriptorTable(4, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[3], srvDescSize));

                        mesh.Draw(cmdList, 1);
                    }
                };

            BindAndDrawSingleInstance(pipelineManager->GetTransparentPSO_DepthOnly());
            BindAndDrawSingleInstance(pipelineManager->GetTransparentPSO_Color());
        }
    }
};

#endif