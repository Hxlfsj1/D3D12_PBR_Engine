#pragma once

#include "EditorSelection.h"
#include "ResourceManager.h"
#include "EditorHistory.h"
#include "PBR_Shader.h"
#include <cmath>
#include <cstddef>

// Editor overlay shared by forward/deferred rendering, after all scene post-processing.
// Owns its targets independently of the scene RDG, just like the ImGui overlay.
class EditorSelectionRenderer
{
public:
    bool Initialize(RenderDevice& deviceContext, UINT frameCount)
    {
        ID3D12Device* device = deviceContext.GetDevice();
        const auto output = deviceContext.GetRenderTarget(0)->GetDesc();
        m_width = static_cast<UINT>(output.Width);
        m_height = output.Height;
        if (!CreatePipelines(device, output.Format))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 1;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap)), "create ID RTV heap")) return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_dsvHeap)), "create selection depth heap")) return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_srvHeap)), "create ID SRV heap")) return false;

        const CD3DX12_HEAP_PROPERTIES gpuHeap(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, m_width, m_height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        if (!Check(device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_ids)), "create ID texture")) return false;
        device->CreateRenderTargetView(m_ids.Get(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        device->CreateShaderResourceView(m_ids.Get(), nullptr, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        m_ids->SetName(L"Editor object IDs");

        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = desc.Format;
        clear.DepthStencil.Depth = 1.0f;
        if (!Check(device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&m_depth)), "create selection depth")) return false;
        device->CreateDepthStencilView(m_depth.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
        m_depth->SetName(L"Editor selection depth");

        m_readbacks.resize(frameCount);
        const CD3DX12_HEAP_PROPERTIES cpuHeap(D3D12_HEAP_TYPE_READBACK);
        // A one-pixel texture copy still needs a 256-byte aligned row pitch.
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        for (auto& slot : m_readbacks)
            if (!Check(device->CreateCommittedResource(&cpuHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.buffer)), "create pick readback")) return false;
        return true;
    }

    void Poll(RenderDevice& deviceContext, ResourceManager& resources, EditorSelection& selection, EditorHistory& history)
    {
        for (size_t i = 0; i < m_readbacks.size(); ++i)
        {
            auto& slot = m_readbacks[i];
            if (slot.fenceValue == 0) continue;
            const UINT64 completed = deviceContext.GetFence(static_cast<int>(i))->GetCompletedValue();
            if (completed == UINT64_MAX || completed < slot.fenceValue) continue;
            UINT* data = nullptr;
            const D3D12_RANGE readRange{ 0, sizeof(UINT) };
            if (Check(slot.buffer->Map(0, &readRange, reinterpret_cast<void**>(&data)), "map pick result"))
            {
                UINT id = *data;
                const D3D12_RANGE noWrites{ 0, 0 };
                slot.buffer->Unmap(0, &noWrites);
                bool exists = false;
                for (const auto& instance : resources.GetSceneInstances())
                    exists |= instance.editorId == id;
                const auto previousId = selection.SelectedId();
                selection.ApplyPick(slot.revision, exists ? id : 0);
                history.RecordSelection(resources.GetSceneInstances(), previousId, selection.SelectedId());
            }
            slot.fenceValue = 0;
        }
        if (selection.SelectedId() != 0)
        {
            bool exists = false;
            for (const auto& instance : resources.GetSceneInstances())
                exists |= instance.editorId == selection.SelectedId();
            if (!exists) selection.Select(0);
        }
    }

    void Record(RenderDevice& deviceContext, ResourceManager& resources, EditorSelection& selection,
        int frameIndex, const DirectX::XMFLOAT4X4& unjitteredViewProjectionGpu,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv, float clientWidth, float clientHeight)
    {
        auto& slot = m_readbacks[frameIndex];
        slot.recorded = false;
        if (!selection.HasPickRequest() && selection.SelectedId() == 0) return;

        auto* cmd = deviceContext.GetCommandList();
        Transition(cmd, m_ids.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const float background[4] = {};
        cmd->ClearRenderTargetView(rtv, background, 0, nullptr);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{ 0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1 };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* materialHeaps[] = { resources.GetMainDescriptorHeap() };
        cmd->SetDescriptorHeaps(1, materialHeaps);
        cmd->SetGraphicsRootSignature(m_idRoot.Get());
        cmd->SetGraphicsRootShaderResourceView(1, resources.GetMaterialBufferGPUAddress());
        const auto viewProjection = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&unjitteredViewProjectionGpu));
        for (auto& instance : resources.GetSceneInstances())
        {
            if (!instance.isVisible || !instance.pModel) continue;
            cmd->SetPipelineState(instance.isCutout ? m_cutoutPso.Get() : m_idPso.Get());
            ObjectConstants constants{};
            DirectX::XMStoreFloat4x4(&constants.wvp,
                DirectX::XMMatrixTranspose(instance.cachedWorldMat * viewProjection));
            constants.objectId = instance.editorId;
            constants.alphaCutoff = instance.isCutout ? 0.5f : (instance.isTransparent ? 0.05f : -1.0f);
            for (auto& mesh : instance.pModel->meshes)
            {
                constants.materialId = instance.customMaterialID == UINT_MAX ? mesh.materialID : instance.customMaterialID;
                cmd->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
                mesh.Draw(cmd, 1, instance.currentLodLevel);
            }
        }

        if (selection.HasPickRequest() && slot.fenceValue == 0)
        {
            const auto request = selection.TakePickRequest();
            const UINT x = static_cast<UINT>(request.u * static_cast<float>(m_width));
            const UINT y = static_cast<UINT>(request.v * static_cast<float>(m_height));
            if (x < m_width && y < m_height)
            {
                Transition(cmd, m_ids.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = m_ids.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = slot.buffer.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
                const D3D12_BOX box{ x, y, 0, x + 1, y + 1, 1 };
                cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
                slot.revision = request.revision;
                slot.recorded = true;
                Transition(cmd, m_ids.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            else
                Transition(cmd, m_ids.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
            Transition(cmd, m_ids.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (selection.SelectedId() == 0) return;
        auto* backBuffer = deviceContext.GetRenderTarget(frameIndex);
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        ID3D12DescriptorHeap* outlineHeaps[] = { m_srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, outlineHeaps);
        cmd->SetGraphicsRootSignature(m_outlineRoot.Get());
        cmd->SetPipelineState(m_outlinePso.Get());
        cmd->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        // Approximately two client pixels, even when presentation stretches the back buffer.
        const UINT constants[] = { selection.SelectedId(), OutlineRadius(m_width, clientWidth), OutlineRadius(m_height, clientHeight) };
        cmd->SetGraphicsRoot32BitConstants(1, 3, constants, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        Transition(cmd, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    // Only arm a readback after this exact command list was submitted and signalled.
    void OnSubmitted(int frameIndex, UINT64 fenceValue)
    {
        auto& slot = m_readbacks[frameIndex];
        if (slot.recorded) slot.fenceValue = fenceValue;
        slot.recorded = false;
    }

private:
    struct ObjectConstants
    {
        DirectX::XMFLOAT4X4 wvp;
        UINT objectId, materialId;
        float alphaCutoff;
    };
    static_assert(sizeof(ObjectConstants) == 19 * sizeof(UINT));
    struct Readback
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        UINT64 fenceValue = 0;
        uint64_t revision = 0;
        bool recorded = false;
    };

    static UINT OutlineRadius(UINT dimension, float clientDimension)
    {
        const float radius = clientDimension > 0 ? std::ceil(2.0f * dimension / clientDimension) : 2.0f;
        return static_cast<UINT>(radius < 1.0f ? 1.0f : (radius > 16.0f ? 16.0f : radius));
    }
    static bool Check(HRESULT hr, const char* operation)
    {
        if (SUCCEEDED(hr)) return true;
        ErrorLog::HRESULT(std::string("EditorSelection: failed to ") + operation + '.', hr);
        return false;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        cmd->ResourceBarrier(1, &barrier);
    }
    static bool CreateRoot(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& root)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) ErrorLog::Write(static_cast<const char*>(errors->GetBufferPointer()));
        return Check(hr, "serialize selection root signature") &&
            Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "create selection root signature");
    }
    bool CreatePipelines(ID3D12Device* device, DXGI_FORMAT outputFormat)
    {
        CD3DX12_ROOT_PARAMETER idParams[2];
        idParams[0].InitAsConstants(19, 0);
        idParams[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        const CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
        const CD3DX12_ROOT_SIGNATURE_DESC idRootDesc(2, idParams, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        if (!CreateRoot(device, idRootDesc, m_idRoot)) return false;
        CD3DX12_DESCRIPTOR_RANGE range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER outlineParams[2];
        outlineParams[0].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
        outlineParams[1].InitAsConstants(3, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        const CD3DX12_ROOT_SIGNATURE_DESC outlineRootDesc(2, outlineParams);
        if (!CreateRoot(device, outlineRootDesc, m_outlineRoot)) return false;

        const std::wstring shader = L"Shaders/EditorSelection.hlsl";
        auto idVs = ShaderCompiler::CompileFromFile(shader, L"IdVS", L"vs_6_6", { L"OBJECT_IDS=1" });
        auto idPs = ShaderCompiler::CompileFromFile(shader, L"IdPS", L"ps_6_6", { L"OBJECT_IDS=1" });
        auto outlineVs = ShaderCompiler::CompileFromFile(shader, L"OutlineVS", L"vs_6_6");
        auto outlinePs = ShaderCompiler::CompileFromFile(shader, L"OutlinePS", L"ps_6_6");
        if (!idVs || !idPs || !outlineVs || !outlinePs) return false;
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, Position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Vertex, TexCoords)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_idRoot.Get();
        pso.VS = { idVs->GetBufferPointer(), idVs->GetBufferSize() };
        pso.PS = { idPs->GetBufferPointer(), idPs->GetBufferSize() };
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.InputLayout = { layout, _countof(layout) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_idPso)), "create ID PSO")) return false;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_cutoutPso)), "create cutout ID PSO")) return false;
        pso.pRootSignature = m_outlineRoot.Get();
        pso.VS = { outlineVs->GetBufferPointer(), outlineVs->GetBufferSize() };
        pso.PS = { outlinePs->GetBufferPointer(), outlinePs->GetBufferSize() };
        pso.InputLayout = {};
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.RTVFormats[0] = outputFormat;
        return Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_outlinePso)), "create outline PSO");
    }

    UINT m_width = 0, m_height = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ids, m_depth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap, m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_idRoot, m_outlineRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_idPso, m_cutoutPso, m_outlinePso;
    std::vector<Readback> m_readbacks;
};
