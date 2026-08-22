#ifndef SMAA_PASS_H
#define SMAA_PASS_H

#include "stdafx.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RDG.h"

namespace SMAALookupData
{
#include "LUTs/AreaTex.h"
#include "LUTs/SearchTex.h"

    constexpr UINT AreaWidth = AREATEX_WIDTH;
    constexpr UINT AreaHeight = AREATEX_HEIGHT;
    constexpr size_t AreaByteCount = AREATEX_SIZE;
    constexpr UINT SearchWidth = SEARCHTEX_WIDTH;
    constexpr UINT SearchHeight = SEARCHTEX_HEIGHT;
    constexpr size_t SearchByteCount = SEARCHTEX_SIZE;

    static_assert(sizeof(areaTexBytes) == AreaByteCount);
    static_assert(sizeof(searchTexBytes) == SearchByteCount);
}

#undef AREATEX_WIDTH
#undef AREATEX_HEIGHT
#undef AREATEX_PITCH
#undef AREATEX_SIZE
#undef SEARCHTEX_WIDTH
#undef SEARCHTEX_HEIGHT
#undef SEARCHTEX_PITCH
#undef SEARCHTEX_SIZE

struct SMAAConstants
{
    DirectX::XMFLOAT4 rtMetrics;

    UINT colorTextureIdx = UINT_MAX;
    UINT edgesTextureIdx = UINT_MAX;
    UINT blendTextureIdx = UINT_MAX;
    UINT areaTextureIdx = UINT_MAX;

    UINT searchTextureIdx = UINT_MAX;
    float edgeThreshold = 0.1f;
    float localContrastAdaptationFactor = 2.0f;
    UINT padding = 0;
};

static_assert(sizeof(SMAAConstants) == 12 * sizeof(UINT));

class SMAAPass
{
public:
    struct Input
    {
        // SMAA edge detection expects display-referred, tone-mapped color.
        RDGTextureHandle color;

        // Optional render target. If omitted, SMAA creates an RGBA8 transient output.
        RDGTextureHandle output;
    };

    struct EdgeOutput
    {
        RDGTextureHandle edgeTexture;
        RDGPassHandle edgePass;
    };

    struct WeightOutput
    {
        RDGTextureHandle edgeTexture;
        RDGTextureHandle blendWeightTexture;
        RDGPassHandle edgePass;
        RDGPassHandle weightPass;
    };

    struct Output
    {
        RDGTextureHandle color;
        RDGTextureHandle edgeTexture;
        RDGTextureHandle blendWeightTexture;
        RDGPassHandle edgePass;
        RDGPassHandle weightPass;
        RDGPassHandle neighborhoodPass;
    };

    static bool InitializeLookupTextures(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager)
    {
        if (areaTexture && searchTexture)
        {
            return true;
        }

        if (deviceContext == nullptr ||
            resourceManager == nullptr ||
            resourceManager->GetMainDescriptorHeap() == nullptr)
        {
            return false;
        }

        const int frameIndex = static_cast<int>(
            deviceContext->GetSwapChain()->GetCurrentBackBufferIndex());
        if (FAILED(deviceContext->GetCommandAllocator(frameIndex)->Reset()) ||
            FAILED(deviceContext->GetCommandList()->Reset(
                deviceContext->GetCommandAllocator(frameIndex),
                nullptr)))
        {
            return false;
        }

        ID3D12GraphicsCommandList* commandList = deviceContext->GetCommandList();
        if (!CreateLookupTexture(
                deviceContext,
                resourceManager,
                commandList,
                SMAALookupData::areaTexBytes,
                SMAALookupData::AreaWidth,
                SMAALookupData::AreaHeight,
                DXGI_FORMAT_R8G8_UNORM,
                2,
                L"SMAA.AreaTex",
                areaTexture,
                areaUpload,
                areaTextureIdx) ||
            !CreateLookupTexture(
                deviceContext,
                resourceManager,
                commandList,
                SMAALookupData::searchTexBytes,
                SMAALookupData::SearchWidth,
                SMAALookupData::SearchHeight,
                DXGI_FORMAT_R8_UNORM,
                1,
                L"SMAA.SearchTex",
                searchTexture,
                searchUpload,
                searchTextureIdx))
        {
            return false;
        }

        if (FAILED(commandList->Close()))
        {
            return false;
        }

        ID3D12CommandList* commandLists[] = { commandList };
        deviceContext->GetCommandQueue()->ExecuteCommandLists(1, commandLists);

        ++deviceContext->GetFenceValue(frameIndex);
        if (FAILED(deviceContext->GetCommandQueue()->Signal(
                deviceContext->GetFence(frameIndex),
                deviceContext->GetFenceValue(frameIndex))))
        {
            return false;
        }

        if (deviceContext->GetFence(frameIndex)->GetCompletedValue() <
            deviceContext->GetFenceValue(frameIndex))
        {
            if (FAILED(deviceContext->GetFence(frameIndex)->SetEventOnCompletion(
                    deviceContext->GetFenceValue(frameIndex),
                    deviceContext->GetFenceEvent())))
            {
                return false;
            }
            WaitForSingleObject(deviceContext->GetFenceEvent(), INFINITE);
        }

        return true;
    }

