#ifndef TAA_PASS_H
#define TAA_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "Camera.h"

class TAAPass
{
public:
    static UINT Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        Camera& camera,
        const DirectX::XMFLOAT4X4& prevViewProj,
        float jitterX, float jitterY,
        int frameIndex, int width, int height,
        int frameCount)
    {
        XMMATRIX view = camera.GetViewMatrix();
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)width / height, 0.1f, 1000.0f);
        XMVECTOR det;
        XMMATRIX invViewProj = XMMatrixInverse(&det, view * proj);

        XMFLOAT4X4 currentInvViewProj;
        XMStoreFloat4x4(&currentInvViewProj, XMMatrixTranspose(invViewProj));

        int taaCurrentIdx = resourceManager->GetTAACurrentHistoryIdx();
        bool isFirstFrame = (frameCount == 1);

        auto cmdList = deviceContext->GetCommandList();

        ID3D12Resource* currentHistoryTarget = resourceManager->GetTAAHistoryRT(taaCurrentIdx);
        ID3D12Resource* offscreenLitBuffer = resourceManager->GetPostProcessRT();

        CD3DX12_RESOURCE_BARRIER barriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(offscreenLitBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(currentHistoryTarget, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(3, barriers);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle = resourceManager->GetTAARtvHandle(taaCurrentIdx);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetTAARootSignature());
        cmdList->SetPipelineState(pipelineManager->GetTAAPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        const UINT64 taaConstantsOffset = 1024 * 1024 * 12;
        UINT8* cbvCpuAddress = resourceManager->GetCBVAddress(frameIndex) + taaConstantsOffset;

        TAAConstants cb = {};
        cb.invViewProj = currentInvViewProj;
        cb.prevViewProj = prevViewProj;
        cb.jitterOffset = DirectX::XMFLOAT2(jitterX, jitterY);
        cb.blendAlpha = isFirstFrame ? 0.0f : 0.95f;
        cb.varianceScale = 1.5f;

        cb.colorTextureIdx = resourceManager->GetPostProcessSrvIdx();
        cb.historyTextureIdx = resourceManager->GetTAAHistorySrvIdx(1 - taaCurrentIdx);
        cb.depthTextureIdx = resourceManager->GetDepthBufferSrvIdx();

        memcpy(cbvCpuAddress, &cb, sizeof(TAAConstants));
        cmdList->SetGraphicsRootConstantBufferView(0, resourceManager->GetCBVGPUAddress(frameIndex) + taaConstantsOffset);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);

        CD3DX12_RESOURCE_BARRIER revertBarriers[3] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(offscreenLitBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(currentHistoryTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(deviceContext->GetDepthStencilBuffer(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
        };
        cmdList->ResourceBarrier(3, revertBarriers);

        resourceManager->FlipTAAHistoryIndex();

        return resourceManager->GetTAAHistorySrvIdx(taaCurrentIdx);
    }
};

#endif