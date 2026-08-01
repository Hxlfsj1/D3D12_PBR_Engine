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
        UINT inputSrvIdx,
        bool visualizeScalar = false)
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
            inputSrvIdx,
            visualizeScalar);

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
        UINT inputSrvIdx,
        bool visualizeScalar)
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

        UINT postProcessConstants[2] =
        {
            inputSrvIdx,
            visualizeScalar ? 1u : 0u
        };
        cmdList->SetGraphicsRoot32BitConstants(0, 2, postProcessConstants, 0);

        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        bool visualizeScalar = false)
    {
        RDGBuilder graph(deviceContext, "PostProcessGraph");
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
                resourceManager->GetPostProcessSrvIdx(),
                visualizeScalar);
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
                    inputSrv.descriptorIndex,
                    visualizeScalar);
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
        bool visualizeScalar = false,
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
                    inputSrv.descriptorIndex,
                    visualizeScalar);
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
        RDGTextureHandle inputTexture = {},
        bool visualizeScalar = false)
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
            visualizeScalar,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT);
    }
};

#endif
