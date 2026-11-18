#pragma once

#include "RenderDevice.h"
#include "PBR_Shader.h"
#include "VXGIGPUScene.h"
#include "Settings_Manager.h"
#include "VXGIGlobalSDFPass.h"
#include "SurfaceCacheCards.h"
#include <span>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Full-screen world-SDF diagnostics after post-process and before selection/UI.
// This renderer creates no editor widgets. Geometry reads use DEFAULT buffers;
// a swap-chain slot retains staging until its previous submission completes.
class VXGISDFDebugRenderer
{
public:
    bool Initialize(RenderDevice& deviceContext, UINT frameCount)
    {
        if (frameCount == 0 || !CreatePipeline(deviceContext.GetDevice(),
            deviceContext.GetRenderTarget(0)->GetDesc().Format)) return false;
        m_constants.resize(frameCount);
        m_cardBuffers.resize(frameCount);
        m_cardBufferBytes.resize(frameCount);
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(512);
        for(auto& buffer:m_constants)
            if(!Check(deviceContext.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"create diagnostic constants")) return false;
        return true;
    }

    // inverseViewProjectionGpu uses the engine's transposed GPU matrix convention.
    // Call once per frame after WaitForPreviousFrame; back buffer enters/leaves PRESENT.
    // globalSDFIndex is a copy of global.GetSRVCPUHandle() in engineHeap.
    // Modes: 4..7 Mesh geometry/shadow; 13..15 Global geometry; 17 hybrid hit;
    // 18 Card placement, 19 geometric capture coverage (no material atlas yet).
    bool RecordGeometry(RenderDevice& deviceContext, ID3D12DescriptorHeap* engineHeap, int frameIndex,
        const DirectX::XMFLOAT4X4& inverseViewProjectionGpu, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const VXGIGPUScene& scene, const VXGIGlobalSDFPass& global, UINT globalSDFIndex,
        const VXGIConfig& config, uint32_t debugMode, const DirectX::XMFLOAT3& sunDirectionTowardLight,
        D3D12_GPU_VIRTUAL_ADDRESS materialBuffer, uint32_t materialCount,
        std::span<const SurfaceCacheCards::DebugCard> cards = {})
    {
        if(frameIndex<0 || size_t(frameIndex)>=m_constants.size()) return false;
        if (!scene.GetGrid(frameIndex).valid || !scene.GetTriangleCount(frameIndex)) return true;
        if (!m_pipeline || !engineHeap || !scene.IsMeshBackend(frameIndex) || !scene.Get(frameIndex,0)) return false;
        const auto& grid = scene.GetGrid(frameIndex);
        const bool usesGlobal = debugMode >= 13 && debugMode <= 17;
        auto* cmd = deviceContext.GetCommandList();
        auto* backBuffer = deviceContext.GetRenderTarget(frameIndex);
        const auto output = backBuffer->GetDesc();
        Constants constants{};
        constants.inverseViewProjection = inverseViewProjectionGpu;
        constants.gridOrigin = grid.origin;
        constants.cellSize = grid.cellSize;
        constants.cellDimensions = grid.cellDimensions;
        constants.debugMode = debugMode;
        constants.boundsMaximum = grid.boundsMaximum;
        constants.triangleCount = scene.GetTriangleCount(frameIndex);
        DirectX::XMStoreFloat3(&constants.sunDirectionTowardLight,
            DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&sunDirectionTowardLight)));
        constants.sliceCoordinateWorld = config.sdfSlicePosition;
        constants.sliceAxis = static_cast<uint32_t>(config.sdfSliceAxis);
        constants.inverseOutputSize = { 1.0f / static_cast<float>(output.Width), 1.0f / static_cast<float>(output.Height) };
        constants.materialMipLevel = config.surfaceMaterialMipLevel;
        constants.rayMinDistance = config.surfaceRayMinDistance;
        constants.maxRayDistance = config.surfaceMaxRayDistance;
        constants.materialCount = materialCount;
        constants.globalSDFIndex = globalSDFIndex;
        constants.meshTraceDistance = config.meshTraceDistance;
        constants.globalTraceDistance = config.globalSDFTraceDistance;
        constants.globalMaxSteps = static_cast<uint32_t>(config.globalSDFMaxSteps);
        constants.cardCount = static_cast<uint32_t>(cards.size());
        constants.global = global.GetLayout().constants;

        if (!cards.empty())
        {
            const uint64_t bytes=cards.size_bytes();
            auto& buffer=m_cardBuffers[frameIndex];
            if(m_cardBufferBytes[frameIndex]!=bytes)
            {
                const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes);
                if(!Check(deviceContext.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())),"create Card debug buffer"))return false;
                m_cardBufferBytes[frameIndex]=bytes;
            }
            void* mappedCards=nullptr;const D3D12_RANGE noReads{0,0};
            if(!Check(buffer->Map(0,&noReads,&mappedCards),"map Card debug buffer"))return false;
            std::memcpy(mappedCards,cards.data(),bytes);buffer->Unmap(0,nullptr);
        }

        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(m_constants[frameIndex]->Map(0,&noReads,&mapped),"map diagnostic constants")) return false;
        std::memcpy(mapped,&constants,sizeof(constants));
        const D3D12_RANGE written{0,sizeof(constants)};m_constants[frameIndex]->Unmap(0,&written);
        // The composition pass tracks NON_PIXEL reads. Restore that state after
        // this pixel-shader diagnostic so its next update retains the contract.
        if (usesGlobal)
            Transition(cmd, global.GetTexture(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                VXGIGPUScene::ReadState);
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{ 0, 0, static_cast<float>(output.Width), static_cast<float>(output.Height), 0, 1 };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(output.Width), static_cast<LONG>(output.Height) };
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = { engineHeap };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(m_root.Get());
        cmd->SetPipelineState(m_pipeline.Get());
        cmd->SetGraphicsRootConstantBufferView(0,m_constants[frameIndex]->GetGPUVirtualAddress());
        for (UINT i = 0; i < 4; ++i)
            cmd->SetGraphicsRootShaderResourceView(i + 1, scene.Get(frameIndex,i)->GetGPUVirtualAddress());
        // The geometry query retains the existing alpha-coverage material pool.
        cmd->SetGraphicsRootShaderResourceView(5, materialBuffer);
        for(UINT i=0;i<3;++i) cmd->SetGraphicsRootShaderResourceView(6+i,scene.Get(frameIndex,4+i)->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(9,cards.empty()?scene.Get(frameIndex,0)->GetGPUVirtualAddress():
            m_cardBuffers[frameIndex]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (usesGlobal)
            Transition(cmd, global.GetTexture(), VXGIGPUScene::ReadState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

private:
    struct Constants
    {
        DirectX::XMFLOAT4X4 inverseViewProjection;
        DirectX::XMFLOAT3 gridOrigin; float cellSize;
        std::array<uint32_t, 3> cellDimensions; uint32_t debugMode;
        DirectX::XMFLOAT3 boundsMaximum; uint32_t triangleCount;
        DirectX::XMFLOAT3 sunDirectionTowardLight; float sliceCoordinateWorld;
        uint32_t sliceAxis; DirectX::XMFLOAT2 inverseOutputSize; float materialMipLevel;
        float rayMinDistance, maxRayDistance; uint32_t materialCount, globalSDFIndex;
        float meshTraceDistance, globalTraceDistance; uint32_t globalMaxSteps, cardCount;
        VXGIGlobalSDF::Constants global;
    };
    // The CBV leaves room for the three shared Mesh SDF SRVs under the root limit.
    static_assert(sizeof(Constants) == 352 && offsetof(Constants, global) == 176);
    static bool Check(HRESULT hr, const char* operation)
    {
        if (SUCCEEDED(hr)) return true;
        ErrorLog::HRESULT(std::string("VXGI SDF debug: failed to ") + operation + '.', hr);
        return false;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        cmd->ResourceBarrier(1, &barrier);
    }
    bool CreatePipeline(ID3D12Device* device, DXGI_FORMAT outputFormat)
    {
        CD3DX12_ROOT_PARAMETER parameters[10];
        parameters[0].InitAsConstantBufferView(0,0,D3D12_SHADER_VISIBILITY_PIXEL);
        for (UINT i = 0; i < 5; ++i)
            parameters[i + 1].InitAsShaderResourceView(i, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        for(UINT i=0;i<3;++i) parameters[i+6].InitAsShaderResourceView(i,1,D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[9].InitAsShaderResourceView(5,0,D3D12_SHADER_VISIBILITY_PIXEL);
        const auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(10, parameters, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) ErrorLog::Write(static_cast<const char*>(errors->GetBufferPointer()));
        if (!Check(hr, "serialize root signature") ||
            !Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)), "create root signature")) return false;
        const std::wstring shader = L"Shaders/Shaders_For_VXGI_SDF_Debug.hlsl";
        const auto vs = ShaderCompiler::CompileFromFile(shader, L"VSMain", L"vs_6_6",{L"VXGI_MESH_SDF=1"});
        const auto ps = ShaderCompiler::CompileFromFile(shader, L"PSMain", L"ps_6_6",{L"VXGI_MESH_SDF=1"});
        if (!vs || !ps) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_root.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = outputFormat;
        pso.SampleDesc.Count = 1;
        return Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)), "create diagnostic pipeline");
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_constants;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_cardBuffers;
    std::vector<uint64_t> m_cardBufferBytes;
};
