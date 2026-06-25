#ifndef DEFERRED_LIGHTING_PASS_H
#define DEFERRED_LIGHTING_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RenderStructs.h"
#include "RDG.h"

class DeferredLightingPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex)
    {
        auto cmdList = deviceContext->GetCommandList();

        ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            invViewProjMat,
            width,
            height,
            frameIndex);

        CD3DX12_RESOURCE_BARRIER depthToWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &depthToWrite);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = resourceManager->GetPostProcessRtvHandle();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        const UINT64 deferredConstantsOffset = 1024 * 1024 * 8;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + deferredConstantsOffset;
        D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex) + deferredConstantsOffset;

        DeferredConstants deferredCb = {};
        deferredCb.invViewProj = invViewProjMat;
        deferredCb.gbufferAlbedoIdx = resourceManager->GetGBufferAlbedoSrvIdx();
        deferredCb.gbufferNormalIdx = resourceManager->GetGBufferNormalSrvIdx();
        deferredCb.gbufferORMIdx = resourceManager->GetGBufferORMSrvIdx();
        deferredCb.depthBufferIdx = resourceManager->GetDepthBufferSrvIdx();
        deferredCb.hbaoIdx = resourceManager->GetHBAOBlurredSrvIdx();
        deferredCb.gbufferEmissiveIdx = resourceManager->GetGBufferEmissiveSrvIdx();

        memcpy(cbvCpuAddress, &deferredCb, sizeof(DeferredConstants));

        cmdList->SetGraphicsRootSignature(pipelineManager->GetDeferredRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetDeferredPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetGraphicsRootConstantBufferView(0, resourceManager->GetCBVGPUAddress(frameIndex));
        cmdList->SetGraphicsRootConstantBufferView(1, cbvGpuAddress);
        cmdList->SetGraphicsRootConstantBufferView(2, resourceManager->GetSHBufferGPUAddress());

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex)
    {
        RDGTextureHandle gbufferAlbedo = graph.RegisterExternalTexture(
            resourceManager->GetGBufferAlbedo(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "GBufferAlbedo");

        RDGTextureHandle gbufferNormal = graph.RegisterExternalTexture(
            resourceManager->GetGBufferNormal(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "GBufferNormal");

        RDGTextureHandle gbufferORM = graph.RegisterExternalTexture(
            resourceManager->GetGBufferORM(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "GBufferORM");

        RDGTextureHandle gbufferEmissive = graph.RegisterExternalTexture(
            resourceManager->GetGBufferEmissive(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "GBufferEmissive");

        RDGTextureHandle depth = graph.RegisterExternalTexture(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            "SceneDepth");

        RDGTextureHandle hbaoBlurred = graph.RegisterExternalTexture(
            resourceManager->GetHBAOBlurredRT(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "HBAOBlurred");

        RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "ShadowMap");

        RDGTextureHandle sceneColor = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");
        graph.MarkTextureAsOutput(sceneColor);

        RDGPassParameters params;
        params.ReadSRV(gbufferAlbedo);
        params.ReadSRV(gbufferNormal);
        params.ReadSRV(gbufferORM);
        params.ReadSRV(gbufferEmissive);
        params.ReadSRV(depth);
        params.ReadSRV(hbaoBlurred);
        params.ReadSRV(shadowMap);
        params.WriteRTV(sceneColor);

        graph.AddPass(
            "DeferredLighting",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    invViewProjMat,
                    width,
                    height,
                    frameIndex);
            });
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex)
    {
        RDGBuilder graph(deviceContext, "DeferredLightingGraph");

        AddToGraph(
            graph,
            deviceContext,
            resourceManager,
            pipelineManager,
            invViewProjMat,
            width,
            height,
            frameIndex);

        graph.Execute(deviceContext->GetCommandList());
    }
};

#endif