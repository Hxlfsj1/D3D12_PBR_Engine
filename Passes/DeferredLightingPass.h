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
    struct Output
    {
        RDGTextureHandle sceneColor;
        RDGPassHandle pass;
    };

    struct Input
    {
        RDGTextureHandle gbufferAlbedo;
        RDGTextureHandle gbufferNormal;
        RDGTextureHandle gbufferORM;
        RDGTextureHandle gbufferEmissive;
        RDGTextureHandle depth;
        RDGTextureHandle hbaoBlurred;
        RDGTextureSRVHandle shadowMap;
    };

    struct SrvIndices
    {
        UINT gbufferAlbedo;
        UINT gbufferNormal;
        UINT gbufferORM;
        UINT depthBuffer;
        UINT hbao;
        UINT gbufferEmissive;
        UINT shadowMap;
    };

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
            frameIndex,
            resourceManager->GetPostProcessRtvHandle(),
            {
                resourceManager->GetGBufferAlbedoSrvIdx(),
                resourceManager->GetGBufferNormalSrvIdx(),
                resourceManager->GetGBufferORMSrvIdx(),
                resourceManager->GetDepthBufferSrvIdx(),
                resourceManager->GetHBAOBlurredSrvIdx(),
                resourceManager->GetGBufferEmissiveSrvIdx(),
                resourceManager->GetShadowSrvIdx()
            });

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
        int frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        const SrvIndices& srvIndices)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        const UINT64 deferredConstantsOffset = 1024 * 1024 * 8;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + deferredConstantsOffset;
        D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex) + deferredConstantsOffset;

        DeferredConstants deferredCb = {};
        deferredCb.invViewProj = invViewProjMat;
        deferredCb.gbufferAlbedoIdx = srvIndices.gbufferAlbedo;
        deferredCb.gbufferNormalIdx = srvIndices.gbufferNormal;
        deferredCb.gbufferORMIdx = srvIndices.gbufferORM;
        deferredCb.depthBufferIdx = srvIndices.depthBuffer;
        deferredCb.hbaoIdx = srvIndices.hbao;
        deferredCb.gbufferEmissiveIdx = srvIndices.gbufferEmissive;

        memcpy(cbvCpuAddress, &deferredCb, sizeof(DeferredConstants));

        PassConstants* passConstants = reinterpret_cast<PassConstants*>(
            resourceManager->GetCBVAddress(frameIndex));
        passConstants->shadowMapIdx = srvIndices.shadowMap;

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

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex,
        const Input& input = {})
    {
        RDGTextureHandle gbufferAlbedo = input.gbufferAlbedo;
        if (!gbufferAlbedo.IsValid())
        {
            gbufferAlbedo = graph.RegisterExternalTexture(
                resourceManager->GetGBufferAlbedo(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "GBufferAlbedo");
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

        RDGTextureHandle gbufferORM = input.gbufferORM;
        if (!gbufferORM.IsValid())
        {
            gbufferORM = graph.RegisterExternalTexture(
                resourceManager->GetGBufferORM(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "GBufferORM");
        }

        RDGTextureHandle gbufferEmissive = input.gbufferEmissive;
        if (!gbufferEmissive.IsValid())
        {
            gbufferEmissive = graph.RegisterExternalTexture(
                resourceManager->GetGBufferEmissive(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "GBufferEmissive");
        }

        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        RDGTextureHandle hbaoBlurred = input.hbaoBlurred;
        if (!hbaoBlurred.IsValid())
        {
            hbaoBlurred = graph.RegisterExternalTexture(
                resourceManager->GetHBAOBlurredRT(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "HBAOBlurred");
        }

        RDGTextureSRVHandle shadowMapSrv = input.shadowMap;
        if (!shadowMapSrv.IsValid())
        {
            RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
                resourceManager->GetShadowMap(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                "ShadowMap");

            D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
            shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            shadowSrvDesc.Texture2DArray.MipLevels = 1;
            shadowSrvDesc.Texture2DArray.ArraySize = NUM_CASCADES;
            shadowMapSrv = graph.CreateTextureSRVView(shadowMap, &shadowSrvDesc);
        }

        RDGTextureHandle sceneColor = graph.RegisterExternalTexture(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            "PostProcessRT");
        graph.MarkTextureAsOutput(sceneColor);

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle gbufferAlbedoSrv = graph.CreateTextureSRVView(gbufferAlbedo);
        RDGTextureSRVHandle gbufferNormalSrv = graph.CreateTextureSRVView(gbufferNormal);
        RDGTextureSRVHandle gbufferORMSrv = graph.CreateTextureSRVView(gbufferORM);
        RDGTextureSRVHandle gbufferEmissiveSrv = graph.CreateTextureSRVView(gbufferEmissive);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
        RDGTextureSRVHandle hbaoBlurredSrv = graph.CreateTextureSRVView(hbaoBlurred);
        RDGTextureRTVHandle sceneColorRtv = graph.CreateTextureRTVView(sceneColor);

        if (!gbufferAlbedoSrv.IsValid() ||
            !gbufferNormalSrv.IsValid() ||
            !gbufferORMSrv.IsValid() ||
            !gbufferEmissiveSrv.IsValid() ||
            !depthSrv.IsValid() ||
            !hbaoBlurredSrv.IsValid() ||
            !shadowMapSrv.IsValid() ||
            !sceneColorRtv.IsValid())
        {
            return {};
        }

        SrvIndices srvIndices =
        {
            gbufferAlbedoSrv.descriptorIndex,
            gbufferNormalSrv.descriptorIndex,
            gbufferORMSrv.descriptorIndex,
            depthSrv.descriptorIndex,
            hbaoBlurredSrv.descriptorIndex,
            gbufferEmissiveSrv.descriptorIndex,
            shadowMapSrv.descriptorIndex
        };

        RDGPassParameters params;
        params.ReadSRV(gbufferAlbedoSrv);
        params.ReadSRV(gbufferNormalSrv);
        params.ReadSRV(gbufferORMSrv);
        params.ReadSRV(gbufferEmissiveSrv);
        params.ReadSRV(depthSrv);
        params.ReadSRV(hbaoBlurredSrv);
        params.ReadSRV(shadowMapSrv);
        params.WriteRTV(sceneColorRtv);

        RDGPassHandle pass = graph.AddPass(
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
                    frameIndex,
                    sceneColorRtv.cpuHandle,
                    srvIndices);
            });

        return { sceneColor, pass };
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
        graph.SetTransientSrvUavDescriptorAllocator(
            [resourceManager](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return resourceManager->AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

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
