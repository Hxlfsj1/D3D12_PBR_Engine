#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// CPU submission state for fixed-position GPU probes. World allocation owns the
// positions; the GPU history buffers, rather than World::Probe, own classification
// and unconvolved RGB radiance SH. No camera state belongs in this scheduler.
class VXGIProbeState
{
public:
    enum State : uint32_t { Unclassified=0,Valid=1,Disabled=2 };
    enum Reason : uint32_t { None=0,Backface=1,UnsafeOrigin=2,TraceFailure=3,SampleFailure=4 };
    enum ResetMode : uint32_t { KeepHistory=0,ResetClassification=1,ResetSH=2 };

    struct GPURecord
    {
        // Real SH order [Y00,X,Y,Z], RGB radiance before cosine convolution.
        // Coefficients may be negative. All four w components must remain zero.
        DirectX::XMFLOAT4 sh[4]={};
        // updateCount/sampleCount are saturating cumulative successful totals;
        // maxHistoryUpdates is the finite successful-update target for each dirty probe.
        uint32_t state=Unclassified,updateCount=0,sampleCount=0,lastUpdateFrame=0;
        float backfaceRatio=0,minHitDistance=0;
        uint32_t classificationRays=0,reason=None;
    };
    static_assert(std::is_standard_layout_v<GPURecord> && sizeof(GPURecord)==96);
    static_assert(offsetof(GPURecord,sh)==0 && offsetof(GPURecord,state)==64);
    static_assert(offsetof(GPURecord,updateCount)==68 && offsetof(GPURecord,sampleCount)==72);
    static_assert(offsetof(GPURecord,lastUpdateFrame)==76 && offsetof(GPURecord,backfaceRatio)==80);
    static_assert(offsetof(GPURecord,minHitDistance)==84 && offsetof(GPURecord,classificationRays)==88);
    static_assert(offsetof(GPURecord,reason)==92);

    struct Parameters
    {
        uint32_t probesPerFrame=256,classifyRays=64,raysPerUpdate=64,maxHistoryUpdates=32;
        float backfaceThreshold=0.25f;
        float rayMinDistance=0.0001f; // Copy Surface::Parameters::rayMinDistance.
        float rayMaxDistance=1000; // Copy Surface::Parameters::maxRayDistance.
    };
    struct Sources
    {
        uint64_t worldGeometryRevision=0,worldAllocationRevision=0,sdfGeometryRevision=0,surfaceRevision=0;
        uint64_t probeCount=0;
        uint64_t coverageRevision=0; // Appended for legacy aggregate callers.
    };
    struct WorkItem
    {
        uint32_t probeIndex=0,resetMode=KeepHistory,generation=0,padding=0;
    };
    static_assert(sizeof(WorkItem)==16 && std::is_standard_layout_v<WorkItem>);
    struct PendingPlan
    {
        uint32_t resetMode=KeepHistory,start=0,count=0,frameIndex=0;
        uint32_t readHistoryIndex=0,writeHistoryIndex=1;
        uint64_t token=0,epoch=0;
        bool layoutChanged=false;
        uint64_t layoutRevision=0;
        bool HasWork() const { return count!=0 || layoutChanged; }
    };
    struct Stats
    {
        uint64_t probeCount=0,valid=0,disabled=0,unclassified=0;
        std::array<uint64_t,5> reasons={};
        uint64_t totalUpdates=0,totalSamples=0,probesWithUpdates=0;
        uint32_t minimumUpdates=0,maximumUpdates=0,minimumAge=0,maximumAge=0;
        double averageUpdates=0,averageAge=0;
        // Estimated ownership: two GPU history records per probe; no CPU record
        // copy is retained after a completed readback has been summarized.
        uint64_t cpuMemoryBytes=0,gpuHistoryBytes=0,memoryBytes=0;
        uint64_t worldGeometryRevision=0,worldAllocationRevision=0,sdfGeometryRevision=0,surfaceRevision=0;
        uint64_t coverageRevision=0;
        uint64_t submissions=0,classificationResets=0,shResets=0,acceptedReadbacks=0;
        uint64_t epoch=0;
        uint32_t submittedFrameIndex=0,completedFrameIndex=0,lastResetFrameIndex=0;
        bool hasCompletedReadback=false;
    };

