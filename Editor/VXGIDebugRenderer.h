#pragma once

#include "RenderDevice.h"
#include "PBR_Shader.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

// World-space diagnostics, independent of editor panel construction and the scene
// render graph. Record after post-processing while the back buffer is PRESENT.
// Stage 1 deliberately draws through scene geometry (x-ray); no depth is sampled.
class VXGIDebugRenderer
{
public:
    bool Initialize(RenderDevice& deviceContext, UINT frameCount)
    {
        if (frameCount == 0) return false;
        if (!CreatePipeline(deviceContext.GetDevice(), deviceContext.GetRenderTarget(0)->GetDesc().Format))
            return false;
        m_frames.resize(frameCount);
        return true;
    }

    // The matrix has the same GPU-transposed convention as EditorSelectionRenderer.
    // Increment geometryRevision when the centers change. Camera movement, cell size,
    // and visibility toggles only change constants/draw calls, not the upload data.
    // Call once per frame, after RenderDevice::WaitForPreviousFrame(frameIndex).
    bool Record(RenderDevice& deviceContext, ID3D12DescriptorHeap* engineHeap, int frameIndex,
        const DirectX::XMFLOAT4X4& unjitteredViewProjectionGpu,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const std::vector<DirectX::XMFLOAT3>& occupiedCenters, float voxelSize,
        uint64_t geometryRevision, bool showCells, bool showProbes)
    {
        if ((!showCells && !showProbes) || occupiedCenters.empty() || voxelSize <= 0.0f)
            return true;
        if (!m_pipeline || !engineHeap || frameIndex < 0 || static_cast<size_t>(frameIndex) >= m_frames.size())
            return false;
        if (occupiedCenters.size() > (std::numeric_limits<UINT>::max)() / sizeof(DirectX::XMFLOAT3))
        {
            ErrorLog::Write("VXGI debug: cell centers exceed the vertex-buffer size limit; no partial grid was drawn.");
            return false;
        }

        auto& frame = m_frames[frameIndex];
        const UINT byteCount = static_cast<UINT>(occupiedCenters.size() * sizeof(DirectX::XMFLOAT3));
        const bool changed = !frame.buffer || frame.revision != geometryRevision || frame.byteCount != byteCount;
        if (changed)
        {
            // Each swap-chain slot owns its upload buffer. Never overwrite or release
            // that slot's old geometry until its previous submission has completed.
            const UINT64 completed = deviceContext.GetFence(frameIndex)->GetCompletedValue();
            if (completed == UINT64_MAX || completed < deviceContext.GetFenceValue(frameIndex))
            {
                ErrorLog::Write("VXGI debug: frame upload requested before its GPU fence completed.");
                return false;
            }
            if (!frame.buffer || frame.capacity < byteCount)
            {
                Microsoft::WRL::ComPtr<ID3D12Resource> replacement;
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteCount);
                if (!Check(deviceContext.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&replacement)), "create center upload"))
                    return false;
                void* mapped = nullptr;
                const D3D12_RANGE noReads{ 0, 0 };
                if (!Check(replacement->Map(0, &noReads, &mapped), "map center upload")) return false;
                if (frame.buffer) frame.buffer->Unmap(0, nullptr);
                frame.buffer = std::move(replacement);
                frame.mapped = mapped;
                frame.capacity = byteCount;
                frame.buffer->SetName(L"VXGI diagnostic cell centers");
            }
            std::memcpy(frame.mapped, occupiedCenters.data(), byteCount);
            frame.revision = geometryRevision;
            frame.byteCount = byteCount;
        }

        auto* cmd = deviceContext.GetCommandList();
        auto* backBuffer = deviceContext.GetRenderTarget(frameIndex);
        const auto output = backBuffer->GetDesc();
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
        cmd->SetGraphicsRoot32BitConstants(0, 16, &unjitteredViewProjectionGpu, 0);
        cmd->SetGraphicsRoot32BitConstants(0, 1, &voxelSize, 16);
        const D3D12_VERTEX_BUFFER_VIEW centers{ frame.buffer->GetGPUVirtualAddress(), byteCount, sizeof(DirectX::XMFLOAT3) };
        cmd->IASetVertexBuffers(0, 1, &centers);
        cmd->IASetIndexBuffer(nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        const UINT instanceCount = static_cast<UINT>(occupiedCenters.size());
        if (showCells)
        {
            const UINT kind = 0;
            cmd->SetGraphicsRoot32BitConstants(0, 1, &kind, 17);
            cmd->DrawInstanced(24, instanceCount, 0, 0);
        }
        if (showProbes)
        {
            const UINT kind = 1;
            cmd->SetGraphicsRoot32BitConstants(0, 1, &kind, 17);
            cmd->DrawInstanced(6, instanceCount, 0, 0);
        }
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    }

private:
    struct FrameUpload
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        void* mapped = nullptr;
        UINT capacity = 0, byteCount = 0;
        uint64_t revision = 0;
    };

    static bool Check(HRESULT hr, const char* operation)
    {
        if (SUCCEEDED(hr)) return true;
        ErrorLog::HRESULT(std::string("VXGI debug: failed to ") + operation + '.', hr);
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
        CD3DX12_ROOT_PARAMETER constants;
        constants.InitAsConstants(18, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(1, &constants, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) ErrorLog::Write(static_cast<const char*>(errors->GetBufferPointer()));
        if (!Check(hr, "serialize root signature") ||
            !Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)), "create root signature"))
            return false;
        const std::wstring shader = L"Shaders/Shaders_For_VXGI_Debug.hlsl";
        const auto vs = ShaderCompiler::CompileFromFile(shader, L"VSMain", L"vs_6_6");
        const auto ps = ShaderCompiler::CompileFromFile(shader, L"PSMain", L"ps_6_6");
        if (!vs || !ps) return false;
        const D3D12_INPUT_ELEMENT_DESC input[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
        };
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
        pso.InputLayout = { input, _countof(input) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = outputFormat;
        pso.SampleDesc.Count = 1;
        return Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)), "create line pipeline");
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    std::vector<FrameUpload> m_frames;
};
