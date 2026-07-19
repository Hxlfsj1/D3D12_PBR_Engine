#ifndef OCCLUSION_CULL_PASS_H
#define OCCLUSION_CULL_PASS_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "HiZPass.h"
#include "RDG.h"

#include <cstring>
#include <vector>

class OcclusionCullPass
{
public:
    struct Output
    {
        RDGBufferHandle visibilityBuffer;
        RDGPassHandle clearPass;
        RDGPassHandle pass;
    };

    static Output AddToGraph(
        RDGBuilder& graph,
        RenderDevice* deviceContext,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        const DirectX::XMFLOAT4X4& viewProj,
        int width,
        int height,
        int frameIndex,
        UINT visibilityCount,
        const std::vector<OcclusionCandidateData>& candidates,
        const HiZPass::Output& hiz)
    {
        Output output = {};

        if (deviceContext == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            width <= 0 ||
            height <= 0 ||
            visibilityCount == 0 ||
            candidates.empty() ||
            hiz.levels.empty())
        {
            return output;
        }

        const UINT hizLevelCount = static_cast<UINT>(
            hiz.levels.size() > MaxHiZLevels ? MaxHiZLevels : hiz.levels.size());

        const size_t candidateBytes = candidates.size() * sizeof(OcclusionCandidateData);
        if (candidateBytes > MaxCandidateUploadBytes)
        {
            return output;
        }

        UINT8* candidateCpuAddress = resourceManager->GetCBVAddress(frameIndex);
        if (candidateCpuAddress == nullptr)
        {
            return output;
        }

        std::memcpy(candidateCpuAddress + CandidateUploadOffset, candidates.data(), candidateBytes);

        D3D12_GPU_VIRTUAL_ADDRESS candidateGpuAddress =
            resourceManager->GetCBVGPUAddress(frameIndex) + CandidateUploadOffset;

        if (candidateGpuAddress == 0)
        {
            return output;
        }

        RDGBufferDesc visibilityDesc = {};
        visibilityDesc.sizeInBytes = static_cast<uint64_t>(visibilityCount) * sizeof(UINT);
        visibilityDesc.structureByteStride = sizeof(UINT);
        visibilityDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        output.visibilityBuffer = graph.CreateBuffer(
            visibilityDesc,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            "OcclusionVisibility");

        if (!output.visibilityBuffer.IsValid())
        {
            return {};
        }

        RDGBufferUAVHandle visibilityUav = graph.CreateBufferUAVView(output.visibilityBuffer);
        if (!visibilityUav.IsValid())
        {
            return {};
        }

        output.clearPass = AddClearPass(
            graph,
            resourceManager,
            pipelineManager,
            visibilityUav,
            visibilityCount);

        if (!output.clearPass.IsValid())
        {
            return {};
        }

        Constants constants = {};
        constants.viewProj = viewProj;
        constants.candidateCount = static_cast<UINT>(candidates.size());
        constants.visibilityCount = visibilityCount;
        constants.visibilityBufferIdx = visibilityUav.descriptorIndex;
        constants.hizLevelCount = hizLevelCount;
        constants.screenWidth = static_cast<UINT>(width);
        constants.screenHeight = static_cast<UINT>(height);

        RDGPassParameters params = {};
        params.WriteUAV(visibilityUav);

        for (UINT level = 0; level < hizLevelCount; ++level)
        {
            RDGTextureSRVHandle hizSrv = graph.CreateTextureSRVView(hiz.levels[level]);
            if (!hizSrv.IsValid())
            {
                return {};
            }

            constants.hizTextureIdx[level] = hizSrv.descriptorIndex;
            params.ReadSRV(hizSrv);
        }

        output.pass = graph.AddPass(
            "OcclusionCull",
            ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                ExecuteNoBarrier(
                    cmdList,
                    resourceManager,
                    pipelineManager,
                    candidateGpuAddress,
                    constants);
            });