    static EdgeOutput AddEdgeDetectionToGraph(
        RDGBuilder& graph,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        const Input& input)
    {
        if (resourceManager == nullptr ||
            pipelineManager == nullptr ||
            width <= 0 ||
            height <= 0 ||
            !input.color.IsValid())
        {
            return {};
        }

        RDGTextureDesc edgeDesc = {};
        edgeDesc.width = static_cast<uint32_t>(width);
        edgeDesc.height = static_cast<uint32_t>(height);
        edgeDesc.format = DXGI_FORMAT_R8G8_UNORM;
        edgeDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        edgeDesc.hasClearValue = true;
        edgeDesc.clearValue.Format = edgeDesc.format;

        RDGTextureHandle edgeTexture = graph.CreateTexture(
            edgeDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "SMAA.Edges");
        if (!edgeTexture.IsValid())
        {
            return {};
        }

        RDGTextureSRVHandle colorSrv = graph.CreateTextureSRVView(input.color);
        RDGTextureRTVHandle edgeRtv = graph.CreateTextureRTVView(edgeTexture);
        if (!colorSrv.IsValid() || !edgeRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters parameters = {};
        parameters.ReadSRV(colorSrv);
        parameters.WriteRTV(edgeRtv);

        RDGPassHandle edgePass = graph.AddPass(
            "SMAA.EdgeDetection",
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteEdgeDetectionNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    width,
                    height,
                    edgeRtv.cpuHandle,
                    colorSrv.descriptorIndex);
            });

