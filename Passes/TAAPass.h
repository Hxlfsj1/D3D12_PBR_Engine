#ifndef TAA_PASS_H
#define TAA_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "RDG.h"

class TAAPass
{
public:
    struct Input
    {
        RDGTextureHandle color;
        RDGTextureHandle depth;
    };

    struct Output
    {
        RDGTextureHandle historyTexture;
        UINT historySrvIdx = 0;
        int historyIndex = 0;
        RDGPassHandle pass;
    };

    static UINT Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currentInvViewProj,
        const DirectX::XMFLOAT4X4& prevViewProj,
        float jitterX, float jitterY,
        int frameIndex, int width, int height,
        bool historyValid)
    {
        int taaCurrentIdx = resourceManager->GetTAACurrentHistoryIdx();

        auto cmdList = deviceContext->GetCommandList();

        ID3D12Resource* currentHistoryTarget = resourceManager->GetTAAHistoryRT(taaCurrentIdx);
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
            currentInvViewProj,
            prevViewProj,
            jitterX,
            jitterY,
            frameIndex,
            width,
            height,
            historyValid,
            taaCurrentIdx);

        CD3DX12_RESOURCE_BARRIER revertBarriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(offscreenLitBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(currentHistoryTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
        };
        cmdList->ResourceBarrier(3, revertBarriers);

        resourceManager->FlipTAAHistoryIndex();

        return resourceManager->GetTAAHistorySrvIdx(taaCurrentIdx);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currentInvViewProj,
        const DirectX::XMFLOAT4X4& prevViewProj,
        float jitterX, float jitterY,
        int frameIndex, int width, int height,
        bool historyValid,
        int taaCurrentIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle = resourceManager->GetTAARtvHandle(taaCurrentIdx);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

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
        cb.invViewProj = currentInvViewProj;
        cb.prevViewProj = prevViewProj;
        cb.jitterOffset = DirectX::XMFLOAT2(jitterX, jitterY);
        cb.blendAlpha = historyValid ? 0.95f : 0.0f;
        cb.varianceScale = 1.5f;

        cb.colorTextureIdx = resourceManager->GetPostProcessSrvIdx();
        cb.historyTextureIdx = resourceManager->GetTAAHistorySrvIdx(1 - taaCurrentIdx);
        cb.depthTextureIdx = resourceManager->GetDepthBufferSrvIdx();

        memcpy(cbvCpuAddress, &cb, sizeof(TAAConstants));
        cmdList->SetGraphicsRootConstantBufferView(0, resourceManager->GetCBVGPUAddress(frameIndex) + taaConstantsOffset);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static UINT ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currentInvViewProj,
        const DirectX::XMFLOAT4X4& prevViewProj,
        float jitterX, float jitterY,
        int frameIndex, int width, int height,
        bool historyValid)
    {
        int taaCurrentIdx = resourceManager->GetTAACurrentHistoryIdx();

        RDGBuilder graph(deviceContext, "TAAGraph");

        RDGTextureHandle offscreenLitBuffer = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");

        RDGTextureHandle currentHistoryTarget = graph.RegisterExternalTexture(
            resourceManager->GetTAAHistoryRT(taaCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAACurrentHistory");

        RDGTextureHandle previousHistory = graph.RegisterExternalTexture(
            resourceManager->GetTAAHistoryRT(1 - taaCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAAPreviousHistory");

        RDGTextureHandle depth = graph.RegisterExternalTexture(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");

        RDGPassParameters params;
        params.ReadSRV(offscreenLitBuffer);
        params.ReadSRV(previousHistory);
        params.ReadSRV(depth);
        params.WriteRTV(currentHistoryTarget);

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
                    currentInvViewProj,
                    prevViewProj,
                    jitterX,
                    jitterY,
                    frameIndex,
                    width,
                    height,
                    historyValid,
                    taaCurrentIdx);
            });

        graph.Execute(deviceContext->GetCommandList());

        resourceManager->FlipTAAHistoryIndex();

        return resourceManager->GetTAAHistorySrvIdx(taaCurrentIdx);
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currentInvViewProj,
        const DirectX::XMFLOAT4X4& prevViewProj,
        float jitterX, float jitterY,
        int frameIndex, int width, int height,
        bool historyValid,
        const Input& input = {})
    {
        int taaCurrentIdx = resourceManager->GetTAACurrentHistoryIdx();

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
            resourceManager->GetTAAHistoryRT(taaCurrentIdx),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TAACurrentHistory");
        graph.MarkTextureAsOutput(currentHistoryTarget);

        RDGTextureHandle previousHistory = graph.RegisterExternalTexture(
            resourceManager->GetTAAHistoryRT(1 - taaCurrentIdx),
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

        RDGPassParameters params;
        params.ReadSRV(color);
        params.ReadSRV(previousHistory);
        params.ReadSRV(depth);
        params.WriteRTV(currentHistoryTarget);

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
                    currentInvViewProj,
                    prevViewProj,
                    jitterX,
                    jitterY,
                    frameIndex,
                    width,
                    height,
                    historyValid,
                    taaCurrentIdx);
            });

        return { currentHistoryTarget, resourceManager->GetTAAHistorySrvIdx(taaCurrentIdx), taaCurrentIdx, pass };
    }
};

#endif