    // New allocation indices map to the previous history indices. UINT_MAX is a
    // new probe. Empty remap means unchanged topology. Dirty entries are merged;
    // classification reset dominates SH reset. Generation numbers are scheduler
    // owned, so callers pass zero in dirty.generation.
    bool ConfigureCache(uint64_t layoutRevision,std::span<const uint32_t> remap,
        std::span<const WorkItem> dirty)
    {
        return Configure(layoutRevision,remap,dirty,true);
    }
    bool HasSparseCache() const { return m_controllerManaged; }
    bool IsCacheEnabled() const { return m_controllerManaged; }
    uint32_t GetGeneration(uint32_t index) const
    {
        return index<m_entries.size() ? m_entries[index].generation : 0;
    }
    bool IsCurrentWork(const WorkItem& work,uint32_t completedFrameIndex) const
    {
        return completedFrameIndex!=0 && work.probeIndex<m_entries.size() && work.generation==m_entries[work.probeIndex].generation &&
            completedFrameIndex==m_entries[work.probeIndex].inFlightFrame;
    }
    std::span<const WorkItem> GetPendingWork() const { return m_pendingWork; }
    std::span<const WorkItem> GetPendingInvalidations() const { return m_pendingInvalidations; }
    std::span<const uint32_t> GetPendingRemap() const { return m_remap; }

    // Prepare never consumes a ready item. A retry has the same token and work;
    // only successful submission moves items into the in-flight state.
    bool Prepare(const Sources& sources,const Parameters& parameters)
    {
        if(!m_controllerManaged) return PrepareLegacy(sources,parameters);
        if(!Validate(sources,parameters)) { m_pendingValid=false; return false; }
        if(m_pendingValid && Same(sources,m_pendingSources) && Same(parameters,m_pendingParameters))
        { m_error.clear(); return true; }
        if(!m_cacheConfigured || (!m_controllerManaged && sources.probeCount!=m_entries.size()))
        {
            std::vector<uint32_t> remap(static_cast<size_t>(sources.probeCount),UINT32_MAX);
            if(!Configure(m_layoutRevision+1,remap,{},false)) return false;
        }
        if(sources.probeCount!=m_entries.size())
        { m_pendingValid=false; m_error="VXGI probe cache remap must match the current allocation."; return false; }
        uint32_t resetMode=KeepHistory;
        if(!m_initialized || parameters.classifyRays!=m_parameters.classifyRays ||
            parameters.backfaceThreshold!=m_parameters.backfaceThreshold ||
            parameters.rayMinDistance!=m_parameters.rayMinDistance || parameters.rayMaxDistance!=m_parameters.rayMaxDistance ||
            (!m_controllerManaged && (sources.worldGeometryRevision!=m_sources.worldGeometryRevision ||
             sources.worldAllocationRevision!=m_sources.worldAllocationRevision ||
             sources.sdfGeometryRevision!=m_sources.sdfGeometryRevision || sources.coverageRevision!=m_sources.coverageRevision)))
            resetMode=ResetClassification;
        else if(parameters.raysPerUpdate!=m_parameters.raysPerUpdate ||
            parameters.maxHistoryUpdates!=m_parameters.maxHistoryUpdates ||
            (!m_controllerManaged && sources.surfaceRevision!=m_sources.surfaceRevision)) resetMode=ResetSH;
        // Do not re-issue a global invalidation merely because a recorder retries
        // the same uncommitted source/configuration.
        if(resetMode!=KeepHistory && (!m_preparedInputs || !Same(sources,m_preparedSources) || !Same(parameters,m_preparedParameters)))
        {
            for(uint32_t index=0;index<m_entries.size();++index) Invalidate(index,resetMode);
            m_contentChanged=true;
        }
        m_preparedInputs=true; m_preparedSources=sources; m_preparedParameters=parameters;
        m_pendingWork.clear(); m_pendingInvalidations.clear();
        for(const auto index:m_resetReady)
        {
            const auto& entry=m_entries[index];
            if(entry.resetQueued) m_pendingInvalidations.push_back({index,entry.resetMode,entry.generation,0});
        }
        size_t end=m_queueCursor;
        while(end<m_ready.size() && m_pendingWork.size()<parameters.probesPerFrame)
        {
            const auto index=m_ready[end++];
            const auto& entry=m_entries[index];
            if(entry.queued && entry.inFlightFrame==0 && !entry.complete)
                m_pendingWork.push_back({index,entry.resetMode,entry.generation,0});
        }
        const bool hasWork=!m_pendingWork.empty() || m_layoutChanged;
        if(hasWork && m_frameIndex==(std::numeric_limits<uint32_t>::max)())
        { m_pendingValid=false; m_error="VXGI probe submission frame index exhausted."; return false; }
        if(hasWork) { ++m_token; if(m_token==0) ++m_token; }
        m_pending={resetMode,0,static_cast<uint32_t>(m_pendingWork.size()),m_frameIndex+(hasWork ? 1u : 0u),
            m_readHistoryIndex,hasWork ? 1-m_readHistoryIndex : m_readHistoryIndex,m_token,
            m_revision+(m_contentChanged || m_layoutChanged ? 1u : 0u),m_layoutChanged,m_layoutRevision};
        m_pendingQueueEnd=end;
        m_pendingSources=sources; m_pendingParameters=parameters; m_pendingValid=true;
        m_error.clear(); return true;
    }
    bool Update(const Sources& sources,const Parameters& parameters) { return Prepare(sources,parameters); }