        return { edgeTexture, edgePass };
    }

    static WeightOutput AddBlendingWeightsToGraph(
        RDGBuilder& graph,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        const Input& input)
    {
        if (!areaTexture ||
            !searchTexture ||
            areaTextureIdx == UINT_MAX ||
            searchTextureIdx == UINT_MAX)
        {
            return {};
        }

        const EdgeOutput edgeOutput = AddEdgeDetectionToGraph(
            graph,
            resourceManager,
            pipelineManager,
            width,
            height,
            input);
        if (!edgeOutput.edgeTexture.IsValid() || !edgeOutput.edgePass.IsValid())
        {
            return {};
        }

        RDGTextureDesc blendWeightDesc = {};
        blendWeightDesc.width = static_cast<uint32_t>(width);
        blendWeightDesc.height = static_cast<uint32_t>(height);
        blendWeightDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        blendWeightDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        blendWeightDesc.hasClearValue = true;
        blendWeightDesc.clearValue.Format = blendWeightDesc.format;

        RDGTextureHandle blendWeightTexture = graph.CreateTexture(
            blendWeightDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "SMAA.BlendWeights");
        if (!blendWeightTexture.IsValid())
        {
            return {};
        }

        RDGTextureSRVHandle edgeSrv = graph.CreateTextureSRVView(
            edgeOutput.edgeTexture);
        RDGTextureRTVHandle blendWeightRtv = graph.CreateTextureRTVView(
            blendWeightTexture);
        if (!edgeSrv.IsValid() || !blendWeightRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters parameters = {};
        parameters.ReadSRV(edgeSrv);
        parameters.WriteRTV(blendWeightRtv);

        RDGPassHandle weightPass = graph.AddPassAfter(
            edgeOutput.edgePass,
            "SMAA.BlendingWeights",
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteBlendingWeightsNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    width,
                    height,
                    blendWeightRtv.cpuHandle,
                    edgeSrv.descriptorIndex);
            });

        return {
            edgeOutput.edgeTexture,
            blendWeightTexture,
            edgeOutput.edgePass,
            weightPass
        };
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        const Input& input)
    {
        if (width <= 0 || height <= 0 || !input.color.IsValid())
        {
            return {};
        }

        const RDGTextureDesc* colorDesc = graph.GetTextureDesc(input.color);
        if (colorDesc == nullptr ||
            colorDesc->width != static_cast<uint32_t>(width) ||
            colorDesc->height != static_cast<uint32_t>(height))
        {
            return {};
        }

        const WeightOutput weightOutput = AddBlendingWeightsToGraph(
            graph,
            resourceManager,
            pipelineManager,
            width,
            height,
            input);
        if (!weightOutput.blendWeightTexture.IsValid() ||
            !weightOutput.weightPass.IsValid())
        {
            return {};
        }

        RDGTextureHandle outputTexture = input.output;
        if (outputTexture.IsValid())
        {
            if (outputTexture.index == input.color.index)
            {
                return {};
            }

            const RDGTextureDesc* outputDesc = graph.GetTextureDesc(outputTexture);
            if (outputDesc == nullptr ||
                outputDesc->width != static_cast<uint32_t>(width) ||
                outputDesc->height != static_cast<uint32_t>(height) ||
                outputDesc->format != DXGI_FORMAT_R8G8B8A8_UNORM ||
                (outputDesc->flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
            {
                return {};
            }
        }
        else
        {
            RDGTextureDesc outputDesc = {};
            outputDesc.width = static_cast<uint32_t>(width);
            outputDesc.height = static_cast<uint32_t>(height);
            outputDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            outputDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            outputDesc.hasClearValue = true;
            outputDesc.clearValue.Format = outputDesc.format;

            outputTexture = graph.CreateTexture(
                outputDesc,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COMMON,
                "SMAA.Output");
            if (!outputTexture.IsValid())
            {
                return {};
            }
        }

        RDGTextureSRVHandle colorSrv = graph.CreateTextureSRVView(input.color);
        RDGTextureSRVHandle blendWeightSrv = graph.CreateTextureSRVView(
            weightOutput.blendWeightTexture);
        RDGTextureRTVHandle outputRtv = graph.CreateTextureRTVView(outputTexture);
        if (!colorSrv.IsValid() ||
            !blendWeightSrv.IsValid() ||
            !outputRtv.IsValid())
        {
            return {};
        }

        RDGPassParameters parameters = {};
        parameters.ReadSRV(colorSrv);
        parameters.ReadSRV(blendWeightSrv);
        parameters.WriteRTV(outputRtv);

        RDGPassHandle neighborhoodPass = graph.AddPassAfter(
            weightOutput.weightPass,
            "SMAA.NeighborhoodBlending",
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNeighborhoodBlendingNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    width,
                    height,
                    outputRtv.cpuHandle,
                    colorSrv.descriptorIndex,
                    blendWeightSrv.descriptorIndex);
            });

        return {
            outputTexture,
            weightOutput.edgeTexture,
            weightOutput.blendWeightTexture,
            weightOutput.edgePass,
            weightOutput.weightPass,
            neighborhoodPass
        };
    }

