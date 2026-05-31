#ifndef DEFERRED_LIGHTING_PASS_H
#define DEFERRED_LIGHTING_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RenderStructs.h"

class DeferredLightingPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        Camera& camera,
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

        XMMATRIX view = camera.GetViewMatrix();
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)width / height, 0.1f, 1000.0f);
        XMMATRIX viewProj = view * proj;
        XMVECTOR det;
        XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);
        XMStoreFloat4x4(&deferredCb.invViewProj, XMMatrixTranspose(invViewProj));

        deferredCb.gbufferAlbedoIdx = resourceManager->GetGBufferAlbedoSrvIdx();
        deferredCb.gbufferNormalIdx = resourceManager->GetGBufferNormalSrvIdx();
        deferredCb.gbufferORMIdx = resourceManager->GetGBufferORMSrvIdx();

        deferredCb.depthBufferIdx = resourceManager->GetDepthBufferSrvIdx();

        deferredCb.hbaoIdx = resourceManager->GetHBAOBlurredSrvIdx();

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

        CD3DX12_RESOURCE_BARRIER depthToWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            deviceContext->GetDepthStencilBuffer(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &depthToWrite);
    }
};

#endif