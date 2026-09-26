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
        const DirectX::XMFLOAT4X4& currUnjitteredViewProjGpu,
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
            ErrorLog::Write("MotionVectorPass: invalid device, resource manager, dimensions, or depth input.");
            return {};
        }

        RDGTextureDesc motionDesc = {};
        motionDesc.width = static_cast<uint32_t>(width);
        motionDesc.height = static_cast<uint32_t>(height);
        motionDesc.format = PipelineManager::Formats::MotionVector;
        motionDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        motionDesc.hasClearValue = true;
        motionDesc.clearValue.Format = PipelineManager::Formats::MotionVector;
        motionDesc.clearValue.Color[0] = 0.0f;
        motionDesc.clearValue.Color[1] = 0.0f;

        RDGTextureHandle motionTexture = graph.CreateTexture(
            motionDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "MotionUV");

        if (!motionTexture.IsValid())
        {
            ErrorLog::Write("MotionVectorPass: failed to create the motion-vector texture.");
            return {};
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = PipelineManager::Formats::DepthSRV;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(input.depth, &depthSrvDesc);
        RDGTextureRTVHandle motionRtv = graph.CreateTextureRTVView(motionTexture);
        if (!depthSrv.IsValid() || !motionRtv.IsValid())
        {
            ErrorLog::Write("MotionVectorPass: failed to create the depth SRV or motion-vector RTV.");
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
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    currJitteredInvViewProjGpu,
                    currUnjitteredViewProjGpu,
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
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProjGpu,
        const DirectX::XMFLOAT4X4& currUnjitteredViewProjGpu,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProjGpu,
        int width,
        int height,
        int frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        UINT depthSrvIdx)
    {
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

        MotionVectorConstants motionCb = {};
        motionCb.currJitteredInvViewProj = currJitteredInvViewProjGpu;
        motionCb.currUnjitteredViewProj = currUnjitteredViewProjGpu;
        motionCb.prevUnjitteredViewProj = prevUnjitteredViewProjGpu;
        motionCb.depthTextureIdx = depthSrvIdx;

        const auto allocation = resourceManager->AllocatePassConstants(frameIndex, sizeof(MotionVectorConstants));
        if (!allocation) return;
        memcpy(allocation.cpuAddress, &motionCb, sizeof(MotionVectorConstants));
        cmdList->SetGraphicsRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, allocation.gpuAddress);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
};

#endif
