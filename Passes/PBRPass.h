#ifndef PBR_PASS_H
#define PBR_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include "RDG.h"
#include <vector>

class PBRPass
{
private:
    enum class OpaqueDrawMode
    {
        ZPrepass,
        PBRConsumeDepth,
        PBRBuildDepth
    };

public:
    struct TransparentInput
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
    };

    struct ZPrepassOutput
    {
        RDGTextureHandle depth;
        RDGPassHandle pass;
    };

    static size_t ExecuteOpaque(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        bool useZPrepass)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = resourceManager->GetPostProcessRtvHandle();
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = deviceContext->GetDSVHandle();

        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = BindOpaqueState(
            cmdList,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect);

        // ====================================================================================================
        // Opaque Objects Rendering (Only if visible)
        // ====================================================================================================

        size_t transparentStartIndex = FindTransparentStartIndex(visibleInstances);

        if (!visibleInstances.empty())
        {
            if (useZPrepass)
            {
                cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                DrawOpaqueRange(cmdList, pipelineManager, baseGpuAddress, visibleInstances, 0, transparentStartIndex, OpaqueDrawMode::ZPrepass);

                cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                DrawOpaqueRange(cmdList, pipelineManager, baseGpuAddress, visibleInstances, 0, transparentStartIndex, OpaqueDrawMode::PBRConsumeDepth);
            }
            else
            {
                cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                DrawOpaqueRange(cmdList, pipelineManager, baseGpuAddress, visibleInstances, 0, transparentStartIndex, OpaqueDrawMode::PBRBuildDepth);
            }
        }
        else
        {
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }

        return transparentStartIndex;
    }

    static ZPrepassOutput AddZPrepassToGraph(
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
        RDGTextureHandle depth = graph.RegisterExternalTexture(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");

        RDGPassParameters params;
        params.WriteDSV(depth);

        RDGPassHandle pass = graph.AddPass(
            "ZPrepass",
            ERDGPassFlags::Graphics,
            params,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                transparentStartIndex = ExecuteZPrepassNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    deviceContext->GetDSVHandle());
            });

        return { depth, pass };
    }

    static void ExecuteTransparent(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = resourceManager->GetPostProcessRtvHandle();
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = deviceContext->GetDSVHandle();

        ExecuteTransparentNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            visibleInstances,
            transparentStartIndex,
            rtv,
            dsv);
    }

    static void ExecuteTransparentNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv)
    {
        // ====================================================================================================
        // Transparent Object Pass (Only if visible)
        // ====================================================================================================

        if (visibleInstances.empty() || transparentStartIndex >= visibleInstances.size())
        {
            return;
        }

        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);
        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);

        cmdList->SetGraphicsRootShaderResourceView(3, resourceManager->GetMaterialBufferGPUAddress());

        for (size_t i = transparentStartIndex; i < visibleInstances.size(); ++i)
        {
            ModelInstance* instance = visibleInstances[i];
            if (!instance->pModel)
            {
                continue;
            }

            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + (i * sizeof(InstanceData));

            cmdList->SetPipelineState(pipelineManager->GetTransparentPSO(instance->currentLodLevel));
            cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

            for (auto& mesh : instance->pModel->meshes)
            {
                cmdList->SetGraphicsRoot32BitConstants(4, 1, &mesh.materialID, 0);
                mesh.Draw(cmdList, 1, instance->currentLodLevel);
            }
        }
    }

    static RDGPassHandle AddTransparentToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t& transparentStartIndex,
        const TransparentInput& input = {})
    {
        RDGTextureHandle sceneColor = input.sceneColor;
        if (!sceneColor.IsValid())
        {
            sceneColor = graph.RegisterExternalTexture(
                resourceManager->GetPostProcessRT(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                "PostProcessRT");
        }
        graph.MarkTextureAsOutput(sceneColor);

        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        RDGPassParameters params;
        params.WriteRTV(sceneColor);
        params.WriteDSV(depth);

        return graph.AddPass(
            "Transparent",
            ERDGPassFlags::Graphics,
            params,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteTransparentNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    transparentStartIndex,
                    resourceManager->GetPostProcessRtvHandle(),
                    deviceContext->GetDSVHandle());
            });
    }

