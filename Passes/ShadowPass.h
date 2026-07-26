#ifndef SHADOW_PASS_H
#define SHADOW_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "Camera.h"
#include "RenderStructs.h"
#include "SceneObject.h"
#include "PBR_Model.h"
#include "RDG.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <vector>

class ShadowPass
{
public:
    struct FramePreparationInput
    {
        Camera* camera = nullptr;
        DirectX::XMFLOAT3 lightDir = {};
        float aspectRatio = 1.0f;
        float shadowRadius = 0.0f;
        float shadowMaxDistance = 100.0f;
        float shadowMapSize = 4096.0f;
    };

    struct FrameData
    {
        DirectX::XMFLOAT3 lightDir = {};
        DirectX::XMFLOAT4X4 lightView = {};
        std::array<DirectX::XMFLOAT4X4, NUM_CASCADES> lightViewProj = {};
        DirectX::XMFLOAT4 cascadeSplits = {};
        DirectX::XMFLOAT4 cascadeOrthoWidths = {};
        DirectX::XMFLOAT4 cascadeDepthRanges = {};
        DirectX::BoundingBox shadowArea = {};
        std::array<DirectX::BoundingBox, NUM_CASCADES> cascadeShadowAreas = {};
    };

    struct Output
    {
        RDGTextureHandle shadowMap;
        RDGTextureSRVHandle shadowMapSrv;
        RDGPassHandle pass;
    };

