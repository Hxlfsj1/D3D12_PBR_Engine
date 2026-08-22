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
    struct TextureOutput
    {
        RDGTextureHandle texture;
        RDGPassHandle pass;
    };

    static void ExecuteNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        UINT inputSrvIdx,
        bool visualizeScalar,
        bool enableSharpen)
    {
        cmdList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetPostProcessRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetPostProcessPSO(enableSharpen));

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

    static TextureOutput AddToTextureGraph(
        RDGBuilder& graph,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        int width,
        int height,
        RDGTextureHandle inputTexture,
        bool visualizeScalar = false,
        bool enableSharpen = false)
    {
        if (resourceManager == nullptr ||
            pipelineManager == nullptr ||
            width <= 0 ||
            height <= 0 ||
            !inputTexture.IsValid())
        {
            return {};
        }

        RDGTextureDesc outputDesc = {};
        outputDesc.width = static_cast<uint32_t>(width);
        outputDesc.height = static_cast<uint32_t>(height);
        outputDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        outputDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        outputDesc.hasClearValue = true;
        outputDesc.clearValue.Format = outputDesc.format;

        RDGTextureHandle outputTexture = graph.CreateTexture(
            outputDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "PostProcess.ToneMappedColor");
        if (!outputTexture.IsValid())
        {
            return {};
        }

        RDGTextureSRVHandle inputSrv = graph.CreateTextureSRVView(inputTexture);
        RDGTextureRTVHandle outputRtv = graph.CreateTextureRTVView(outputTexture);
        if (!inputSrv.IsValid() || !outputRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters parameters = {};
        parameters.ReadSRV(inputSrv);
        parameters.WriteRTV(outputRtv);

        RDGPassHandle pass = graph.AddPass(
            "PostProcess.ToneMap",
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    outputRtv.cpuHandle,
                    inputSrv.descriptorIndex,
                    visualizeScalar,
                    enableSharpen);
            });

        return { outputTexture, pass };
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
        bool enableSharpen = false,
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
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    backBufferRtv.cpuHandle,
                    inputSrv.descriptorIndex,
                    visualizeScalar,
                    enableSharpen);
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
        bool visualizeScalar = false,
        bool enableSharpen = false)
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
            enableSharpen,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT);
    }
};

#endif
