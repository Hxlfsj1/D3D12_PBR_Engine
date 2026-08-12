#ifndef TAA_PASS_H
#define TAA_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "MotionVectorPass.h"
#include "RDG.h"

#include <cmath>

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) TAAConstants
{
    DirectX::XMFLOAT4X4 currJitteredInvViewProj;
    DirectX::XMFLOAT4X4 prevUnjitteredViewProj;
    DirectX::XMFLOAT4 currentReconstructionWeights[3];

    float blendAlpha;
    UINT colorTextureIdx;
    UINT historyTextureIdx;
    UINT depthTextureIdx;

    UINT motionTextureIdx;
    DirectX::XMFLOAT2 currentJitterPixels;
    UINT pad;
};

class TAAPass
{
public:
    struct Input
    {
        RDGTextureHandle color;
        RDGTextureHandle depth;
        RDGTextureHandle motion;
    };

    struct Output
    {
        RDGTextureHandle historyTexture;
        int historyIndex = 0;
        RDGPassHandle pass;
    };

    struct TextureViews
    {
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv = {};
        UINT colorSrvIdx = UINT_MAX;
        UINT historySrvIdx = UINT_MAX;
        UINT depthSrvIdx = UINT_MAX;
        UINT motionSrvIdx = UINT_MAX;
    };

