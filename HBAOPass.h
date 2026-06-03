#ifndef HBAO_PASS_H
#define HBAO_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RenderStructs.h"

class HBAOPass
{
public:
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

        CD3DX12_CPU_DESCRIPTOR_HANDLE hbaoRtv = resourceManager->GetHBAORawRtvHandle();
        cmdList->OMSetRenderTargets(1, &hbaoRtv, FALSE, nullptr);

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

        UINT bindlessIndices1[4] = { resourceManager->GetDepthBufferSrvIdx(), resourceManager->GetGBufferNormalSrvIdx(), 0, 0 };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, bindlessIndices1, 0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);

        CD3DX12_RESOURCE_BARRIER blurBarriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetHBAORawRT(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(resourceManager->GetHBAOBlurredRT(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)
        };
        cmdList->ResourceBarrier(2, blurBarriers);

        CD3DX12_CPU_DESCRIPTOR_HANDLE blurRtv = resourceManager->GetHBAOBlurredRtvHandle();
        cmdList->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);

        cmdList->SetPipelineState(pipelineManager->GetHBAOBlurPSO());

        UINT bindlessIndices2[4] = { resourceManager->GetHBAORawSrvIdx(), resourceManager->GetDepthBufferSrvIdx(), resourceManager->GetGBufferNormalSrvIdx(), 0 };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, bindlessIndices2, 0);

        cmdList->DrawInstanced(3, 1, 0, 0);

        CD3DX12_RESOURCE_BARRIER finalBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetHBAOBlurredRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &finalBarrier);
    }
};

#endif