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
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        D3D12_GPU_VIRTUAL_ADDRESS constantsGpuAddress,
        D3D12_CPU_DESCRIPTOR_HANDLE hbaoRtv,
        UINT depthSrvIdx,
        UINT gbufferNormalSrvIdx)
    {
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

        cmdList->SetGraphicsRootConstantBufferView(PipelineManager::HBAOBinding::Constants, constantsGpuAddress);

        UINT bindlessIndices1[PipelineManager::HBAOBinding::TextureIndexCount] = { depthSrvIdx, gbufferNormalSrvIdx, 0, 0 };
        cmdList->SetGraphicsRoot32BitConstants(PipelineManager::HBAOBinding::TextureIndices, PipelineManager::HBAOBinding::TextureIndexCount, bindlessIndices1, 0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteBlurNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        D3D12_CPU_DESCRIPTOR_HANDLE blurRtv,
        UINT hbaoRawSrvIdx,
        UINT depthSrvIdx,
        UINT gbufferNormalSrvIdx,
        int width,
        int height,
        D3D12_GPU_VIRTUAL_ADDRESS constantsGpuAddress)
    {
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

        cmdList->SetGraphicsRootConstantBufferView(PipelineManager::HBAOBinding::Constants, constantsGpuAddress);

        UINT bindlessIndices2[PipelineManager::HBAOBinding::TextureIndexCount] = { hbaoRawSrvIdx, depthSrvIdx, gbufferNormalSrvIdx, 0 };
        cmdList->SetGraphicsRoot32BitConstants(PipelineManager::HBAOBinding::TextureIndices, PipelineManager::HBAOBinding::TextureIndexCount, bindlessIndices2, 0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static Output AddToGraph(
        RDGBuilder& graph,
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
            ErrorLog::Write("HBAOPass: depth or G-buffer normal input is invalid.");
            return {};
        }

        RDGTextureHandle depth = input.depth;
        RDGTextureHandle gbufferNormal = input.gbufferNormal;

        RDGTextureDesc hbaoDesc;
        hbaoDesc.width = static_cast<uint32_t>(width);
        hbaoDesc.height = static_cast<uint32_t>(height);
        hbaoDesc.format = PipelineManager::Formats::ScalarSignal;
        hbaoDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hbaoDesc.hasClearValue = true;
        hbaoDesc.clearValue.Format = PipelineManager::Formats::ScalarSignal;
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
            ErrorLog::Write("HBAOPass: failed to create the raw or blurred AO texture.");
            return {};
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = PipelineManager::Formats::DepthSRV;
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
            ErrorLog::Write("HBAOPass: failed to create one or more AO resource views.");
            return {};
        }

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

        const auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(HBAOConstants));
        if (!allocation) return {};
        memcpy(allocation.cpuAddress, &hbaoCb, sizeof(HBAOConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS constantsGpuAddress = allocation.gpuAddress;

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
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    width,
                    height,
                    constantsGpuAddress,
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
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    hbaoBlurredRtv.cpuHandle,
                    hbaoRawSrv.descriptorIndex,
                    depthSrv.descriptorIndex,
                    gbufferNormalSrv.descriptorIndex,
                    width,
                    height,
                    constantsGpuAddress);
            });

        return { hbaoBlurred, rawPass, blurPass };
    }
};

#endif
