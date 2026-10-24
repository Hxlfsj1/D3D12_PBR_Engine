#pragma once

#include "VXGIGlobalSDFState.h"
#include "VXGIGlobalSDFPass.h"
#include "VXGIGPUScene.h"
#include "PBR_Shader.h"
#include "RenderDevice.h"

// One persistent texture on the renderer's direct queue. Root constants are
// copied into the command list; neither texture descriptors nor upload memory
// are overwritten while in flight. Only resizing retires all frame slots.
class VXGIGlobalSDFRuntime
{
public:
    struct Stats
    {
        uint64_t frames = 0, idleFrames = 0, dispatches = 0;
        uint64_t updatedNodes = 0, fullLayerUpdates = 0, allocations = 0;
    };

    bool Initialize(RenderDevice& device, UINT frameCount)
    {
        if (m_initialized) return true;
        if (!frameCount) return Fail("Global SDF requires a frame slot.");
        const auto shader = ShaderCompiler::CompileFromFile(
            L"Shaders/Shaders_For_VXGI_GlobalSDF.hlsl", L"CSCompose", L"cs_6_6", {}, true);
        if (!shader || !m_pass.Initialize(device.GetDevice(), shader->GetBufferPointer(), shader->GetBufferSize()))
            return Fail("Global SDF compute pipeline initialization failed: " + m_pass.GetLastError());
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
        if (FAILED(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_empty))))
            return Fail("Global SDF empty scene buffer allocation failed.");
        void* mapped = nullptr;
        const D3D12_RANGE reads{0, 0};
        if (FAILED(m_empty->Map(0, &reads, &mapped))) return Fail("Global SDF empty scene buffer map failed.");
        std::memset(mapped, 0, 256);
        m_empty->Unmap(0, nullptr);
        m_frameCount = frameCount;
        m_initialized = true;
        m_error.clear();
        return true;
    }

    bool Record(RenderDevice& device, int slot, const VXGIMeshSDFScene& scene,
        const VXGIGPUScene& gpuScene, const DirectX::XMFLOAT3& center,
        const VXGIGlobalSDF::Settings& settings)
    {
        if (!m_initialized || slot < 0 || UINT(slot) >= m_frameCount)
            return Fail("Global SDF runtime is uninitialized or has an invalid frame slot.");
        // A previous unsubmitted recording may be abandoned when rendering
        // stops early. Its caller has reset/discarded that command list.
        CancelRecording();
        const uint32_t assets = gpuScene.GetAssetCount(slot);
        const uint32_t nodes = gpuScene.GetDistanceNodeCount(slot);
        if (gpuScene.GetInstanceCount(slot) != scene.GetInstances().size())
            return Fail("Global SDF scene tables do not match this frame slot.");
        if (!m_state.Prepare(center, settings, scene, m_stats.frames, assets, nodes, &m_error)) return false;
        const auto& plan = m_state.GetPendingPlan();
        if (plan.recreate)
        {
            if (m_pass.GetTexture())
                for (UINT index = 0; index < m_frameCount; ++index)
                {
                    device.WaitForPreviousFrame(int(index));
                    const auto completed = device.GetFence(int(index))->GetCompletedValue();
                    if (completed == UINT64_MAX || completed < device.GetFenceValue(int(index)))
                        return Fail("Global SDF texture resize could not retire previous frames.");
                }
            if (!m_pass.Create(device.GetDevice(), plan.layout)) return Fail(m_pass.GetLastError());
            ++m_stats.allocations;
        }
        VXGIGlobalSDFPass::Inputs inputs{m_empty.Get(), m_empty.Get(), m_empty.Get()};
        if (!scene.GetInstances().empty())
            inputs = {gpuScene.Get(slot, 0), gpuScene.Get(slot, 7), gpuScene.Get(slot, 8)};
        if (!m_pass.RecordRegions(device.GetCommandList(), plan.layout, inputs, plan.regions))
            return Fail(m_pass.GetLastError());
        m_pending = true;
        m_error.clear();
        return true;
    }

    bool OnSubmitted()
    {
        if (!m_pending) return true;
        const auto& plan = m_state.GetPendingPlan();
        const auto work = m_pass.GetStats();
        ++m_stats.frames;
        if (!work.dispatchCount) ++m_stats.idleFrames;
        m_stats.dispatches += work.dispatchCount;
        m_stats.updatedNodes += work.updatedNodeCount;
        for (bool full : plan.fullUpdate) if (full) ++m_stats.fullLayerUpdates;
        if (!m_state.Commit()) return Fail("Global SDF could not commit submitted state.");
        m_pass.OnSubmitted();
        m_pending = false;
        return true;
    }

    // Call only after discarding a command list that has not been executed.
    void CancelRecording()
    {
        if (m_pending) m_pass.CancelRecording();
        m_state.Cancel();
        m_pending = false;
    }

    const Stats& GetStats() const { return m_stats; }
    const VXGIGlobalSDFState& GetState() const { return m_state; }
    const VXGIGlobalSDFPass& GetPass() const { return m_pass; }
    const std::string& GetLastError() const { return m_error; }

private:
    VXGIGlobalSDFState m_state;
    VXGIGlobalSDFPass m_pass;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_empty;
    Stats m_stats;
    UINT m_frameCount = 0;
    bool m_initialized = false, m_pending = false;
    std::string m_error;
    bool Fail(const std::string& message) { m_error = message; return false; }
};
