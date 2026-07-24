#ifndef MOTION_VECTOR_PASS_H
#define MOTION_VECTOR_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "RDG.h"

class MotionVectorPass
{
public:
    struct Input
    {
        RDGTextureHandle depth;
    };

    struct Output
    {
        RDGTextureHandle motionTexture;
        RDGPassHandle pass;
    };

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        int width,
        int height,
        int frameIndex,
        const Input& input)
    {
        if (deviceContext == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            width <= 0 ||
            height <= 0 ||
            !input.depth.IsValid())
        {
            return {};
        }

        RDGTextureDesc motionDesc = {};
        motionDesc.width = static_cast<uint32_t>(width);
        motionDesc.height = static_cast<uint32_t>(height);
        motionDesc.format = DXGI_FORMAT_R16G16_FLOAT;
        motionDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        motionDesc.hasClearValue = true;
        motionDesc.clearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
        motionDesc.clearValue.Color[0] = 0.0f;
        motionDesc.clearValue.Color[1] = 0.0f;

        RDGTextureHandle motionTexture = graph.CreateTexture(
            motionDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "MotionUV");

        if (!motionTexture.IsValid())
        {
            return {};
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(input.depth, &depthSrvDesc);
        RDGTextureRTVHandle motionRtv = graph.CreateTextureRTVView(motionTexture);
        if (!depthSrv.IsValid() || !motionRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters params = {};
        params.ReadSRV(depthSrv);
        params.WriteRTV(motionRtv);

        RDGPassHandle pass = graph.AddPass(
            "MotionUV",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    currJitteredInvViewProjGpu,
                    prevUnjitteredViewProjGpu,
                    width,
                    height,
                    frameIndex,
                    motionRtv.cpuHandle,
                    depthSrv.descriptorIndex);
            });

        return { motionTexture, pass };
    }

private:
    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        int width,
        int height,
        int frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        UINT depthSrvIdx)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);
        const float clearMotion[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(outputRtv, clearMotion, 0, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetMotionVectorRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetMotionVectorPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const UINT64 motionConstantsOffset = 1024 * 1024 * 10;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + motionConstantsOffset;
        D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex) + motionConstantsOffset;

        MotionVectorConstants motionCb = {};
        motionCb.currJitteredInvViewProj = currJitteredInvViewProjGpu;
        motionCb.prevUnjitteredViewProj = prevUnjitteredViewProjGpu;
        motionCb.depthTextureIdx = depthSrvIdx;

        memcpy(cbvCpuAddress, &motionCb, sizeof(MotionVectorConstants));
        cmdList->SetGraphicsRootConstantBufferView(0, cbvGpuAddress);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
};

#endif
