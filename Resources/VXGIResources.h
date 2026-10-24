#pragma once

#include "PBR_Model.h"
#include "SceneObject.h"
#include "RenderStructs.h"
#include "Settings_Manager.h"
#include "RenderDevice.h"
#include "VXGIWorld.h"
#include "VXGISDF.h"
#include "VXGIMeshSDFScene.h"
#include "VXGISurface.h"
#include "VXGIProbeTopology.h"
#include "VXGIGPUScene.h"
#include "VXGIProbeState.h"
#include "VXGIProbeCache.h"
#include "VXGIProbePass.h"
#include "VXGIProbeFilterPass.h"
#include "VXGIGlobalSDFRuntime.h"
#include "InitializationProgress.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Persistent scene lighting resources. Startup completes the same finite cache
// work used for later local invalidations before the first rendered frame.
class VXGIResources
{
public:
    VXGIWorld world;
    VXGISDF legacySDF;
    VXGIMeshSDFCache meshSDFCache;
    VXGIMeshSDFScene meshSDFScene;
    VXGISurface surface;
    VXGIProbeTopology topology;
    VXGIGPUScene gpuScene;
    VXGIProbeState probes;
    VXGIProbeCache probeCache;
    VXGIProbePass probePass;
    VXGIProbeFilterPass filterPass;
    VXGIGlobalSDFRuntime globalSDF;

