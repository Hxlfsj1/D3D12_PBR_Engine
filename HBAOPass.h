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
    };

    struct Input
    {
        RDGTextureHandle depth;
        RDGTextureHandle gbufferNormal;
    };

    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& viewMat,
        const DirectX::XMFLOAT4X4& projMat,
        const DirectX::XMFLOAT4X4& invProjMat,
        int width,
        int height,
        int frameIndex)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_RESOURCE_BARRIER barriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetHBAORawRT(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmdList->ResourceBarrier(2, barriers);

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
            resourceManager->GetHBAORawRtvHandle(),
            resourceManager->GetDepthBufferSrvIdx(),
            resourceManager->GetGBufferNormalSrvIdx());

        CD3DX12_RESOURCE_BARRIER blurBarriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetHBAORawRT(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetHBAOBlurredRT(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmdList->ResourceBarrier(2, blurBarriers);

        ExecuteBlurNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            resourceManager->GetHBAOBlurredRtvHandle(),
            resourceManager->GetHBAORawSrvIdx(),
            resourceManager->GetDepthBufferSrvIdx(),
            resourceManager->GetGBufferNormalSrvIdx());

        CD3DX12_RESOURCE_BARRIER finalBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetHBAOBlurredRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &finalBarrier);
    }

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
        UINT gbufferNormalSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);
        const float clearAO[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        cmdList->ClearRenderTargetView(blurRtv, clearAO, 0, nullptr);

        cmdList->SetPipelineState(pipelineManager->GetHBAOBlurPSO());

        UINT bindlessIndices2[4] = { hbaoRawSrvIdx, depthSrvIdx, gbufferNormalSrvIdx, 0 };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, bindlessIndices2, 0);

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
        const Input& input = {})
    {
        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "SceneDepth");
        }

        RDGTextureHandle gbufferNormal = input.gbufferNormal;
        if (!gbufferNormal.IsValid())
        {
            gbufferNormal = graph.RegisterExternalTexture(
                resourceManager->GetGBufferNormal(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "GBufferNormal");
        }

        RDGTextureDesc hbaoDesc = {};
        hbaoDesc.width = static_cast<uint32_t>(width);
        hbaoDesc.height = static_cast<uint32_t>(height);
        hbaoDesc.format = DXGI_FORMAT_R16_FLOAT;
        hbaoDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hbaoDesc.hasClearValue = true;
        hbaoDesc.clearValue.Format = DXGI_FORMAT_R16_FLOAT;
        hbaoDesc.clearValue.Color[0] = 1.0f;

        RDGTextureHandle hbaoRaw = graph.CreateTexture(
            hbaoDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "HBAORawTransient");

        if (!hbaoRaw.IsValid())
        {
            hbaoRaw = graph.RegisterExternalTexture(
                resourceManager->GetHBAORawRT(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "HBAORaw");
        }

        RDGTextureHandle hbaoBlurred = graph.CreateTexture(
            hbaoDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "HBAOBlurredTransient");

        if (!hbaoBlurred.IsValid())
        {
            hbaoBlurred = graph.RegisterExternalTexture(
                resourceManager->GetHBAOBlurredRT(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "HBAOBlurred");
        }

        UINT hbaoRawSrvIdx = resourceManager->GetHBAORawSrvIdx();
        UINT transientHbaoRawSrvIdx = resourceManager->AllocateTransientSrvUavDescriptor();
        if (transientHbaoRawSrvIdx != UINT_MAX &&
            graph.CreateTextureSRV(hbaoRaw, resourceManager->GetSrvUavCPUHandle(transientHbaoRawSrvIdx)))
        {
            hbaoRawSrvIdx = transientHbaoRawSrvIdx;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        UINT depthSrvIdx = resourceManager->GetDepthBufferSrvIdx();
        UINT transientDepthSrvIdx = resourceManager->AllocateTransientSrvUavDescriptor();
        if (transientDepthSrvIdx != UINT_MAX &&
            graph.CreateTextureSRV(depth, resourceManager->GetSrvUavCPUHandle(transientDepthSrvIdx), &depthSrvDesc))
        {
            depthSrvIdx = transientDepthSrvIdx;
        }

        UINT gbufferNormalSrvIdx = resourceManager->GetGBufferNormalSrvIdx();
        UINT transientGBufferNormalSrvIdx = resourceManager->AllocateTransientSrvUavDescriptor();
        if (transientGBufferNormalSrvIdx != UINT_MAX &&
            graph.CreateTextureSRV(gbufferNormal, resourceManager->GetSrvUavCPUHandle(transientGBufferNormalSrvIdx)))
        {
            gbufferNormalSrvIdx = transientGBufferNormalSrvIdx;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE hbaoRawRtv = resourceManager->GetHBAORawRtvHandle();
        D3D12_CPU_DESCRIPTOR_HANDLE transientHbaoRawRtv = {};
        if (graph.CreateTransientTextureRTV(hbaoRaw, &transientHbaoRawRtv))
        {
            hbaoRawRtv = transientHbaoRawRtv;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE hbaoBlurredRtv = resourceManager->GetHBAOBlurredRtvHandle();
        D3D12_CPU_DESCRIPTOR_HANDLE transientHbaoBlurredRtv = {};
        if (graph.CreateTransientTextureRTV(hbaoBlurred, &transientHbaoBlurredRtv))
        {
            hbaoBlurredRtv = transientHbaoBlurredRtv;
        }

        RDGPassParameters rawParams;
        rawParams.ReadSRV(depth);
        rawParams.ReadSRV(gbufferNormal);
        rawParams.WriteRTV(hbaoRaw);

        graph.AddPass(
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
                    hbaoRawRtv,
                    depthSrvIdx,
                    gbufferNormalSrvIdx);
            });

        RDGPassParameters blurParams;
        blurParams.ReadSRV(hbaoRaw);
        blurParams.ReadSRV(depth);
        blurParams.ReadSRV(gbufferNormal);
        blurParams.WriteRTV(hbaoBlurred);

        graph.AddPass(
            "HBAOBlur",
            ERDGPassFlags::Graphics,
            blurParams,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteBlurNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    hbaoBlurredRtv,
                    hbaoRawSrvIdx,
                    depthSrvIdx,
                    gbufferNormalSrvIdx);
            });

        return { hbaoBlurred };
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& viewMat,
        const DirectX::XMFLOAT4X4& projMat,
        const DirectX::XMFLOAT4X4& invProjMat,
        int width,
        int height,
        int frameIndex)
    {
        RDGBuilder graph(deviceContext, "HBAOGraph");

        AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            viewMat,
            projMat,
            invProjMat,
            width,
            height,
            frameIndex);

        graph.Execute(deviceContext->GetCommandList());
    }
};

#endif