    bool CommitSubmitted(const PendingPlan& plan)
    {
        if(!m_controllerManaged) return CommitLegacy(plan);
        if(!m_pendingValid || !Same(plan,m_pending))
        { m_error="VXGI probe submission requires the current unmodified pending plan."; return false; }
        for(const auto& work:m_pendingWork)
        {
            auto& entry=m_entries[work.probeIndex];
            entry.queued=false; entry.inFlightFrame=plan.frameIndex;
        }
        for(const auto& work:m_pendingInvalidations)
        {
            auto& entry=m_entries[work.probeIndex];
            entry.resetQueued=false; entry.resetMode=KeepHistory;
        }
        m_resetReady.clear();
        m_queueCursor=m_pendingQueueEnd;
        if(m_queueCursor==m_ready.size()) { m_ready.clear(); m_queueCursor=0; }
        if(plan.HasWork())
        {
            m_frameIndex=plan.frameIndex; m_readHistoryIndex=plan.writeHistoryIndex;
            ++m_stats.submissions;
            if(m_contentChanged || m_layoutChanged)
            {
                if(plan.resetMode==ResetClassification) ++m_stats.classificationResets;
                else if(plan.resetMode==ResetSH) ++m_stats.shResets;
                m_lastResetFrameIndex=plan.frameIndex;
            }
        }
        m_sources=m_pendingSources; m_parameters=m_pendingParameters; m_initialized=true;
        m_pendingValid=false; m_revision=plan.epoch; m_layoutChanged=false; m_contentChanged=false; m_remap.clear();
        m_stats.epoch=m_revision; m_stats.probeCount=m_sources.probeCount;
        m_stats.cpuMemoryBytes=sizeof(*this)+m_entries.capacity()*sizeof(Entry)+m_ready.capacity()*sizeof(uint32_t)+
            (m_remap.capacity()+m_resetReady.capacity())*sizeof(uint32_t)+
            (m_pendingWork.capacity()+m_pendingInvalidations.capacity())*sizeof(WorkItem)+
            (m_updateHistogram.size()+m_frameHistogram.size())*(sizeof(std::pair<const uint32_t,uint64_t>)+3*sizeof(void*));
        m_stats.gpuHistoryBytes=m_sources.probeCount*sizeof(GPURecord)*2;
        m_stats.memoryBytes=m_stats.cpuMemoryBytes+m_stats.gpuHistoryBytes;
        m_stats.worldGeometryRevision=m_sources.worldGeometryRevision;
        m_stats.worldAllocationRevision=m_sources.worldAllocationRevision;
        m_stats.sdfGeometryRevision=m_sources.sdfGeometryRevision; m_stats.surfaceRevision=m_sources.surfaceRevision;
        m_stats.coverageRevision=m_sources.coverageRevision; m_stats.submittedFrameIndex=m_frameIndex;
        m_stats.lastResetFrameIndex=m_lastResetFrameIndex;
        RefreshSparseStats();
        m_error.clear(); return true;
    }