        return output;
    }

    static RDGPassHandle AddReadbackCopyToGraph(
        RDGBuilder& graph,
        const Output& input,
        ID3D12Resource* readbackBuffer,
        UINT64 byteSize)
    {
        if (!input.visibilityBuffer.IsValid() ||
            readbackBuffer == nullptr ||
            byteSize == 0)
        {
            return {};
        }

        RDGBufferHandle readback = graph.RegisterExternalBuffer(
            readbackBuffer,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_DEST,
            "OcclusionVisibilityReadback");

        ID3D12Resource* visibilityResource = graph.GetBufferResource(input.visibilityBuffer);
        if (!readback.IsValid() || visibilityResource == nullptr)
        {
            return {};
        }

        RDGPassParameters params = {};
        params.ReadCopySrc(input.visibilityBuffer);
        params.WriteCopyDst(readback);

        return graph.AddPass(
            "CopyOcclusionVisibilityToReadback",
            ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                if (cmdList != nullptr)
                {
                    cmdList->CopyBufferRegion(readbackBuffer, 0, visibilityResource, 0, byteSize);
                }
            });
    }

private:
    static constexpr UINT MaxHiZLevels = 16;
    static constexpr UINT64 CandidateUploadOffset = 13ull * 1024ull * 1024ull;
    static constexpr UINT64 MaxCandidateUploadBytes = 1ull * 1024ull * 1024ull;

    struct Constants
    {
        DirectX::XMFLOAT4X4 viewProj;
        UINT candidateCount = 0;
        UINT visibilityCount = 0;
        UINT visibilityBufferIdx = UINT_MAX;
        UINT hizLevelCount = 0;
        UINT screenWidth = 1;
        UINT screenHeight = 1;
        UINT hizTextureIdx[MaxHiZLevels] = {};
    };

    static RDGPassHandle AddClearPass(
        RDGBuilder& graph,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        RDGBufferUAVHandle visibilityUav,
        UINT visibilityCount)
    {
        if (!visibilityUav.IsValid() ||
            visibilityCount == 0)
        {
            return {};
        }

        RDGPassParameters params = {};
        params.WriteUAV(visibilityUav);

        return graph.AddPass(
            "ClearOcclusionVisibility",
            ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
            params,
            [=](ID3D12GraphicsCommandList* cmdList)
            {
                if (cmdList == nullptr ||
                    resourceManager == nullptr ||
                    pipelineManager == nullptr)
                {
                    return;
                }

                Constants constants = {};
                constants.visibilityCount = visibilityCount;
                constants.visibilityBufferIdx = visibilityUav.descriptorIndex;

                ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
                cmdList->SetDescriptorHeaps(1, heaps);
                cmdList->SetComputeRootSignature(pipelineManager->GetOcclusionCullRootSignature());
                cmdList->SetPipelineState(pipelineManager->GetOcclusionClearPSO());
                cmdList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(UINT), &constants, 0);

                const UINT groupCountX = (visibilityCount + 63) / 64;
                cmdList->Dispatch(groupCountX, 1, 1);
            });
    }

    static void ExecuteNoBarrier(
        ID3D12GraphicsCommandList* cmdList,
        ResourceManager* resourceManager,
        PipelineManager* pipelineManager,
        D3D12_GPU_VIRTUAL_ADDRESS candidateGpuAddress,
        const Constants& constants)
    {
        if (cmdList == nullptr ||
            resourceManager == nullptr ||
            pipelineManager == nullptr ||
            candidateGpuAddress == 0 ||
            constants.candidateCount == 0)
        {
            return;
        }

        ID3D12DescriptorHeap* heaps[] = { resourceManager->GetMainDescriptorHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(pipelineManager->GetOcclusionCullRootSignature());
        cmdList->SetPipelineState(pipelineManager->GetOcclusionCullPSO());
        cmdList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(UINT), &constants, 0);
        cmdList->SetComputeRootShaderResourceView(1, candidateGpuAddress);

        const UINT groupCountX = (constants.candidateCount + 63) / 64;
        cmdList->Dispatch(groupCountX, 1, 1);
    }
};

#endif
