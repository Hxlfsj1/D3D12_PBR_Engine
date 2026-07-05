#ifndef POST_PROCESS_PASS_H
#define POST_PROCESS_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RDG.h"

class PostProcessPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        UINT inputSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmdList->ResourceBarrier(1, &toSrv);

        ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            inputSrvIdx);

        CD3DX12_RESOURCE_BARRIER toRtv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->ResourceBarrier(1, &toRtv);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        UINT inputSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = deviceContext->GetRTVHandle(frameIndex);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetPostProcessRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetPostProcessPSO());

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        UINT sceneTexIdx = inputSrvIdx;
        cmdList->SetGraphicsRoot32BitConstants(0, 1, &sceneTexIdx, 0);

        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        UINT inputSrvIdx)
    {
        RDGBuilder graph(deviceContext, "PostProcessGraph");

        RDGTextureHandle postProcessRT = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");

        RDGTextureHandle backBuffer = graph.RegisterExternalTexture(
            deviceContext->GetRenderTarget(frameIndex),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "BackBuffer");

        RDGPassParameters postParams;
        postParams.ReadSRV(postProcessRT);
        postParams.WriteRTV(backBuffer);

        graph.AddPass(
            "PostProcess",
            ERDGPassFlags::Graphics,
            postParams,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    inputSrvIdx);
            });

        graph.Execute(deviceContext->GetCommandList());
    }

    static RDGPassHandle AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        UINT inputSrvIdx,
        RDGTextureHandle inputTexture = {},
        D3D12_RESOURCE_STATES backBufferInitialState = D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATES backBufferFinalState = D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        if (!inputTexture.IsValid())
        {
            inputTexture = graph.RegisterExternalTexture(
                resourceManager->GetPostProcessRT(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                "PostProcessRT");
        }

        RDGTextureHandle backBuffer = graph.RegisterExternalTextureOutput(
            deviceContext->GetRenderTarget(frameIndex),
            backBufferInitialState,
            backBufferFinalState,
            "BackBuffer");

        RDGPassParameters postParams;
        postParams.ReadSRV(inputTexture);
        postParams.WriteRTV(backBuffer);

        return graph.AddPass(
            "PostProcess",
            ERDGPassFlags::Graphics,
            postParams,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    inputSrvIdx);
            });
    }

    static RDGPassHandle AddFinalToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        UINT inputSrvIdx,
        RDGTextureHandle inputTexture = {})
    {
        return AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            inputSrvIdx,
            inputTexture,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT);
    }
};

#endif
