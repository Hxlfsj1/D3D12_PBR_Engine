#include <windows.h> // UINT used by the real CPU-only ModelInstance contract.
#include "../Resources/VXGISDF.h"
#include "../Resources/VXGIProbeState.h"
#include "../Resources/VXGISurface.h"
#include "../Resources/SSGIHybridState.h"
#include "../Resources/SceneEditState.h"
#include "../Editor/EditorHistory.h"
#include "../Editor/EditorTransform.h"
#include <cstdio>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Resource { void* pointer=nullptr; void* Get() const { return pointer; } };
    struct Texture { uint32_t type=0; Resource Resource; std::string path="fixture.png"; };
    struct Vertex
    {
        DirectX::XMFLOAT3 Position,Normal={0,0,1};
        DirectX::XMFLOAT2 TexCoords={};
        DirectX::XMFLOAT3 Tangent={1,0,0},Bitangent={0,1,0};
    };
    struct Mesh
    {
        // A small open triangle crosses two allocation cells per horizontal
        // axis. A non-lattice X translation adds one real intersected cell.
        std::vector<Vertex> vertices={{{0.1f,0.1f,0.1f}},{{0.9f,0.1f,0.1f}},{{0.1f,0.9f,0.1f}}};
        std::vector<std::vector<unsigned>> lodIndices={{0,1,2},{},{}};
        std::vector<Texture> textures={{}};
        uint32_t materialID=2;
        bool isUnlit=false;
        DirectX::XMFLOAT4 baseColorFactor={1,1,1,1};
        DirectX::XMFLOAT3 emissiveFactor={};
    };
    struct Model { std::vector<Mesh> meshes={{}}; };
    struct Instance
    {
        uint32_t editorId=17;
        Model* pModel=nullptr;
        DirectX::XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=false;
        int currentLodLevel=0;
    };
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    bool Same(DirectX::XMFLOAT3 a,DirectX::XMFLOAT3 b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
    bool SameCenters(const std::vector<DirectX::XMFLOAT3>& a,const std::vector<DirectX::XMFLOAT3>& b)
    {
        if(a.size()!=b.size()) return false;
        for(size_t index=0;index<a.size();++index) if(!Same(a[index],b[index])) return false;
        return true;
    }
    bool SameCells(const std::vector<VXGIWorld::Cell>& a,const std::vector<VXGIWorld::Cell>& b)
    {
        if(a.size()!=b.size()) return false;
        for(size_t index=0;index<a.size();++index)
            if(!(a[index].coordinate==b[index].coordinate) || a[index].probeIndex!=b[index].probeIndex ||
                a[index].firstTriangle!=b[index].firstTriangle || a[index].triangleCount!=b[index].triangleCount) return false;
        return true;
    }
    struct Fixture
    {
        Model model;
        std::vector<Instance> instances={{}};
        VXGIWorld world;
        VXGISDF sdf;
        VXGISurface surface;
        std::vector<VXGISurface::MaterialRecord> materials=std::vector<VXGISurface::MaterialRecord>(3);
        VXGISurface::Parameters surfaceParameters;
        VXGIProbeState probes;
        VXGIProbeState::Parameters parameters;
        Fixture() { instances[0].pModel=&model; parameters.probesPerFrame=2; UpdateGeometry(); }
        void UpdateGeometry()
        {
            Check(world.Update(instances,0.5f),world.GetLastError().c_str());
            Check(sdf.Update(world,0.25f,128),sdf.GetLastError().c_str());
            Check(surface.Update(materials,world.GetGeometryRevision(),sdf.GetGeometryRevision(),world.GetMaterialRevision(),
                surfaceParameters,world.GetCoverageRevision()),surface.GetLastError().c_str());
        }
        VXGIProbeState::Sources Sources() const
        {
            return {world.GetGeometryRevision(),world.GetAllocationRevision(),sdf.GetGeometryRevision(),surface.GetRevision(),world.GetProbes().size(),surface.GetCoverageRevision()};
        }
        VXGIProbeState::PendingPlan Prepare()
        {
            Check(probes.Prepare(Sources(),parameters),probes.GetLastError().c_str());
            return probes.GetPendingPlan();
        }
        VXGIProbeState::PendingPlan Submit()
        {
            const auto plan=Prepare();
            Check(probes.CommitSubmitted(plan),probes.GetLastError().c_str());
            return plan;
        }
    };
    void FillCompletedBatch(std::vector<VXGIProbeState::GPURecord>& records,const VXGIProbeState::PendingPlan& plan)
    {
        // Synthetic fence-completed records validate CPU lifecycle only. This
        // fixture makes no assertion about GPU sampling/classification quality.
        if(plan.resetMode!=VXGIProbeState::KeepHistory)
            for(auto& record:records) record=VXGIProbeState::ResetRecord(record,plan.resetMode);
        for(uint32_t index=plan.start;index<plan.start+plan.count;++index)
        {
            auto& record=records[index];
            record.state=VXGIProbeState::Valid;
            record.reason=VXGIProbeState::None;
            record.classificationRays=64;
            record.minHitDistance=0.15f;
            ++record.updateCount;
            record.sampleCount+=64;
            record.lastUpdateFrame=plan.frameIndex;
            record.sh[0]={1,2,3,0};
        }
    }
    SSGIHybridState::Key HybridKey(const Fixture& fixture,uint32_t mode,uint64_t epoch,uint64_t screenStamp=11)
    {
        SSGIHybridState::Key key;
        key.effectiveWorldMode=mode;
        key.worldGeometryRevision=mode ? fixture.world.GetGeometryRevision() : 0;
        key.sdfGeometryRevision=fixture.sdf.GetGeometryRevision();
        key.surfaceRevision=mode ? fixture.surface.GetRevision() : 0;
        key.probeResetEpoch=epoch;
        key.screenSourceStamp=screenStamp;
        return key;
    }
    void TestRegisteredSceneEditsToHybridHistory()
    {
        Fixture fixture;
        SceneEditState edits;
        std::array<SSGIHybridState,3> histories;
        Check(edits.Update(fixture.instances) && fixture.instances[0].isDirty,"initial registration missed raster transform rebuild");
        fixture.instances[0].isDirty=false;
        const Instance original=fixture.instances[0];
        const auto initial=fixture.Submit();
        std::vector<VXGIProbeState::GPURecord> oldRecords(fixture.world.GetProbes().size());
        FillCompletedBatch(oldRecords,initial);
        for(uint32_t mode=0;mode<3;++mode) histories[mode].Commit(HybridKey(fixture,mode,initial.epoch));

        // Culling, LOD and a batching reorder cannot change source identity.
        fixture.instances[0].isVisible=false; fixture.instances[0].currentLodLevel=2;
        Check(!edits.Update(fixture.instances),"camera visibility/LOD changed the temporal scene snapshot");
        fixture.UpdateGeometry();
        auto stable=fixture.Submit();
        Check(stable.resetMode==VXGIProbeState::KeepHistory && stable.epoch==initial.epoch,"ordinary batch discarded probe history");
        for(uint32_t mode=0;mode<3;++mode)
            Check(!histories[mode].NeedsReset(HybridKey(fixture,mode,stable.epoch)),"ordinary batch invalidated SSGI history");

        auto added=original; added.editorId=29; added.translation={1.2f,0,0}; added.isDirty=false;
        fixture.instances.push_back(added);
        Check(edits.Update(fixture.instances) && fixture.instances.back().isDirty,"new registered model missed scene/raster update");
        for(auto& history:histories) history.Invalidate(); // Actual app policy for scene edits, in every source mode.
        const auto beforeAdd=fixture.Sources();
        fixture.UpdateGeometry();
        Check(fixture.world.GetTriangles().size()==2 && fixture.Sources().worldGeometryRevision>beforeAdd.worldGeometryRevision &&
            fixture.Sources().worldAllocationRevision>beforeAdd.worldAllocationRevision &&
            fixture.Sources().sdfGeometryRevision>beforeAdd.sdfGeometryRevision &&
            fixture.sdf.Trace({1.45f,0.7f,1},{0,0,-1},10).instanceId==29,"registered instance was absent from real SDF hit identity");
        auto add=fixture.Prepare();
        Check(add.resetMode==VXGIProbeState::ResetClassification && add.start==0 && add.epoch==initial.epoch+1,
            "registration did not reset classification/epoch");
        for(uint32_t mode=0;mode<3;++mode)
            Check(histories[mode].NeedsReset(HybridKey(fixture,mode,add.epoch)),"registration retained old SSGI history");
        const auto addRetry=fixture.Prepare();
        Check(addRetry.token==add.token && fixture.probes.GetRevision()==initial.epoch,
            "unsubmitted registration consumed the probe epoch");
        Check(!edits.Update(fixture.instances),"unchanged retry reported another source edit");
        for(uint32_t mode=0;mode<3;++mode)
            Check(histories[mode].NeedsReset(HybridKey(fixture,mode,add.epoch)),
                "CPU snapshot synchronization consumed an unsubmitted SSGI invalidation");
        Check(fixture.probes.CommitSubmitted(add),"registration commit failed");
        Check(!fixture.probes.AcceptReadback(oldRecords,add.frameIndex,initial.epoch),"pre-registration radiance survived epoch reset");
        for(uint32_t mode=0;mode<3;++mode) histories[mode].Commit(HybridKey(fixture,mode,add.epoch));
        for(auto& instance:fixture.instances) instance.isDirty=false;
        std::swap(fixture.instances[0],fixture.instances[1]);
        Check(!edits.Update(fixture.instances),"stable-ID batching reorder invalidated scene history");
        fixture.UpdateGeometry();
        stable=fixture.Submit();
        Check(stable.resetMode==VXGIProbeState::KeepHistory && stable.epoch==add.epoch,
            "batching reorder rebaked/classified unchanged geometry");
        for(uint32_t mode=0;mode<3;++mode)
            Check(!histories[mode].NeedsReset(HybridKey(fixture,mode,stable.epoch)),"source-index refresh discarded SSGI history");

        fixture.instances.erase(fixture.instances.begin()); // Remove the added identity after reorder.
        Check(edits.Update(fixture.instances),"removed registered instance missed temporal invalidation");
        for(auto& temporal:histories) temporal.Invalidate();
        fixture.UpdateGeometry();
        Check(fixture.world.GetTriangles().size()==1 && !fixture.sdf.Trace({1.45f,0.7f,1},{0,0,-1},10).hit,
            "removed instance retained SDF occlusion");
        const auto removed=fixture.Submit();
        Check(removed.resetMode==VXGIProbeState::ResetClassification && removed.epoch==add.epoch+1,
            "removal retained prior probe classification");
        for(uint32_t mode=0;mode<3;++mode)
        {
            const auto key=HybridKey(fixture,mode,removed.epoch);
            Check(histories[mode].NeedsReset(key),"removed source retained SSGI temporal history");
            histories[mode].Commit(key);
        }
        fixture.instances.clear();
        Check(edits.Update(fixture.instances),"removing the final instance missed temporal invalidation");
        for(auto& temporal:histories) temporal.Invalidate();
        fixture.UpdateGeometry();
        const auto empty=fixture.Submit();
        Check(fixture.world.GetCells().empty() && fixture.sdf.GetNodes().empty() && fixture.sdf.GetTriangles().empty() &&
            empty.resetMode==VXGIProbeState::ResetClassification && empty.count==0 && empty.start==0 &&
            empty.epoch==removed.epoch+1 && !fixture.sdf.Trace({0.15f,0.7f,1},{0,0,-1},10).hit,
            "empty registered scene retained old allocation or occlusion");
        for(uint32_t mode=0;mode<3;++mode)
        {
            const auto key=HybridKey(fixture,mode,empty.epoch);
            Check(histories[mode].NeedsReset(key),"empty source retained SSGI temporal history");
            histories[mode].Commit(key);
        }
        fixture.instances.push_back(original);
        Check(edits.Update(fixture.instances),"restoring the final instance missed registration");
        fixture.UpdateGeometry();
        const auto restored=fixture.Submit();
        Check(restored.resetMode==VXGIProbeState::ResetClassification && restored.epoch==empty.epoch+1 &&
            fixture.world.GetProbes().size()==oldRecords.size() &&
            !fixture.probes.AcceptReadback(oldRecords,restored.frameIndex,initial.epoch),
            "same-sized restored scene reused stale pre-edit radiance");
        std::cout<<"Registered edit evidence: add/remove/empty/restore update real World/SDF; epochs 1 -> 2 -> 3 -> 4 -> 5; "
            "all 3 SSGI modes invalidate, stable-ID reorder/culling/LOD and ordinary batches retain history.\n";
    }
    void TestMaterialLightToProbeAndHybridHistory()
    {
        Fixture fixture;
        SceneEditState edits; edits.Update(fixture.instances); fixture.instances[0].isDirty=false;
        auto submitted=fixture.Submit();
        SSGIHybridState history; history.Commit(HybridKey(fixture,2,submitted.epoch));
        const auto* nodes=fixture.sdf.GetNodes().data();
        const auto* offsets=fixture.sdf.GetCellOffsets().data();
        const auto* references=fixture.sdf.GetTriangleReferences().data();
        const auto geometry=fixture.world.GetGeometryRevision(),sdfGeometry=fixture.sdf.GetGeometryRevision();
        const auto allocation=fixture.world.GetAllocationRevision();
        const auto verifyReuse=[&]() {
            Check(fixture.world.GetGeometryRevision()==geometry && fixture.world.GetAllocationRevision()==allocation &&
                fixture.sdf.GetGeometryRevision()==sdfGeometry && fixture.sdf.GetNodes().data()==nodes &&
                fixture.sdf.GetCellOffsets().data()==offsets && fixture.sdf.GetTriangleReferences().data()==references,
                "material/light edit rebuilt independent geometry or sparse allocation");
        };
        const auto verifyReset=[&](const char* message,uint32_t mode=VXGIProbeState::ResetSH) {
            fixture.UpdateGeometry(); verifyReuse();
            const auto plan=fixture.Prepare();
            Check(plan.resetMode==mode && plan.start==0 && plan.epoch==submitted.epoch+1,message);
            const auto key=HybridKey(fixture,2,plan.epoch);
            Check(history.NeedsReset(key),"source edit failed to reset hybrid history using current pending epoch");
            Check(fixture.probes.GetRevision()==submitted.epoch && history.NeedsReset(key),"unsubmitted source edit consumed history state");
            VXGIProbeState::GPURecord reliable;
            reliable.state=VXGIProbeState::Valid; reliable.reason=VXGIProbeState::None;
            reliable.classificationRays=64; reliable.minHitDistance=0.15f;
            reliable.sh[0]={1,2,3,0}; reliable.updateCount=3; reliable.sampleCount=192;
            const auto reset=VXGIProbeState::ResetRecord(reliable,plan.resetMode);
            Check((mode==VXGIProbeState::ResetSH ? (reset.state==reliable.state && reset.reason==reliable.reason &&
                reset.classificationRays==64 && reset.minHitDistance==0.15f) :
                (reset.state==VXGIProbeState::Unclassified && reset.classificationRays==0)) && reset.updateCount==0 && reset.sampleCount==0 &&
                reset.sh[0].x==0 && reset.sh[0].y==0 && reset.sh[0].z==0,
                "coverage/source reset retained stale classification or radiance");
            const auto retry=fixture.Prepare();
            Check(retry.token==plan.token && retry.epoch==plan.epoch,"unsubmitted coverage/source reset lost retry transaction");
            Check(fixture.probes.CommitSubmitted(plan),"source-edit submission failed");
            history.Commit(key); submitted=plan;
        };
        fixture.instances[0].customMaterialID=1;
        Check(edits.Update(fixture.instances) && !fixture.instances[0].isDirty,"material override missed scene edit or dirtied unchanged TRS");
        verifyReset("resolved material override failed to reset classification",VXGIProbeState::ResetClassification);
        const auto hit=fixture.sdf.Trace({0.15f,0.7f,1},{0,0,-1},10);
        Check(hit.hit && hit.materialId==1,"material override left stale SDF hit metadata");
        fixture.materials[1].baseColorFactor={0.2f,0.4f,0.6f,1};
        verifyReset("uploaded material factor edit failed to reset SH only");
        fixture.materials[1].normalIdx=7;
        verifyReset("uploaded material texture descriptor edit failed to reset SH only");
        fixture.materials[1].albedoIdx=9;
        verifyReset("alpha texture descriptor edit retained classification",VXGIProbeState::ResetClassification);
        fixture.materials[1].baseColorFactor.w=0.4f;
        verifyReset("alpha factor edit retained classification",VXGIProbeState::ResetClassification);
        fixture.materials[1].emissiveFactor={2,3,4};
        verifyReset("emission edit failed to preserve classification");
        fixture.instances[0].isCutout=true;
        Check(edits.Update(fixture.instances),"cutout source flag missed temporal scene invalidation");
        verifyReset("cutout source identity edit retained classification",VXGIProbeState::ResetClassification);
        Check(fixture.sdf.GetTriangles()[0].isCutout && fixture.world.GetStats().cutoutInstances==1,
            "cutout identity edit left stale world/SDF hit metadata");
        fixture.surfaceParameters.lightColor={3,2,1};
        verifyReset("lighting edit failed to reset SH only");
        fixture.surfaceParameters.materialMipLevel=2;
        verifyReset("explicit world material mip edit retained classification",VXGIProbeState::ResetClassification);
        fixture.instances[0].customMaterialID=UINT32_MAX;
        verifyReset("restored resolved material retained classification",VXGIProbeState::ResetClassification);
        const auto coverage=fixture.surface.GetCoverageRevision();
        fixture.instances[0].customMaterialID=2; // Same resolved material identity.
        fixture.UpdateGeometry(); verifyReuse();
        const auto sameResolved=fixture.Submit();submitted=sameResolved;
        Check(fixture.surface.GetCoverageRevision()==coverage && sameResolved.resetMode!=VXGIProbeState::ResetClassification,
            "same resolved material override unnecessarily invalidated alpha coverage");
        history.Commit(HybridKey(fixture,2,sameResolved.epoch));
        fixture.UpdateGeometry(); verifyReuse();
        const auto steady=fixture.Submit();
        Check(steady.resetMode==VXGIProbeState::KeepHistory && steady.epoch==submitted.epoch &&
            !history.NeedsReset(HybridKey(fixture,2,steady.epoch)),"unchanged source repeatedly reset probe/SSGI history");
        std::cout<<"Source edit evidence: resolved identity/alpha descriptor/alpha factor/cutout/mip reset classification; "
            "RGB/normal/emission/light reset SH only; same resolved override preserves coverage; abort retries retain token. "
            "SDF node/offset/reference storage and geometry/allocation revisions remain unchanged. Material records emulate a completed pool upload.\n";
    }
    void TestMoveRestoreAndReadbackEpoch()
    {
        Fixture fixture;
        const auto originalCenters=fixture.world.GetOccupiedCenters();
        const auto originalCells=fixture.world.GetCells();
        const auto originalWorldReferences=fixture.world.GetTriangleReferences();
        const auto originalNodes=fixture.sdf.GetNodes();
        const auto originalOffsets=fixture.sdf.GetCellOffsets();
        const auto originalSDFReferences=fixture.sdf.GetTriangleReferences();
        const auto originalGrid=fixture.sdf.GetGrid();
        const auto originalFingerprint=fixture.world.GetStats().geometryFingerprint;
        const auto originalTranslation=fixture.instances[0].translation;
        const auto originalSource=fixture.Sources();
        Check(originalCenters.size()==4 && originalCells.size()==4,"baseline SAT allocation must contain four cells/probes");
        const auto originalHit=fixture.sdf.Trace({0.15f,0.7f,1},{0,0,-1},10);
        Check(originalHit.hit && originalHit.materialId==2 && originalHit.instanceId==17,"baseline surface ray missed source identity");
        std::vector<VXGIProbeState::GPURecord> originalRecords(originalCenters.size());
        auto initial=fixture.Submit();
        Check(initial.resetMode==VXGIProbeState::ResetClassification && initial.epoch==1,"initial scene did not require classification");
        FillCompletedBatch(originalRecords,initial);
        initial=fixture.Submit(); FillCompletedBatch(originalRecords,initial);
        initial=fixture.Submit(); FillCompletedBatch(originalRecords,initial);
        Check(fixture.probes.AcceptReadback(originalRecords,initial.frameIndex,initial.epoch),"baseline completed readback rejected");
        Check(fixture.probes.GetStats().valid==4 && fixture.probes.GetCursor()==2,"baseline progress did not update allocated probes");

        // Neither render visibility nor camera LOD may hide this registered
        // instance. isDirty is false on purpose: TRS snapshots drive invalidation.
        fixture.instances[0].isVisible=false;
        fixture.instances[0].currentLodLevel=2;
        fixture.instances[0].isDirty=false;
        const auto* stableNodes=fixture.sdf.GetNodes().data();
        fixture.UpdateGeometry();
        Check(fixture.Sources().worldGeometryRevision==originalSource.worldGeometryRevision &&
            fixture.Sources().worldAllocationRevision==originalSource.worldAllocationRevision &&
            fixture.Sources().sdfGeometryRevision==originalSource.sdfGeometryRevision &&
            fixture.sdf.GetNodes().data()==stableNodes,"visibility/LOD/dirty state rebuilt static source geometry");

        fixture.instances[0].translation={0.17f,0,0};
        fixture.UpdateGeometry();
        const auto movedSource=fixture.Sources();
        Check(movedSource.probeCount==5 && movedSource.probeCount!=originalSource.probeCount,
            "non-grid-aligned movement did not change real SAT allocation count");
        Check(movedSource.worldGeometryRevision>originalSource.worldGeometryRevision &&
            movedSource.worldAllocationRevision>originalSource.worldAllocationRevision &&
            movedSource.sdfGeometryRevision>originalSource.sdfGeometryRevision && fixture.sdf.GetStats().geometryRebuilt,
            "movement did not rebuild World allocation and SDF distance data");
        Check(!fixture.sdf.Trace({0.15f,0.7f,1},{0,0,-1},10).hit,"moved SDF still traced the old surface");
        const auto oldFrame=fixture.probes.GetFrameIndex(),oldCursor=fixture.probes.GetCursor();
        const auto oldEpoch=fixture.probes.GetRevision();
        const auto move=fixture.Prepare();
        Check(move.resetMode==VXGIProbeState::ResetClassification && move.start==0 && move.count==2 && move.epoch==oldEpoch+1,
            "moved geometry did not plan a complete classification reset");
        Check(fixture.probes.GetFrameIndex()==oldFrame && fixture.probes.GetCursor()==oldCursor &&
            fixture.probes.GetRevision()==oldEpoch,"geometry prepare advanced state before submission");
        const auto retry=fixture.Prepare();
        Check(retry.token==move.token && retry.frameIndex==move.frameIndex,"failed recording lost the moved-geometry plan");
        Check(fixture.probes.CommitSubmitted(move),"moved scene commit failed");
        Check(!fixture.probes.AcceptReadback(originalRecords,initial.frameIndex,initial.epoch),
            "pre-move readback survived allocation/history invalidation");
        std::vector<VXGIProbeState::GPURecord> movedRecords(static_cast<size_t>(movedSource.probeCount));
        FillCompletedBatch(movedRecords,move);
        Check(fixture.probes.AcceptReadback(movedRecords,move.frameIndex,move.epoch),"moved allocation readback rejected");
        Check(fixture.probes.GetStats().valid==2 && fixture.probes.GetStats().unclassified==3 &&
            fixture.probes.GetStats().probeCount==5,"moved snapshot used the old allocation count");

        // Emulate undo by restoring the saved exact instance transform. Content
        // returns to the old geometry, but revision counters must keep advancing.
        fixture.instances[0].translation=originalTranslation;
        fixture.UpdateGeometry();
        const auto undoSource=fixture.Sources();
        Check(undoSource.probeCount==originalSource.probeCount && undoSource.worldGeometryRevision>movedSource.worldGeometryRevision &&
            undoSource.worldAllocationRevision>movedSource.worldAllocationRevision &&
            undoSource.sdfGeometryRevision>movedSource.sdfGeometryRevision,"undo reused a stale geometry revision");
        Check(SameCenters(fixture.world.GetOccupiedCenters(),originalCenters) && SameCells(fixture.world.GetCells(),originalCells) &&
            fixture.world.GetTriangleReferences()==originalWorldReferences &&
            fixture.world.GetStats().geometryFingerprint==originalFingerprint,"undo did not restore deterministic sparse allocation");
        Check(fixture.sdf.GetNodes()==originalNodes && fixture.sdf.GetCellOffsets()==originalOffsets &&
            fixture.sdf.GetTriangleReferences()==originalSDFReferences && Same(fixture.sdf.GetGrid().origin,originalGrid.origin) &&
            Same(fixture.sdf.GetGrid().boundsMaximum,originalGrid.boundsMaximum) &&
            fixture.sdf.GetGrid().nodeDimensions==originalGrid.nodeDimensions,"undo did not restore exact SDF nodes/CSR/grid");
        const auto undoHit=fixture.sdf.Trace({0.15f,0.7f,1},{0,0,-1},10);
        Check(undoHit.hit && undoHit.distance==originalHit.distance && Same(undoHit.position,originalHit.position),
            "undo did not restore the original exact surface hit");
        const auto undo=fixture.Submit();
        Check(undo.resetMode==VXGIProbeState::ResetClassification && undo.start==0 && undo.epoch==move.epoch+1,
            "undo did not reset classification despite restored probe count");
        // Original and restored buffers have the same size and centers. Epoch
        // alone must reject old radiance, even if a caller supplies a current stamp.
        Check(!fixture.probes.AcceptReadback(originalRecords,undo.frameIndex,initial.epoch),
            "same-sized pre-move history accepted after undo");
        Check(!fixture.probes.AcceptReadback(movedRecords,undo.frameIndex,move.epoch),"moved epoch accepted after undo");
        Check(!fixture.probes.GetStats().hasCompletedReadback,"stale snapshot rejection exposed invalid statistics");
        std::vector<VXGIProbeState::GPURecord> restoredRecords(originalCenters.size());
        FillCompletedBatch(restoredRecords,undo);
        Check(fixture.probes.AcceptReadback(restoredRecords,undo.frameIndex,undo.epoch),"restored epoch readback rejected");
        Check(fixture.probes.GetStats().valid==2 && fixture.probes.GetStats().unclassified==2 &&
            fixture.probes.GetStats().totalUpdates==2 && fixture.probes.GetStats().classificationResets==3,
            "undo retained old radiance/history counts");
        const auto steady=fixture.Submit();
        Check(steady.resetMode==VXGIProbeState::KeepHistory && steady.start==2 && steady.count==2 && steady.epoch==undo.epoch,
            "stable restored scene did not resume its next batch");
        std::cout<<"Scene evidence: probes 4 -> 5 -> 4; World geometry/allocation and SDF revisions 1 -> 2 -> 3; "
            "history epochs 1 -> 2 -> 3; centers/cells/nodes/CSR restored exactly; stale readbacks rejected.\n";
    }
    void TestLeadingDecimalParsing()
    {
        // InputScalar's DataTypeApplyFromText uses sscanf with %f for floats.
        // This checks accepted scalar syntax without creating any UI context.
        float leading=0,explicitZero=0;
        Check(sscanf_s(".25","%f",&leading)==1 && sscanf_s("0.25","%f",&explicitZero)==1 &&
            leading==0.25f && explicitZero==leading,"CRT float parser rejected a leading decimal point");
        std::cout<<"Scalar syntax evidence: .25 and 0.25 both parse to 0.25; no window or input automation used.\n";
    }
    void TestActualEditorHistoryRoundtrip()
    {
        std::vector<ModelInstance> instances(2);
        instances[0].editorId=17; instances[0].pModel=nullptr;
        instances[1].editorId=29; instances[1].pModel=nullptr;
        EditorSelection selection;
        selection.Select(17);
        EditorHistory history;
        auto& object=instances[0];
        const auto original=EditorHistory::Transform::Capture(object);
        constexpr uint32_t fieldId=0x51717;
        history.BeginTransform(instances,object,fieldId);
        // Real editor axes: editor Y maps to engine X. Repeated valid changes
        // belong to one numeric field transaction, without an ImGui context.
        for(float position:{0.05f,0.12f,0.17f})
        {
            history.BeginTransform(instances,object,fieldId);
            Check(EditorTransform::SetLocation(object,{0,position,0}),"actual editor transform rejected finite movement");
        }
        const auto moved=EditorHistory::Transform::Capture(object);
        Check(object.translation.x==0.17f && object.isDirty,"real ModelInstance setter did not update movement/dirty state");
        history.CommitTransform(instances,fieldId);
        Check(history.Size()==1 && history.UndoCount()==1 && history.RedoCount()==0,"field transaction created multiple history entries");
        object.UpdateTransform();
        DirectX::XMFLOAT4X4 matrix;
        DirectX::XMStoreFloat4x4(&matrix,object.cachedWorldMat);
        Check(matrix._41==0.17f && !object.isDirty,"actual transform matrix did not reflect committed translation");
        std::swap(instances[0],instances[1]); // History resolves stable editorId.
        Check(history.Undo(instances,selection),"actual EditorHistory undo failed");
        Check(EditorHistory::Transform::Capture(instances[1])==original && instances[1].isDirty &&
            history.UndoCount()==0 && history.RedoCount()==1,"actual undo did not restore before transform by stable identity");
        Check(history.Redo(instances,selection),"actual EditorHistory redo failed");
        Check(EditorHistory::Transform::Capture(instances[1])==moved && history.UndoCount()==1 && history.RedoCount()==0,
            "actual redo did not restore after transform");
        Check(instances[0].translation.x==0 && selection.SelectedId()==17,"actual history touched an unrelated object or selection");
        SceneEditState edits;
        edits.Update(instances);
        for(auto& instance:instances) instance.UpdateTransform();
        instances[1].translation.x=0.4f; // Scene script did not set isDirty.
        Check(!instances[1].isDirty && edits.Update(instances) && instances[1].isDirty,
            "direct real ModelInstance TRS edit missed scene/raster invalidation");
        instances[1].UpdateTransform();
        DirectX::XMStoreFloat4x4(&matrix,instances[1].cachedWorldMat);
        Check(matrix._41==0.4f && !instances[1].isDirty,"direct TRS edit left the actual raster world matrix stale");
        std::cout<<"Editor history evidence: actual ModelInstance edit -> Undo -> Redo restored transforms by stable ID; "
            "one transaction, no assets/device/UI context.\n";
    }
}

int main()
{
    try { TestMoveRestoreAndReadbackEpoch(); TestRegisteredSceneEditsToHybridHistory(); TestMaterialLightToProbeAndHybridHistory();
        TestActualEditorHistoryRoundtrip(); TestLeadingDecimalParsing();
        std::cout<<"VXGIProbeSceneTests: 5 groups passed.\n"; return 0; }
    catch(const std::exception& exception) { std::cerr<<"VXGIProbeSceneTests failed: "<<exception.what()<<'\n'; return 1; }
}
