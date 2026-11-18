#pragma once

#include <cstdint>

// Source identity for accumulated GI using the Surface Cache light source.
// Scheduling, cache layout and update counters are not source-content changes.
// The caller supplies validated settings and commits only after submission.
class SSGISourceState
{
public:
    struct Key
    {
        uint64_t sourceStamp=0;
        uint32_t traceDownsampleFactor=2;
        uint32_t denoiseDownsampleFactor=2;
        bool screenProbes=false;
        bool lumen=true;
        uint32_t screenRaysPerProbe=64,screenProbeHistoryUpdates=4;
        uint32_t emissiveSamplesPerProbe=0;
        uint32_t maxTraceIterations=96;
        float screenProbeJitterWidthFraction=0.125f;
        bool screenProbeAdaptiveEnabled=false;
        float screenProbeAdaptiveMaxFraction=0.25f;
        float screenTraceDistance=2.0f;
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
        // Includes source content and active tracing settings, never cache layout
        // or capture/relight counters. Normal updates preserve temporal history.
        if(a.lumen!=b.lumen || a.screenProbes!=b.screenProbes ||
            a.screenRaysPerProbe!=b.screenRaysPerProbe ||
            a.emissiveSamplesPerProbe!=b.emissiveSamplesPerProbe ||
            a.maxTraceIterations!=b.maxTraceIterations ||
            a.screenProbeJitterWidthFraction!=b.screenProbeJitterWidthFraction ||
            a.screenProbeAdaptiveEnabled!=b.screenProbeAdaptiveEnabled ||
            a.screenProbeAdaptiveMaxFraction!=b.screenProbeAdaptiveMaxFraction ||
            a.screenProbeHistoryUpdates!=b.screenProbeHistoryUpdates ||
            a.screenTraceDistance!=b.screenTraceDistance) return false;
        return a.traceDownsampleFactor==b.traceDownsampleFactor &&
            a.denoiseDownsampleFactor==b.denoiseDownsampleFactor && a.sourceStamp==b.sourceStamp;
    }

    Key m_key{};
    bool m_committed=false;
};