private:
    static size_t FindTransparentStartIndex(const std::vector<ModelInstance*>& visibleInstances)
    {
        for (size_t i = 0; i < visibleInstances.size(); ++i)
        {
            if (visibleInstances[i]->isTransparent)
            {
                return i;
            }
        }

        return visibleInstances.size();
    }

    static D3D12_GPU_VIRTUAL_ADDRESS BindOpaqueState(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect)
    {
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);
        cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);
        cmdList->SetGraphicsRootConstantBufferView(2, resourceManager->GetSHBufferGPUAddress());
        cmdList->SetGraphicsRootShaderResourceView(3, resourceManager->GetMaterialBufferGPUAddress());

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        return baseGpuAddress;
    }

    static ID3D12PipelineState* GetOpaquePSO(
        PipelineManager* pipelineManager,
        OpaqueDrawMode mode,
        bool isCutout,
        int lodLevel)
    {
        switch (mode)
        {
        case OpaqueDrawMode::ZPrepass:
            return isCutout ? pipelineManager->GetZPrepassCutout_PSO() : pipelineManager->GetZPrepass_PSO();
        case OpaqueDrawMode::PBRBuildDepth:
            return isCutout ? pipelineManager->GetPBRCutoutBuildDepth_PSO(lodLevel) : pipelineManager->GetPBRBuildDepth_PSO(lodLevel);
        case OpaqueDrawMode::PBRConsumeDepth:
        default:
            return isCutout ? pipelineManager->GetPBRCutout_PSO(lodLevel) : pipelineManager->GetPBR_PSO(lodLevel);
        }
    }

    static void DrawOpaqueRange(
        ID3D12GraphicsCommandList* cmdList,
        PipelineManager* pipelineManager,
        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t startIdx,
        size_t endIdx,
        OpaqueDrawMode mode)
    {
        if (startIdx >= endIdx)
        {
            return;
        }

        Model* currentModel = nullptr;
        int currentLod = -1;
        bool currentIsCutout = false;
        UINT instanceStartOffset = 0;
        UINT currentInstanceCount = 0;

        for (size_t i = startIdx; i <= endIdx; ++i)
        {
            bool isEnd = (i == endIdx);
            Model* thisModel = isEnd ? nullptr : visibleInstances[i]->pModel;
            int thisLod = isEnd ? -1 : visibleInstances[i]->currentLodLevel;
            bool thisIsCutout = isEnd ? false : visibleInstances[i]->isCutout;

            if ((isEnd || thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout) && currentInstanceCount > 0 && currentModel != nullptr)
            {
                D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + (instanceStartOffset * sizeof(InstanceData));
                cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                for (auto& mesh : currentModel->meshes)
                {
                    if (mode != OpaqueDrawMode::ZPrepass || currentIsCutout)
                    {
                        cmdList->SetGraphicsRoot32BitConstants(4, 1, &mesh.materialID, 0);
                    }

                    mesh.Draw(cmdList, currentInstanceCount, currentLod);
                }
            }

            if (!isEnd)
            {
                if (thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout)
                {
                    if (thisLod != currentLod || thisIsCutout != currentIsCutout)
                    {
                        cmdList->SetPipelineState(GetOpaquePSO(pipelineManager, mode, thisIsCutout, thisLod));
                    }

                    currentModel = thisModel;
                    currentLod = thisLod;
                    currentIsCutout = thisIsCutout;
                    instanceStartOffset = static_cast<UINT>(i);
                    currentInstanceCount = 1;
                }
                else
                {
                    currentInstanceCount++;
                }
            }
        }
    }

    static size_t ExecuteZPrepassNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv)
    {
        auto cmdList = deviceContext->GetCommandList();

        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = BindOpaqueState(
            cmdList,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect);

        size_t transparentStartIndex = FindTransparentStartIndex(visibleInstances);

        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        DrawOpaqueRange(cmdList, pipelineManager, baseGpuAddress, visibleInstances, 0, transparentStartIndex, OpaqueDrawMode::ZPrepass);

        return transparentStartIndex;
    }
};

#endif
