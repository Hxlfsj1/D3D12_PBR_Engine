#ifndef SKYBOX_PASS_H
#define SKYBOX_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RDG.h"

class SkyboxPass
{
public:
    struct Input
    {
        RDGTextureHandle sceneColor;
        RDGTextureHandle depth;
    };

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        Camera& camera,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        int width,
        int height,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv)
    {
        auto cmdList = deviceContext->GetCommandList();

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

    static RDGPassHandle AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        Camera& camera,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        int width,
        int height,
        const Input& input = {})
    {
        RDGTextureHandle sceneColor = input.sceneColor;
        if (!sceneColor.IsValid())
        {
            sceneColor = graph.RegisterExternalTexture(
                resourceManager->GetPostProcessRT(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                "PostProcessRT");
        }
        graph.MarkTextureAsOutput(sceneColor);

        RDGTextureHandle depth = input.depth;
        if (!depth.IsValid())
        {
            depth = graph.RegisterExternalTexture(
                deviceContext->GetDepthStencilBuffer(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                "SceneDepth");
        }

        RDGTextureRTVHandle sceneColorRtv = graph.CreateTextureRTVView(sceneColor);
        RDGTextureDSVHandle depthDsv = graph.CreateTextureDSVView(depth);
        if (!sceneColorRtv.IsValid() || !depthDsv.IsValid())
        {
            return {};
        }

        RDGPassParameters params;
        params.WriteRTV(sceneColorRtv);
        params.WriteDSV(depthDsv);

        return graph.AddPass(
            "Skybox",
            ERDGPassFlags::Graphics,
            params,
            [=, &camera](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    camera,
                    viewport,
                    scissorRect,
                    width,
                    height,
                    sceneColorRtv.cpuHandle,
                    depthDsv.cpuHandle);
            });
    }
};

#endif