private:
    static bool CreateLookupTexture(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        ID3D12GraphicsCommandList* commandList,
        const uint8_t* sourceData,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        UINT bytesPerPixel,
        const wchar_t* debugName,
        ComPtr<ID3D12Resource>& texture,
        ComPtr<ID3D12Resource>& upload,
        UINT& descriptorIndex)
    {
        ID3D12Device* device = deviceContext->GetDevice();
        const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            format,
            width,
            height,
            1,
            1);
        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &textureDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&texture))))
        {
            return false;
        }
        texture->SetName(debugName);

        const UINT64 uploadSize = GetRequiredIntermediateSize(
            texture.Get(),
            0,
            1);
        const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC uploadDesc =
            CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload))))
        {
            return false;
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = sourceData;
        subresource.RowPitch = static_cast<LONG_PTR>(width) * bytesPerPixel;
        subresource.SlicePitch = subresource.RowPitch * height;
        if (UpdateSubresources(
                commandList,
                texture.Get(),
                upload.Get(),
                0,
                0,
                1,
                &subresource) == 0)
        {
            return false;
        }

        const CD3DX12_RESOURCE_BARRIER barrier =
            CD3DX12_RESOURCE_BARRIER::Transition(
                texture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &barrier);

        descriptorIndex = resourceManager->AllocateSrvUavDescriptor();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            texture.Get(),
            &srvDesc,
            resourceManager->GetSrvUavCPUHandle(descriptorIndex));

        return true;
    }

    static void ExecuteEdgeDetectionNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        D3D12_CPU_DESCRIPTOR_HANDLE edgeRtv,
        UINT colorSrvIdx)
    {
        cmdList->OMSetRenderTargets(1, &edgeRtv, FALSE, nullptr);
        const float clearEdges[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(edgeRtv, clearEdges, 0, nullptr);

        const D3D12_VIEWPORT viewport =
        {
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f
        };
        const D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetSMAARootSignature());
        cmdList->SetPipelineState(pipelineManager->GetSMAAEdgePSO());

        ID3D12DescriptorHeap* descriptorHeaps[] =
        {
            resourceManager->GetMainDescriptorHeap()
        };
        cmdList->SetDescriptorHeaps(1, descriptorHeaps);

        SMAAConstants constants = {};
        constants.rtMetrics = DirectX::XMFLOAT4(
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            static_cast<float>(width),
            static_cast<float>(height));
        constants.colorTextureIdx = colorSrvIdx;
        constants.edgeThreshold = 0.1f;
        constants.localContrastAdaptationFactor = 2.0f;

        cmdList->SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(sizeof(SMAAConstants) / sizeof(UINT)),
            &constants,
            0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteBlendingWeightsNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        D3D12_CPU_DESCRIPTOR_HANDLE blendWeightRtv,
        UINT edgeSrvIdx)
    {
        cmdList->OMSetRenderTargets(1, &blendWeightRtv, FALSE, nullptr);
        const float clearWeights[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cmdList->ClearRenderTargetView(
            blendWeightRtv,
            clearWeights,
            0,
            nullptr);

        const D3D12_VIEWPORT viewport =
        {
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f
        };
        const D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(
            pipelineManager->GetSMAARootSignature());
        cmdList->SetPipelineState(pipelineManager->GetSMAAWeightPSO());

        ID3D12DescriptorHeap* descriptorHeaps[] =
        {
            resourceManager->GetMainDescriptorHeap()
        };
        cmdList->SetDescriptorHeaps(1, descriptorHeaps);

        SMAAConstants constants = {};
        constants.rtMetrics = DirectX::XMFLOAT4(
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            static_cast<float>(width),
            static_cast<float>(height));
        constants.edgesTextureIdx = edgeSrvIdx;
        constants.areaTextureIdx = areaTextureIdx;
        constants.searchTextureIdx = searchTextureIdx;

        cmdList->SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(sizeof(SMAAConstants) / sizeof(UINT)),
            &constants,
            0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    static void ExecuteNeighborhoodBlendingNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv,
        UINT colorSrvIdx,
        UINT blendWeightSrvIdx)
    {
        cmdList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);

        const D3D12_VIEWPORT viewport =
        {
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f
        };
        const D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(
            pipelineManager->GetSMAARootSignature());
        cmdList->SetPipelineState(
            pipelineManager->GetSMAANeighborhoodPSO());

        ID3D12DescriptorHeap* descriptorHeaps[] =
        {
            resourceManager->GetMainDescriptorHeap()
        };
        cmdList->SetDescriptorHeaps(1, descriptorHeaps);

        SMAAConstants constants = {};
        constants.rtMetrics = DirectX::XMFLOAT4(
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            static_cast<float>(width),
            static_cast<float>(height));
        constants.colorTextureIdx = colorSrvIdx;
        constants.blendTextureIdx = blendWeightSrvIdx;

        cmdList->SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(sizeof(SMAAConstants) / sizeof(UINT)),
            &constants,
            0);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    inline static ComPtr<ID3D12Resource> areaTexture;
    inline static ComPtr<ID3D12Resource> areaUpload;
    inline static ComPtr<ID3D12Resource> searchTexture;
    inline static ComPtr<ID3D12Resource> searchUpload;
    inline static UINT areaTextureIdx = UINT_MAX;
    inline static UINT searchTextureIdx = UINT_MAX;
};

#endif