    bool Initialize(RenderDevice& device, UINT frameCount,
        const std::vector<ModelInstance>& instances, const std::vector<MaterialData>& materials,
        const VXGIConfig& config, const LightingConfig& lighting,
        ID3D12DescriptorHeap* heap, ID3D12Resource* materialBuffer,
        const InitializationProgress& progress = {})
    {
        ReportInitializationProgress(progress, 0.0f, "Preparing VXGI probe shader pipelines");
        const auto start = std::chrono::steady_clock::now();
        if (frameCount == 0)
            return Fail("VXGI initialization requires at least one frame slot.");
        m_frameCount = frameCount;
        m_initializationBatches = 0;
        m_initializationMilliseconds = 0;
        gpuScene.Initialize(frameCount);
        if (!config.enabled)
        {
            ReportInitializationProgress(progress, 1.0f, "VXGI disabled; skipping probe cache");
            m_error.clear();
            return true;
        }
        if (!probePass.Initialize(device, frameCount, true) ||
            !filterPass.Initialize(device, frameCount, true) ||
            (config.globalSDFEnabled && !globalSDF.Initialize(device, frameCount)))
            return Fail("VXGI probe pipeline initialization failed.");
        const InitializationProgress sceneProgress = progress ? InitializationProgress{
            [&progress](float fraction, const std::string& status)
            {
                ReportInitializationProgress(progress, 0.08f + 0.22f * fraction, status);
            }} : InitializationProgress{};
        if (!Prepare(device, instances, materials, config, lighting, sceneProgress))
            return false;

        const auto& parameters = probes.GetPendingParameters();
        const uint64_t count = world.GetOccupiedCenters().size();
        const uint64_t sweeps = (count + parameters.probesPerFrame - 1) / parameters.probesPerFrame;
        // Each probe needs one classification and at most the configured number
        // of successful captures. A persistent failure must not stall loading.
        const uint64_t batchLimit = sweeps * (uint64_t(parameters.maxHistoryUpdates) + 1) + frameCount + 1;
        const auto reportCaptureProgress = [&]()
        {
            if (!progress)
                return;
            const auto& current = probes.GetStats();
            const uint64_t captureTarget = count * uint64_t(parameters.maxHistoryUpdates);
            // Disabled probes finish at classification and require no radiance
            // captures. Credit their full budget so settled work is monotonic.
            const uint64_t settled = current.totalUpdates + current.disabled * uint64_t(parameters.maxHistoryUpdates);
            const float fraction = captureTarget == 0 ? 1.0f :
                static_cast<float>((std::min)(settled, captureTarget)) / static_cast<float>(captureTarget);
            ReportInitializationProgress(progress, 0.30f + 0.68f * fraction,
                "Baking VXGI probe cache: " + std::to_string(current.totalUpdates) + " / " +
                std::to_string(captureTarget - current.disabled * uint64_t(parameters.maxHistoryUpdates)) +
                " captures; " + std::to_string(current.disabled) + " disabled probes");
        };
        reportCaptureProgress();
        while (probes.GetPendingPlan().HasWork())
        {
            if (m_initializationBatches >= batchLimit)
                return Fail("VXGI initial probe cache did not finish within its finite capture budget.");
            const int slot = static_cast<int>(m_initializationBatches % frameCount);
            if (!BeginBatch(device, slot))
                return false;
            if (!Record(device, slot, heap, materialBuffer, config))
                return CloseFailedBatch(device);
            uint64_t fence = 0;
            if (!SubmitBatch(device, slot, fence) || !OnSubmitted(slot, fence))
                return false;
            ++m_initializationBatches;
            // Complete and consume this batch before testing for an idle plan;
            // an empty ready queue alone can also mean work is still in flight.
            if (!WaitSlot(device, slot) || !Prepare(device, instances, materials, config, lighting))
                return false;
            // UI callbacks occur only between completed initialization batches;
            // they must never reset the scene command list while it is recording.
            reportCaptureProgress();
        }

        const auto& stats = probes.GetStats();
        if (stats.unclassified != 0 || stats.reasons[VXGIProbeState::TraceFailure] != 0 ||
            stats.reasons[VXGIProbeState::SampleFailure] != 0)
            return Fail("VXGI initial probe cache contains unfinished or failed captures.");

        // Even small or empty scenes must have all per-frame scene tables ready
        // before rendering. This records no probe or filter capture work.
        for (UINT index = 0; index < frameCount; ++index)
        {
            ReportInitializationProgress(progress, 0.98f + 0.02f * static_cast<float>(index) /
                static_cast<float>(frameCount), "Uploading VXGI scene tables for rendering");
            const int slot = static_cast<int>(index);
            if (!BeginBatch(device, slot))
                return false;
            if (!gpuScene.Prepare(device, slot, world, meshSDFScene, &topology))
            {
                Fail("VXGI initial frame-slot scene upload failed.");
                return CloseFailedBatch(device);
            }
            uint64_t fence = 0;
            if (!SubmitBatch(device, slot, fence) || !WaitSlot(device, slot))
                return false;
        }

        m_initializationMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        ErrorLog::Write("VXGI: initial cache ready before rendering; " +
            std::to_string(stats.valid) + " valid probes, " + std::to_string(stats.disabled) +
            " disabled, " + std::to_string(m_initializationBatches) + " capture batches, " +
            std::to_string(m_initializationMilliseconds) + " ms.");
        m_error.clear();
        ReportInitializationProgress(progress, 1.0f, "VXGI initial probe cache ready");
        return true;
    }

