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
    struct OpaqueInput
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
        RDGTextureSRVHandle shadowMap;
    };

    struct OpaqueOutput
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
        RDGPassHandle pass;
        size_t transparentStartIndex = 0;
    };

    struct TransparentInput
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
        RDGTextureSRVHandle shadowMap;
    };

    struct ZPrepassOutput
    {
        RDGTextureHandle depth;
        RDGPassHandle pass;
    };

    static OpaqueOutput AddOpaqueToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        bool consumeZPrepassDepth,
        const OpaqueInput& input = {})
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
            depth = GetOrCreateSceneDepth(graph, deviceContext, viewport);
        }

        RDGTextureSRVHandle shadowMapSrv = input.shadowMap;
        if (!shadowMapSrv.IsValid())
        {
            RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
                resourceManager->GetShadowMap(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "ShadowMap");

            D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
            shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            shadowSrvDesc.Texture2DArray.MipLevels = 1;
            shadowSrvDesc.Texture2DArray.ArraySize = NUM_CASCADES;
            shadowMapSrv = graph.CreateTextureSRVView(shadowMap, &shadowSrvDesc);
        }

        RDGTextureRTVHandle sceneColorRtv = graph.CreateTextureRTVView(sceneColor);
        RDGTextureDSVHandle depthDsv = graph.CreateTextureDSVView(depth);
        if (!sceneColorRtv.IsValid() ||
            !depthDsv.IsValid() ||
            !shadowMapSrv.IsValid())
        {
            return {};
        }

        const size_t transparentStartIndex = FindTransparentStartIndex(visibleInstances);

        RDGPassParameters params;
        params.ReadSRV(shadowMapSrv);
        params.WriteRTV(sceneColorRtv);
        // Keep DEPTH_WRITE here to match the existing DSV and PSO state. The consume-depth
        // PSO disables depth writes; a future read-only DSV can narrow this to ReadDSV.
        params.WriteDSV(depthDsv);

        RDGPassHandle pass = graph.AddPass(
            "ForwardOpaque",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                if (!consumeZPrepassDepth)
                {
                    cmdList->ClearDepthStencilView(
                        depthDsv.cpuHandle,
                        D3D12_CLEAR_FLAG_DEPTH,
                        1.0f,
                        0,
                        0,
                        nullptr);
                }

                PassConstants* passConstants = reinterpret_cast<PassConstants*>(
                    resourceManager->GetCBVAddress(frameIndex));
                passConstants->shadowMapIdx = shadowMapSrv.descriptorIndex;

                D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = BindOpaqueState(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect);

                ExecuteOpaqueDrawNoBarrier(
                    cmdList,
                    pipelineManager,
                    baseGpuAddress,
                    visibleInstances,
                    transparentStartIndex,
                    consumeZPrepassDepth,
                    sceneColorRtv.cpuHandle,
                    depthDsv.cpuHandle);
            });

        return { sceneColor, depth, pass, transparentStartIndex };
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
        RDGTextureHandle depth = GetOrCreateSceneDepth(graph, deviceContext, viewport);

        RDGTextureDSVHandle depthDsv = graph.CreateTextureDSVView(depth);
        if (!depthDsv.IsValid())
        {
            return {};
        }

        RDGPassParameters params;
        params.WriteDSV(depthDsv);

        RDGPassHandle pass = graph.AddPass(
            "ZPrepass",
            ERDGPassFlags::Graphics,
            params,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                cmdList->ClearDepthStencilView(
                    depthDsv.cpuHandle,
                    D3D12_CLEAR_FLAG_DEPTH,
                    1.0f,
                    0,
                    0,
                    nullptr);

                transparentStartIndex = ExecuteZPrepassNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    depthDsv.cpuHandle);
            });

        return { depth, pass };
    }

    static void ExecuteTransparentDrawNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex,
        size_t transparentEndIndex,
        UINT sceneColorCopySrvIdx,
        UINT sceneDepthSrvIdx,
        UINT shadowMapSrvIdx,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        bool enableRefraction)
    {
        // ====================================================================================================
        // Transparent Object Pass (Only if visible)
        // ====================================================================================================

        transparentEndIndex = std::min(transparentEndIndex, visibleInstances.size());
        if (visibleInstances.empty() || transparentStartIndex >= transparentEndIndex)
        {
            return;
        }

        PassConstants* passConstants = reinterpret_cast<PassConstants*>(
            resourceManager->GetCBVAddress(frameIndex));
        passConstants->shadowMapIdx = shadowMapSrvIdx;

        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
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

        UINT refractionEnabled = enableRefraction ? 1u : 0u;

        for (size_t i = transparentStartIndex; i < transparentEndIndex; ++i)
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
                UINT transparentConstants[] =
                {
                    mesh.materialID,
                    sceneColorCopySrvIdx,
                    sceneDepthSrvIdx,
                    refractionEnabled
                };

                cmdList->SetGraphicsRoot32BitConstants(4, _countof(transparentConstants), transparentConstants, 0);
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

        RDGTextureHandle sceneColorCopy = graph.RegisterExternalTexture(
            resourceManager->GetTransparentSceneColorCopy(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TransparentSceneColorCopy");

        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        RDGTextureSRVHandle shadowMapSrv = input.shadowMap;
        if (!shadowMapSrv.IsValid())
        {
            RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
                resourceManager->GetShadowMap(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "ShadowMap");

            D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
            shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            shadowSrvDesc.Texture2DArray.MipLevels = 1;
            shadowSrvDesc.Texture2DArray.ArraySize = NUM_CASCADES;
            shadowMapSrv = graph.CreateTextureSRVView(shadowMap, &shadowSrvDesc);
        }

        ID3D12Resource* sceneColorResource = graph.GetTextureResource(sceneColor);
        ID3D12Resource* sceneColorCopyResource = graph.GetTextureResource(sceneColorCopy);

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle sceneColorCopySrv = graph.CreateTextureSRVView(sceneColorCopy);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
        RDGTextureRTVHandle sceneColorRtv = graph.CreateTextureRTVView(sceneColor);
        if (!sceneColorCopySrv.IsValid() ||
            !depthSrv.IsValid() ||
            !shadowMapSrv.IsValid() ||
            !sceneColorRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters regularDrawParams;
        regularDrawParams.ReadSRV(depthSrv);
        regularDrawParams.ReadSRV(shadowMapSrv);
        regularDrawParams.WriteRTV(sceneColorRtv);

        graph.AddPass(
            "TransparentRegular",
            ERDGPassFlags::Graphics,
            regularDrawParams,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                size_t nearestTransparentIndex = FindNearestDrawableTransparentIndex(visibleInstances, transparentStartIndex);
                ExecuteTransparentDrawNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    transparentStartIndex,
                    nearestTransparentIndex,
                    sceneColorCopySrv.descriptorIndex,
                    depthSrv.descriptorIndex,
                    shadowMapSrv.descriptorIndex,
                    sceneColorRtv.cpuHandle,
                    false);
            });

        RDGPassParameters copyParams;
        copyParams.ReadCopySrc(sceneColor);
        copyParams.WriteCopyDst(sceneColorCopy);

        graph.AddPass(
            "TransparentSceneColorCopy",
            ERDGPassFlags::Copy,
            copyParams,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                if (FindNearestDrawableTransparentIndex(visibleInstances, transparentStartIndex) >= visibleInstances.size())
                {
                    return;
                }

                cmdList->CopyResource(sceneColorCopyResource, sceneColorResource);
            });

        RDGPassParameters refractionDrawParams;
        refractionDrawParams.ReadSRV(sceneColorCopySrv);
        refractionDrawParams.ReadSRV(depthSrv);
        refractionDrawParams.ReadSRV(shadowMapSrv);
        refractionDrawParams.WriteRTV(sceneColorRtv);

        return graph.AddPass(
            "TransparentRefraction",
            ERDGPassFlags::Graphics,
            refractionDrawParams,
            [=, &transparentStartIndex](ID3D12GraphicsCommandList* cmdList)
            {
                size_t nearestTransparentIndex = FindNearestDrawableTransparentIndex(visibleInstances, transparentStartIndex);
                ExecuteTransparentDrawNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    visibleInstances,
                    nearestTransparentIndex,
                    nearestTransparentIndex + 1,
                    sceneColorCopySrv.descriptorIndex,
                    depthSrv.descriptorIndex,
                    shadowMapSrv.descriptorIndex,
                    sceneColorRtv.cpuHandle,
                    true);
            });
    }

