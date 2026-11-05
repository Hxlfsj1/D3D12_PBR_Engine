#pragma once

#include <cstdint>
#include <array>

// Source identity for accumulated screen GI. Scheduling/frame counters are not
// source changes: normal probe updates must keep the screen temporal history.
// The caller supplies validated settings and commits only after submission.
class SSGIHybridState
{
public:
    struct Key
    {
        uint32_t effectiveWorldMode=0; // 0 ScreenOnly; 1/2 cached-radiance fallback.
        uint64_t worldGeometryRevision=0;
        uint64_t sdfGeometryRevision=0;
        uint64_t surfaceRevision=0;
        uint64_t probeResetEpoch=0;
        uint64_t screenSourceStamp=0; // Actual uploaded screen source, all modes.
        uint32_t traceDownsampleFactor=2;
        bool screenProbes=false;
        uint32_t raysPerScreenProbe=128,screenProbeHistoryUpdates=4;
        bool worldSharing=false;
        uint32_t worldOwnerHistoryUpdates=1;
        bool worldSpatialFilter=false;
        uint32_t worldSpatialStep=2;
        float worldNearDistance=2.0f,worldNearExitDistance=2.2f;
        float worldMeshMinHitDistance=0.0f;
        uint32_t worldNearRayCount=8,worldFarRayCount=8;
        uint32_t worldSamplingVersion=12; // Threshold marches use the full conservative safeDistance.
        bool globalEnabled=false;
        float globalTraceDistance=0,globalCellSize=0;
        uint32_t globalMaxSteps=0,globalCells=0,globalClipmaps=0;
        std::array<uint32_t,3> globalLayerCells{};
    };

    // Pure read: repeated recording attempts keep requiring the same reset
    // until the caller commits a command list that was successfully submitted.
    bool NeedsReset(const Key& key) const noexcept
    {
        return !m_committed || !SameSource(m_key,key);
    }

    void Commit(const Key& key) noexcept
    {
        m_key=key;
        m_committed=true;
    }

    // Resolution/resource recreation, explicit camera cuts and other external
    // history invalidation may use this without inventing a source revision.
    void Invalidate() noexcept { m_committed=false; }

private:
    static bool SameSource(const Key& a,const Key& b) noexcept
    {
        // World geometry/material/light revisions may affect the screen bounce
        // source as well. Pass zero when that source identity is not supplied.
        if(a.screenProbes!=b.screenProbes || a.raysPerScreenProbe!=b.raysPerScreenProbe ||
            a.screenProbeHistoryUpdates!=b.screenProbeHistoryUpdates) return false;
        if(a.effectiveWorldMode!=b.effectiveWorldMode || a.traceDownsampleFactor!=b.traceDownsampleFactor ||
            a.worldGeometryRevision!=b.worldGeometryRevision ||
            a.surfaceRevision!=b.surfaceRevision || a.screenSourceStamp!=b.screenSourceStamp) return false;
        if(a.effectiveWorldMode>=1 && (a.sdfGeometryRevision!=b.sdfGeometryRevision ||
            (!a.screenProbes && (a.worldSharing!=b.worldSharing || a.worldNearDistance!=b.worldNearDistance ||
                a.worldOwnerHistoryUpdates!=b.worldOwnerHistoryUpdates ||
                a.worldSpatialFilter!=b.worldSpatialFilter || a.worldSpatialStep!=b.worldSpatialStep ||
                a.worldNearExitDistance!=b.worldNearExitDistance ||
                a.worldNearRayCount!=b.worldNearRayCount || a.worldFarRayCount!=b.worldFarRayCount)) ||
            a.worldMeshMinHitDistance!=b.worldMeshMinHitDistance ||
            a.worldSamplingVersion!=b.worldSamplingVersion)) return false;
        if(a.effectiveWorldMode>=1 && (a.globalEnabled!=b.globalEnabled ||
            (a.globalEnabled && (a.globalTraceDistance!=b.globalTraceDistance ||
                a.globalCellSize!=b.globalCellSize || a.globalMaxSteps!=b.globalMaxSteps ||
                a.globalCells!=b.globalCells || a.globalClipmaps!=b.globalClipmaps ||
                a.globalLayerCells!=b.globalLayerCells)))) return false;
        return a.effectiveWorldMode==0 || a.probeResetEpoch==b.probeResetEpoch;
    }

    Key m_key{};
    bool m_committed=false;
};
