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
            deviceContext->GetRTVHandle(frameIndex),
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
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        UINT inputSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);
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
        const D3D12_RECT& scissorRect)
    {
        RDGBuilder graph(deviceContext, "PostProcessGraph");
        graph.SetTransientSrvUavDescriptorAllocator(
            [resourceManager](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return resourceManager->AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

        RDGTextureHandle postProcessRT = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");

        RDGTextureHandle backBuffer = graph.RegisterExternalTextureOutput(
            deviceContext->GetRenderTarget(frameIndex),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "BackBuffer");

        RDGTextureSRVHandle inputSrv = graph.CreateTextureSRVView(postProcessRT);
        RDGTextureRTVHandle backBufferRtv = graph.CreateTextureRTVView(backBuffer);
        if (!inputSrv.IsValid() || !backBufferRtv.IsValid())
        {
            Execute(
                deviceContext,
                resourceManager,
                pipelineManager,
                frameIndex,
                viewport,
                scissorRect,
                resourceManager->GetPostProcessSrvIdx());
            return;
        }

        RDGPassParameters postParams;
        postParams.ReadSRV(inputSrv);
        postParams.WriteRTV(backBufferRtv);

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
                    backBufferRtv.cpuHandle,
                    inputSrv.descriptorIndex);
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

        RDGTextureSRVHandle inputSrv = graph.CreateTextureSRVView(inputTexture);
        RDGTextureRTVHandle backBufferRtv = graph.CreateTextureRTVView(backBuffer);
        if (!inputSrv.IsValid() || !backBufferRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters postParams;
        postParams.ReadSRV(inputSrv);
        postParams.WriteRTV(backBufferRtv);

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
                    backBufferRtv.cpuHandle,
                    inputSrv.descriptorIndex);
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
            inputTexture,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT);
    }
};

#endif
