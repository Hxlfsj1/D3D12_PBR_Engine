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

    static void ExecuteNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        const SrvIndices& srvIndices)
    {
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
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& invViewProjMat,
        int width,
        int height,
        int frameIndex,
        const Input& input)
    {
        if (!input.gbufferAlbedo.IsValid() ||
            !input.gbufferNormal.IsValid() ||
            !input.gbufferORM.IsValid() ||
            !input.gbufferEmissive.IsValid() ||
            !input.depth.IsValid() ||
            !input.hbaoBlurred.IsValid() ||
            !input.shadowMap.IsValid())
        {
            return {};
        }

        RDGTextureHandle gbufferAlbedo = input.gbufferAlbedo;
        RDGTextureHandle gbufferNormal = input.gbufferNormal;
        RDGTextureHandle gbufferORM = input.gbufferORM;
        RDGTextureHandle gbufferEmissive = input.gbufferEmissive;
        RDGTextureHandle depth = input.depth;
        RDGTextureHandle hbaoBlurred = input.hbaoBlurred;
        RDGTextureSRVHandle shadowMapSrv = input.shadowMap;

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
                    cmdList,
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
};

#endif
