#ifndef HBAO_PASS_H
#define HBAO_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RenderStructs.h"
#include "RDG.h"

class HBAOPass
{
public:
    struct Output
    {
        RDGTextureHandle blurredTexture;
        RDGPassHandle rawPass;
        RDGPassHandle blurPass;
    };

    struct Input
    {
        RDGTextureHandle depth;
        RDGTextureHandle gbufferNormal;
    };

    static void ExecuteRawNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& viewMat,
        const DirectX::XMFLOAT4X4& projMat,
        const DirectX::XMFLOAT4X4& invProjMat,
        int width,
        int height,
        int frameIndex,
        UINT temporalFrameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE hbaoRtv,
        UINT depthSrvIdx,
        UINT gbufferNormalSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &hbaoRtv, FALSE, nullptr);
        const float clearAO[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(hbaoRtv, clearAO, 0, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetHBAORootSignature());
        cmdList->SetPipelineState(pipelineManager->GetHBAOPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const UINT64 hbaoConstantsOffset = 1024 * 1024 * 9;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + hbaoConstantsOffset;
        D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex) + hbaoConstantsOffset;

        HBAOConstants hbaoCb = {};
        hbaoCb.projMat = projMat;
        hbaoCb.invProjMat = invProjMat;
        hbaoCb.viewMat = viewMat;
        hbaoCb.radius = 1.0f;
        hbaoCb.bias = 0.1f;
        hbaoCb.power = 2.0f;
        hbaoCb.resolutionX = (float)width;
        hbaoCb.resolutionY = (float)height;
        hbaoCb.temporalFrameIndex = temporalFrameIndex;

        memcpy(cbvCpuAddress, &hbaoCb, sizeof(HBAOConstants));

        cmdList->SetGraphicsRootConstantBufferView(0, cbvGpuAddress);

        UINT bindlessIndices1[4] = { depthSrvIdx, gbufferNormalSrvIdx, 0, 0 };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, bindlessIndices1, 0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteBlurNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        D3D12_CPU_DESCRIPTOR_HANDLE blurRtv,
        UINT hbaoRawSrvIdx,
        UINT depthSrvIdx,
        UINT gbufferNormalSrvIdx,
        int width,
        int height,
        int frameIndex)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);
        const float clearAO[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(blurRtv, clearAO, 0, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetHBAORootSignature());
        cmdList->SetPipelineState(pipelineManager->GetHBAOBlurPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const UINT64 hbaoConstantsOffset = 1024 * 1024 * 9;
        cmdList->SetGraphicsRootConstantBufferView(0, resourceManager->GetCBVGPUAddress(frameIndex) + hbaoConstantsOffset);

        UINT bindlessIndices2[4] = { hbaoRawSrvIdx, depthSrvIdx, gbufferNormalSrvIdx, 0 };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, bindlessIndices2, 0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& viewMat,
        const DirectX::XMFLOAT4X4& projMat,
        const DirectX::XMFLOAT4X4& invProjMat,
        int width,
        int height,
        int frameIndex,
        UINT temporalFrameIndex,
        const Input& input)
    {
        if (!input.depth.IsValid() || !input.gbufferNormal.IsValid())
        {
            return {};
        }

        RDGTextureHandle depth = input.depth;
        RDGTextureHandle gbufferNormal = input.gbufferNormal;

        RDGTextureDesc hbaoDesc;
        hbaoDesc.width = static_cast<uint32_t>(width);
        hbaoDesc.height = static_cast<uint32_t>(height);
        hbaoDesc.format = DXGI_FORMAT_R16_FLOAT;
        hbaoDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hbaoDesc.hasClearValue = true;
        hbaoDesc.clearValue.Format = DXGI_FORMAT_R16_FLOAT;
        hbaoDesc.clearValue.Color[0] = 1.0f;

        RDGTextureHandle hbaoRaw = graph.CreateTexture(
            hbaoDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "HBAORaw");

        RDGTextureHandle hbaoBlurred = graph.CreateTexture(
            hbaoDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "HBAOBlurred");

        if (!hbaoRaw.IsValid() || !hbaoBlurred.IsValid())
        {
            return {};
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
        RDGTextureSRVHandle gbufferNormalSrv = graph.CreateTextureSRVView(gbufferNormal);
        RDGTextureSRVHandle hbaoRawSrv = graph.CreateTextureSRVView(hbaoRaw);
        RDGTextureRTVHandle hbaoRawRtv = graph.CreateTextureRTVView(hbaoRaw);
        RDGTextureRTVHandle hbaoBlurredRtv = graph.CreateTextureRTVView(hbaoBlurred);

        if (!depthSrv.IsValid() ||
            !gbufferNormalSrv.IsValid() ||
            !hbaoRawSrv.IsValid() ||
            !hbaoRawRtv.IsValid() ||
            !hbaoBlurredRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters rawParams;
        rawParams.ReadSRV(depthSrv);
        rawParams.ReadSRV(gbufferNormalSrv);
        rawParams.WriteRTV(hbaoRawRtv);

        RDGPassHandle rawPass = graph.AddPass(
            "HBAORaw",
            ERDGPassFlags::Graphics,
            rawParams,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteRawNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    viewMat,
                    projMat,
                    invProjMat,
                    width,
                    height,
                    frameIndex,
                    temporalFrameIndex,
                    hbaoRawRtv.cpuHandle,
                    depthSrv.descriptorIndex,
                    gbufferNormalSrv.descriptorIndex);
            });

        RDGPassParameters blurParams;
        blurParams.ReadSRV(hbaoRawSrv);
        blurParams.ReadSRV(depthSrv);
        blurParams.ReadSRV(gbufferNormalSrv);
        blurParams.WriteRTV(hbaoBlurredRtv);

        RDGPassHandle blurPass = graph.AddPass(
            "HBAOBlur",
            ERDGPassFlags::Graphics,
            blurParams,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteBlurNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    hbaoBlurredRtv.cpuHandle,
                    hbaoRawSrv.descriptorIndex,
                    depthSrv.descriptorIndex,
                    gbufferNormalSrv.descriptorIndex,
                    width,
                    height,
                    frameIndex);
            });

        return { hbaoBlurred, rawPass, blurPass };
    }
};

#endif
