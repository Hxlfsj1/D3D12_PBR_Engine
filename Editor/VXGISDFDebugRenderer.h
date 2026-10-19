#pragma once

#include "RenderDevice.h"
#include "PBR_Shader.h"
#include "VXGISurface.h"
#include "VXGIGPUScene.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
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
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(256);
        for(auto& buffer:m_constants)
            if(!Check(deviceContext.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"create diagnostic constants")) return false;
        return true;
    }

    // SDF contract: GetGrid/GetNodes/GetCellOffsets/GetTriangleReferences/
    // GetTriangles/GetGeometryRevision/GetMaterialRevision. Grid is independent
    // of the sparse probe grid; material edits reuse distance/CSR GPU buffers.
    // inverseViewProjectionGpu uses the engine's transposed GPU matrix convention.
    // Call once per frame after WaitForPreviousFrame; back buffer enters/leaves PRESENT.
    template<class SDF>
    bool Record(RenderDevice& deviceContext, ID3D12DescriptorHeap* engineHeap, int frameIndex,
        const DirectX::XMFLOAT4X4& inverseViewProjectionGpu, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const SDF& sdf, const VXGIGPUScene& scene, uint32_t debugMode, uint32_t sliceAxis, float sliceCoordinateWorld,
        const DirectX::XMFLOAT3& sunDirectionTowardLight,
        const VXGISurface* surfaceState = nullptr, D3D12_GPU_VIRTUAL_ADDRESS materialBuffer = 0,
        float voxelSize = 1, uint32_t probeCount = 0, ID3D12Resource* rawProbes = nullptr)
    {
        (void)sdf;
        if(frameIndex<0 || size_t(frameIndex)>=m_constants.size()) return false;
        if (!((debugMode >= 4 && debugMode <= 12) || (debugMode >= 18 && debugMode <= 20)) || !scene.GetGrid(frameIndex).valid) return true;
        if (debugMode >= 18 && probeCount && (!rawProbes || !scene.Get(frameIndex,5))) return false;
        // The diagnostic shares the production mesh backend and alpha coverage.
        if (!surfaceState || !materialBuffer) return false;
        if (!m_pipeline || !engineHeap || !scene.IsMeshBackend(frameIndex) || !scene.Get(frameIndex,0)) return false;
        const auto& grid = scene.GetGrid(frameIndex);
        if (!scene.GetTriangleCount(frameIndex)) return true;
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
        constants.voxelSize = voxelSize;constants.probeCount = probeCount;
        const auto sun = DirectX::XMLoadFloat3(&sunDirectionTowardLight);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(sun)) > 1e-20f)
            DirectX::XMStoreFloat3(&constants.sunDirectionTowardLight, DirectX::XMVector3Normalize(sun));
        else
            constants.sunDirectionTowardLight = { 0, 1, 0 };
        constants.sliceCoordinateWorld = sliceCoordinateWorld;
        constants.sliceAxis = sliceAxis < 3 ? sliceAxis : 2;
        constants.inverseOutputSize = { 1.0f / static_cast<float>(output.Width), 1.0f / static_cast<float>(output.Height) };
        if (surfaceState)
        {
            const auto& parameters = surfaceState->GetParameters();
            constants.sunRadiance = parameters.lightColor;
            constants.materialMipLevel = parameters.materialMipLevel;
            constants.rayMinDistance = parameters.rayMinDistance;
            constants.maxRayDistance = parameters.maxRayDistance;
            constants.materialCount = static_cast<uint32_t>(surfaceState->GetMaterials().size());
        }

        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(m_constants[frameIndex]->Map(0,&noReads,&mapped),"map diagnostic constants")) return false;
        std::memcpy(mapped,&constants,sizeof(constants));
        const D3D12_RANGE written{0,sizeof(constants)};m_constants[frameIndex]->Unmap(0,&written);
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
        // Alpha-aware tracing reuses the same t4 material pool as surface shading.
        cmd->SetGraphicsRootShaderResourceView(5, materialBuffer);
        const auto fallback = scene.Get(frameIndex,3)->GetGPUVirtualAddress();
        cmd->SetGraphicsRootShaderResourceView(6, scene.Get(frameIndex,4) ? scene.Get(frameIndex,4)->GetGPUVirtualAddress() : fallback);
        cmd->SetGraphicsRootShaderResourceView(7, scene.Get(frameIndex,5) ? scene.Get(frameIndex,5)->GetGPUVirtualAddress() : fallback);
        cmd->SetGraphicsRootShaderResourceView(8, rawProbes ? rawProbes->GetGPUVirtualAddress() : fallback);
        for(UINT i=0;i<3;++i) cmd->SetGraphicsRootShaderResourceView(9+i,scene.Get(frameIndex,7+i)->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
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
        uint32_t sliceAxis; DirectX::XMFLOAT2 inverseOutputSize; float unusedPadding;
        DirectX::XMFLOAT3 sunRadiance; float materialMipLevel;
        float rayMinDistance, maxRayDistance; uint32_t materialCount, surfacePadding;
        float voxelSize;uint32_t probeCount,queryPadding[2];
    };
    // The CBV leaves room for the three shared Mesh SDF SRVs under the root limit.
    static_assert(sizeof(Constants) == 48 * sizeof(UINT));
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
        CD3DX12_ROOT_PARAMETER parameters[12];
        parameters[0].InitAsConstantBufferView(0,0,D3D12_SHADER_VISIBILITY_PIXEL);
        for (UINT i = 0; i < 8; ++i)
            parameters[i + 1].InitAsShaderResourceView(i, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        for(UINT i=0;i<3;++i) parameters[i+9].InitAsShaderResourceView(i,1,D3D12_SHADER_VISIBILITY_PIXEL);
        const auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(12, parameters, 1, &sampler,
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
};