    // Compact completed packets are tagged with the exact submitted work. A
    // changed probe or remapped allocation rejects old work independently of
    // other probes; unrelated older batches can still finish successfully.
    bool AcceptBatchResults(std::span<const WorkItem> work,std::span<const GPURecord> records,
        uint32_t completedFrameIndex,uint64_t epoch)
    {
        if(!m_initialized || work.size()!=records.size() || completedFrameIndex>m_frameIndex || epoch>m_revision)
        { m_error="VXGI probe feedback must match a completed submitted batch."; return false; }
        for(const auto& record:records)
            if(!ValidRecord(record,completedFrameIndex))
            { m_error="VXGI probe feedback contains an invalid GPU record."; return false; }
        uint32_t accepted=0;
        for(size_t item=0;item<work.size();++item)
        {
            if(!IsCurrentWork(work[item],completedFrameIndex)) continue;
            auto& entry=m_entries[work[item].probeIndex]; const auto& record=records[item];
            RemoveSparseSummary(entry);
            entry.inFlightFrame=0; entry.state=record.state; entry.updateCount=record.updateCount;
            entry.sampleCount=record.sampleCount;entry.lastUpdateFrame=record.lastUpdateFrame;entry.reason=record.reason;
            entry.complete=record.state==Disabled || (record.state==Valid && record.updateCount>=m_parameters.maxHistoryUpdates);
            AddSparseSummary(entry);++accepted;
            if(!entry.complete) Enqueue(work[item].probeIndex);
        }
        if(accepted)
        {
            ++m_stats.acceptedReadbacks;m_stats.hasCompletedReadback=true;
            m_stats.completedFrameIndex=(std::max)(m_stats.completedFrameIndex,completedFrameIndex);
            RefreshSparseStats();
        }
        m_pendingValid=false; m_error.clear(); return true;
    }
    // Caller must wait for the GPU fence and tag the actual history snapshot's
    // frame. Pre-reset or out-of-order readbacks are rejected. Statistics are
    // committed only when every record passes layout/finite-value validation.
    bool AcceptReadback(std::span<const GPURecord> records,uint32_t completedFrameIndex,uint64_t epoch)
    {
        if(!m_initialized || epoch!=m_revision || records.size()!=m_sources.probeCount || completedFrameIndex>m_frameIndex ||
            completedFrameIndex<m_lastResetFrameIndex ||
            (m_stats.hasCompletedReadback && completedFrameIndex<m_stats.completedFrameIndex))
        {
            m_error="VXGI probe readback must match the current allocation and a completed frame in the current reset epoch.";
            return false;
        }
        Stats next=m_stats;
        ClearReadbackStats(next);
        next.minimumUpdates=records.empty() ? 0 : (std::numeric_limits<uint32_t>::max)();
        next.minimumAge=(std::numeric_limits<uint32_t>::max)();
        uint64_t totalAge=0;
        for(const auto& record:records)
        {
            if(!ValidRecord(record,completedFrameIndex))
            {
                m_error="VXGI probe readback contains an invalid state, SH layout, classification value or frame stamp.";
                return false;
            }
            if(record.state==Valid) ++next.valid;
            else if(record.state==Disabled) ++next.disabled;
            else ++next.unclassified;
            ++next.reasons[record.reason];
            next.totalUpdates+=record.updateCount;
            next.totalSamples+=record.sampleCount;
            next.minimumUpdates=(std::min)(next.minimumUpdates,record.updateCount);
            next.maximumUpdates=(std::max)(next.maximumUpdates,record.updateCount);
            if(record.updateCount!=0)
            {
                const uint32_t age=completedFrameIndex-record.lastUpdateFrame;
                ++next.probesWithUpdates;
                totalAge+=age;
                next.minimumAge=(std::min)(next.minimumAge,age);
                next.maximumAge=(std::max)(next.maximumAge,age);
            }
        }
        next.averageUpdates=records.empty() ? 0 : double(next.totalUpdates)/double(records.size());
        next.averageAge=next.probesWithUpdates==0 ? 0 : double(totalAge)/double(next.probesWithUpdates);
        if(next.probesWithUpdates==0) next.minimumAge=0;
        next.completedFrameIndex=completedFrameIndex;
        next.hasCompletedReadback=true;
        ++next.acceptedReadbacks;
        m_stats=next;
        m_error.clear();
        return true;
    }
    bool AcceptReadback(std::span<const GPURecord> records,uint32_t completedFrameIndex)
    {
        return AcceptReadback(records,completedFrameIndex,m_revision);
    }

    const PendingPlan& GetPendingPlan() const { return m_pending; }
    bool HasPendingPlan() const { return m_pendingValid; }
    const Parameters& GetParameters() const { return m_parameters; }
    const Parameters& GetPendingParameters() const { return m_pendingParameters; }
    const Sources& GetSources() const { return m_sources; }
    const Sources& GetPendingSources() const { return m_pendingSources; }
    const Stats& GetStats() const { return m_stats; }
    // Source/history epoch, unchanged by ordinary frame submissions or budget.
    uint64_t GetRevision() const { return m_revision; }
    uint32_t GetCursor() const { return m_cursor; }
    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetReadHistoryIndex() const { return m_readHistoryIndex; }
    const std::string& GetLastError() const { return m_error; }

