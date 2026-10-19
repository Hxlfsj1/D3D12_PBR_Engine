#include <windows.h> // Keep Windows min/max macros active: project include order.
#include "../Resources/VXGIProbeState.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#if !defined(min) || !defined(max)
#error VXGIProbeTests must exercise the public header with Windows min/max macros active.
#endif

namespace
{
    using Probe=VXGIProbeState;
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    void Near(float actual,float expected,const char* message)
    {
        Check(std::abs(actual-expected)<0.00002f,message);
    }
    Probe::PendingPlan Submit(Probe& state,const Probe::Sources& sources,const Probe::Parameters& parameters)
    {
        Check(state.Prepare(sources,parameters),"prepare failed");
        const auto plan=state.GetPendingPlan();
        Check(state.CommitSubmitted(plan),"commit failed");
        return plan;
    }
    void TestSubmissionAndWrap()
    {
        Probe state;
        Probe::Sources sources={1,1,1,1,10};
        Probe::Parameters parameters;
        parameters.probesPerFrame=4;
        Check(state.Prepare(sources,parameters),"initial prepare failed");
        const auto initial=state.GetPendingPlan();
        Check(initial.resetMode==Probe::ResetClassification && initial.start==0 && initial.count==4 && initial.frameIndex==1,
            "initial classify plan wrong");
        Check(initial.readHistoryIndex==0 && initial.writeHistoryIndex==1 && initial.epoch==1,"initial pingpong/epoch wrong");
        Check(state.GetRevision()==0 && state.GetCursor()==0 && state.GetFrameIndex()==0 && state.GetReadHistoryIndex()==0,
            "prepare advanced committed state");
        // Simulate recorder/submission failure by not committing, then retry.
        Check(state.Prepare(sources,parameters),"retry prepare failed");
        Check(state.GetPendingPlan().token==initial.token,"failed submission did not retain exact plan");
        auto modified=initial; ++modified.count;
        Check(!state.CommitSubmitted(modified) && state.HasPendingPlan() && state.GetRevision()==0,
            "modified plan was committed");
        Check(state.CommitSubmitted(initial),"initial commit failed");
        Check(state.GetCursor()==4 && state.GetFrameIndex()==1 && state.GetReadHistoryIndex()==1,"commit did not advance state");
        Check(!state.CommitSubmitted(initial) && state.GetFrameIndex()==1,"duplicate commit advanced state");
        const auto second=Submit(state,sources,parameters);
        Check(second.start==4 && second.count==4 && second.resetMode==Probe::KeepHistory && second.frameIndex==2 &&
            second.readHistoryIndex==1 && second.writeHistoryIndex==0,"second batch wrong");
        Check(second.epoch==initial.epoch && state.GetRevision()==1,"stable frame advanced source/history epoch");
        const auto tail=Submit(state,sources,parameters);
        Check(tail.start==8 && tail.count==2 && state.GetCursor()==0,"tail did not cut at allocation boundary");
        const auto wrap=Submit(state,sources,parameters);
        Check(wrap.start==0 && wrap.count==4 && wrap.frameIndex==4,"cursor did not wrap");
        parameters.probesPerFrame=20;
        const auto budget=Submit(state,sources,parameters);
        Check(budget.resetMode==Probe::KeepHistory && budget.start==4 && budget.count==6,"budget reset history or exceeded end");
        Check(budget.epoch==initial.epoch,"dispatch budget changed source/history epoch");
        sources.probeCount=0;
        const auto empty=Submit(state,sources,parameters);
        Check(empty.resetMode==Probe::ResetClassification && empty.count==0 && empty.start==0 && state.GetCursor()==0,
            "empty allocation wrong");
        Check(state.AcceptReadback({},empty.frameIndex) && state.GetStats().minimumAge==0 && state.GetStats().averageUpdates==0,
            "empty completed readback wrong");
    }
    void TestRevisionCategoriesAndFailure()
    {
        Probe state;
        Probe::Sources sources={1,2,3,4,17};
        Probe::Parameters parameters;
        parameters.probesPerFrame=3;
        Submit(state,sources,parameters);
        // Camera position/visibility are absent from Sources, so the unchanged
        // revision inputs retain both classification and radiance history.
        Check(Submit(state,sources,parameters).resetMode==Probe::KeepHistory,"stable scene reset history");
        ++sources.surfaceRevision; // Sun/material/explicit surface LOD/rayMax.
        const auto surface=Submit(state,sources,parameters);
        Check(surface.resetMode==Probe::ResetSH && surface.start==0,"surface change did not preserve classification");
        Check(surface.epoch==2 && state.GetRevision()==2,"surface reset did not advance epoch");
        parameters.raysPerUpdate=128;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetSH,"sampling rays reset classification");
        parameters.maxHistoryUpdates=7;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetSH,"history window reset classification");
        parameters.classifyRays=128;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"classification rays did not reclassify");
        parameters.backfaceThreshold=0.5f;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"threshold did not reclassify");
        parameters.rayMinDistance=0.001f;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"origin safety interval did not reclassify");
        parameters.rayMaxDistance=100;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"classification distance did not reclassify");
        ++sources.worldGeometryRevision;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"world geometry did not reclassify");
        ++sources.worldAllocationRevision;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"allocation did not reclassify");
        ++sources.sdfGeometryRevision;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"SDF geometry did not reclassify");
        ++sources.probeCount;
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetClassification,"probe count did not reclassify");
        const auto revision=state.GetRevision();
        const auto frame=state.GetFrameIndex(),cursor=state.GetCursor(),history=state.GetReadHistoryIndex();
        ++sources.surfaceRevision;
        Check(state.Prepare(sources,parameters),"pending surface prepare failed");
        const auto stale=state.GetPendingPlan();
        parameters.probesPerFrame=5;
        Check(state.Prepare(sources,parameters),"replacement prepare failed");
        Check(!state.CommitSubmitted(stale),"stale token committed");
        Check(state.GetRevision()==revision && state.GetFrameIndex()==frame && state.GetCursor()==cursor &&
            state.GetReadHistoryIndex()==history,"stale token advanced state");
        auto bad=parameters; bad.classifyRays=9;
        Check(!state.Prepare(sources,bad) && !state.HasPendingPlan(),"odd ray budget accepted");
        Check(!state.CommitSubmitted(state.GetPendingPlan()),"invalid prepare left committable plan");
        Check(state.GetRevision()==revision,"invalid prepare consumed invalidation");
        Check(Submit(state,sources,parameters).resetMode==Probe::ResetSH,"failed prepare consumed required reset");
        const std::array<uint32_t,4> badRays={0,6,257,300};
        for(const auto count:badRays)
        {
            bad=parameters; bad.raysPerUpdate=count;
            Check(!state.Prepare(sources,bad),"invalid sample ray count accepted");
        }
        bad=parameters; bad.maxHistoryUpdates=0;
        Check(!state.Prepare(sources,bad),"zero history window accepted");
        bad=parameters; bad.probesPerFrame=0;
        Check(!state.Prepare(sources,bad),"zero dispatch budget accepted");
        bad=parameters; bad.backfaceThreshold=std::numeric_limits<float>::quiet_NaN();
        Check(!state.Prepare(sources,bad),"NaN threshold accepted");
        bad=parameters; bad.rayMinDistance=-1;
        Check(!state.Prepare(sources,bad),"negative origin interval accepted");
        bad=parameters; bad.rayMaxDistance=bad.rayMinDistance;
        Check(!state.Prepare(sources,bad),"empty trace interval accepted");
        auto enormous=sources; enormous.probeCount=uint64_t(UINT32_MAX)+1;
        Check(!state.Prepare(enormous,parameters),"oversized shader allocation accepted");
        parameters.classifyRays=8; parameters.raysPerUpdate=256;
        Check(state.Prepare(sources,parameters),"inclusive ray budget limits rejected");
        parameters.maxHistoryUpdates=INT_MAX;
        parameters.probesPerFrame=INT_MAX;
        Check(state.Prepare(sources,parameters) && state.GetPendingPlan().count==sources.probeCount,
            "INT_MAX history/batch with 256 rays overflowed scheduling");
        Check(uint64_t(parameters.maxHistoryUpdates)*parameters.raysPerUpdate==549755813632ULL,
            "history/ray verification product must use wide arithmetic");
    }
    void TestResetRecords()
    {
        Probe::GPURecord valid;
        valid.state=Probe::Valid; valid.updateCount=10; valid.sampleCount=640; valid.lastUpdateFrame=12;
        valid.backfaceRatio=0.125f; valid.minHitDistance=0.75f; valid.classificationRays=64;
        valid.sh[0]={1,2,3,0}; valid.sh[1]={-4,-5,-6,0};
        const auto kept=Probe::ResetRecord(valid,Probe::KeepHistory);
        Check(kept.sh[1].x==-4 && kept.updateCount==10,"keep-history modified record");
        const auto reset=Probe::ResetRecord(valid,Probe::ResetSH);
        Check(reset.state==Probe::Valid && reset.reason==Probe::None && reset.classificationRays==64 &&
            reset.backfaceRatio==valid.backfaceRatio && reset.minHitDistance==valid.minHitDistance,"SH reset lost valid classification");
        Check(reset.sh[0].x==0 && reset.sh[1].x==0 && reset.updateCount==0 && reset.sampleCount==0 && reset.lastUpdateFrame==0,
            "SH reset retained radiance history");
        for(const auto reason:{Probe::Backface,Probe::UnsafeOrigin})
        {
            auto disabled=valid; disabled.state=Probe::Disabled; disabled.reason=reason;
            const auto reliable=Probe::ResetRecord(disabled,Probe::ResetSH);
            Check(reliable.state==Probe::Disabled && reliable.reason==reason && reliable.classificationRays==64,
                "reliable disabled classification was retried");
        }
        for(const auto reason:{Probe::TraceFailure,Probe::SampleFailure})
        {
            auto disabled=valid; disabled.state=Probe::Disabled; disabled.reason=reason;
            const auto retry=Probe::ResetRecord(disabled,Probe::ResetSH);
            Check(retry.state==Probe::Unclassified && retry.reason==Probe::None && retry.classificationRays==0 &&
                retry.updateCount==0,"recoverable failure did not reclassify");
        }
        const auto all=Probe::ResetRecord(valid,Probe::ResetClassification);
        Check(all.state==Probe::Unclassified && all.classificationRays==0 && all.sh[0].x==0 && all.minHitDistance==0,
            "full reset retained classification");
    }
    void TestCompletedReadback()
    {
        Probe state;
        Probe::Sources sources={5,7,9,11,6};
        Probe::Parameters parameters;
        std::vector<Probe::GPURecord> records(6);
        Check(!state.AcceptReadback(records,0),"readback accepted before initialization");
        for(unsigned frame=0;frame<5;++frame) Submit(state,sources,parameters);
        records[0].state=Probe::Valid; records[0].updateCount=3; records[0].sampleCount=192; records[0].lastUpdateFrame=5;
        records[0].classificationRays=64; records[0].sh[1].x=-2;
        for(uint32_t index=1;index<=4;++index)
        {
            records[index].state=Probe::Disabled; records[index].reason=index; records[index].classificationRays=64;
            records[index].minHitDistance=(std::numeric_limits<float>::max)();
        }
        records[4].updateCount=2; records[4].sampleCount=128; records[4].lastUpdateFrame=3;
        Check(state.AcceptReadback(records,5),"valid completed readback rejected");
        const auto stats=state.GetStats();
        Check(stats.valid==1 && stats.disabled==4 && stats.unclassified==1 && stats.reasons[0]==2 &&
            stats.reasons[1]==1 && stats.reasons[2]==1 && stats.reasons[3]==1 && stats.reasons[4]==1,
            "state/reason counts wrong");
        Check(stats.totalUpdates==5 && stats.totalSamples==320 && stats.minimumUpdates==0 && stats.maximumUpdates==3 &&
            stats.probesWithUpdates==2 && stats.minimumAge==0 && stats.maximumAge==2 && stats.averageAge==1,
            "sample/update/age statistics wrong");
        Check(stats.averageUpdates==5.0/6 && stats.gpuHistoryBytes==6*96*2 && stats.memoryBytes==stats.cpuMemoryBytes+stats.gpuHistoryBytes,
            "mean or buffer memory wrong");
        Check(stats.worldGeometryRevision==5 && stats.worldAllocationRevision==7 && stats.sdfGeometryRevision==9 &&
            stats.surfaceRevision==11 && stats.epoch==1 && stats.hasCompletedReadback,"committed source revisions missing");
        const auto accepted=stats.acceptedReadbacks;
        auto malformed=records; malformed[0].sh[0].w=1;
        Check(!state.AcceptReadback(malformed,5),"nonzero SH padding accepted");
        malformed=records; malformed[0].sh[0].x=std::numeric_limits<float>::infinity();
        Check(!state.AcceptReadback(malformed,5),"infinite SH coefficient accepted");
        malformed=records; malformed[0].state=3;
        Check(!state.AcceptReadback(malformed,5),"unknown probe state accepted");
        malformed=records; malformed[0].reason=5;
        Check(!state.AcceptReadback(malformed,5),"unknown disable reason accepted");
        malformed=records; malformed[0].classificationRays=7;
        Check(!state.AcceptReadback(malformed,5),"malformed classify rays accepted");
        malformed=records; malformed[0].backfaceRatio=1.1f;
        Check(!state.AcceptReadback(malformed,5),"ratio outside range accepted");
        malformed=records; malformed[0].lastUpdateFrame=6;
        Check(!state.AcceptReadback(malformed,5),"future record update accepted");
        Check(!state.AcceptReadback(records,6),"unsubmitted frame readback accepted");
        Check(!state.AcceptReadback(records,5,0),"wrong source/history epoch readback accepted");
        Check(!state.AcceptReadback(records,4),"out-of-order readback accepted");
        Check(!state.AcceptReadback(std::span<const Probe::GPURecord>(records.data(),5),5),"incorrect allocation readback accepted");
        Check(state.GetStats().acceptedReadbacks==accepted && state.GetStats().valid==stats.valid,
            "invalid readback partially committed statistics");
        ++sources.surfaceRevision;
        const auto reset=Submit(state,sources,parameters);
        Check(!state.GetStats().hasCompletedReadback && state.GetStats().shResets==1,"reset retained stale available stats");
        Check(!state.AcceptReadback(records,5),"pre-reset completed readback polluted current epoch");
        Check(!state.AcceptReadback(records,reset.frameIndex,reset.epoch-1),"old epoch accepted with a current frame stamp");
        for(auto& record:records) record=Probe::ResetRecord(record,Probe::ResetSH);
        Check(state.AcceptReadback(records,reset.frameIndex,reset.epoch),"post-reset readback rejected");
        Check(state.GetStats().valid==1 && state.GetStats().disabled==2 && state.GetStats().unclassified==3 &&
            state.GetStats().totalUpdates==0 && state.GetStats().maximumAge==0,"post-reset GPU classification stats wrong");
    }
    struct SparseBatch
    {
        Probe::PendingPlan plan;
        std::vector<Probe::WorkItem> work;
    };
    SparseBatch SubmitSparse(Probe& state,const Probe::Sources& sources,const Probe::Parameters& parameters)
    {
        Check(state.Prepare(sources,parameters),"sparse prepare failed");
        SparseBatch batch={state.GetPendingPlan(),{state.GetPendingWork().begin(),state.GetPendingWork().end()}};
        Check(batch.plan.count==batch.work.size(),"sparse dispatch count does not match work list");
        Check(state.CommitSubmitted(batch.plan),"sparse commit failed");
        return batch;
    }
    void CompleteSparse(Probe& state,const SparseBatch& batch,std::vector<uint32_t>& counts,
        uint32_t disabledIndex=UINT32_MAX)
    {
        std::vector<Probe::GPURecord> records;
        for(const auto& work:batch.work)
        {
            Probe::GPURecord record;
            record.classificationRays=64;
            if(work.probeIndex==disabledIndex)
            { record.state=Probe::Disabled; record.reason=Probe::Backface; }
            else
            {
                if(work.resetMode!=Probe::KeepHistory) counts[work.probeIndex]=0;
                record.state=Probe::Valid; record.updateCount=++counts[work.probeIndex];
                record.sampleCount=record.updateCount*64; record.lastUpdateFrame=batch.plan.frameIndex;
            }
            records.push_back(record);
        }
        Check(state.AcceptBatchResults(batch.work,records,batch.plan.frameIndex,batch.plan.epoch),"sparse feedback rejected");
    }
    void TestSparseConvergenceAndIdle()
    {
        Probe state; Probe::Sources sources={1,1,1,1,8}; Probe::Parameters parameters;
        parameters.probesPerFrame=3; parameters.maxHistoryUpdates=3;
        const std::vector<uint32_t> remap(8,UINT32_MAX);
        Check(state.ConfigureCache(1,remap,{}),"initial sparse configure failed");
        Check(state.IsCacheEnabled(),"sparse cache not enabled");
        Check(state.Prepare(sources,parameters),"sparse initial prepare failed");
        const auto initial=state.GetPendingPlan();
        Check(initial.layoutChanged && initial.count==3 && state.GetPendingRemap().size()==8 &&
            state.GetPendingInvalidations().size()==8,"initial topology did not reset every new record immediately");
        Check(state.Prepare(sources,parameters) && state.GetPendingPlan().token==initial.token,"sparse prepare retry changed token");
        auto modified=initial; ++modified.count;
        Check(!state.CommitSubmitted(modified) && state.GetFrameIndex()==0,"sparse modified plan committed");
        std::vector<uint32_t> counts(8),visits(8);
        for(uint32_t iteration=0;iteration<50;++iteration)
        {
            const auto batch=SubmitSparse(state,sources,parameters);
            if(batch.work.empty()) break;
            for(const auto& work:batch.work) ++visits[work.probeIndex];
            CompleteSparse(state,batch,counts,0);
        }
        Check(visits[0]==1 && counts[0]==0,"disabled probe was rescheduled");
        for(size_t index=1;index<counts.size();++index)
            Check(visits[index]==3 && counts[index]==3,"successful-update target did not stop sampling");
        const auto convergedStats=state.GetStats();
        Check(convergedStats.valid==7 && convergedStats.disabled==1 && convergedStats.unclassified==0 &&
            convergedStats.reasons[Probe::Backface]==1 && convergedStats.totalUpdates==21 && convergedStats.totalSamples==1344 &&
            convergedStats.minimumUpdates==0 && convergedStats.maximumUpdates==3 && convergedStats.probesWithUpdates==7 &&
            convergedStats.averageUpdates==21.0/8 && convergedStats.minimumAge==0 && convergedStats.hasCompletedReadback,
            "compact completion statistics did not converge with the cache");
        const auto frame=state.GetFrameIndex(),history=state.GetReadHistoryIndex();
        const auto epoch=state.GetRevision(),submissions=state.GetStats().submissions,token=state.GetPendingPlan().token;
        for(uint32_t iteration=0;iteration<1000;++iteration)
        {
            // Controller has resolved these broad source changes to no affected
            // probes; sparse mode must not fall back to whole-scene invalidation.
            ++sources.worldGeometryRevision; ++sources.surfaceRevision;
            Check(state.ConfigureCache(1,{},{}),"idle cache configuration failed");
            const auto idle=SubmitSparse(state,sources,parameters);
            Check(!idle.plan.HasWork() && idle.plan.readHistoryIndex==idle.plan.writeHistoryIndex && idle.plan.token==token,
                "idle generated a dispatch, pingpong or new token");
        }
        Check(state.GetFrameIndex()==frame && state.GetReadHistoryIndex()==history && state.GetRevision()==epoch &&
            state.GetStats().submissions==submissions,"idle advanced GPU history or scheduling state");
        Check(state.GetStats().valid==7 && state.GetStats().disabled==1 && state.GetStats().totalUpdates==21 &&
            state.GetStats().totalSamples==1344 && state.GetStats().acceptedReadbacks==convergedStats.acceptedReadbacks,
            "idle frames changed completed compact statistics");
    }
    void TestSparseInvalidationAndRemap()
    {
        Probe state; Probe::Sources sources={1,1,1,1,4}; Probe::Parameters parameters;
        parameters.probesPerFrame=4; parameters.maxHistoryUpdates=1;
        Check(state.ConfigureCache(1,std::vector<uint32_t>(4,UINT32_MAX),{}),"sparse setup failed");
        std::vector<uint32_t> counts(4);
        CompleteSparse(state,SubmitSparse(state,sources,parameters),counts);
        const auto clean=SubmitSparse(state,sources,parameters);
        Check(clean.work.empty(),"target-complete cache not idle");
        const auto oldGeneration=state.GetGeneration(2);
        const std::array<Probe::WorkItem,3> dirty={{{2,Probe::ResetSH,0,0},{2,Probe::ResetClassification,0,0},{2,Probe::ResetSH,0,0}}};
        Check(state.ConfigureCache(1,{},dirty),"local invalidation failed");
        Check(state.GetStats().valid==3 && state.GetStats().unclassified==1 && state.GetStats().totalUpdates==3 &&
            state.GetStats().totalSamples==192,"local invalidation discarded unrelated completed statistics");
        const auto local=SubmitSparse(state,sources,parameters);
        Check(local.work.size()==1 && local.work[0].probeIndex==2 && local.work[0].resetMode==Probe::ResetClassification &&
            local.work[0].generation!=oldGeneration && !local.plan.layoutChanged,"local reset was duplicated or broadened");
        Check(state.GetStats().valid==3 && state.GetStats().unclassified==1 && state.GetStats().totalUpdates==3,
            "partial dirty submission cleared whole-cache statistics");
        CompleteSparse(state,local,counts);
        Check(state.GetStats().valid==4 && state.GetStats().unclassified==0 && state.GetStats().totalUpdates==4,
            "partial feedback failed to restore aggregate statistics");
        // Move old probe 2 to index 0, retain old 0 at index 1, insert a new
        // probe at index 2. Only the new record needs work; old history survives.
        const std::array<uint32_t,3> moved={2,0,UINT32_MAX};
        Check(state.ConfigureCache(2,moved,{}),"topology remap failed");
        Check(state.GetStats().valid==2 && state.GetStats().unclassified==1 && state.GetStats().totalUpdates==2 &&
            state.GetStats().totalSamples==128,"topology remap did not preserve retained probe statistics");
        sources.probeCount=3; ++sources.worldAllocationRevision;
        const auto topology=SubmitSparse(state,sources,parameters);
        Check(topology.plan.layoutChanged && topology.work.size()==1 && topology.work[0].probeIndex==2 &&
            topology.work[0].resetMode==Probe::ResetClassification,"retained clean probes were recaptured after remap");
        counts.resize(3); CompleteSparse(state,topology,counts);
        const auto generation=state.GetGeneration(0);
        const std::array<uint32_t,1> badRemap={99};
        Check(!state.ConfigureCache(3,badRemap,{}) && state.GetGeneration(0)==generation,"invalid remap partially committed");
        const std::array<Probe::WorkItem,1> badDirty={{{3,Probe::ResetSH,0,0}}};
        Check(!state.ConfigureCache(2,{},badDirty) && state.GetGeneration(0)==generation,"invalid dirty index mutated cache");
        // Sampling parameters globally reset SH while preserving classification.
        parameters.raysPerUpdate=128;
        const auto sampleChange=SubmitSparse(state,sources,parameters);
        Check(sampleChange.work.size()==3 && sampleChange.plan.resetMode==Probe::ResetSH,"sampling change did not invalidate cache");
        for(const auto& work:sampleChange.work) Check(work.resetMode==Probe::ResetSH,"sampling change unnecessarily reclassified");
    }
    void TestSparseInFlightAndStaleFeedback()
    {
        Probe state; Probe::Sources sources={1,1,1,1,2}; Probe::Parameters parameters;
        parameters.probesPerFrame=1; parameters.maxHistoryUpdates=1;
        Check(state.ConfigureCache(1,std::vector<uint32_t>(2,UINT32_MAX),{}),"inflight setup failed");
        const auto first=SubmitSparse(state,sources,parameters),second=SubmitSparse(state,sources,parameters);
        Check(first.work.size()==1 && second.work.size()==1 && first.work[0].probeIndex!=second.work[0].probeIndex,
            "inflight probe was scheduled again");
        Check(SubmitSparse(state,sources,parameters).work.empty(),"uncompleted probes were cyclically resubmitted");
        const std::array<Probe::WorkItem,1> dirty={{{first.work[0].probeIndex,Probe::ResetClassification,0,0}}};
        Check(state.ConfigureCache(1,{},dirty),"inflight invalidation failed");
        const auto replacement=SubmitSparse(state,sources,parameters);
        Check(replacement.work.size()==1 && replacement.work[0].generation!=first.work[0].generation,"inflight reset did not version work");
        Check(!state.IsCurrentWork(first.work[0],first.plan.frameIndex) &&
            state.IsCurrentWork(replacement.work[0],replacement.plan.frameIndex),"stale work considered current");
        std::vector<uint32_t> counts(2);
        CompleteSparse(state,first,counts); // ignored: a new generation is active
        Check(state.IsCurrentWork(replacement.work[0],replacement.plan.frameIndex),"stale completion released replacement flight");
        CompleteSparse(state,second,counts); // unaffected older epoch is accepted
        Check(!state.IsCurrentWork(second.work[0],second.plan.frameIndex),"unaffected old batch was discarded");
        const Probe::GPURecord malformed=[] { Probe::GPURecord result; result.sh[0].w=1; return result; }();
        Check(!state.AcceptBatchResults(replacement.work,std::span<const Probe::GPURecord>(&malformed,1),
            replacement.plan.frameIndex,replacement.plan.epoch),"malformed feedback accepted");
        Check(state.IsCurrentWork(replacement.work[0],replacement.plan.frameIndex),"malformed feedback partially consumed flight");
        CompleteSparse(state,replacement,counts);
        Check(SubmitSparse(state,sources,parameters).work.empty(),"stale feedback requeued completed probes");
    }
    void TestSHContract()
    {
        constexpr float fourPi=12.56637061435917295f;
        const std::array<DirectX::XMFLOAT3,6> directions={{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
        const auto x=Probe::SHBasis({1,0,0}),y=Probe::SHBasis({0,1,0}),z=Probe::SHBasis({0,0,1});
        Near(x[0],0.28209479f,"Y00 differs from shader");
        Near(x[1],0.48860251f,"X coefficient ordering wrong");
        Near(y[2],0.48860251f,"Y coefficient ordering wrong");
        Near(z[3],0.48860251f,"Z coefficient ordering wrong");
        Check(x[2]==0 && x[3]==0 && y[1]==0 && z[1]==0,"SH world axes mixed");
        Probe::GPURecord constant;
        for(const auto direction:directions) Probe::AddRadianceSample(constant,direction,{2,3,4},fourPi/6);
        for(const auto direction:directions)
        {
            const auto radiance=Probe::EvaluateRadianceSH(constant,direction);
            const auto irradiance=Probe::EvaluateIrradianceOverPi(constant,direction);
            Near(radiance.x,2,"constant radiance reconstruction wrong");
            Near(irradiance.x,2,"constant E/pi must equal radiance");
            Near(irradiance.y,3,"constant green E/pi wrong"); Near(irradiance.z,4,"constant blue E/pi wrong");
        }
        for(const auto coefficient:constant.sh) Check(coefficient.w==0,"SH projection wrote padding");
        // Six antipodal axes integrate constant + first-order radiance exactly.
        // Separate RGB slopes expose sign/order mistakes and l1 convolution.
        Probe::GPURecord linear;
        for(const auto direction:directions)
            Probe::AddRadianceSample(linear,direction,{2+direction.x,3-direction.y,4+0.5f*direction.z},fourPi/6);
        Check(linear.sh[1].x>0 && linear.sh[2].y<0 && linear.sh[3].z>0,"signed SH direction coefficients lost");
        for(const auto direction:directions)
        {
            const auto radiance=Probe::EvaluateRadianceSH(linear,direction);
            const auto irradiance=Probe::EvaluateIrradianceOverPi(linear,direction);
            Near(radiance.x,2+direction.x,"first-order X radiance reconstruction wrong");
            Near(radiance.y,3-direction.y,"first-order Y radiance reconstruction wrong");
            Near(radiance.z,4+0.5f*direction.z,"first-order Z radiance reconstruction wrong");
            Near(irradiance.x,2+(2.0f/3)*direction.x,"l1 E/pi convolution wrong");
            Near(irradiance.y,3-(2.0f/3)*direction.y,"signed Y E/pi convolution wrong");
            Near(irradiance.z,4+(1.0f/3)*direction.z,"Z E/pi convolution wrong");
        }
        Probe::GPURecord negative; negative.sh[0].x=-1;
        Check(Probe::EvaluateRadianceSH(negative,{1,0,0}).x<0 &&
            Probe::EvaluateIrradianceOverPi(negative,{1,0,0}).x==0,"clamp should apply to final E/pi only");
    }
}

int main()
{
    try
    {
        TestSubmissionAndWrap(); TestRevisionCategoriesAndFailure(); TestResetRecords(); TestCompletedReadback(); TestSHContract();
        TestSparseConvergenceAndIdle(); TestSparseInvalidationAndRemap(); TestSparseInFlightAndStaleFeedback();
        std::cout<<"VXGIProbeTests: 8 groups passed (legacy/reference, SH, sparse convergence/idle, local invalidation/remap, stale feedback).\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr<<"VXGIProbeTests failed: "<<exception.what()<<'\n';
        return 1;
    }
}
