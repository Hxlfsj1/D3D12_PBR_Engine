#ifndef POST_PROCESS_PASS_H
#define POST_PROCESS_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"

class PostProcessPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmdList->ResourceBarrier(1, &toSrv);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = deviceContext->GetRTVHandle(frameIndex);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetPostProcessRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetPostProcessPSO());

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        UINT sceneTexIdx = resourceManager->GetPostProcessSrvIdx();
        cmdList->SetGraphicsRoot32BitConstants(0, 1, &sceneTexIdx, 0);

        cmdList->DrawInstanced(3, 1, 0, 0);

        CD3DX12_RESOURCE_BARRIER toRtv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetPostProcessRT(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->ResourceBarrier(1, &toRtv);
    }
};

#endif