    // Reference of the GPU reset contract, useful for tests and buffer init.
    // ResetSH retains only reliable classifications. Trace/sample failures may
    // depend on changed ray intervals or sources, so both are reclassified.
    static GPURecord ResetRecord(const GPURecord& source,uint32_t resetMode)
    {
        if(resetMode==ResetClassification) return {};
        if(resetMode!=ResetSH) return source;
        GPURecord record=source;
        for(auto& coefficient:record.sh) coefficient={};
        record.updateCount=0; record.sampleCount=0; record.lastUpdateFrame=0;
        if(record.reason==TraceFailure || record.reason==SampleFailure) return {};
        return record;
    }

    // Caller supplies a unit direction; these helpers share shader constants and
    // ordering and perform no direction normalization or probe movement.
    static std::array<float,4> SHBasis(DirectX::XMFLOAT3 unitDirection)
    {
        return {0.28209479177387814f,0.4886025119029199f*unitDirection.x,
            0.4886025119029199f*unitDirection.y,0.4886025119029199f*unitDirection.z};
    }
    static void AddRadianceSample(GPURecord& record,DirectX::XMFLOAT3 unitDirection,
        DirectX::XMFLOAT3 radiance,float solidAngleWeight)
    {
        const auto basis=SHBasis(unitDirection);
        for(size_t index=0;index<basis.size();++index)
        {
            auto& value=record.sh[index];
            const float weight=basis[index]*solidAngleWeight;
            value.x+=radiance.x*weight; value.y+=radiance.y*weight; value.z+=radiance.z*weight; value.w=0;
        }
    }
    static DirectX::XMFLOAT3 EvaluateRadianceSH(const GPURecord& record,DirectX::XMFLOAT3 unitDirection)
    {
        return Evaluate(record,unitDirection,1.0f,false);
    }
    static DirectX::XMFLOAT3 EvaluateIrradianceOverPi(const GPURecord& record,DirectX::XMFLOAT3 unitNormal)
    {
        // Cosine convolution / pi: l0=1, l1=2/3. Clamp only reconstruction,
        // preserving signed coefficients throughout projection/history storage.
        return Evaluate(record,unitNormal,2.0f/3.0f,true);
    }

private:
    static bool Same(const Sources& a,const Sources& b)
    {
        return a.worldGeometryRevision==b.worldGeometryRevision && a.worldAllocationRevision==b.worldAllocationRevision &&
            a.sdfGeometryRevision==b.sdfGeometryRevision && a.surfaceRevision==b.surfaceRevision && a.probeCount==b.probeCount &&
            a.coverageRevision==b.coverageRevision;
    }
    static bool Same(const Parameters& a,const Parameters& b)
    {
        return a.probesPerFrame==b.probesPerFrame && a.classifyRays==b.classifyRays && a.raysPerUpdate==b.raysPerUpdate &&
            a.maxHistoryUpdates==b.maxHistoryUpdates && a.backfaceThreshold==b.backfaceThreshold &&
            a.rayMinDistance==b.rayMinDistance && a.rayMaxDistance==b.rayMaxDistance;
    }
    static bool Same(const PendingPlan& a,const PendingPlan& b)
    {
        return a.resetMode==b.resetMode && a.start==b.start && a.count==b.count && a.frameIndex==b.frameIndex &&
            a.readHistoryIndex==b.readHistoryIndex && a.writeHistoryIndex==b.writeHistoryIndex && a.token==b.token && a.epoch==b.epoch && a.layoutChanged==b.layoutChanged && a.layoutRevision==b.layoutRevision;
    }
    static bool ValidRayCount(uint32_t count) { return count>=8 && count<=256 && count%2==0; }
    bool Validate(const Sources& sources,const Parameters& parameters)
    {
        if(sources.probeCount>(std::numeric_limits<uint32_t>::max)() || parameters.probesPerFrame==0 ||
            !ValidRayCount(parameters.classifyRays) || !ValidRayCount(parameters.raysPerUpdate) ||
            parameters.maxHistoryUpdates==0 || !std::isfinite(parameters.backfaceThreshold) ||
            parameters.backfaceThreshold<0 || parameters.backfaceThreshold>1 ||
            !std::isfinite(parameters.rayMinDistance) || parameters.rayMinDistance<0 ||
            !std::isfinite(parameters.rayMaxDistance) || parameters.rayMaxDistance<=parameters.rayMinDistance)
        {
            m_error="VXGI probes require a uint32 allocation, positive batch/history budget, even ray counts in [8,256], finite threshold in [0,1] and 0<=rayMinDistance<rayMaxDistance.";
            return false;
        }
        return true;
    }
    static bool ValidRecord(const GPURecord& record,uint32_t completedFrameIndex)
    {
        if(record.state>Disabled || record.reason>SampleFailure || record.lastUpdateFrame>completedFrameIndex ||
            !std::isfinite(record.backfaceRatio) || record.backfaceRatio<0 || record.backfaceRatio>1 ||
            !std::isfinite(record.minHitDistance) || record.minHitDistance<0 ||
            (record.classificationRays!=0 && !ValidRayCount(record.classificationRays))) return false;
        for(const auto& coefficient:record.sh)
            if(!std::isfinite(coefficient.x) || !std::isfinite(coefficient.y) || !std::isfinite(coefficient.z) || coefficient.w!=0) return false;
        return true;
    }
    static void ClearReadbackStats(Stats& stats)
    {
        stats.valid=0; stats.disabled=0; stats.unclassified=0; stats.reasons={};
        stats.totalUpdates=0; stats.totalSamples=0; stats.probesWithUpdates=0;
        stats.minimumUpdates=0; stats.maximumUpdates=0; stats.minimumAge=0; stats.maximumAge=0;
        stats.averageUpdates=0; stats.averageAge=0; stats.completedFrameIndex=0; stats.hasCompletedReadback=false;
    }
    void ClearReadbackStats() { ClearReadbackStats(m_stats); }
    static DirectX::XMFLOAT3 Evaluate(const GPURecord& record,DirectX::XMFLOAT3 direction,float firstOrderScale,bool clamp)
    {
        const auto basis=SHBasis(direction);
        DirectX::XMFLOAT3 result={};
        for(size_t index=0;index<basis.size();++index)
        {
            const float weight=basis[index]*(index==0 ? 1 : firstOrderScale);
            result.x+=record.sh[index].x*weight; result.y+=record.sh[index].y*weight; result.z+=record.sh[index].z*weight;
        }
        if(clamp) { result.x=(std::max)(0.0f,result.x); result.y=(std::max)(0.0f,result.y); result.z=(std::max)(0.0f,result.z); }
        return result;
    }

