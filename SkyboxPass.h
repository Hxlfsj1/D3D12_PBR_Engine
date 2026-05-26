#ifndef SKYBOX_PASS_H
#define SKYBOX_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"

class SkyboxPass
{
public:
    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        Camera& camera,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        int width,
        int height)
    {
        auto cmdList = deviceContext->GetCommandList();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = resourceManager->GetPostProcessRtvHandle();
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = deviceContext->GetDSVHandle();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());
        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Bind the PSO (Pipeline State Object) for Skybox rendering
        cmdList->SetPipelineState(pipelineManager->GetSkybox_PSO());

        // Bind the vertex data directly
        D3D12_VERTEX_BUFFER_VIEW skyboxVBV = resourceManager->GetSkyboxVBV();
        cmdList->IASetVertexBuffers(0, 1, &skyboxVBV);

        // Ensure the skybox remains centered relative to the camera
        XMMATRIX view = camera.GetViewMatrix();
        view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)width / height, 0.1f, 1000.0f);
        XMMATRIX skyVPMat = XMMatrixTranspose(view * proj);

        // Pass the Skybox MVP matrix via registers (Root Constants)
        cmdList->SetGraphicsRoot32BitConstants(4, 16, &skyVPMat, 0);

        // Bind the skybox texture (Cubemap)
        UINT skyboxTexIdx = resourceManager->GetIblEnvCubeIdx();
        cmdList->SetGraphicsRoot32BitConstants(4, 1, &skyboxTexIdx, 16);

        cmdList->DrawInstanced(36, 1, 0, 0);
    }
};

#endif