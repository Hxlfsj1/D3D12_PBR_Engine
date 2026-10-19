#pragma once

#include "VXGIWorld.h"
#include <cstddef>
#include <type_traits>

// Allocation-only sparse lookup for fixed probes. The world already owns the
// triangle allocation/hash table; this cache only exports compact GPU topology.
// Material, lighting, SH history and SDF bake revisions are deliberately absent.
class VXGIProbeTopology
{
public:
    static constexpr uint32_t Missing=UINT32_MAX;
    struct LookupRecord
    {
        VXGIWorld::Coordinate coordinate;
        uint32_t probeIndex=Missing;
    };
    struct NeighborRecord
    {
        // Fixed order [-X,+X,-Y,+Y,-Z,+Z]. Missing neighbors remain UINT_MAX.
        std::array<uint32_t,6> indices={Missing,Missing,Missing,Missing,Missing,Missing};
    };
    static_assert(std::is_standard_layout_v<LookupRecord> && sizeof(LookupRecord)==16);
    static_assert(offsetof(LookupRecord,coordinate)==0 && offsetof(LookupRecord,probeIndex)==12);
    static_assert(std::is_standard_layout_v<NeighborRecord> && sizeof(NeighborRecord)==24);
    static_assert(offsetof(NeighborRecord,indices)==0);

    struct QueryCandidate
    {
        uint32_t probeIndex=Missing;
        float weight=0; // Geometric trilinear weight before visibility/state tests.
    };
    struct Stats
    {
        uint64_t cells=0,neighborLinks=0,missingNeighborLinks=0;
        uint64_t lookupBytes=0,neighborBytes=0,memoryBytes=0;
        uint64_t worldAllocationRevision=0;
        double buildMilliseconds=0;
    };

    bool Update(const VXGIWorld& world)
    {
        const float voxelSize=world.GetVoxelSize();
        if(!std::isfinite(voxelSize) || voxelSize<=0)
        {
            m_error="VXGI topology requires a successfully built world allocation with positive voxelSize.";
            return false;
        }
        if(m_initialized && m_world==&world && m_stats.worldAllocationRevision==world.GetAllocationRevision() &&
            m_voxelSize==voxelSize && m_lookup.size()==world.GetProbeCells().size())
        {
            m_error.clear();
            return true;
        }
        const auto start=std::chrono::steady_clock::now();
        try
        {
            const auto& cells=world.GetProbeCells();
            if(cells.size()!=world.GetProbes().size() || cells.size()!=world.GetOccupiedCenters().size() || cells.size()>UINT32_MAX)
            {
                m_error="VXGI topology requires one probe and center per compact probe allocation cell.";
                return false;
            }
            std::vector<LookupRecord> lookup;
            std::vector<NeighborRecord> neighbors;
            lookup.reserve(cells.size()); neighbors.resize(cells.size());
            for(size_t index=0;index<cells.size();++index)
            {
                const auto& cell=cells[index];
                if(cell.probeIndex!=index || (index>0 && !(cells[index-1].coordinate<cell.coordinate)))
                {
                    m_error="VXGI topology requires unique lexicographically sorted cells with compact probe IDs.";
                    return false;
                }
                lookup.push_back({cell.coordinate,cell.probeIndex});
            }
            uint64_t links=0;
            for(size_t index=0;index<lookup.size();++index)
                for(uint32_t direction=0;direction<6;++direction)
                {
                    VXGIWorld::Coordinate coordinate;
                    if(!NeighborCoordinate(lookup[index].coordinate,direction,coordinate)) continue;
                    // Reuse World's existing CPU hash lookup. Do not allocate
                    // another hash table or repeat any triangle/SDF work.
                    const auto cellIndex=world.FindProbeCell(coordinate);
                    if(cellIndex<0) continue;
                    neighbors[index].indices[direction]=static_cast<uint32_t>(cellIndex);
                    ++links;
                }
            Stats stats;
            stats.cells=lookup.size(); stats.neighborLinks=links; stats.missingNeighborLinks=lookup.size()*6-links;
            stats.lookupBytes=lookup.capacity()*sizeof(LookupRecord);
            stats.neighborBytes=neighbors.capacity()*sizeof(NeighborRecord);
            stats.memoryBytes=sizeof(*this)+stats.lookupBytes+stats.neighborBytes;
            stats.worldAllocationRevision=world.GetAllocationRevision();
            stats.buildMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            // Publish only after all validation/build work succeeds. Failed
            // updates preserve the previously uploaded revision and buffers.
            m_lookup=std::move(lookup); m_neighbors=std::move(neighbors); m_stats=stats;
            m_world=&world; m_voxelSize=voxelSize; m_initialized=true; ++m_revision;
            m_error.clear();
            return true;
        }
        catch(const std::exception& exception)
        {
            m_error=std::string("VXGI topology update failed: ")+exception.what();
            return false;
        }
    }

