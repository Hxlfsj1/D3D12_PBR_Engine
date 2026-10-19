#pragma once
#include "RDG.h"
#include "PBR_Shader.h"
#include "VXGIGPUScene.h"
#include "VXGIProbeState.h"
#include "VXGIProbeTopology.h"
#include "VXGISurface.h"
#include <array>
#include <span>
#include <cstring>
#include <vector>

// Derived diagnostic SH and alpha-aware cached visibility. Surface queries use
// the raw history with visibility tested from every contributor to the target.
class VXGIProbeFilterPass
{
public:
    struct ActivityStats
    {
        uint64_t idleFrames=0,activeBatches=0,remapDispatches=0,invalidationDispatches=0,resetDispatches=0,visibilityDispatches=0,filterDispatches=0;
    };
    const ActivityStats& GetActivityStats() const { return m_activity; }
    void SetTopology(std::span<const VXGIProbeTopology::NeighborRecord> topology)
    {
        m_previousTopology=std::move(m_topology);
        m_topology.assign(topology.begin(),topology.end());
    }
    bool Initialize(RenderDevice& device,UINT frames,bool meshBackend=false)
    {
        m_meshBackend=meshBackend;
        m_frames.resize(frames);
        D3D12_QUERY_HEAP_DESC queries{};queries.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queries.Count=frames*2;
        if(!Check(device.GetDevice()->CreateQueryHeap(&queries,IID_PPV_ARGS(&m_queries)),"create timestamps") ||
            !Check(device.GetCommandQueue()->GetTimestampFrequency(&m_frequency),"get timestamp frequency")) return false;
        for(auto& frame:m_frames) if(!CreateBuffer(device.GetDevice(),16,D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,frame.timestamps)) return false;
        CD3DX12_ROOT_PARAMETER roots[22];roots[0].InitAsConstants(20,0);
        for(UINT i=0;i<10;++i) roots[i+1].InitAsShaderResourceView(i);
        roots[11].InitAsUnorderedAccessView(0);roots[12].InitAsUnorderedAccessView(1);
        for(UINT i=0;i<3;++i) roots[13+i].InitAsShaderResourceView(i,1);
        for(UINT i=0;i<6;++i) roots[16+i].InitAsShaderResourceView(10+i);
        const auto sampler=CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_WRAP,D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        // 20 constants + nineteen SRVs + two UAVs = 62 DWORDs.
        const CD3DX12_ROOT_SIGNATURE_DESC desc(22,roots,1,&sampler,
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        if(!Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"serialize root") ||
            !Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root)),"create root")) return false;
        const wchar_t* entries[]={L"CSResetFilter",L"CSVisibility",L"CSFilter",L"CSRemapFilter",L"CSResetSparseFilter",L"CSVisibilitySparse",L"CSFilterSparse"};
        for(UINT i=0;i<7;++i)
        {
            const auto shader=ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_VXGI_Probe_Filter.hlsl",entries[i],L"cs_6_6",
                meshBackend?std::vector<std::wstring>{L"VXGI_MESH_SDF=1"}:std::vector<std::wstring>{},true);
            if(!shader) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            if(!Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&m_pipelines[i])),"create pipeline")) return false;
        }
        return true;
    }
    bool Record(RenderDevice& device,ID3D12DescriptorHeap* heap,int index,const VXGIGPUScene& scene,
        const VXGISDF& sdf,ID3D12Resource* raw,const VXGIProbeState& state,float voxelSize,float neighborWeight,
        const VXGISurface& surface,ID3D12Resource* materialBuffer)
    {
        m_recorded=false;
        if(index<0 || size_t(index)>=m_frames.size() || !state.HasPendingPlan() || !std::isfinite(neighborWeight) ||
            neighborWeight<0 || neighborWeight>1 || !std::isfinite(voxelSize) || voxelSize<=0) return false;
        auto& frame=m_frames[index];const auto completed=device.GetFence(index)->GetCompletedValue();
        if(completed==UINT64_MAX || completed<device.GetFenceValue(index) || (frame.submitted && completed<frame.fence)) return false;
        if(frame.submitted)
        {
            void* mapped=nullptr;const D3D12_RANGE reads{0,16};
            if(!Check(frame.timestamps->Map(0,&reads,&mapped),"map timestamps")) return false;
            const auto* times=static_cast<const uint64_t*>(mapped);
            m_gpuMilliseconds=times[1]>=times[0]?double(times[1]-times[0])*1000.0/double(m_frequency):0;
            const D3D12_RANGE writes{0,0};frame.timestamps->Unmap(0,&writes);frame.submitted=false;
        }
        if(state.IsCacheEnabled()) return RecordSparse(device,heap,index,scene,sdf,raw,state,voxelSize,neighborWeight,surface,materialBuffer);
        const auto plan=state.GetPendingPlan();const auto count=state.GetPendingSources().probeCount;
        m_pendingWeight=neighborWeight;m_pendingCoverageRevision=state.GetPendingSources().coverageRevision;m_hasRecordedWork=false;
        if(!count) { m_recorded=true;return true; }
        if(!heap || !raw || !materialBuffer || scene.IsMeshBackend(index)!=m_meshBackend || !scene.Get(index,5) || !scene.Get(index,6)) return false;
        bool resized=false;
        if(count!=m_count)
        {
            for(UINT slot=0;slot<m_frames.size();++slot)
            {
                device.WaitForPreviousFrame(static_cast<int>(slot));
                const auto value=device.GetFence(static_cast<int>(slot))->GetCompletedValue();
                if(value==UINT64_MAX || value<device.GetFenceValue(static_cast<int>(slot))) return false;
            }
            Microsoft::WRL::ComPtr<ID3D12Resource> filtered,edges;
            if(!CreateBuffer(device.GetDevice(),count*sizeof(VXGIProbeState::GPURecord),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,filtered) ||
                !CreateBuffer(device.GetDevice(),count*sizeof(uint32_t),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,edges)) return false;
            for(auto* resource:{filtered.Get(),edges.Get()})
            {
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);
                device.GetCommandList()->ResourceBarrier(1,&barrier);
            }
            filtered->SetName(L"VXGI diagnostic filtered SH");edges->SetName(L"VXGI cached neighbor visibility");
            m_filtered=std::move(filtered);m_edges=std::move(edges);m_count=count;resized=true;
        }
        Constants c{};const auto& grid=m_meshBackend?scene.GetGrid(index):sdf.GetGrid();c.gridOrigin=grid.origin;c.cellSize=grid.cellSize;
        c.cellDimensions=grid.cellDimensions;c.probeCount=static_cast<uint32_t>(count);c.boundsMaximum=grid.boundsMaximum;
        c.triangleCount=m_meshBackend?scene.GetTriangleCount(index):static_cast<uint32_t>(sdf.GetTriangles().size());c.voxelSize=voxelSize;
        c.updateStart=plan.start;c.updateCount=plan.count;c.neighborWeight=neighborWeight;
        c.materialCount=static_cast<uint32_t>(surface.GetMaterials().size());c.materialMipLevel=surface.GetParameters().materialMipLevel;
        c.resetMode=resized || !m_initialized ? 1u : plan.resetMode;
        if(m_pendingCoverageRevision!=m_coverageRevision) c.resetMode=1;
        if(!c.resetMode && neighborWeight!=m_weight) c.resetMode=2;
        RDGBuilder graph(&device,"VXGIProbeFilterGraph");
        std::vector<RDGBufferHandle> geometry(m_meshBackend?10:7);
        for(UINT i=0;i<7;++i) geometry[i]=graph.RegisterExternalBuffer(scene.Get(index,i),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI filter scene");
        if(m_meshBackend) for(UINT i=0;i<3;++i) geometry[7+i]=graph.RegisterExternalBuffer(scene.Get(index,7+i),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI filter mesh instances and assets");
        const auto input=graph.RegisterExternalBuffer(raw,VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI raw SH input");
        const auto materials=graph.RegisterExternalBuffer(materialBuffer,VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI coverage materials");
        const auto edges=graph.RegisterExternalBuffer(m_edges.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI neighbor visibility");
        const auto output=graph.RegisterExternalBuffer(m_filtered.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI diagnostic filtered SH");
        graph.MarkBufferAsOutput(edges);graph.MarkBufferAsOutput(output);
        const auto bind=[&](ID3D12GraphicsCommandList* cmd,const Constants& constants)
        {
            ID3D12DescriptorHeap* heaps[]={heap};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(m_root.Get());
            cmd->SetComputeRoot32BitConstants(0,20,&constants,0);
            for(UINT i=0;i<7;++i) cmd->SetComputeRootShaderResourceView(i+1,scene.Get(index,i)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(8,raw->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(9,m_edges->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(10,materialBuffer->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(11,m_edges->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(12,m_filtered->GetGPUVirtualAddress());
            if(m_meshBackend) for(UINT i=0;i<3;++i) cmd->SetComputeRootShaderResourceView(13+i,scene.Get(index,7+i)->GetGPUVirtualAddress());
        };
        RDGPassParameters visibility;for(const auto handle:geometry) visibility.ReadSRV(handle);
        visibility.ReadSRV(input);visibility.ReadSRV(materials);visibility.WriteUAV(edges);if(c.resetMode) visibility.WriteUAV(output);
        const auto visiblePass=graph.AddPass("Probe neighbor visibility",ERDGPassFlags::Compute,visibility,[&,c](ID3D12GraphicsCommandList* cmd)
        {
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2);bind(cmd,c);
            if(c.resetMode)
            {
                cmd->SetPipelineState(m_pipelines[0].Get());cmd->Dispatch((c.probeCount+63)/64,1,1);
                const D3D12_RESOURCE_BARRIER barriers[]={CD3DX12_RESOURCE_BARRIER::UAV(m_edges.Get()),CD3DX12_RESOURCE_BARRIER::UAV(m_filtered.Get())};
                cmd->ResourceBarrier(2,barriers);
            }
            cmd->SetPipelineState(m_pipelines[1].Get());
            for(uint32_t base=0;base<c.updateCount;)
            {
                const uint32_t batch=(std::min)(c.updateCount-base,65535u),start=c.updateStart+base;
                cmd->SetComputeRoot32BitConstants(0,1,&start,13);cmd->SetComputeRoot32BitConstants(0,1,&batch,14);
                cmd->Dispatch(batch,1,1);base+=batch;
            }
        });
        RDGPassParameters filter;filter.ReadSRV(input);filter.ReadSRV(edges);filter.ReadSRV(materials);filter.WriteUAV(output);
        for(const auto handle:geometry) filter.ReadSRV(handle);
        const auto filterPass=graph.AddPass("Probe signed SH filter",ERDGPassFlags::Compute,filter,[&,c](ID3D12GraphicsCommandList* cmd)
        {
            bind(cmd,c);cmd->SetPipelineState(m_pipelines[2].Get());cmd->Dispatch((c.updateCount+63)/64,1,1);
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2+1);
            cmd->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2,2,frame.timestamps.Get(),0);
        });
        graph.AddPassDependency(filterPass,visiblePass);graph.Execute(device.GetCommandList());
        if(!graph.GetCompileStats().compileSucceeded) return false;
        m_hasRecordedWork=true;m_recorded=true;return true;
    }
    bool OnSubmitted(int index,uint64_t fence)
    {
        if(!m_recorded) return false;
        auto& frame=m_frames.at(index);if(m_hasRecordedWork) { frame.submitted=true;frame.fence=fence; }
        m_initialized=true;m_weight=m_pendingWeight;m_coverageRevision=m_pendingCoverageRevision;m_recorded=false;return true;
    }
    ID3D12Resource* GetFilteredRecords() const { return m_filtered.Get(); }
    ID3D12Resource* GetVisibilityMasks() const { return m_edges.Get(); }
    uint64_t GetGPUMemoryBytes() const
    {
        uint64_t bytes=m_count*(sizeof(VXGIProbeState::GPURecord)+sizeof(uint32_t))+m_frames.size()*16;
        for(const auto& frame:m_frames)
        {
            bytes+=frame.indicesCapacity+frame.invalidationsCapacity+frame.remapCapacity+frame.neighborsCapacity;
            if(frame.remapFiltered) bytes+=frame.remapFiltered->GetDesc().Width;
            if(frame.remapEdges) bytes+=frame.remapEdges->GetDesc().Width;
        }
        return bytes;
    }
    double GetGPUMilliseconds() const { return m_gpuMilliseconds; }
private:
    bool RecordSparse(RenderDevice& device,ID3D12DescriptorHeap* heap,int index,const VXGIGPUScene& scene,
        const VXGISDF& sdf,ID3D12Resource* raw,const VXGIProbeState& state,float voxelSize,float neighborWeight,
        const VXGISurface& surface,ID3D12Resource* materialBuffer)
    {
        const auto plan=state.GetPendingPlan(); const auto count=state.GetPendingSources().probeCount;
        m_pendingWeight=neighborWeight; m_pendingCoverageRevision=state.GetPendingSources().coverageRevision;
        m_hasRecordedWork=false;
        auto& frame=m_frames[index]; frame.remapFiltered.Reset(); frame.remapEdges.Reset();
        const bool weightChanged=m_initialized && neighborWeight!=m_weight;
        if((!plan.HasWork() && !weightChanged) || count==0)
        { m_gpuMilliseconds=0; ++m_activity.idleFrames; m_recorded=true; return true; }
        if(count!=m_topology.size() || !heap || !raw || !materialBuffer || scene.IsMeshBackend(index)!=m_meshBackend ||
            !scene.Get(index,5) || !scene.Get(index,6)) return false;
        const bool remap=plan.layoutChanged || !m_initialized || count!=m_count;
        const uint32_t oldCount=static_cast<uint32_t>(m_count);
        if(remap)
        {
            if(state.GetPendingRemap().size()!=count) return false;
            // Allocation events are uncommon. Wait before replacing these shared
            // diagnostic resources; ordinary sparse/idle frames never wait here.
            for(UINT slot=0;slot<m_frames.size();++slot) device.WaitForPreviousFrame(static_cast<int>(slot));
            Microsoft::WRL::ComPtr<ID3D12Resource> filtered,edges;
            if(!CreateBuffer(device.GetDevice(),count*sizeof(VXGIProbeState::GPURecord),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,filtered) ||
                !CreateBuffer(device.GetDevice(),count*sizeof(uint32_t),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,edges)) return false;
            for(auto* resource:{filtered.Get(),edges.Get()})
            {
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);
                device.GetCommandList()->ResourceBarrier(1,&barrier);
            }
            frame.remapFiltered=m_filtered; frame.remapEdges=m_edges;
            m_filtered=std::move(filtered); m_edges=std::move(edges); m_count=count;
        }
        std::vector<uint32_t> selected,filtered;
        std::vector<VXGIProbeState::WorkItem> invalidations;
        const auto addSelected=[&](uint32_t probe)
        {
            if(probe>=count) return;
            selected.push_back(probe);
            for(const auto neighbor:m_topology[probe].indices) if(neighbor<count) selected.push_back(neighbor);
        };
        const auto addFilter=[&](uint32_t probe)
        {
            if(probe>=count) return;
            filtered.push_back(probe);
            for(const auto neighbor:m_topology[probe].indices) if(neighbor<count) filtered.push_back(neighbor);
        };
        const auto addInvalidation=[&](uint32_t probe,uint32_t reset)
        {
            if(probe>=count) return;
            invalidations.push_back({probe,reset,0,0});
            // SH changes retain neighbors' own raw/filtered history until they
            // are recomputed below. Geometry also invalidates incident edges.
            if(reset==VXGIProbeState::ResetClassification)
                for(const auto neighbor:m_topology[probe].indices) if(neighbor<count) invalidations.push_back({neighbor,reset,0,0});
            addFilter(probe);
        };
        for(const auto& work:state.GetPendingWork()) addSelected(work.probeIndex);
        for(const auto& work:state.GetPendingInvalidations()) addInvalidation(work.probeIndex,work.resetMode);
        if(remap)
        {
            const auto map=state.GetPendingRemap();
            for(uint32_t probe=0;probe<count;++probe)
            {
                const auto old=map[probe]; bool unchanged=old<oldCount && old<m_previousTopology.size();
                if(unchanged) for(size_t direction=0;direction<6;++direction)
                {
                    const auto neighbor=m_topology[probe].indices[direction];
                    const auto mapped=neighbor<count ? map[neighbor] : UINT32_MAX;
                    if(mapped!=m_previousTopology[old].indices[direction]) unchanged=false;
                }
                if(!unchanged)
                {
                    addInvalidation(probe,VXGIProbeState::ResetClassification);
                    // Removed neighbors can change a retained clean probe even
                    // when no new raw probe is queued there. Refresh its edges
                    // now; new probes follow the normal bounded warm-up work.
                    if(old<oldCount) addSelected(probe);
                }
            }
        }
        if(weightChanged) for(uint32_t probe=0;probe<count;++probe) filtered.push_back(probe);
        std::sort(selected.begin(),selected.end()); selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
        const uint32_t visibilityCount=static_cast<uint32_t>(selected.size());
        filtered.insert(filtered.end(),selected.begin(),selected.end());
        std::sort(filtered.begin(),filtered.end()); filtered.erase(std::unique(filtered.begin(),filtered.end()),filtered.end());
        const uint32_t filterCount=static_cast<uint32_t>(filtered.size());
        selected.insert(selected.end(),filtered.begin(),filtered.end());
        std::sort(invalidations.begin(),invalidations.end(),[](const auto& a,const auto& b)
        { return a.probeIndex<b.probeIndex || (a.probeIndex==b.probeIndex && a.resetMode<b.resetMode); });
        invalidations.erase(std::unique(invalidations.begin(),invalidations.end(),[](const auto& a,const auto& b)
        { return a.probeIndex==b.probeIndex; }),invalidations.end());
        if(!Upload(device.GetDevice(),frame.indicesUpload,frame.indicesCapacity,selected.data(),selected.size()*sizeof(uint32_t)) ||
            !Upload(device.GetDevice(),frame.invalidationsUpload,frame.invalidationsCapacity,invalidations.data(),invalidations.size()*sizeof(VXGIProbeState::WorkItem)) ||
            !Upload(device.GetDevice(),frame.remapUpload,frame.remapCapacity,state.GetPendingRemap().data(),state.GetPendingRemap().size_bytes()) ||
            !Upload(device.GetDevice(),frame.neighborsUpload,frame.neighborsCapacity,remap?m_previousTopology.data():nullptr,
                remap?m_previousTopology.size()*sizeof(VXGIProbeTopology::NeighborRecord):0)) return false;
        Constants c{}; const auto& grid=m_meshBackend?scene.GetGrid(index):sdf.GetGrid(); c.gridOrigin=grid.origin; c.cellSize=grid.cellSize;
        c.cellDimensions=grid.cellDimensions; c.probeCount=static_cast<uint32_t>(count); c.boundsMaximum=grid.boundsMaximum;
        c.triangleCount=m_meshBackend?scene.GetTriangleCount(index):static_cast<uint32_t>(sdf.GetTriangles().size()); c.voxelSize=voxelSize;
        c.neighborWeight=neighborWeight; c.materialCount=static_cast<uint32_t>(surface.GetMaterials().size());
        c.materialMipLevel=surface.GetParameters().materialMipLevel; c.padding2=oldCount;
        RDGBuilder graph(&device,"VXGI sparse probe filter graph");
        std::vector<RDGBufferHandle> geometry(m_meshBackend?10:7);
        for(UINT slot=0;slot<7;++slot) geometry[slot]=graph.RegisterExternalBuffer(scene.Get(index,slot),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI filter scene");
        if(m_meshBackend) for(UINT slot=0;slot<3;++slot) geometry[7+slot]=graph.RegisterExternalBuffer(scene.Get(index,7+slot),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI filter mesh");
        const auto input=graph.RegisterExternalBuffer(raw,VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI raw SH");
        const auto materials=graph.RegisterExternalBuffer(materialBuffer,VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI materials");
        const auto edges=graph.RegisterExternalBuffer(m_edges.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI edges");
        const auto output=graph.RegisterExternalBuffer(m_filtered.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI filtered SH");
        graph.MarkBufferAsOutput(edges); graph.MarkBufferAsOutput(output);
        const auto bind=[&](ID3D12GraphicsCommandList* cmd)
        {
            ID3D12DescriptorHeap* heaps[]={heap}; cmd->SetDescriptorHeaps(1,heaps); cmd->SetComputeRootSignature(m_root.Get());
            cmd->SetComputeRoot32BitConstants(0,20,&c,0);
            for(UINT slot=0;slot<7;++slot) cmd->SetComputeRootShaderResourceView(slot+1,scene.Get(index,slot)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(8,raw->GetGPUVirtualAddress()); cmd->SetComputeRootShaderResourceView(9,m_edges->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(10,materialBuffer->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(11,m_edges->GetGPUVirtualAddress()); cmd->SetComputeRootUnorderedAccessView(12,m_filtered->GetGPUVirtualAddress());
            if(m_meshBackend) for(UINT slot=0;slot<3;++slot) cmd->SetComputeRootShaderResourceView(13+slot,scene.Get(index,7+slot)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(16,frame.indicesUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(17,frame.invalidationsUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(18,frame.remapUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(19,(frame.remapFiltered?frame.remapFiltered.Get():m_filtered.Get())->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(20,(frame.remapEdges?frame.remapEdges.Get():m_edges.Get())->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(21,frame.neighborsUpload->GetGPUVirtualAddress());
        };
        const auto dispatch=[&](ID3D12GraphicsCommandList* cmd,uint32_t pipeline,uint32_t total,bool groups,uint32_t offset=0)
        {
            if(total==0) return;
            cmd->SetPipelineState(m_pipelines[pipeline].Get());
            const uint32_t capacity=groups?65535u:65535u*64u;
            for(uint32_t base=0;base<total;)
            {
                const auto batch=(std::min)(total-base,capacity);
                const auto start=base+offset;
                cmd->SetComputeRoot32BitConstants(0,1,&start,13); cmd->SetComputeRoot32BitConstants(0,1,&batch,14);
                cmd->Dispatch(groups?batch:(batch+63)/64,1,1); base+=batch;
            }
        };
        RDGPassParameters visibility; for(const auto handle:geometry) visibility.ReadSRV(handle);
        visibility.ReadSRV(input); visibility.ReadSRV(materials); visibility.WriteUAV(edges); visibility.WriteUAV(output);
        if(remap && frame.remapFiltered)
        {
            visibility.ReadSRV(graph.RegisterExternalBuffer(frame.remapFiltered.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI previous filtered"));
            visibility.ReadSRV(graph.RegisterExternalBuffer(frame.remapEdges.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI previous edges"));
        }
        const auto first=graph.AddPass("Sparse probe cache visibility",ERDGPassFlags::Compute,visibility,[&](ID3D12GraphicsCommandList* cmd)
        {
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2); bind(cmd);
            const D3D12_RESOURCE_BARRIER barriers[]={CD3DX12_RESOURCE_BARRIER::UAV(m_edges.Get()),CD3DX12_RESOURCE_BARRIER::UAV(m_filtered.Get())};
            if(remap) { dispatch(cmd,3,c.probeCount,false); cmd->ResourceBarrier(2,barriers); ++m_activity.remapDispatches; }
            if(!invalidations.empty()) { dispatch(cmd,4,static_cast<uint32_t>(invalidations.size()),false); cmd->ResourceBarrier(2,barriers); ++m_activity.invalidationDispatches; ++m_activity.resetDispatches; }
            if(visibilityCount) { dispatch(cmd,5,visibilityCount,true); ++m_activity.visibilityDispatches; }
        });
        RDGPassParameters filtering; filtering.ReadSRV(input); filtering.ReadSRV(edges); filtering.WriteUAV(output);
        for(const auto handle:geometry) filtering.ReadSRV(handle);
        const auto last=graph.AddPass("Sparse probe SH filter",ERDGPassFlags::Compute,filtering,[&](ID3D12GraphicsCommandList* cmd)
        {
            bind(cmd); if(filterCount) { dispatch(cmd,6,filterCount,false,visibilityCount); ++m_activity.filterDispatches; }
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2+1);
            cmd->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2,2,frame.timestamps.Get(),0);
        });
        graph.AddPassDependency(last,first); graph.Execute(device.GetCommandList());
        if(!graph.GetCompileStats().compileSucceeded) return false;
        m_hasRecordedWork=true; m_recorded=true; ++m_activity.activeBatches; return true;
    }
    static bool Upload(ID3D12Device* device,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,uint64_t& capacity,
        const void* data,uint64_t bytes)
    {
        const auto required=(std::max)(bytes,uint64_t(32));
        if(capacity<required)
        {
            if(!CreateBuffer(device,required,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,buffer)) return false;
            capacity=required;
        }
        void* mapped=nullptr; const D3D12_RANGE noReads{0,0}; if(!Check(buffer->Map(0,&noReads,&mapped),"map sparse filter upload")) return false;
        if(bytes) std::memcpy(mapped,data,static_cast<size_t>(bytes)); else std::memset(mapped,0,static_cast<size_t>(required));
        const D3D12_RANGE writes{0,static_cast<SIZE_T>(required)}; buffer->Unmap(0,&writes); return true;
    }
    bool m_meshBackend=false;
    struct Constants
    {
        DirectX::XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;uint32_t probeCount;
        DirectX::XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        float voxelSize;uint32_t updateStart,updateCount,resetMode;
        float neighborWeight;uint32_t materialCount;float materialMipLevel;uint32_t padding2;
    };
    static_assert(sizeof(Constants)==80);
    static_assert(offsetof(Constants,materialCount)==68 && offsetof(Constants,materialMipLevel)==72);
    struct Frame
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> timestamps,indicesUpload,invalidationsUpload,remapUpload,neighborsUpload,remapFiltered,remapEdges;
        uint64_t fence=0,indicesCapacity=0,invalidationsCapacity=0,remapCapacity=0,neighborsCapacity=0;
        bool submitted=false;
    };
    static bool Check(HRESULT hr,const char* operation)
    { if(SUCCEEDED(hr)) return true;ErrorLog::HRESULT(std::string("VXGI filter: ")+operation,hr);return false; }
    static bool CreateBuffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer)
    {
        const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);
        return Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&buffer)),"create buffer");
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> m_filtered,m_edges;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queries;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,7> m_pipelines;
    std::vector<Frame> m_frames;uint64_t m_count=0,m_frequency=0;
    double m_gpuMilliseconds=0;float m_weight=0,m_pendingWeight=0;
    uint64_t m_coverageRevision=0,m_pendingCoverageRevision=0;
    std::vector<VXGIProbeTopology::NeighborRecord> m_topology,m_previousTopology;
    ActivityStats m_activity;
    bool m_initialized=false,m_recorded=false,m_hasRecordedWork=false;
};
