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
        float screenRadius=0.5f;
        uint32_t quality=1;
        uint64_t worldGeometryRevision=0;
        uint64_t sdfGeometryRevision=0;
        uint64_t surfaceRevision=0;
        uint64_t probeResetEpoch=0;
        uint64_t screenSourceStamp=0; // Actual uploaded screen source, all modes.
        uint32_t traceDownsampleFactor=2;
        bool checkerboard=false;
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
        if(a.effectiveWorldMode!=b.effectiveWorldMode || a.screenRadius!=b.screenRadius ||
            a.quality!=b.quality || a.traceDownsampleFactor!=b.traceDownsampleFactor || a.checkerboard!=b.checkerboard ||
            a.worldGeometryRevision!=b.worldGeometryRevision ||
            a.surfaceRevision!=b.surfaceRevision || a.screenSourceStamp!=b.screenSourceStamp) return false;
        if(a.effectiveWorldMode>=1 && a.sdfGeometryRevision!=b.sdfGeometryRevision) return false;
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