    static UINT Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX, float currJitterNdcY,
        int frameIndex, int width, int height,
        bool historyValid)
    {
        int temporalCurrentIdx = resourceManager->GetTemporalCurrentHistoryIdx();

        auto cmdList = deviceContext->GetCommandList();

        ID3D12Resource* currentHistoryTarget = resourceManager->GetTemporalHistoryRT(temporalCurrentIdx);
        ID3D12Resource* offscreenLitBuffer = resourceManager->GetPostProcessRT();

        CD3DX12_RESOURCE_BARRIER barriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(offscreenLitBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(currentHistoryTarget, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(3, barriers);

        ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            currJitteredInvViewProjGpu,
            prevUnjitteredViewProjGpu,
            currJitterNdcX,
            currJitterNdcY,
            frameIndex,
            width,
            height,
            historyValid,
            {
                resourceManager->GetTemporalRtvHandle(temporalCurrentIdx),
                resourceManager->GetPostProcessSrvIdx(),
                resourceManager->GetTemporalHistorySrvIdx(1 - temporalCurrentIdx),
                resourceManager->GetDepthBufferSrvIdx(),
                UINT_MAX
            });

        CD3DX12_RESOURCE_BARRIER revertBarriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(offscreenLitBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(currentHistoryTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
        };
        cmdList->ResourceBarrier(3, revertBarriers);

        resourceManager->FlipTemporalHistoryIndex();

        return resourceManager->GetTemporalHistorySrvIdx(temporalCurrentIdx);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX, float currJitterNdcY,
        int frameIndex,
        int width, int height,
        bool historyValid,
        const TextureViews& views)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &views.outputRtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetTAARootSignature());
        cmdList->SetPipelineState(pipelineManager->GetTAAPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const UINT64 taaConstantsOffset = 1024 * 1024 * 12;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + taaConstantsOffset;

        TAAConstants cb = {};
        cb.currJitteredInvViewProj = currJitteredInvViewProjGpu;
        cb.prevUnjitteredViewProj = prevUnjitteredViewProjGpu;
        cb.blendAlpha = historyValid ? 0.95f : 0.0f;

        const float jitterPixelX =
            currJitterNdcX * 0.5f * static_cast<float>(width);
        const float jitterPixelY =
            -currJitterNdcY * 0.5f * static_cast<float>(height);

        cb.currentJitterPixels = DirectX::XMFLOAT2(jitterPixelX, jitterPixelY);

        float reconstructionWeights[3][3] = {};
        float reconstructionWeightSum = 0.0f;
        for (int x = -1; x <= 1; ++x)
        {
            for (int y = -1; y <= 1; ++y)
            {
                const float offsetX = static_cast<float>(x) - jitterPixelX;
                const float offsetY = static_cast<float>(y) - jitterPixelY;
                const float weight = std::exp(
                    -2.29f * (offsetX * offsetX + offsetY * offsetY));

                reconstructionWeights[x + 1][y + 1] = weight;
                reconstructionWeightSum += weight;
            }
        }

        const float safeReconstructionWeightSum =
            reconstructionWeightSum > 1.0e-6f ? reconstructionWeightSum : 1.0e-6f;
        const float inverseReconstructionWeightSum =
            1.0f / safeReconstructionWeightSum;
        for (int x = 0; x < 3; ++x)
        {
            cb.currentReconstructionWeights[x] = DirectX::XMFLOAT4(
                reconstructionWeights[x][0] * inverseReconstructionWeightSum,
                reconstructionWeights[x][1] * inverseReconstructionWeightSum,
                reconstructionWeights[x][2] * inverseReconstructionWeightSum,
                0.0f);
        }

        cb.colorTextureIdx = views.colorSrvIdx;
        cb.historyTextureIdx = views.historySrvIdx;
        cb.depthTextureIdx = views.depthSrvIdx;
        cb.motionTextureIdx = views.motionSrvIdx;

        memcpy(cbvCpuAddress, &cb, sizeof(TAAConstants));
        cmdList->SetGraphicsRootConstantBufferView(0, resourceManager->GetCBVGPUAddress(frameIndex) + taaConstantsOffset);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static UINT ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& currUnjitteredViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX, float currJitterNdcY,
        int frameIndex, int width, int height,
        bool historyValid)
    {
        int temporalCurrentIdx = resourceManager->GetTemporalCurrentHistoryIdx();

        RDGBuilder graph(deviceContext, "TAAGraph");
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
        graph.SetTransientSrvUavDescriptorAllocator(
            [resourceManager](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return resourceManager->AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

        RDGTextureHandle offscreenLitBuffer = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");

        RDGTextureHandle currentHistoryTarget = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(temporalCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAACurrentHistory");
        graph.MarkTextureAsOutput(currentHistoryTarget);

        RDGTextureHandle previousHistory = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(1 - temporalCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAAPreviousHistory");

        RDGTextureHandle depth = graph.RegisterExternalTexture(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        MotionVectorPass::Output motionOutput = MotionVectorPass::AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            currJitteredInvViewProjGpu,
            currUnjitteredViewProjGpu,
            prevUnjitteredViewProjGpu,
            width,
            height,
            frameIndex,
            { depth });

        RDGTextureSRVHandle colorSrv = graph.CreateTextureSRVView(offscreenLitBuffer);
        RDGTextureSRVHandle historySrv = graph.CreateTextureSRVView(previousHistory);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
        RDGTextureSRVHandle motionSrv = {};
        if (motionOutput.motionTexture.IsValid())
        {
            motionSrv = graph.CreateTextureSRVView(motionOutput.motionTexture);
        }
        RDGTextureRTVHandle historyRtv = graph.CreateTextureRTVView(currentHistoryTarget);

        if (!colorSrv.IsValid() ||
            !historySrv.IsValid() ||
            !depthSrv.IsValid() ||
            !historyRtv.IsValid())
        {
            return Execute(
                deviceContext,
                resourceManager,
                pipelineManager,
                currJitteredInvViewProjGpu,
                prevUnjitteredViewProjGpu,
                currJitterNdcX,
                currJitterNdcY,
                frameIndex,
                width,
                height,
                historyValid);
        }

        TextureViews views =
        {
            historyRtv.cpuHandle,
            colorSrv.descriptorIndex,
            historySrv.descriptorIndex,
            depthSrv.descriptorIndex,
            motionSrv.IsValid() ? motionSrv.descriptorIndex : UINT_MAX
        };

        RDGPassParameters params;
        params.ReadSRV(colorSrv);
        params.ReadSRV(historySrv);
        params.ReadSRV(depthSrv);
        if (motionSrv.IsValid())
        {
            params.ReadSRV(motionSrv);
        }
        params.WriteRTV(historyRtv);

        graph.AddPass(
            "TAA",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    currJitteredInvViewProjGpu,
                    prevUnjitteredViewProjGpu,
                    currJitterNdcX,
                    currJitterNdcY,
                    frameIndex,
                    width,
                    height,
                    historyValid,
                    views);
            });

        graph.Execute(deviceContext->GetCommandList());

        resourceManager->FlipTemporalHistoryIndex();

        return resourceManager->GetTemporalHistorySrvIdx(temporalCurrentIdx);
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX, float currJitterNdcY,
        int frameIndex,
        int width, int height,
        bool historyValid,
        const Input& input = {})
    {
        int temporalCurrentIdx = resourceManager->GetTemporalCurrentHistoryIdx();

        RDGTextureHandle color = input.color;
        if (!color.IsValid())
        {
            color = graph.RegisterExternalTexture(
                resourceManager->GetPostProcessRT(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                "PostProcessRT");
        }

        RDGTextureHandle currentHistoryTarget = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(temporalCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAACurrentHistory");
        graph.MarkTextureAsOutput(currentHistoryTarget);

        RDGTextureHandle previousHistory = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(1 - temporalCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAAPreviousHistory");

        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle colorSrv = graph.CreateTextureSRVView(color);
        RDGTextureSRVHandle historySrv = graph.CreateTextureSRVView(previousHistory);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
        RDGTextureSRVHandle motionSrv = {};
        if (input.motion.IsValid())
        {
            motionSrv = graph.CreateTextureSRVView(input.motion);
        }
        RDGTextureRTVHandle historyRtv = graph.CreateTextureRTVView(currentHistoryTarget);

        if (!colorSrv.IsValid() ||
            !historySrv.IsValid() ||
            !depthSrv.IsValid() ||
            !historyRtv.IsValid())
        {
            return {};
        }

        TextureViews views =
        {
            historyRtv.cpuHandle,
            colorSrv.descriptorIndex,
            historySrv.descriptorIndex,
            depthSrv.descriptorIndex,
            motionSrv.IsValid() ? motionSrv.descriptorIndex : UINT_MAX
        };

        RDGPassParameters params;
        params.ReadSRV(colorSrv);
        params.ReadSRV(historySrv);
        params.ReadSRV(depthSrv);
        if (motionSrv.IsValid())
        {
            params.ReadSRV(motionSrv);
        }
        params.WriteRTV(historyRtv);

        RDGPassHandle pass = graph.AddPass(
            "TAA",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    currJitteredInvViewProjGpu,
                    prevUnjitteredViewProjGpu,
                    currJitterNdcX,
                    currJitterNdcY,
                    frameIndex,
                    width,
                    height,
                    historyValid,
                    views);
            });

        return { currentHistoryTarget, temporalCurrentIdx, pass };
    }
};

#endif
