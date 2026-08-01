#ifndef SCALAR_TEMPORAL_FILTER_PASS_H
#define SCALAR_TEMPORAL_FILTER_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RDG.h"

#include <algorithm>
#include <cmath>

class ScalarTemporalFilterPass
{
public:
    enum class Signal : UINT
    {
        AmbientOcclusion = 0,
        ShadowVisibility,
        Count
    };

    struct Settings
    {
        Signal signal = Signal::AmbientOcclusion;
        float historyWeight = 0.90f;
        float depthThreshold = 0.0015f;
        float normalThreshold = 0.85f;
        float motionSensitivity = 0.02f;
        float signalDifferenceSensitivity = 1.50f;
        float backgroundValue = 1.0f;
        bool enableGeometryRejection = true;
    };

    struct Input
    {
        RDGTextureHandle currentSignal;
        RDGTextureHandle previousHistory;
        RDGTextureHandle historyOutput;
        RDGTextureHandle depth;
        RDGTextureHandle motion;

        // Optional. Geometry rejection is disabled unless all three are valid.
        // Depth must contain previous-frame device Z; both normal textures must
        // use the same world-space GBuffer normal encoding.
        RDGTextureHandle normal;
        RDGTextureHandle previousDepth;
        RDGTextureHandle previousNormal;
    };

    struct Output
    {
        RDGTextureHandle historyTexture;
        RDGPassHandle pass;
    };

    static Settings GetAmbientOcclusionSettings(float frameDeltaSeconds)
    {
        Settings settings;
        constexpr float historyHalfLifeSeconds = 0.20f;
        const float safeDeltaSeconds =
            std::clamp(frameDeltaSeconds, 1.0f / 10000.0f, 0.10f);

        settings.historyWeight = std::exp2(
            -safeDeltaSeconds / historyHalfLifeSeconds);
        settings.motionSensitivity = 0.005f;
        settings.signalDifferenceSensitivity = 0.25f;
        return settings;
    }

    static Settings GetShadowVisibilitySettings()
    {
        Settings settings;
        settings.signal = Signal::ShadowVisibility;
        settings.historyWeight = 0.85f;
        settings.signalDifferenceSensitivity = 2.0f;
        return settings;
    }

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProj,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProj,
        int frameIndex,
        int width,
        int height,
        bool historyValid,
        const Settings& settings,
        const Input& input)
    {
        const UINT signalIndex = static_cast<UINT>(settings.signal);
        if (deviceContext == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            signalIndex >= static_cast<UINT>(Signal::Count) ||
            width <= 0 ||
            height <= 0 ||
            !input.currentSignal.IsValid() ||
            !input.previousHistory.IsValid() ||
            !input.historyOutput.IsValid() ||
            !input.depth.IsValid())
        {
            return {};
        }

        const bool useGeometryRejection =
            settings.enableGeometryRejection &&
            input.normal.IsValid() &&
            input.previousDepth.IsValid() &&
            input.previousNormal.IsValid();

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        RDGTextureSRVHandle currentSignalSrv = graph.CreateTextureSRVView(input.currentSignal);
        RDGTextureSRVHandle previousHistorySrv = graph.CreateTextureSRVView(input.previousHistory);
        RDGTextureSRVHandle depthSrv = graph.CreateTextureSRVView(input.depth, &depthSrvDesc);
        RDGTextureSRVHandle motionSrv = {};
        RDGTextureSRVHandle normalSrv = {};
        RDGTextureSRVHandle previousDepthSrv = {};
        RDGTextureSRVHandle previousNormalSrv = {};

        if (input.motion.IsValid())
        {
            motionSrv = graph.CreateTextureSRVView(input.motion);
        }

        if (useGeometryRejection)
        {
            normalSrv = graph.CreateTextureSRVView(input.normal);
            previousDepthSrv = graph.CreateTextureSRVView(input.previousDepth, &depthSrvDesc);
            previousNormalSrv = graph.CreateTextureSRVView(input.previousNormal);
        }

        RDGTextureRTVHandle historyOutputRtv = graph.CreateTextureRTVView(input.historyOutput);

        if (!currentSignalSrv.IsValid() ||
            !previousHistorySrv.IsValid() ||
            !depthSrv.IsValid() ||
            !historyOutputRtv.IsValid() ||
            (input.motion.IsValid() && !motionSrv.IsValid()) ||
            (useGeometryRejection &&
                (!normalSrv.IsValid() ||
                 !previousDepthSrv.IsValid() ||
                 !previousNormalSrv.IsValid())))
        {
            return {};
        }

        TextureViews views = {};
        views.outputRtv = historyOutputRtv.cpuHandle;
        views.currentSignalSrvIdx = currentSignalSrv.descriptorIndex;
        views.previousHistorySrvIdx = previousHistorySrv.descriptorIndex;
        views.depthSrvIdx = depthSrv.descriptorIndex;
        views.motionSrvIdx = motionSrv.IsValid() ? motionSrv.descriptorIndex : UINT_MAX;
        views.normalSrvIdx = normalSrv.IsValid() ? normalSrv.descriptorIndex : UINT_MAX;
        views.previousDepthSrvIdx = previousDepthSrv.IsValid() ? previousDepthSrv.descriptorIndex : UINT_MAX;
        views.previousNormalSrvIdx = previousNormalSrv.IsValid() ? previousNormalSrv.descriptorIndex : UINT_MAX;

        RDGPassParameters parameters;
        parameters.ReadSRV(currentSignalSrv);
        parameters.ReadSRV(previousHistorySrv);
        parameters.ReadSRV(depthSrv);
        if (motionSrv.IsValid())
        {
            parameters.ReadSRV(motionSrv);
        }
        if (useGeometryRejection)
        {
            parameters.ReadSRV(normalSrv);
            parameters.ReadSRV(previousDepthSrv);
            parameters.ReadSRV(previousNormalSrv);
        }
        parameters.WriteRTV(historyOutputRtv);

        graph.MarkTextureAsOutput(input.historyOutput);

        const char* passName =
            settings.signal == Signal::ShadowVisibility
            ? "ScalarTemporalShadow"
            : "ScalarTemporalAO";

        RDGPassHandle pass = graph.AddPass(
            passName,
            ERDGPassFlags::Graphics,
            parameters,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    deviceContext,
                    resourceManager,
                    pipelineManager,
                    currJitteredInvViewProj,
                    prevUnjitteredViewProj,
                    frameIndex,
                    width,
                    height,
                    historyValid,
                    useGeometryRejection,
                    settings,
                    views);
            });

        return { input.historyOutput, pass };
    }