    const std::vector<LookupRecord>& GetLookup() const { return m_lookup; }
    const std::vector<NeighborRecord>& GetNeighbors() const { return m_neighbors; }
    const Stats& GetStats() const { return m_stats; }
    uint64_t GetRevision() const { return m_revision; }
    float GetVoxelSize() const { return m_voxelSize; }
    const std::string& GetLastError() const { return m_error; }
    uint32_t FindProbe(VXGIWorld::Coordinate coordinate) const
    {
        const auto found=std::lower_bound(m_lookup.begin(),m_lookup.end(),coordinate,
            [](const LookupRecord& entry,VXGIWorld::Coordinate target) { return entry.coordinate<target; });
        return found!=m_lookup.end() && found->coordinate==coordinate ? found->probeIndex : Missing;
    }

    // Eight center-lattice corners, ordered x + 2*y + 4*z. World cell coordinates
    // are global: center=(coordinate+.5)*voxelSize. Grid.origin is only a bound;
    // subtracting it here would misidentify negative/nonzero-minimum cells.
    // Input float arithmetic mirrors HLSL. Out-of-range/nonfinite inputs fail
    // closed; no radius-1/27-cell fallback broadens the geometric support.
    bool GetQueryCandidates(DirectX::XMFLOAT3 position,std::array<QueryCandidate,8>& output) const
    {
        output={};
        if(!m_initialized) return false;
        const std::array<float,3> centered={position.x/m_voxelSize-0.5f,position.y/m_voxelSize-0.5f,position.z/m_voxelSize-0.5f};
        std::array<int32_t,3> base;
        std::array<float,3> fraction;
        for(size_t axis=0;axis<3;++axis)
        {
            const float lower=std::floor(centered[axis]);
            if(!std::isfinite(lower) || double(lower)<INT32_MIN || double(lower)>double(INT32_MAX)-1) return false;
            base[axis]=static_cast<int32_t>(lower);
            fraction[axis]=centered[axis]-lower;
        }
        for(uint32_t corner=0;corner<8;++corner)
        {
            const int32_t x=static_cast<int32_t>(corner&1),y=static_cast<int32_t>((corner>>1)&1),z=static_cast<int32_t>((corner>>2)&1);
            const VXGIWorld::Coordinate coordinate={base[0]+x,base[1]+y,base[2]+z};
            output[corner]={FindProbe(coordinate),(x ? fraction[0] : 1-fraction[0])*
                (y ? fraction[1] : 1-fraction[1])*(z ? fraction[2] : 1-fraction[2])};
        }
        return true;
    }

    static bool NeighborCoordinate(VXGIWorld::Coordinate source,uint32_t direction,VXGIWorld::Coordinate& result)
    {
        if(direction>=6) return false;
        result=source;
        int32_t* axes[3]={&result.x,&result.y,&result.z};
        const uint32_t axis=direction/2;
        const int64_t value=int64_t(*axes[axis])+(direction%2==0 ? -1 : 1);
        if(value<INT32_MIN || value>INT32_MAX) return false;
        *axes[axis]=static_cast<int32_t>(value);
        return true;
    }

private:
    std::vector<LookupRecord> m_lookup;
    std::vector<NeighborRecord> m_neighbors;
    Stats m_stats;
    const VXGIWorld* m_world=nullptr;
    std::string m_error;
    float m_voxelSize=0;
    uint64_t m_revision=0;
    bool m_initialized=false;
};