    static FrameData PrepareFrame(const FramePreparationInput& input)
    {
        using namespace DirectX;

        FrameData frameData = {};
        if (input.camera == nullptr || input.aspectRatio <= 0.0f || input.shadowMapSize <= 0.0f)
        {
            return frameData;
        }

        XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&input.lightDir));
        XMStoreFloat3(&frameData.lightDir, lightDir);

        XMVECTOR cameraPosition = XMLoadFloat3(&input.camera->Position);
        XMVECTOR lightPosition = XMVectorSubtract(cameraPosition, XMVectorScale(lightDir, 200.0f));
        XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::abs(XMVectorGetY(lightDir)) > 0.99f)
        {
            lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }

        XMMATRIX lightView = XMMatrixLookAtLH(
            lightPosition,
            XMVectorAdd(lightPosition, lightDir),
            lightUp);
        XMStoreFloat4x4(&frameData.lightView, lightView);

        constexpr float nearClip = 0.1f;
        BoundingFrustum shadowFrustum = input.camera->GetWorldSpaceFrustum(
            input.aspectRatio,
            nearClip,
            input.shadowMaxDistance);

        XMFLOAT3 frustumCorners[8];
        shadowFrustum.GetCorners(frustumCorners);

        float minZ = FLT_MAX;
        float maxZ = -FLT_MAX;
        for (const XMFLOAT3& corner : frustumCorners)
        {
            XMVECTOR lightSpaceCorner = XMVector3Transform(XMLoadFloat3(&corner), lightView);
            minZ = (std::min)(minZ, XMVectorGetZ(lightSpaceCorner));
            maxZ = (std::max)(maxZ, XMVectorGetZ(lightSpaceCorner));
        }

        constexpr float shadowDepthPadding = 50.0f;
        minZ -= shadowDepthPadding;
        maxZ += shadowDepthPadding;

        frameData.shadowArea.Center = XMFLOAT3(0.0f, 0.0f, (minZ + maxZ) * 0.5f);
        frameData.shadowArea.Extents = XMFLOAT3(
            input.shadowRadius,
            input.shadowRadius,
            (maxZ - minZ) * 0.5f);

        const std::array<float, NUM_CASCADES + 1> cascadeSplits = {
            nearClip,
            5.0f,
            15.0f,
            50.0f,
            input.shadowMaxDistance
        };
        frameData.cascadeSplits = XMFLOAT4(
            cascadeSplits[1],
            cascadeSplits[2],
            cascadeSplits[3],
            cascadeSplits[4]);

        std::array<float, NUM_CASCADES> orthoWidths = {};
        std::array<float, NUM_CASCADES> depthRanges = {};
        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            BoundingFrustum cascadeFrustum = input.camera->GetWorldSpaceFrustum(
                input.aspectRatio,
                cascadeSplits[cascadeIdx],
                cascadeSplits[cascadeIdx + 1]);

            XMFLOAT3 cascadeCorners[8];
            cascadeFrustum.GetCorners(cascadeCorners);

            XMVECTOR frustumCenter = XMVectorZero();
            for (const XMFLOAT3& corner : cascadeCorners)
            {
                frustumCenter = XMVectorAdd(frustumCenter, XMLoadFloat3(&corner));
            }
            frustumCenter = XMVectorScale(frustumCenter, 1.0f / 8.0f);

            float sphereRadius = 0.0f;
            for (const XMFLOAT3& corner : cascadeCorners)
            {
                XMVECTOR distance = XMVector3Length(
                    XMVectorSubtract(XMLoadFloat3(&corner), frustumCenter));
                sphereRadius = (std::max)(sphereRadius, XMVectorGetX(distance));
            }
            sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

            XMVECTOR lightSpaceCenter = XMVector3Transform(frustumCenter, lightView);
            float orthoWidth = sphereRadius * 2.0f;
            float texelSize = orthoWidth / input.shadowMapSize;
            float snappedX = std::floor(XMVectorGetX(lightSpaceCenter) / texelSize) * texelSize;
            float snappedY = std::floor(XMVectorGetY(lightSpaceCenter) / texelSize) * texelSize;
            float snappedZ = XMVectorGetZ(lightSpaceCenter);

            float minX = snappedX - sphereRadius;
            float maxX = snappedX + sphereRadius;
            float minY = snappedY - sphereRadius;
            float maxY = snappedY + sphereRadius;
            float cascadeMinZ = snappedZ - sphereRadius - shadowDepthPadding;
            float cascadeMaxZ = snappedZ + sphereRadius;

            BoundingBox& cascadeArea = frameData.cascadeShadowAreas[cascadeIdx];
            cascadeArea.Center = XMFLOAT3(
                (minX + maxX) * 0.5f,
                (minY + maxY) * 0.5f,
                (cascadeMinZ + cascadeMaxZ) * 0.5f);
            cascadeArea.Extents = XMFLOAT3(
                (maxX - minX) * 0.5f,
                (maxY - minY) * 0.5f,
                (cascadeMaxZ - cascadeMinZ) * 0.5f);

            XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
                minX,
                maxX,
                minY,
                maxY,
                cascadeMinZ,
                cascadeMaxZ);
            XMStoreFloat4x4(
                &frameData.lightViewProj[cascadeIdx],
                XMMatrixTranspose(lightView * lightProjection));
            orthoWidths[cascadeIdx] = orthoWidth;
            depthRanges[cascadeIdx] = cascadeMaxZ - cascadeMinZ;
        }

        frameData.cascadeOrthoWidths = XMFLOAT4(
            orthoWidths[0],
            orthoWidths[1],
            orthoWidths[2],
            orthoWidths[3]);
        frameData.cascadeDepthRanges = XMFLOAT4(
            depthRanges[0],
            depthRanges[1],
            depthRanges[2],
            depthRanges[3]);

        return frameData;
    }

    static void Execute(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        auto cmdList = deviceContext->GetCommandList();

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES> shadowDsvHandles = {};
        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            shadowDsvHandles[cascadeIdx] = resourceManager->GetShadowDsvHandle(cascadeIdx);
        }

        CD3DX12_RESOURCE_BARRIER toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &toDepthWrite);

        ExecuteNoBarrier(
            deviceContext,
            resourceManager,
            pipelineManager,
            frameIndex,
            shadowVisibleInstancesByCascade,
            shadowInstanceOffsets,
            visibleInstancesSize,
            shadowDsvHandles);

        CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toSrv);
    }

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize,
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES>& shadowDsvHandles)
    {
        auto cmdList = deviceContext->GetCommandList();

        cmdList->SetGraphicsRootSignature(pipelineManager->GetRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetShadowPSO());

        D3D12_VIEWPORT shadowViewport = { 0.0f, 0.0f, 4096.0f, 4096.0f, 0.0f, 1.0f };
        D3D12_RECT shadowScissor = { 0, 0, 4096, 4096 };

        cmdList->RSSetViewports(1, &shadowViewport);
        cmdList->RSSetScissorRects(1, &shadowScissor);

        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            const auto& shadowVisibleInstances = shadowVisibleInstancesByCascade[cascadeIdx];
            size_t cascadeInstanceOffset = shadowInstanceOffsets[cascadeIdx];

            D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHandles[cascadeIdx];
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
            cmdList->SetDescriptorHeaps(1, heaps);

            D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = resourceManager->GetCBVGPUAddress(frameIndex);
            cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);
            cmdList->SetGraphicsRootShaderResourceView(3, resourceManager->GetMaterialBufferGPUAddress());

            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            Model* currentModel = nullptr;
            int currentLod = -1;
            bool currentIsCutout = false;
            size_t instanceStartOffset = 0;
            UINT currentInstanceCount = 0;

            for (size_t i = 0; i <= shadowVisibleInstances.size(); ++i)
            {
                bool isEnd = (i == shadowVisibleInstances.size());
                Model* thisModel = isEnd ? nullptr : shadowVisibleInstances[i]->pModel;
                int thisLod = isEnd ? -1 : shadowVisibleInstances[i]->currentLodLevel;
                bool thisIsCutout = isEnd ? false : shadowVisibleInstances[i]->isCutout;

                if ((isEnd || thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout) && currentInstanceCount > 0 && currentModel != nullptr)
                {
                    D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + kPassConstantsAlignedSize + ((visibleInstancesSize + cascadeInstanceOffset + instanceStartOffset) * sizeof(InstanceData));
                    cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

                    for (auto& mesh : currentModel->meshes)
                    {
                        UINT constants[2] = { mesh.materialID, cascadeIdx };
                        cmdList->SetGraphicsRoot32BitConstants(4, 2, constants, 0);
                        mesh.Draw(cmdList, currentInstanceCount, currentLod);
                    }
                }

                if (!isEnd)
                {
                    if (thisModel != currentModel || thisLod != currentLod || thisIsCutout != currentIsCutout)
                    {
                        if (currentModel == nullptr || thisIsCutout != currentIsCutout)
                        {
                            if (thisIsCutout)
                                cmdList->SetPipelineState(pipelineManager->GetShadowCutoutPSO());
                            else
                                cmdList->SetPipelineState(pipelineManager->GetShadowPSO());
                        }

                        currentModel = thisModel;
                        currentLod = thisLod;
                        currentIsCutout = thisIsCutout;
                        instanceStartOffset = i;
                        currentInstanceCount = 1;
                    }
                    else
                    {
                        currentInstanceCount++;
                    }
                }
            }
        }
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        RDGTextureHandle shadowMap = graph.RegisterExternalTexture(
            resourceManager->GetShadowMap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            "ShadowMap");
        graph.MarkTextureAsOutput(shadowMap);

        D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDesc = {};
        shadowDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        shadowDsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        shadowDsvDesc.Texture2DArray.MipSlice = 0;
        shadowDsvDesc.Texture2DArray.ArraySize = 1;

        std::array<RDGTextureDSVHandle, NUM_CASCADES> shadowDsvViews = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_CASCADES> shadowDsvHandles = {};
        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            shadowDsvDesc.Texture2DArray.FirstArraySlice = cascadeIdx;
            shadowDsvViews[cascadeIdx] = graph.CreateTextureDSVView(shadowMap, &shadowDsvDesc);
            if (!shadowDsvViews[cascadeIdx].IsValid())
            {
                return {};
            }

            shadowDsvHandles[cascadeIdx] = shadowDsvViews[cascadeIdx].cpuHandle;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
        shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2DArray.MipLevels = 1;
        shadowSrvDesc.Texture2DArray.ArraySize = NUM_CASCADES;

        RDGTextureSRVHandle shadowMapSrv = graph.CreateTextureSRVView(shadowMap, &shadowSrvDesc);
        if (!shadowMapSrv.IsValid())
        {
            return {};
        }

        RDGPassParameters params;
        params.WriteDSV(shadowDsvViews[0]);

        RDGPassHandle pass = graph.AddPass(
            "ShadowMap",
            ERDGPassFlags::Graphics,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    frameIndex,
                    shadowVisibleInstancesByCascade,
                    shadowInstanceOffsets,
                    visibleInstancesSize,
                    shadowDsvHandles);
            });

        return { shadowMap, shadowMapSrv, pass };
    }

    static void ExecuteRDG(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int frameIndex,
        const std::array<std::vector<ModelInstance*>, NUM_CASCADES>& shadowVisibleInstancesByCascade,
        const std::array<size_t, NUM_CASCADES>& shadowInstanceOffsets,
        size_t visibleInstancesSize)
    {
        RDGBuilder graph(deviceContext, "ShadowGraph");
        graph.SetTransientResourceAllocator(
            [deviceContext, resourceManager, frameIndex](
                const D3D12_RESOURCE_DESC& resourceDesc,
                D3D12_RESOURCE_STATES initialState,
                D3D12_RESOURCE_STATES finalState,
                const D3D12_CLEAR_VALUE* clearValue,
                Microsoft::WRL::ComPtr<ID3D12Resource>* outResource)
            {
                return resourceManager->AllocateRDGTransientResource(
                    deviceContext,
                    frameIndex,
                    resourceDesc,
                    initialState,
                    finalState,
                    clearValue,
                    outResource);
            });
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
            frameIndex,
            shadowVisibleInstancesByCascade,
            shadowInstanceOffsets,
            visibleInstancesSize);

        graph.Execute(deviceContext->GetCommandList());
    }
};

#endif