    bool Prepare(RenderDevice& device, const std::vector<ModelInstance>& instances,
        const std::vector<MaterialData>& materials, const VXGIConfig& config, const LightingConfig& lighting,
        const InitializationProgress& progress = {})
    {
        // Runtime consumes only already completed slots. Initialization waits
        // explicitly outside this method; ordinary frames add no global waits.
        for (UINT index = 0; index < m_frameCount; ++index)
        {
            const int slot = static_cast<int>(index);
            const auto completed = device.GetFence(slot)->GetCompletedValue();
            if (completed == UINT64_MAX || (completed >= device.GetFenceValue(slot) &&
                !probePass.Poll(device, slot, probes)))
                return Fail("VXGI failed to receive completed probe work.");
        }
        probeCache.AcceptDependencies(probePass.TakeCompletedDependencies(), probes);

        if (progress)
            ReportInitializationProgress(progress, 0.0f, "Allocating VXGI surface cells and surrounding probe layers");
        const uint64_t previousWorldRevision = world.GetRevision();
        const uint32_t probePaddingLayers = static_cast<uint32_t>(std::clamp(config.probePaddingLayers, 0, 2));
        if (!world.Update(instances, config.voxelSize, probePaddingLayers))
            return Fail("VXGI: " + world.GetLastError());
        if (world.GetRevision() != previousWorldRevision)
        {
            const auto& stats = world.GetStats();
            ErrorLog::Write("VXGI: revision " + std::to_string(world.GetRevision()) + ", " +
                std::to_string(stats.includedInstances) + "/" + std::to_string(stats.sceneInstances) +
                " instances, " + std::to_string(stats.triangles) + " triangles, " +
                std::to_string(stats.cells) + " occupied geometry cells, " +
                std::to_string(world.GetOccupiedCenters().size()) + " fixed probes (" +
                std::to_string(probePaddingLayers) + " padding layers).");
        }

        if (progress)
            ReportInitializationProgress(progress, 0.35f, "Building shared mesh SDF assets and instance BVH");
        const uint64_t previousSDFRevision = meshSDFScene.GetRevision();
        const VXGIMeshSDFCache::BuildSettings meshSettings{ config.sdfCellSize };
        if (!meshSDFScene.UpdateInstances(instances, meshSDFCache, meshSettings))
            return Fail("VXGI Mesh SDF: " + meshSDFScene.GetLastError());
        if (meshSDFScene.GetRevision() != previousSDFRevision)
        {
            const auto& stats = meshSDFCache.GetStats();
            ErrorLog::Write("VXGI Mesh SDF: " + std::to_string(stats.assets) + " shared assets, " +
                std::to_string(meshSDFScene.GetInstances().size()) + " instances, " +
                std::to_string(stats.nodes) + " local distance nodes, " +
                std::to_string(stats.builds) + " asset builds.");
        }

        if (progress)
            ReportInitializationProgress(progress, 0.75f, "Building VXGI probe neighbors and material sources");
        VXGISurface::Parameters sourceParameters{};
        sourceParameters.lightDirection = lighting.lightDir;
        sourceParameters.lightColor = lighting.lightColor;
        sourceParameters.materialMipLevel = config.surfaceMaterialMipLevel;
        sourceParameters.rayMinDistance = config.surfaceRayMinDistance;
        sourceParameters.maxRayDistance = config.surfaceMaxRayDistance;
        const uint64_t previousTopologyRevision = topology.GetRevision();
        if (!topology.Update(world))
            return Fail("VXGI topology: " + topology.GetLastError());
        if (!surface.Update(materials, world.GetGeometryRevision(), meshSDFScene.GetRevision(),
            world.GetMaterialRevision(), sourceParameters, world.GetCoverageRevision()))
            return Fail("VXGI surface: " + surface.GetLastError());

        if (progress)
            ReportInitializationProgress(progress, 0.90f, "Allocating the initial VXGI probe cache");
        VXGIProbeState::Parameters probeParameters{};
        probeParameters.probesPerFrame = static_cast<uint32_t>(config.probeProbesPerFrame);
        probeParameters.classifyRays = static_cast<uint32_t>(config.probeClassifyRays);
        probeParameters.raysPerUpdate = static_cast<uint32_t>(config.probeRaysPerUpdate);
        probeParameters.maxHistoryUpdates = static_cast<uint32_t>(config.probeMaxHistoryUpdates);
        probeParameters.backfaceThreshold = config.probeBackfaceThreshold;
        probeParameters.rayMinDistance = sourceParameters.rayMinDistance;
        probeParameters.rayMaxDistance = sourceParameters.maxRayDistance;
        if (!probeCache.Prepare(world, meshSDFScene, surface, probes, probeParameters))
            return Fail("VXGI probe cache: " + probeCache.GetLastError());
        probePass.SetDependencyDomain(probeCache.GetDomain());
        if (topology.GetRevision() != previousTopologyRevision)
            filterPass.SetTopology(topology.GetNeighbors());
        m_error.clear();
        if (progress)
            ReportInitializationProgress(progress, 1.0f, "VXGI scene and probe allocation ready");
        return true;
    }

