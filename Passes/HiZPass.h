#ifndef HIZ_PASS_H
#define HIZ_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RDG.h"

#include <vector>

class HiZPass
{
public:
    struct Output
    {
        std::vector<RDGTextureHandle> levels;
        std::vector<RDGPassHandle> passes;
    };

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        int width,
        int height,
        RDGTextureHandle depth)
    {
        Output output = {};

        if (!depth.IsValid() ||
            deviceContext == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            width <= 0 ||
            height <= 0)
        {
            return output;
        }

        const uint32_t mipCount = CalculateMipCount(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        output.levels.reserve(mipCount);
        output.passes.reserve(mipCount);

        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            const uint32_t mipWidth = CalculateMipDimension(static_cast<uint32_t>(width), mip);
            const uint32_t mipHeight = CalculateMipDimension(static_cast<uint32_t>(height), mip);

            RDGTextureDesc desc = {};
            desc.width = mipWidth;
            desc.height = mipHeight;
            desc.format = DXGI_FORMAT_R32_FLOAT;
            desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            output.levels.push_back(graph.CreateTexture(
                desc,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COMMON,
                "HiZLevel"));

            if (!output.levels.back().IsValid())
            {
                output.levels.clear();
                output.passes.clear();
                return output;
            }
        }

        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            const uint32_t srcWidth = (mip == 0)
                ? static_cast<uint32_t>(width)
                : CalculateMipDimension(static_cast<uint32_t>(width), mip - 1);
            const uint32_t srcHeight = (mip == 0)
                ? static_cast<uint32_t>(height)
                : CalculateMipDimension(static_cast<uint32_t>(height), mip - 1);
            const uint32_t dstWidth = CalculateMipDimension(static_cast<uint32_t>(width), mip);
            const uint32_t dstHeight = CalculateMipDimension(static_cast<uint32_t>(height), mip);

            RDGTextureSRVHandle srcSrv = {};
            if (mip == 0)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
                depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                depthSrvDesc.Texture2D.MipLevels = 1;

                srcSrv = graph.CreateTextureSRVView(depth, &depthSrvDesc);
            }
            else
            {
                srcSrv = graph.CreateTextureSRVView(output.levels[mip - 1]);
            }

            RDGTextureUAVHandle dstUav = graph.CreateTextureUAVView(output.levels[mip]);
            if (!srcSrv.IsValid() || !dstUav.IsValid())
            {
                output.levels.clear();
                output.passes.clear();
                return output;
            }

            RDGPassParameters params = {};
            params.ReadSRV(srcSrv);
            params.WriteUAV(dstUav);

            RDGPassHandle pass = graph.AddPass(
                mip == 0 ? "HiZCopyDepth" : "HiZReduce",
                ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
                params,
                [=](ID3D12GraphicsCommandList* cmdList)
                {
                    ExecuteNoBarrier(
                        cmdList,
                        resourceManager,
                        pipelineManager,
                        srcSrv.descriptorIndex,
                        dstUav.descriptorIndex,
                        srcWidth,
                        srcHeight,
                        dstWidth,
                        dstHeight);
                });

            output.passes.push_back(pass);
        }

        return output;
    }

private:
    struct Constants
    {
        UINT srcTextureIdx = UINT_MAX;
        UINT dstTextureIdx = UINT_MAX;
        UINT srcWidth = 1;
        UINT srcHeight = 1;
        UINT dstWidth = 1;
        UINT dstHeight = 1;
    };

    static uint32_t CalculateMipCount(uint32_t width, uint32_t height)
    {
        uint32_t count = 1;
        while (width > 1 || height > 1)
        {
            width = (width + 1) >> 1;
            height = (height + 1) >> 1;
            ++count;
        }
        return count;
    }

    static uint32_t CalculateMipDimension(uint32_t baseSize, uint32_t mip)
    {
        uint32_t size = baseSize;
        for (uint32_t i = 0; i < mip; ++i)
        {
            size = (size + 1) >> 1;
        }
        return size > 0 ? size : 1;
    }

    static void ExecuteNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        UINT srcTextureIdx,
        UINT dstTextureIdx,
        uint32_t srcWidth,
        uint32_t srcHeight,
        uint32_t dstWidth,
        uint32_t dstHeight)
    {
        if (cmdList == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr)
        {
            return;
        }

        Constants constants = {};
        constants.srcTextureIdx = srcTextureIdx;
        constants.dstTextureIdx = dstTextureIdx;
        constants.srcWidth = srcWidth;
        constants.srcHeight = srcHeight;
        constants.dstWidth = dstWidth;
        constants.dstHeight = dstHeight;

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(pipelineManager->GetHiZRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetHiZPSO());
        cmdList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(UINT), &constants, 0);

        const UINT groupCountX = (dstWidth + 7) / 8;
        const UINT groupCountY = (dstHeight + 7) / 8;
        cmdList->Dispatch(groupCountX, groupCountY, 1);
    }
};

#endif