private:
    static RDGTextureHandle GetOrCreateSceneDepth(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        const D3D12_VIEWPORT& viewport)
    {
        if (deviceContext == nullptr || deviceContext->GetDepthStencilBuffer() == nullptr)
        {
            return {};
        }

        ID3D12Resource* deviceDepth = deviceContext->GetDepthStencilBuffer();
        const D3D12_RESOURCE_DESC deviceDepthDesc = deviceDepth->GetDesc();
        const uint32_t sceneWidth = static_cast<uint32_t>(viewport.Width);
        const uint32_t sceneHeight = static_cast<uint32_t>(viewport.Height);

        if (sceneWidth == 0 || sceneHeight == 0)
        {
            return {};
        }

        if (deviceDepthDesc.Width == sceneWidth && deviceDepthDesc.Height == sceneHeight)
        {
            return graph.RegisterExternalTexture(
                deviceDepth,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        RDGTextureDesc depthTextureDesc = {};
        depthTextureDesc.width = sceneWidth;
        depthTextureDesc.height = sceneHeight;
        depthTextureDesc.format = DXGI_FORMAT_R32_TYPELESS;
        depthTextureDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depthTextureDesc.hasClearValue = true;
        depthTextureDesc.clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthTextureDesc.clearValue.DepthStencil.Depth = 1.0f;
        depthTextureDesc.clearValue.DepthStencil.Stencil = 0;

        return graph.CreateTexture(
            depthTextureDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");
    }

    static void ExecuteOpaqueDrawNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        PipelineManager* pipelineManager,
        D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress,
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex,
        bool consumeZPrepassDepth,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv)
    {
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        DrawOpaqueRange(
            cmdList,
            pipelineManager,
            baseGpuAddress,
            visibleInstances,
            0,
            transparentStartIndex,
            consumeZPrepassDepth ? OpaqueDrawMode::PBRConsumeDepth : OpaqueDrawMode::PBRBuildDepth);
    }

    static size_t FindNearestDrawableTransparentIndex(
        const std::vector<ModelInstance*>& visibleInstances,
        size_t transparentStartIndex)
    {
        if (transparentStartIndex >= visibleInstances.size())
        {
            return visibleInstances.size();
        }

        for (size_t i = visibleInstances.size(); i > transparentStartIndex; --i)
        {
            size_t instanceIndex = i - 1;
            ModelInstance* instance = visibleInstances[instanceIndex];
            if (instance != nullptr && instance->pModel != nullptr)
            {
                return instanceIndex;
            }
        }

        return visibleInstances.size();
    }

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
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        const std::vector<ModelInstance*>& visibleInstances,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv)
    {
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