private:
    struct TextureViews
    {
        D3D12_CPU_DESCRIPTOR_HANDLE outputRtv = {};
        UINT currentSignalSrvIdx = UINT_MAX;
        UINT previousHistorySrvIdx = UINT_MAX;
        UINT depthSrvIdx = UINT_MAX;
        UINT motionSrvIdx = UINT_MAX;
        UINT normalSrvIdx = UINT_MAX;
        UINT previousDepthSrvIdx = UINT_MAX;
        UINT previousNormalSrvIdx = UINT_MAX;
    };

    struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) Constants
    {
        DirectX::XMFLOAT4X4 currJitteredInvViewProj;
        DirectX::XMFLOAT4X4 prevUnjitteredViewProj;

        float historyWeight;
        float depthThreshold;
        float normalThreshold;
        float motionSensitivity;

        float signalDifferenceSensitivity;
        float backgroundValue;
        DirectX::XMFLOAT2 resolution;

        UINT historyValid;
        UINT useGeometryRejection;
        UINT currentSignalTextureIdx;
        UINT previousHistoryTextureIdx;

        UINT depthTextureIdx;
        UINT motionTextureIdx;
        UINT normalTextureIdx;
        UINT previousDepthTextureIdx;

        UINT previousNormalTextureIdx;
        UINT padding[3];
    };

    static_assert(sizeof(Constants) == D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    static void ExecuteNoBarrier(
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& currJitteredInvViewProj,
        const DirectX::XMFLOAT4X4& prevUnjitteredViewProj,
        int frameIndex,
        int width,
        int height,
        bool historyValid,
        bool useGeometryRejection,
        const Settings& settings,
        const TextureViews& views)
    {
        ID3D12GraphicsCommandList* cmdList = deviceContext->GetCommandList();

        cmdList->OMSetRenderTargets(1, &views.outputRtv, FALSE, nullptr);
        const float clearValue[] =
        {
            settings.backgroundValue,
            settings.backgroundValue,
            settings.backgroundValue,
            1.0f
        };
        cmdList->ClearRenderTargetView(views.outputRtv, clearValue, 0, nullptr);

        D3D12_VIEWPORT viewport =
        {
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f
        };
        D3D12_RECT scissorRect = { 0, 0, width, height };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        cmdList->SetGraphicsRootSignature(pipelineManager->GetScalarTemporalRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetScalarTemporalPSO());

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        constexpr UINT64 constantsPageOffset = 1024ull * 1024ull * 11ull;
        constexpr UINT64 constantsStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        const UINT signalIndex = static_cast<UINT>(settings.signal);
        const UINT64 constantsOffset = constantsPageOffset + signalIndex * constantsStride;

        Constants constants = {};
        constants.currJitteredInvViewProj = currJitteredInvViewProj;
        constants.prevUnjitteredViewProj = prevUnjitteredViewProj;
        constants.historyWeight = settings.historyWeight;
        constants.depthThreshold = settings.depthThreshold;
        constants.normalThreshold = settings.normalThreshold;
        constants.motionSensitivity = settings.motionSensitivity;
        constants.signalDifferenceSensitivity = settings.signalDifferenceSensitivity;
        constants.backgroundValue = settings.backgroundValue;
        constants.resolution = DirectX::XMFLOAT2(
            static_cast<float>(width),
            static_cast<float>(height));
        constants.historyValid = historyValid ? 1u : 0u;
        constants.useGeometryRejection = useGeometryRejection ? 1u : 0u;
        constants.currentSignalTextureIdx = views.currentSignalSrvIdx;
        constants.previousHistoryTextureIdx = views.previousHistorySrvIdx;
        constants.depthTextureIdx = views.depthSrvIdx;
        constants.motionTextureIdx = views.motionSrvIdx;
        constants.normalTextureIdx = views.normalSrvIdx;
        constants.previousDepthTextureIdx = views.previousDepthSrvIdx;
        constants.previousNormalTextureIdx = views.previousNormalSrvIdx;

        UINT8* constantsCpuAddress =
            resourceManager->GetCBVAddress(frameIndex) + constantsOffset;
        D3D12_GPU_VIRTUAL_ADDRESS constantsGpuAddress =
            resourceManager->GetCBVGPUAddress(frameIndex) + constantsOffset;
        memcpy(constantsCpuAddress, &constants, sizeof(constants));

        cmdList->SetGraphicsRootConstantBufferView(0, constantsGpuAddress);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
};

#endif
