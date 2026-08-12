#ifndef DLSS_PASS_H
#define DLSS_PASS_H

#include "stdafx.h"
#include "DLSSManager.h"
#include "ResourceManager.h"
#include "RDG.h"

#include <cmath>

class DLSSPass
{
public:
    static DirectX::XMFLOAT2 CalculateJitter(
        UINT frameIndex,
        int renderWidth,
        int outputWidth)
    {
        const float resolutionRatio =
            static_cast<float>((std::max)(outputWidth, 1)) /
            static_cast<float>((std::max)(renderWidth, 1));
        const UINT sampleCount = (std::max)(
            8u,
            static_cast<UINT>(std::ceil(8.0f * resolutionRatio * resolutionRatio)));
        const UINT sampleIndex = (frameIndex % sampleCount) + 1u;

        return DirectX::XMFLOAT2(
            RadicalInverse(sampleIndex, 2u) - 0.5f,
            RadicalInverse(sampleIndex, 3u) - 0.5f);
    }

    struct Input
    {
        RDGTextureHandle color;
        RDGTextureHandle depth;
        RDGTextureHandle motionVectors;
    };

    struct Output
    {
        RDGTextureHandle outputTexture;
        RDGPassHandle pass;
    };

    static Output AddToGraph(
        RDGBuilder& graph,
        DLSSManager* dlssManager,
        ResourceManager* resourceManager,
        float jitterOffsetX,
        float jitterOffsetY,
        float frameTimeDeltaInMsec,
        bool reset,
        const Input& input)
    {
        if (dlssManager == nullptr ||
            resourceManager == nullptr ||
            !dlssManager->CanEvaluate() ||
            !input.color.IsValid() ||
            !input.depth.IsValid() ||
            !input.motionVectors.IsValid())
        {
            return {};
        }

        ID3D12Resource* dlssOutputResource = resourceManager->GetDLSSOutput();
        if (dlssOutputResource == nullptr)
        {
            return {};
        }

        RDGTextureHandle outputTexture = graph.RegisterExternalTextureOutput(
            dlssOutputResource,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON,
            "DLSSOutput");

        DLSSManager::EvaluationInput evaluationInput = {};
        evaluationInput.color = graph.GetTextureResource(input.color);
        evaluationInput.depth = graph.GetTextureResource(input.depth);
        evaluationInput.motionVectors = graph.GetTextureResource(input.motionVectors);
        evaluationInput.output = graph.GetTextureResource(outputTexture);
        evaluationInput.jitterOffsetX = jitterOffsetX;
        evaluationInput.jitterOffsetY = jitterOffsetY;
        evaluationInput.frameTimeDeltaInMsec = frameTimeDeltaInMsec;
        evaluationInput.reset = reset;

        if (evaluationInput.color == nullptr ||
            evaluationInput.depth == nullptr ||
            evaluationInput.motionVectors == nullptr ||
            evaluationInput.output == nullptr)
        {
            return {};
        }

        RDGPassParameters parameters;
        parameters.ReadSRV(input.color);
        parameters.ReadSRV(input.depth);
        parameters.ReadSRV(input.motionVectors);
        parameters.WriteUAV(outputTexture);

        RDGPassHandle pass = graph.AddPass(
            "DLSS",
            ERDGPassFlags::Compute,
            parameters,
            [dlssManager, evaluationInput](ID3D12GraphicsCommandList* commandList)
            {
                dlssManager->EvaluateFeature(commandList, evaluationInput);
            });

        return { outputTexture, pass };
    }

private:
    static float RadicalInverse(UINT value, UINT base)
    {
        const float inverseBase = 1.0f / static_cast<float>(base);
        float inversePower = inverseBase;
        float result = 0.0f;

        while (value != 0)
        {
            result += static_cast<float>(value % base) * inversePower;
            value /= base;
            inversePower *= inverseBase;
        }

        return result;
    }
};

#endif
