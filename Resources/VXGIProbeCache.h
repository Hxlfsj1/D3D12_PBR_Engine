#pragma once

#include "VXGIProbeInvalidation.h"
#include "VXGIProbeState.h"
#include "VXGIMeshSDFScene.h"
#include "VXGISurface.h"

// Connect scene events to the finite probe scheduler. Camera state is absent:
// fixed world-cell histories survive allocation-table rebuilds and idle frames.
class VXGIProbeCache
{
public:
    bool Prepare(const VXGIWorld& world,const VXGIMeshSDFScene& scene,const VXGISurface& surface,
        VXGIProbeState& state,const VXGIProbeState::Parameters& parameters)
    {
        if(!m_initialized || m_sceneRevision!=scene.GetRevision() || m_coverageRevision!=surface.GetCoverageRevision())
        {
            m_instances=VXGIProbeInvalidation::CaptureInstances(scene.GetInstances());
            // Alpha coverage changes classification and visibility, whereas RGB
            // and emission changes only invalidate source SH.
            for(auto& instance:m_instances)
            {
                if(!instance.cutout) continue;
                uint64_t coverage=HashOffset;
                Hash(coverage,surface.GetParameters().materialMipLevel);
                for(const auto id:instance.materialIds)
                {
                    Hash(coverage,id);
                    if(id>=surface.GetMaterials().size()) continue;
                    const auto& material=surface.GetMaterials()[id];
                    Hash(coverage,material.albedoIdx);Hash(coverage,material.baseColorFactor.w);
                }
                instance.coverageFingerprint=coverage;
            }
            m_sceneRevision=scene.GetRevision();m_coverageRevision=surface.GetCoverageRevision();
        }
        if(!m_initialized || m_materialRevision!=surface.GetMaterialRevision())
        {
            m_materials.clear();m_materials.reserve(surface.GetMaterials().size());
            for(const auto& material:surface.GetMaterials())
            {
                uint64_t fingerprint=HashOffset;Hash(fingerprint,material);m_materials.push_back(fingerprint);
            }
            m_materialRevision=surface.GetMaterialRevision();
        }
        uint64_t lighting=HashOffset;
        Hash(lighting,surface.GetLightingRevision());Hash(lighting,surface.GetSamplingRevision());
        const VXGIProbeInvalidation::Input input{world.GetProbeCells(),m_instances,world.GetVoxelSize(),
            world.GetAllocationRevision(),surface.GetMaterialRevision(),lighting,{},m_materials};
        if(!m_invalidation.Update(input)) { m_error=m_invalidation.GetLastError();return false; }
        const auto& change=m_invalidation.GetChange();
        m_dirty.clear();
        for(const auto index:change.dirtyClassification)
            m_dirty.push_back({index,VXGIProbeState::ResetClassification,0,0});
        for(const auto index:change.dirtySH) m_dirty.push_back({index,VXGIProbeState::ResetSH,0,0});
        if(change.topologyChanged) ++m_layoutRevision;
        const auto remap=change.topologyChanged?std::span<const uint32_t>(change.historyRemap):std::span<const uint32_t>{};
        if(!state.ConfigureCache(m_layoutRevision,remap,m_dirty)) { m_error=state.GetLastError();return false; }
        const VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),
            scene.GetRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};
        if(!state.Prepare(sources,parameters)) { m_error=state.GetLastError();return false; }
        m_initialized=true;m_error.clear();return true;
    }
    template<class PacketRange>
    void AcceptDependencies(const PacketRange& completed,const VXGIProbeState& state)
    {
        for(const auto& packet:completed)
            if(state.GetGeneration(packet.probeIndex)==packet.generation)
                m_invalidation.RecordDependencies(packet.probeIndex,packet.mask,packet.domainGeneration);
    }
    // Call only after the actual GPU submission commits successfully. Until its
    // new ray paths return, an intervening geometry edit is conservative.
    void OnSubmitted(std::span<const VXGIProbeState::WorkItem> work)
    {
        m_submitted.clear();m_submitted.reserve(work.size());
        for(const auto& item:work) m_submitted.push_back(item.probeIndex);
        m_invalidation.MarkDependenciesUnknown(m_submitted);
    }
    const VXGIProbeInvalidation::Domain& GetDomain() const { return m_invalidation.GetDomain(); }
    const VXGIProbeInvalidation::Change& GetChange() const { return m_invalidation.GetChange(); }
    const VXGIProbeInvalidation& GetInvalidation() const { return m_invalidation; }
    const std::string& GetLastError() const { return m_error; }
private:
    static constexpr uint64_t HashOffset=14695981039346656037ull;
    template<class T> static void Hash(uint64_t& hash,const T& value)
    {
        const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
        for(size_t i=0;i<sizeof(value);++i) { hash^=bytes[i];hash*=1099511628211ull; }
    }
    VXGIProbeInvalidation m_invalidation;
    std::vector<VXGIProbeInvalidation::InstanceSnapshot> m_instances;
    std::vector<uint64_t> m_materials;
    std::vector<VXGIProbeState::WorkItem> m_dirty;
    std::vector<uint32_t> m_submitted;
    uint64_t m_sceneRevision=0,m_coverageRevision=0,m_materialRevision=0,m_layoutRevision=0;
    bool m_initialized=false;
    std::string m_error;
};
