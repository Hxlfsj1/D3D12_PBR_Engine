#include "../Resources/SSGIHybridState.h"
#include <iostream>
#include <stdexcept>

namespace
{
    using Key=SSGIHybridState::Key;
    void Check(bool condition,const char* message)
    {
        if(!condition) throw std::runtime_error(message);
    }
    Key WorldKey(uint32_t mode=2)
    {
        Key key;
        key.effectiveWorldMode=mode;
        key.screenRadius=0.5f;
        key.quality=2;
        key.worldGeometryRevision=11;
        key.sdfGeometryRevision=12;
        key.surfaceRevision=13;
        key.probeResetEpoch=14;
        key.screenSourceStamp=15;
        return key;
    }
    void TestCommitAndAbortedFrame()
    {
        SSGIHybridState state;
        const auto key=WorldKey();
        Check(state.NeedsReset(key),"initial source did not reset");
        Check(state.NeedsReset(key),"recording attempt consumed initial source");
        state.Commit(key);
        Check(!state.NeedsReset(key),"submitted source did not become stable");
        auto changed=key; ++changed.surfaceRevision;
        Check(state.NeedsReset(changed),"new source did not reset");
        Check(state.NeedsReset(changed),"aborted frame consumed new source");
        Check(!state.NeedsReset(key),"aborted frame replaced committed source");
        state.Commit(changed);
        Check(!state.NeedsReset(changed) && state.NeedsReset(key),"source commit did not publish new identity");
        state.Invalidate();
        Check(state.NeedsReset(changed),"external history invalidation ignored");
        state.Commit(changed);
        Check(!state.NeedsReset(changed),"commit after explicit invalidation remained unstable");
    }
    void TestModesAndConsumedFields()
    {
        for(uint32_t mode=0;mode<=2;++mode)
        {
            SSGIHybridState state;
            const auto key=WorldKey(mode); state.Commit(key);
            for(uint32_t next=0;next<=2;++next)
            {
                auto changed=key; changed.effectiveWorldMode=next;
                Check(state.NeedsReset(changed)==(next!=mode),"effective world mode switch incorrect");
            }
            auto changed=key; changed.screenRadius=0.7f;
            Check(state.NeedsReset(changed),"screen radius change ignored");
            changed=key; changed.quality=4;
            Check(state.NeedsReset(changed),"quality change ignored");
            changed=key; changed.traceDownsampleFactor=4;
            Check(state.NeedsReset(changed),"trace resolution change ignored");
            Check(state.NeedsReset(changed),"unsubmitted trace resolution change consumed reset");
            changed=key; ++changed.worldGeometryRevision;
            Check(state.NeedsReset(changed),"supplied geometry identity ignored");
            changed=key; ++changed.surfaceRevision;
            Check(state.NeedsReset(changed),"supplied screen/source identity ignored");
            changed=key; ++changed.screenSourceStamp;
            Check(state.NeedsReset(changed),"actual uploaded screen source change ignored");
            changed=key; ++changed.sdfGeometryRevision;
            Check(state.NeedsReset(changed)==(mode>=1),"mode's unused/consumed SDF geometry incorrect");
            changed=key; ++changed.probeResetEpoch;
            Check(state.NeedsReset(changed)==(mode>=1),"mode's unused/consumed probe epoch incorrect");
        }
        SSGIHybridState state;
        auto key=WorldKey(0); state.Commit(key);
        key.sdfGeometryRevision=99; key.probeResetEpoch=101;
        Check(!state.NeedsReset(key),"ScreenOnly combined unused world changes reset history");
        state.Commit(key); key.effectiveWorldMode=1;
        Check(state.NeedsReset(key),"entering world mode ignored previously unused fields");
        state.Commit(key); ++key.probeResetEpoch;
        Check(state.NeedsReset(key),"cache fallback ignored probe epoch");
        state.Commit(key);
        key.effectiveWorldMode=2;
        Check(state.NeedsReset(key),"entering canonical cache mode did not invalidate history");
        // A screen-only sun/material edit must reset even without World/SDF
        // updates. Recording the new stamp does not commit an aborted frame.
        SSGIHybridState screenState;
        auto screenKey=WorldKey(0);
        screenKey.worldGeometryRevision=0; screenKey.sdfGeometryRevision=0;
        screenKey.surfaceRevision=0; screenKey.probeResetEpoch=0;
        screenState.Commit(screenKey);
        auto edited=screenKey; ++edited.screenSourceStamp;
        Check(screenState.NeedsReset(edited),"disabled VXGI screen source edit did not invalidate history");
        Check(screenState.NeedsReset(edited) && !screenState.NeedsReset(screenKey),
            "unsubmitted screen source stamp replaced committed history");
        screenState.Commit(edited);
        Check(!screenState.NeedsReset(edited),"submitted screen source stamp did not stabilize");
    }
    void TestOrdinaryProbeUpdatesAndReset()
    {
        SSGIHybridState state;
        auto key=WorldKey(); state.Commit(key);
        // Cursor, capture update count and frame sequence intentionally live
        // outside Key. Advancing normal batches cannot invalidate this source.
        uint32_t cursor=0;
        for(uint32_t frame=1;frame<=128;++frame)
        {
            cursor=(cursor+256)%1024;
            Check(!state.NeedsReset(key),"ordinary probe update reset screen history");
            state.Commit(key);
        }
        Check(cursor==0,"fixture cursor did not exercise wrap");
        ++key.probeResetEpoch;
        Check(state.NeedsReset(key),"probe cache reset did not invalidate screen history");
        Check(state.NeedsReset(key),"unsubmitted probe epoch consumed reset");
        state.Commit(key);
        Check(!state.NeedsReset(key),"submitted reset epoch did not stabilize");
    }
}

int main()
{
    try
    {
        TestCommitAndAbortedFrame();
        TestModesAndConsumedFields();
        TestOrdinaryProbeUpdatesAndReset();
        std::cout<<"SSGIHybridStateTests: 3 groups passed (transactional commit, mode-aware source keys, stable batches/reset epochs).\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr<<"SSGIHybridStateTests failed: "<<exception.what()<<'\n';
        return 1;
    }
}