    // Reference/offscreen fixtures that never opt into ConfigureCache retain the
    // historical cyclic scheduling contract. Production always opts into the
    // sparse cache; this branch does not run in the real-time renderer.
    bool PrepareLegacy(const Sources& sources,const Parameters& parameters)
    {
        if(!Validate(sources,parameters)) { m_pendingValid=false; return false; }
        if(m_pendingValid && Same(sources,m_pendingSources) && Same(parameters,m_pendingParameters))
        { m_error.clear(); return true; }
        if(m_frameIndex==(std::numeric_limits<uint32_t>::max)())
        { m_pendingValid=false; m_error="VXGI probe frame index exhausted."; return false; }
        uint32_t resetMode=KeepHistory;
        if(!m_initialized || sources.worldGeometryRevision!=m_sources.worldGeometryRevision ||
            sources.worldAllocationRevision!=m_sources.worldAllocationRevision ||
            sources.sdfGeometryRevision!=m_sources.sdfGeometryRevision || sources.probeCount!=m_sources.probeCount ||
            sources.coverageRevision!=m_sources.coverageRevision || parameters.classifyRays!=m_parameters.classifyRays ||
            parameters.backfaceThreshold!=m_parameters.backfaceThreshold || parameters.rayMinDistance!=m_parameters.rayMinDistance ||
            parameters.rayMaxDistance!=m_parameters.rayMaxDistance) resetMode=ResetClassification;
        else if(sources.surfaceRevision!=m_sources.surfaceRevision || parameters.raysPerUpdate!=m_parameters.raysPerUpdate ||
            parameters.maxHistoryUpdates!=m_parameters.maxHistoryUpdates) resetMode=ResetSH;
        const auto count=static_cast<uint32_t>(sources.probeCount);
        const auto start=resetMode==KeepHistory ? m_cursor : 0;
        const auto batch=count==0 ? 0 : (std::min)(parameters.probesPerFrame,count-start);
        ++m_token; if(m_token==0) ++m_token;
        m_pending={resetMode,start,batch,m_frameIndex+1,m_readHistoryIndex,1-m_readHistoryIndex,m_token,
            m_revision+(resetMode==KeepHistory ? 0 : 1)};
        m_pendingWork.clear();
        for(uint32_t index=0;index<batch;++index) m_pendingWork.push_back({start+index,resetMode,0,0});
        m_pendingSources=sources; m_pendingParameters=parameters; m_pendingValid=true;
        m_error.clear(); return true;
    }
    bool CommitLegacy(const PendingPlan& plan)
    {
        if(!m_pendingValid || !Same(plan,m_pending))
        { m_error="VXGI probe submission requires the current unmodified pending plan."; return false; }
        const auto count=static_cast<uint32_t>(m_pendingSources.probeCount);
        const uint64_t next=uint64_t(plan.start)+plan.count;
        m_cursor=count==0 || next>=count ? 0 : static_cast<uint32_t>(next);
        m_frameIndex=plan.frameIndex; m_readHistoryIndex=plan.writeHistoryIndex;
        m_sources=m_pendingSources; m_parameters=m_pendingParameters; m_initialized=true;
        m_pendingValid=false; m_revision=plan.epoch; m_stats.epoch=m_revision; ++m_stats.submissions;
        if(plan.resetMode!=KeepHistory)
        {
            if(plan.resetMode==ResetClassification) ++m_stats.classificationResets; else ++m_stats.shResets;
            m_lastResetFrameIndex=plan.frameIndex; ClearReadbackStats();
        }
        m_stats.probeCount=m_sources.probeCount; m_stats.cpuMemoryBytes=sizeof(*this);
        m_stats.gpuHistoryBytes=m_sources.probeCount*sizeof(GPURecord)*2;
        m_stats.memoryBytes=m_stats.cpuMemoryBytes+m_stats.gpuHistoryBytes;
        m_stats.worldGeometryRevision=m_sources.worldGeometryRevision;
        m_stats.worldAllocationRevision=m_sources.worldAllocationRevision;
        m_stats.sdfGeometryRevision=m_sources.sdfGeometryRevision; m_stats.surfaceRevision=m_sources.surfaceRevision;
        m_stats.coverageRevision=m_sources.coverageRevision; m_stats.submittedFrameIndex=m_frameIndex;
        m_stats.lastResetFrameIndex=m_lastResetFrameIndex;
        m_error.clear(); return true;
    }
    struct Entry
    {
        uint32_t generation=0,resetMode=ResetClassification,inFlightFrame=0,state=Unclassified,updateCount=0;
        uint32_t sampleCount=0,lastUpdateFrame=0,reason=None;
        bool queued=false,complete=false,resetQueued=false;
    };
    void Enqueue(uint32_t index)
    {
        auto& entry=m_entries[index];
        if(!entry.queued && entry.inFlightFrame==0 && !entry.complete)
        { m_ready.push_back(index); entry.queued=true; }
    }
    uint32_t NextGeneration()
    {
        ++m_generation; if(m_generation==0) ++m_generation;
        return m_generation;
    }
    void Invalidate(uint32_t index,uint32_t mode)
    {
        auto& entry=m_entries[index];
        RemoveSparseSummary(entry);
        entry.generation=NextGeneration(); entry.inFlightFrame=0; entry.complete=false;
        if(mode==ResetClassification || entry.resetMode==ResetClassification) entry.resetMode=ResetClassification;
        else entry.resetMode=ResetSH;
        entry.updateCount=0;entry.sampleCount=0;entry.lastUpdateFrame=0;
        if(entry.resetMode==ResetClassification || entry.reason==TraceFailure || entry.reason==SampleFailure)
        { entry.state=Unclassified;entry.reason=None; }
        AddSparseSummary(entry);
        if(!entry.resetQueued) { m_resetReady.push_back(index); entry.resetQueued=true; }
        Enqueue(index);
    }
    bool Configure(uint64_t layoutRevision,std::span<const uint32_t> remap,std::span<const WorkItem> dirty,bool managed)
    {
        const bool change=!m_cacheConfigured || layoutRevision!=m_layoutRevision;
        const size_t count=change ? remap.size() : m_entries.size();
        if(count>UINT32_MAX)
        { m_error="VXGI probe remap exceeds the GPU index range."; return false; }
        if(change)
            for(const auto oldIndex:remap)
                if(oldIndex!=UINT32_MAX && oldIndex>=m_entries.size())
                { m_error="VXGI probe remap refers to an absent old probe."; return false; }
        for(const auto& item:dirty)
            if(item.probeIndex>=count || (item.resetMode!=ResetClassification && item.resetMode!=ResetSH))
            { m_error="VXGI probe dirty work has an invalid index or reset mode."; return false; }
        if(change)
        {
            std::vector<Entry> next(count);
            for(size_t index=0;index<count;++index)
            {
                if(remap[index]!=UINT32_MAX) next[index]=m_entries[remap[index]];
                next[index].generation=NextGeneration(); next[index].inFlightFrame=0; next[index].queued=false;
            }
            m_entries.swap(next); m_ready.clear(); m_resetReady.clear(); m_queueCursor=0;
            RebuildSparseStats();
            for(uint32_t index=0;index<m_entries.size();++index)
            {
                auto& entry=m_entries[index];
                if(entry.resetMode!=KeepHistory) { entry.resetQueued=true; m_resetReady.push_back(index); }
                Enqueue(index);
            }
            m_remap.assign(remap.begin(),remap.end()); m_layoutChanged=true; m_layoutRevision=layoutRevision;
        }
        for(const auto& item:dirty) Invalidate(item.probeIndex,item.resetMode);
        if(!dirty.empty()) m_contentChanged=true;
        if(change || !dirty.empty()) m_pendingValid=false;
        m_cacheConfigured=true; m_controllerManaged=managed;RefreshSparseStats(); m_error.clear(); return true;
    }
    // Compact summaries keep diagnostics current without a full GPU snapshot or
    // a per-frame walk over every probe. Histograms usually contain <=33 keys.
    void AddSparseSummary(const Entry& entry)
    {
        if(entry.state==Valid) ++m_stats.valid;else if(entry.state==Disabled) ++m_stats.disabled;else ++m_stats.unclassified;
        ++m_stats.reasons[entry.reason];m_stats.totalUpdates+=entry.updateCount;m_stats.totalSamples+=entry.sampleCount;
        ++m_updateHistogram[entry.updateCount];
        if(entry.updateCount)
        { ++m_stats.probesWithUpdates;++m_frameHistogram[entry.lastUpdateFrame];m_sumUpdateFrames+=entry.lastUpdateFrame; }
    }
    static void RemoveHistogram(std::map<uint32_t,uint64_t>& values,uint32_t key)
    {
        const auto found=values.find(key);if(found==values.end()) return;
        if(--found->second==0) values.erase(found);
    }
    void RemoveSparseSummary(const Entry& entry)
    {
        if(entry.state==Valid) --m_stats.valid;else if(entry.state==Disabled) --m_stats.disabled;else --m_stats.unclassified;
        --m_stats.reasons[entry.reason];m_stats.totalUpdates-=entry.updateCount;m_stats.totalSamples-=entry.sampleCount;
        RemoveHistogram(m_updateHistogram,entry.updateCount);
        if(entry.updateCount)
        { --m_stats.probesWithUpdates;RemoveHistogram(m_frameHistogram,entry.lastUpdateFrame);m_sumUpdateFrames-=entry.lastUpdateFrame; }
    }
    void RefreshSparseStats()
    {
        m_stats.minimumUpdates=m_updateHistogram.empty()?0:m_updateHistogram.begin()->first;
        m_stats.maximumUpdates=m_updateHistogram.empty()?0:m_updateHistogram.rbegin()->first;
        m_stats.averageUpdates=m_entries.empty()?0:double(m_stats.totalUpdates)/double(m_entries.size());
        m_stats.minimumAge=m_frameHistogram.empty()?0:m_frameIndex-m_frameHistogram.rbegin()->first;
        m_stats.maximumAge=m_frameHistogram.empty()?0:m_frameIndex-m_frameHistogram.begin()->first;
        m_stats.averageAge=m_stats.probesWithUpdates?double(uint64_t(m_frameIndex)*m_stats.probesWithUpdates-m_sumUpdateFrames)/double(m_stats.probesWithUpdates):0;
    }
    void RebuildSparseStats()
    {
        m_stats.valid=m_stats.disabled=m_stats.unclassified=0;m_stats.reasons={};
        m_stats.totalUpdates=m_stats.totalSamples=m_stats.probesWithUpdates=0;
        m_updateHistogram.clear();m_frameHistogram.clear();m_sumUpdateFrames=0;
        for(const auto& entry:m_entries) AddSparseSummary(entry);
        RefreshSparseStats();
    }
    std::map<uint32_t,uint64_t> m_updateHistogram,m_frameHistogram;
    uint64_t m_sumUpdateFrames=0;
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_ready,m_remap,m_resetReady;
    std::vector<WorkItem> m_pendingWork,m_pendingInvalidations;
    Sources m_preparedSources;
    Parameters m_preparedParameters;
    uint64_t m_layoutRevision=0;
    uint32_t m_generation=0;
    size_t m_queueCursor=0,m_pendingQueueEnd=0;
    bool m_cacheConfigured=false,m_controllerManaged=false,m_layoutChanged=false,m_contentChanged=false,m_preparedInputs=false;
    Sources m_sources,m_pendingSources;
    Parameters m_parameters,m_pendingParameters;
    PendingPlan m_pending;
    Stats m_stats;
    std::string m_error;
    uint64_t m_revision=0,m_token=0;
    uint32_t m_cursor=0,m_frameIndex=0,m_readHistoryIndex=0,m_lastResetFrameIndex=0;
    bool m_initialized=false,m_pendingValid=false;
};
