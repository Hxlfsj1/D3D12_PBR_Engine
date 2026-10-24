#pragma once

#include "VXGIGlobalSDF.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>
#include "d3dx12.h"

// Dense Global SDF composition with rolling physical storage. The caller owns source-buffer
// lifetimes and keeps them in NON_PIXEL_SHADER_RESOURCE (or a combined read
// state). Create/recreate only after the previous texture has retired. Recorded
// command lists must be submitted in order; this class tracks their end state.
// The CPU update planner owns scrolling and dirty-region selection.
class VXGIGlobalSDFPass
{
public:
    struct Inputs
    {
        ID3D12Resource* nodes = nullptr;
        ID3D12Resource* assets = nullptr;
        ID3D12Resource* instances = nullptr;
    };
    struct Stats
    {
        uint64_t dispatchCount = 0;
        // Total written nodes, counting any overlap between supplied regions.
        uint64_t updatedNodeCount = 0;
    };

    bool Initialize(ID3D12Device* device, const void* shaderBytes, size_t shaderSize)
    {
        if (!device || !shaderBytes || !shaderSize) return Fail("Missing Global SDF device or compute shader.");
        CD3DX12_ROOT_PARAMETER roots[6];
        roots[0].InitAsConstants(sizeof(VXGIGlobalSDF::Constants) / sizeof(uint32_t), 0);
        roots[1].InitAsShaderResourceView(0);
        roots[2].InitAsShaderResourceView(0, 1);
        roots[3].InitAsShaderResourceView(1, 1);
        CD3DX12_DESCRIPTOR_RANGE range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        roots[4].InitAsDescriptorTable(1, &range);
        roots[5].InitAsConstants(sizeof(VXGIGlobalSDF::UpdateRegion) / sizeof(uint32_t), 1);
        static_assert(sizeof(VXGIGlobalSDF::Constants) / sizeof(uint32_t) + 6 + 1 +
            sizeof(VXGIGlobalSDF::UpdateRegion) / sizeof(uint32_t) <= 64,
            "Global SDF root signature exceeds the D3D12 DWORD budget.");
        const CD3DX12_ROOT_SIGNATURE_DESC desc(6, roots);
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
        if (!Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors), "serialize root signature")) return false;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        if (!Check(device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "create root signature")) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = root.Get();
        psoDesc.CS = {shaderBytes, shaderSize};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        if (!Check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline)),
            "create compute pipeline")) return false;
        m_root = std::move(root);
        m_pipeline = std::move(pipeline);
        m_error.clear();
        return true;
    }

    bool Create(ID3D12Device* device, const VXGIGlobalSDF::Layout& layout)
    {
        if (!device || !m_root || !m_pipeline || !LayoutValid(layout))
            return Fail("Invalid Global SDF layout or uninitialized pipeline.");
        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R32_FLOAT,
            layout.width, layout.height, static_cast<UINT16>(layout.depth), 1,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        if (!Check(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)),
            "create distance texture")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        if (!Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)),
            "create descriptor heap")) return false;
        const auto first = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = layout.depth;
        device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, first);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture3D.MipLevels = 1;
        // A shader-visible heap cannot be the source of CopyDescriptorsSimple.
        // Keep the public SRV in a CPU-only heap so later consumers can copy it
        // into their own shader-visible heap without switching away from it.
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        if (!Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)),
            "create source SRV heap")) return false;
        const auto srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(texture.Get(), &srv, srvHandle);
        texture->SetName(L"VXGI Global SDF dense clipmaps");
        m_texture = std::move(texture);
        m_heap = std::move(heap);
        m_srvHeap = std::move(srvHeap);
        m_srvCPU = srvHandle;
        m_layout = layout;
        m_state = D3D12_RESOURCE_STATE_COMMON;
        m_stats = {};
        m_recordingPending = false;
        m_error.clear();
        return true;
    }

    // Leaves the texture in NON_PIXEL_SHADER_RESOURCE. This entry rebuilds every
    // texel (including zeroed atlas padding), using the same PSO as partial updates.
    // Later passes must bind their own heap, root signature and root arguments.
    bool Record(ID3D12GraphicsCommandList* commandList,
        const VXGIGlobalSDF::Layout& layout, const Inputs& inputs)
    {
        VXGIGlobalSDF::UpdateRegion region{};
        region.clipmapIndex = UINT32_MAX;
        region.size = {layout.width, layout.height, layout.depth};
        return RecordRegions(commandList, layout, inputs, {&region, 1});
    }

    // Regions use logical node coordinates. Each write independently wraps to
    // physical storage, so a logical box may cross any rolling seam. Empty work
    // only publishes layout/source counts; it does not bind state or transition
    // the texture and therefore also accepts a null command list and inputs.
    bool RecordRegions(ID3D12GraphicsCommandList* commandList,
        const VXGIGlobalSDF::Layout& layout, const Inputs& inputs,
        std::span<const VXGIGlobalSDF::UpdateRegion> regions)
    {
        if (!m_texture || !m_heap || !LayoutValid(layout) ||
            layout.width != m_layout.width || layout.height != m_layout.height ||
            layout.depth != m_layout.depth)
            return Fail("Global SDF build requires a matching texture and valid source buffers.");
        Stats stats;
        for (const auto& region : regions)
        {
            if (!RegionValid(layout, region)) return Fail("Global SDF update region is outside its clipmap.");
            ++stats.dispatchCount;
            stats.updatedNodeCount += uint64_t(region.size[0]) * region.size[1] * region.size[2];
        }
        if (!regions.empty() && (!commandList || !inputs.nodes || !inputs.assets || !inputs.instances))
            return Fail("Global SDF build requires a command list and valid source buffers.");
        // Replacing an older backup treats that record as submitted, preserving
        // compatibility with callers that submit in order without OnSubmitted.
        // Failed validation above never overwrites an outstanding backup.
        m_recordingBackup = {m_layout, m_state, m_stats};
        m_recordingPending = true;
        if (regions.empty())
        {
            m_layout = layout;
            m_stats = stats;
            m_error.clear();
            return true;
        }
        if (m_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto toWrite = CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(),
                m_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &toWrite);
        }
        ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(m_root.Get());
        commandList->SetPipelineState(m_pipeline.Get());
        commandList->SetComputeRoot32BitConstants(0,
            sizeof(VXGIGlobalSDF::Constants) / sizeof(uint32_t), &layout.constants, 0);
        commandList->SetComputeRootShaderResourceView(1, inputs.nodes->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(2, inputs.assets->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(3, inputs.instances->GetGPUVirtualAddress());
        commandList->SetComputeRootDescriptorTable(4, m_heap->GetGPUDescriptorHandleForHeapStart());
        for (size_t index = 0; index < regions.size(); ++index)
        {
            bool overlaps = false;
            for (size_t previous = 0; previous < index && !overlaps; ++previous)
                overlaps = RegionsOverlap(regions[index], regions[previous]);
            if (overlaps)
            {
                const auto orderWrites = CD3DX12_RESOURCE_BARRIER::UAV(m_texture.Get());
                commandList->ResourceBarrier(1, &orderWrites);
            }
            const auto& region = regions[index];
            commandList->SetComputeRoot32BitConstants(5,
                sizeof(VXGIGlobalSDF::UpdateRegion) / sizeof(uint32_t), &region, 0);
            commandList->Dispatch((region.size[0] + 3) / 4,
                (region.size[1] + 3) / 4, (region.size[2] + 3) / 4);
        }
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
        m_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        m_layout = layout;
        m_stats = stats;
        m_error.clear();
        return true;
    }

    // Restores CPU tracking for the latest successful record only. The caller
    // must first discard its unsubmitted command list. Never call this after
    // ExecuteCommandLists: already submitted GPU writes cannot be rolled back.
    void CancelRecording()
    {
        if (!m_recordingPending) return;
        m_layout = m_recordingBackup.layout;
        m_state = m_recordingBackup.state;
        m_stats = m_recordingBackup.stats;
        m_recordingPending = false;
    }
    void OnSubmitted() { m_recordingPending = false; }

    ID3D12Resource* GetTexture() const { return m_texture.Get(); }
    // CPU-only source descriptor, valid for CopyDescriptorsSimple.
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPUHandle() const { return m_srvCPU; }
    const VXGIGlobalSDF::Layout& GetLayout() const { return m_layout; }
    // Statistics for the most recent successful Record/RecordRegions call.
    const Stats& GetStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_error; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCPU{};
    VXGIGlobalSDF::Layout m_layout{};
    D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
    Stats m_stats;
    struct RecordingBackup
    {
        VXGIGlobalSDF::Layout layout{};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        Stats stats;
    } m_recordingBackup;
    bool m_recordingPending = false;
    std::string m_error;

    static bool LayoutValid(const VXGIGlobalSDF::Layout& layout)
    {
        const auto& c = layout.constants;
        if (c.resolution < 3 || c.resolution > 257 || c.resolution % 2 == 0 ||
            c.clipmapCount < 1 || c.clipmapCount > 3 ||
            layout.width != c.resolution || layout.height != c.resolution ||
            layout.totalNodeCount != uint64_t(layout.width) * layout.height * layout.depth) return false;
        uint32_t offsetZ = 0, maximumResolution = 0;
        for (uint32_t layer = 0; layer < c.clipmapCount; ++layer)
        {
            const auto& clipmap = c.clipmaps[layer];
            const uint32_t resolution = c.clipmapResolutions[layer];
            const auto origin = clipmap.origin;
            const auto maximum = clipmap.boundsMaximum;
            if (resolution < 3 || resolution > c.resolution || resolution % 2 == 0 ||
                !std::isfinite(clipmap.cellSize) || clipmap.cellSize <= 0 ||
                !std::isfinite(clipmap.distanceLimit) || clipmap.distanceLimit <= 0 ||
                !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
                !std::isfinite(maximum.x) || !std::isfinite(maximum.y) || !std::isfinite(maximum.z) ||
                maximum.x <= origin.x || maximum.y <= origin.y || maximum.z <= origin.z ||
                clipmap.nodeOffsetZ != offsetZ) return false;
            const float span = float(resolution - 1) * clipmap.cellSize;
            const float expected[] = {origin.x + span, origin.y + span, origin.z + span};
            const float actual[] = {maximum.x, maximum.y, maximum.z};
            for (uint32_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(expected[axis]) || std::abs(actual[axis] - expected[axis]) >
                    (std::max)(clipmap.cellSize * 1e-4f, std::abs(expected[axis]) * 2e-6f)) return false;
            if (layer > 0)
            {
                const auto& previous = c.clipmaps[layer - 1];
                if (clipmap.cellSize <= previous.cellSize || origin.x > previous.origin.x ||
                    origin.y > previous.origin.y || origin.z > previous.origin.z ||
                    maximum.x < previous.boundsMaximum.x || maximum.y < previous.boundsMaximum.y ||
                    maximum.z < previous.boundsMaximum.z) return false;
            }
            for (uint32_t offset : clipmap.scrollOffset) if (offset >= resolution) return false;
            offsetZ += resolution;
            maximumResolution = (std::max)(maximumResolution, resolution);
        }
        return maximumResolution == c.resolution && layout.depth == offsetZ && layout.depth <= 2048;
    }
    static bool RegionValid(const VXGIGlobalSDF::Layout& layout, const VXGIGlobalSDF::UpdateRegion& region)
    {
        if (region.clipmapIndex == UINT32_MAX)
            return region.minimum[0] == 0 && region.minimum[1] == 0 && region.minimum[2] == 0 &&
                region.size[0] == layout.width && region.size[1] == layout.height && region.size[2] == layout.depth;
        if (region.clipmapIndex >= layout.constants.clipmapCount) return false;
        const uint32_t resolution = layout.constants.clipmapResolutions[region.clipmapIndex];
        for (uint32_t axis = 0; axis < 3; ++axis)
            if (region.minimum[axis] >= resolution || region.size[axis] == 0 ||
                region.size[axis] > resolution - region.minimum[axis]) return false;
        return true;
    }
    static bool RegionsOverlap(const VXGIGlobalSDF::UpdateRegion& a, const VXGIGlobalSDF::UpdateRegion& b)
    {
        if (a.clipmapIndex == UINT32_MAX || b.clipmapIndex == UINT32_MAX) return true;
        if (a.clipmapIndex != b.clipmapIndex) return false;
        for (uint32_t axis = 0; axis < 3; ++axis)
            if (a.minimum[axis] + a.size[axis] <= b.minimum[axis] ||
                b.minimum[axis] + b.size[axis] <= a.minimum[axis]) return false;
        return true;
    }
    bool Fail(const char* error) { m_error = error; return false; }
    bool Check(HRESULT result, const char* operation)
    {
        if (SUCCEEDED(result)) return true;
        m_error = std::string("Global SDF failed to ") + operation +
            " (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ").";
        return false;
    }
};
