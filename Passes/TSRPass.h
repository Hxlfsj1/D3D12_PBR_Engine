#ifndef TSR_PASS_H
#define TSR_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RDG.h"

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) TSRConstants
{
    DirectX::XMFLOAT4X4 currJitteredInvViewProj;
    DirectX::XMFLOAT4X4 prevUnjitteredViewProj;

    float blendAlpha;
    UINT colorTextureIdx;
    UINT historyTextureIdx;
    UINT depthTextureIdx;

    UINT motionTextureIdx;
    DirectX::XMFLOAT2 currentJitterPixels;
    UINT pad;
};

class TSRPass
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

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX,
        float currJitterNdcY,
        int frameIndex,
        int inputWidth,
        int inputHeight,
        int outputWidth,
        int outputHeight,
        bool historyValid,
        const TextureViews& views)
    {
        ID3D12GraphicsCommandList* commandList = deviceContext->GetCommandList();
        commandList->OMSetRenderTargets(1, &views.outputRtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport = {
            0.0f,
            0.0f,
            static_cast<float>(outputWidth),
            static_cast<float>(outputHeight),
            0.0f,
            1.0f
        };
        D3D12_RECT scissorRect = { 0, 0, outputWidth, outputHeight };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        commandList->SetGraphicsRootSignature(pipelineManager->GetTSRRootSignature());
        commandList->SetPipelineState(pipelineManager->GetTSRPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, heaps);

        constexpr UINT64 tsrConstantsOffset = 1024ull * 1024ull * 13ull;
        UINT8* cbvCpuAddress =
            resourceManager->GetCBVAddress(frameIndex) + tsrConstantsOffset;

        TSRConstants constants = {};
        constants.currJitteredInvViewProj = currJitteredInvViewProjGpu;
        constants.prevUnjitteredViewProj = prevUnjitteredViewProjGpu;
        constants.blendAlpha = historyValid ? 0.95f : 0.0f;
        constants.currentJitterPixels = DirectX::XMFLOAT2(
            currJitterNdcX * 0.5f * static_cast<float>(inputWidth),
            -currJitterNdcY * 0.5f * static_cast<float>(inputHeight));
        constants.colorTextureIdx = views.colorSrvIdx;
        constants.historyTextureIdx = views.historySrvIdx;
        constants.depthTextureIdx = views.depthSrvIdx;
        constants.motionTextureIdx = views.motionSrvIdx;

        memcpy(cbvCpuAddress, &constants, sizeof(TSRConstants));
        commandList->SetGraphicsRootConstantBufferView(
            0,
            resourceManager->GetCBVGPUAddress(frameIndex) + tsrConstantsOffset);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        float currJitterNdcX,
        float currJitterNdcY,
        int frameIndex,
        int inputWidth,
        int inputHeight,
        int outputWidth,
        int outputHeight,
        bool historyValid,
        const Input& input)
    {
        if (deviceContext == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            !input.color.IsValid() ||
            !input.depth.IsValid() ||
            inputWidth <= 0 ||
            inputHeight <= 0 ||
            outputWidth <= 0 ||
            outputHeight <= 0)
        {
            return {};
        }

        const int currentHistoryIndex = resourceManager->GetTemporalCurrentHistoryIdx();
        RDGTextureHandle currentHistory = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(currentHistoryIndex),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TSRCurrentHistory");
        graph.MarkTextureAsOutput(currentHistory);

        RDGTextureHandle previousHistory = graph.RegisterExternalTexture(
            resourceManager->GetTemporalHistoryRT(1 - currentHistoryIndex),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "TSRPreviousHistory");

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle colorSrv = graph.CreateTextureSRVView(input.color);
        RDGTextureSRVHandle historySrv = graph.CreateTextureSRVView(previousHistory);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(input.depth, &depthSrvDesc);
        RDGTextureSRVHandle motionSrv = {};
        if (input.motion.IsValid())
        {
            motionSrv = graph.CreateTextureSRVView(input.motion);
        }
        RDGTextureRTVHandle historyRtv = graph.CreateTextureRTVView(currentHistory);

        if (!colorSrv.IsValid() ||
            !historySrv.IsValid() ||
            !depthSrv.IsValid() ||
            !historyRtv.IsValid())
        {
            return {};
        }

        TextureViews views = {
            historyRtv.cpuHandle,
            colorSrv.descriptorIndex,
            historySrv.descriptorIndex,
            depthSrv.descriptorIndex,
            motionSrv.IsValid() ? motionSrv.descriptorIndex : UINT_MAX
        };

        RDGPassParameters parameters;
        parameters.ReadSRV(colorSrv);
        parameters.ReadSRV(historySrv);
        parameters.ReadSRV(depthSrv);
        if (motionSrv.IsValid())
        {
            parameters.ReadSRV(motionSrv);
        }
        parameters.WriteRTV(historyRtv);

        RDGPassHandle pass = graph.AddPass(
            "TSR",
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList*)
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
                    inputWidth,
                    inputHeight,
                    outputWidth,
                    outputHeight,
                    historyValid,
                    views);
            });

        return { currentHistory, currentHistoryIndex, pass };
    }
};

#endif