    bool Record(RenderDevice& device, int slot, ID3D12DescriptorHeap* heap,
        ID3D12Resource* materialBuffer, const VXGIConfig& config)
    {
        if (!gpuScene.Prepare(device, slot, world, meshSDFScene, &topology) ||
            !probePass.Record(device, heap, slot, gpuScene, legacySDF, surface,
                materialBuffer ? materialBuffer->GetGPUVirtualAddress() : 0, probes) ||
            !filterPass.Record(device, heap, slot, gpuScene, legacySDF,
                probePass.GetDisplayHistory(), probes, world.GetVoxelSize(),
                config.probeFilterNeighborWeight, surface, materialBuffer))
            return Fail("VXGI probe GPU update failed.");
        m_error.clear();
        return true;
    }

    bool OnSubmitted(int slot, uint64_t fence)
    {
        if (!probePass.OnSubmitted(slot, fence, probes) || !filterPass.OnSubmitted(slot, fence))
            return Fail("VXGI failed to commit the submitted probe batch.");
        probeCache.OnSubmitted(probes.GetPendingWork());
        if (!globalSDF.OnSubmitted()) return Fail(globalSDF.GetLastError());
        m_error.clear();
        return true;
    }

    const std::string& GetLastError() const { return m_error; }
    double GetInitializationMilliseconds() const { return m_initializationMilliseconds; }
    uint64_t GetInitializationBatches() const { return m_initializationBatches; }
    UINT GetFrameCount() const { return m_frameCount; }

    bool RecordGlobalSDF(RenderDevice& device, int slot, const DirectX::XMFLOAT3& cameraPosition,
        const VXGIConfig& config)
    {
        if (!config.globalSDFEnabled) return true;
        if (!globalSDF.Initialize(device, m_frameCount) ||
            !globalSDF.Record(device, slot, meshSDFScene, gpuScene, cameraPosition,
                {uint32_t(config.globalSDFCells), uint32_t(config.globalSDFClipmaps), config.globalSDFCellSize,
                    config.globalSDFLayerCells}))
            return Fail(globalSDF.GetLastError());
        return true;
    }

private:
    bool Fail(const std::string& message)
    {
        m_error = message;
        return false;
    }

    bool Check(HRESULT result, const char* operation)
    {
        if (SUCCEEDED(result))
            return true;
        ErrorLog::HRESULT(operation, result);
        return Fail(operation);
    }

    bool WaitSlot(RenderDevice& device, int slot)
    {
        device.WaitForPreviousFrame(slot);
        const auto completed = device.GetFence(slot)->GetCompletedValue();
        if (completed == UINT64_MAX || completed < device.GetFenceValue(slot))
            return Fail("VXGI initialization did not complete its GPU fence.");
        return Check(device.GetDevice()->GetDeviceRemovedReason(), "VXGI initialization device status.");
    }

    bool BeginBatch(RenderDevice& device, int slot)
    {
        return WaitSlot(device, slot) &&
            Check(device.GetCommandAllocator(slot)->Reset(), "VXGI initialization allocator reset failed.") &&
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot), nullptr),
                "VXGI initialization command-list reset failed.");
    }

    bool SubmitBatch(RenderDevice& device, int slot, uint64_t& fence)
    {
        if (!Check(device.GetCommandList()->Close(), "VXGI initialization command-list close failed."))
            return false;
        ID3D12CommandList* lists[] = { device.GetCommandList() };
        device.GetCommandQueue()->ExecuteCommandLists(1, lists);
        fence = ++device.GetFenceValue(slot);
        return Check(device.GetCommandQueue()->Signal(device.GetFence(slot), fence),
            "VXGI initialization fence signal failed.");
    }

    bool CloseFailedBatch(RenderDevice& device)
    {
        device.GetCommandList()->Close();
        return false;
    }

    UINT m_frameCount = 0;
    uint64_t m_initializationBatches = 0;
    double m_initializationMilliseconds = 0;
    std::string m_error;
};
