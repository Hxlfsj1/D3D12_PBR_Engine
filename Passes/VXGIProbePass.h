#pragma once
#include "RDG.h"
#include "PBR_Shader.h"
#include "VXGIGPUScene.h"
#include "VXGIProbeState.h"
#include "VXGIProbeInvalidation.h"
#include "VXGISurface.h"
#include <array>
#include <span>
#include <cstring>
#include <vector>

// Queue-ordered persistent histories. Idle frames do not copy, dispatch or read
// back anything; active batches report only the selected probe records.
class VXGIProbePass
{
public:
    struct ActivityStats
    {
        uint64_t historyCopies=0,remapDispatches=0,invalidationDispatches=0,updateDispatches=0;
        uint64_t feedbackCopies=0,legacySnapshotCopies=0,activeBatches=0,idleFrames=0;
    };
    const ActivityStats& GetActivityStats() const { return m_activity; }
    struct CompletedDependency
    {
        uint32_t probeIndex=0,generation=0,domainGeneration=0;
        VXGIProbeInvalidation::Mask mask={};
    };
    void SetDependencyDomain(const VXGIProbeInvalidation::Domain& domain) { m_domain=domain; }
    std::vector<CompletedDependency> TakeCompletedDependencies()
    { std::vector<CompletedDependency> result;result.swap(m_completedDependencies);return result; }
    bool Initialize(RenderDevice& device,UINT frames,bool meshBackend=false)
    {
        m_meshBackend=meshBackend;m_frames.resize(frames);
        D3D12_QUERY_HEAP_DESC queries{};queries.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queries.Count=frames*2;
        if(!Check(device.GetDevice()->CreateQueryHeap(&queries,IID_PPV_ARGS(&m_queries)),"create timestamps") ||
            !Check(device.GetCommandQueue()->GetTimestampFrequency(&m_frequency),"get timestamp frequency")) return false;
        for(auto& frame:m_frames) if(!CreateBuffer(device.GetDevice(),16,D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,frame.timestamps)) return false;
        // 32 constants + sixteen root descriptors = 64 DWORDs, at the 64 limit.
        CD3DX12_ROOT_PARAMETER roots[17];roots[0].InitAsConstants(32,0);
        for(UINT i=0;i<7;++i) roots[i+1].InitAsShaderResourceView(i);
        roots[8].InitAsUnorderedAccessView(0);
        for(UINT i=0;i<3;++i) roots[9+i].InitAsShaderResourceView(i,1);
        roots[12].InitAsShaderResourceView(7);roots[13].InitAsShaderResourceView(8);
        roots[14].InitAsUnorderedAccessView(1);roots[15].InitAsConstantBufferView(1);roots[16].InitAsShaderResourceView(9);
        const CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const CD3DX12_ROOT_SIGNATURE_DESC desc(17,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        if(!Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"serialize root") ||
            !Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root)),"create root")) return false;
        const wchar_t* entries[]={L"CSReset",L"CSUpdateParallel",L"CSInvalidate",L"CSLegacyReset"};
        for(UINT i=0;i<4;++i)
        {
            std::vector<std::wstring> definitions{L"VXGI_PROBE_SPARSE=1"};if(meshBackend) definitions.push_back(L"VXGI_MESH_SDF=1");
            const auto shader=ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_VXGI_Probe_Update.hlsl",entries[i],L"cs_6_6",definitions,true);
            if(!shader) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            if(!Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&m_pipelines[i])),"create compute pipeline")) return false;
        }
        return true;
    }
    bool Poll(RenderDevice& device,int index,VXGIProbeState& state)
    {
        auto& frame=m_frames.at(index);if(!frame.submitted) return true;
        const auto completed=device.GetFence(index)->GetCompletedValue();
        if(completed==UINT64_MAX || completed<frame.fence) return false;
        void* mapped=nullptr;const D3D12_RANGE times{0,16};
        if(!Check(frame.timestamps->Map(0,&times,&mapped),"map timestamps")) return false;
        const auto* values=static_cast<const uint64_t*>(mapped);
        m_gpuMilliseconds=values[1]>=values[0]?double(values[1]-values[0])*1000.0/double(m_frequency):0;
        const D3D12_RANGE noWrites{0,0};frame.timestamps->Unmap(0,&noWrites);
        if(frame.legacySnapshot)
        {
            const D3D12_RANGE reads{0,static_cast<SIZE_T>(frame.fullSnapshotBytes)};
            if(!Check(frame.fullReadback->Map(0,&reads,&mapped),"map legacy probe snapshot")) return false;
            const auto& stats=state.GetStats();
            const bool obsolete=frame.epoch!=state.GetRevision() || frame.frameIndex<stats.lastResetFrameIndex ||
                (stats.hasCompletedReadback && frame.frameIndex<stats.completedFrameIndex);
            const bool accepted=obsolete || state.AcceptReadback(std::span(static_cast<const VXGIProbeState::GPURecord*>(mapped),
                static_cast<size_t>(frame.fullSnapshotBytes/sizeof(VXGIProbeState::GPURecord))),frame.frameIndex,frame.epoch);
            frame.fullReadback->Unmap(0,&noWrites);
            if(!accepted) return false;
        }
        if(frame.sparse && !frame.work.empty())
        {
            const SIZE_T bytes=frame.work.size()*sizeof(Feedback);const D3D12_RANGE reads{0,bytes};
            if(!Check(frame.readback->Map(0,&reads,&mapped),"map probe batch feedback")) return false;
            const auto* feedback=static_cast<const Feedback*>(mapped);
            std::vector<VXGIProbeState::GPURecord> records;records.reserve(frame.work.size());
            std::vector<uint8_t> current(frame.work.size(),0);
            // Preserve the in-flight stamp check before scheduler completion consumes it.
            for(size_t i=0;i<frame.work.size();++i)
            {
                records.push_back(feedback[i].record);
                current[i]=state.IsCurrentWork(frame.work[i],frame.frameIndex)?1u:0u;
            }
            const bool accepted=state.AcceptBatchResults(frame.work,records,frame.frameIndex,frame.epoch);
            if(accepted) for(size_t i=0;i<frame.work.size();++i)
                if(current[i]) m_completedDependencies.push_back({frame.work[i].probeIndex,frame.work[i].generation,
                    frame.domainGeneration,feedback[i].dependencies});
            frame.readback->Unmap(0,&noWrites);
            if(!accepted) { ErrorLog::Write("VXGI probe batch validation failed: "+state.GetLastError());return false; }
        }
        frame.submitted=false;frame.work.clear();frame.remapSource.Reset();return true;
    }
    bool Record(RenderDevice& device,ID3D12DescriptorHeap* heap,int index,const VXGIGPUScene& scene,
        const VXGISDF& sdf,const VXGISurface& surface,D3D12_GPU_VIRTUAL_ADDRESS materialBuffer,VXGIProbeState& state)
    {
        m_recorded=false;m_gpuRecorded=false;
        if(!Poll(device,index,state) || !state.HasPendingPlan()) return false;
        const auto plan=state.GetPendingPlan();const auto count=state.GetPendingSources().probeCount;
        const bool sparse=state.IsCacheEnabled();
        m_plan=plan;m_displayHistoryIndex=plan.readHistoryIndex;
        if(!plan.HasWork() || count==0)
        { m_gpuMilliseconds=0;++m_activity.idleFrames;m_recorded=true;return true; }
        if(!heap || !materialBuffer || scene.IsMeshBackend(index)!=m_meshBackend || !scene.Get(index,0)) return false;
        auto& frame=m_frames[index];const uint64_t bytes=count*sizeof(VXGIProbeState::GPURecord);
        const uint32_t previousCount=static_cast<uint32_t>(m_historyBytes/sizeof(VXGIProbeState::GPURecord));
        Microsoft::WRL::ComPtr<ID3D12Resource> previous=m_history[plan.readHistoryIndex];
        if(m_historyBytes!=bytes)
        {
            for(UINT slot=0;slot<m_frames.size();++slot)
            {
                device.WaitForPreviousFrame(static_cast<int>(slot));
                const auto completed=device.GetFence(static_cast<int>(slot))->GetCompletedValue();
                if(completed==UINT64_MAX || completed<device.GetFenceValue(static_cast<int>(slot))) return false;
            }
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> next;
            for(auto& history:next) if(!CreateBuffer(device.GetDevice(),bytes,D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,history)) return false;
            for(auto& history:next)
            {
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(history.Get(),D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);
                device.GetCommandList()->ResourceBarrier(1,&barrier);
            }
            m_history=std::move(next);m_historyBytes=bytes;
        }
        if(!previous) previous=m_history[plan.readHistoryIndex];
        frame.remapSource=previous;
        const auto work=state.GetPendingWork();const auto remap=state.GetPendingRemap();const auto invalidations=state.GetPendingInvalidations();
        if(work.size()!=plan.count || (plan.layoutChanged && remap.size()!=count)) return false;
        if(!Upload(device.GetDevice(),frame.workUpload,frame.workCapacity,work.data(),work.size_bytes()) ||
            !Upload(device.GetDevice(),frame.remapUpload,frame.remapCapacity,remap.data(),remap.size_bytes()) ||
            !Upload(device.GetDevice(),frame.invalidationUpload,frame.invalidationCapacity,invalidations.data(),invalidations.size_bytes())) return false;
        DependencyConstants dependency{};dependency.domain=m_domain;dependency.previousProbeCount=previousCount;
        dependency.layoutChanged=plan.layoutChanged?1u:0u;dependency.invalidationCount=static_cast<uint32_t>(invalidations.size());
        if(!Upload(device.GetDevice(),frame.constantsUpload,frame.constantsCapacity,&dependency,sizeof(dependency),256)) return false;
        const uint64_t feedbackBytes=(std::max)(uint64_t(work.size())*sizeof(Feedback),uint64_t(sizeof(Feedback)));
        if(frame.feedbackCapacity<feedbackBytes)
        {
            if(!CreateBuffer(device.GetDevice(),feedbackBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,frame.feedback) ||
                !CreateBuffer(device.GetDevice(),feedbackBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE,frame.readback)) return false;
            const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(frame.feedback.Get(),D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);
            device.GetCommandList()->ResourceBarrier(1,&barrier);
            frame.feedbackCapacity=feedbackBytes;
        }
        frame.legacySnapshot=!sparse && (plan.resetMode!=0 || plan.frameIndex%32==0);
        if(frame.legacySnapshot && frame.fullSnapshotBytes!=bytes)
        {
            if(!CreateBuffer(device.GetDevice(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_FLAG_NONE,frame.fullReadback)) return false;
            frame.fullSnapshotBytes=bytes;
        }
        Constants c{};const auto& grid=m_meshBackend?scene.GetGrid(index):sdf.GetGrid();const auto& light=surface.GetParameters();const auto& p=state.GetPendingParameters();
        c.gridOrigin=grid.origin;c.cellSize=grid.cellSize;c.cellDimensions=grid.cellDimensions;c.probeCount=static_cast<uint32_t>(count);
        c.boundsMaximum=grid.boundsMaximum;c.triangleCount=m_meshBackend?scene.GetTriangleCount(index):static_cast<uint32_t>(sdf.GetTriangles().size());
        DirectX::XMStoreFloat3(&c.sunDirectionTowardLight,DirectX::XMVectorNegate(DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&light.lightDirection))));
        c.materialMipLevel=light.materialMipLevel;c.sunRadiance=light.lightColor;c.rayMinDistance=p.rayMinDistance;
        c.maxRayDistance=p.rayMaxDistance;c.raysPerProbe=p.raysPerUpdate;c.maxHistoryUpdates=p.maxHistoryUpdates;c.frameIndex=plan.frameIndex;
        c.updateStart=0;c.updateCount=plan.count;c.resetMode=plan.resetMode;c.materialCount=static_cast<uint32_t>(surface.GetMaterials().size());
        c.classifyRays=p.classifyRays;c.backfaceThreshold=p.backfaceThreshold;
        RDGBuilder graph(&device,"VXGIProbeGraph");
        std::vector<RDGBufferHandle> geometry(m_meshBackend?8:5);
        for(UINT i=0;i<5;++i) geometry[i]=graph.RegisterExternalBuffer(scene.Get(index,i),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI shared scene");
        if(m_meshBackend) for(UINT i=0;i<3;++i) geometry[5+i]=graph.RegisterExternalBuffer(scene.Get(index,7+i),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI mesh instances and assets");
        const auto read=graph.RegisterExternalBuffer(previous.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI previous SH");
        const auto write=graph.RegisterExternalBuffer(m_history[plan.writeHistoryIndex].Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI next SH");
        const auto feedback=graph.RegisterExternalBuffer(frame.feedback.Get(),VXGIGPUScene::ReadState,VXGIGPUScene::ReadState,"VXGI selected batch feedback");
        graph.MarkBufferAsOutput(write);graph.MarkBufferAsOutput(feedback);
        auto bind=[&,c,plan,heap,previous](ID3D12GraphicsCommandList* cmd)
        {
            ID3D12DescriptorHeap* heaps[]={heap};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(m_root.Get());
            cmd->SetComputeRoot32BitConstants(0,32,&c,0);
            for(UINT i=0;i<4;++i) cmd->SetComputeRootShaderResourceView(i+1,scene.Get(index,i)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(5,materialBuffer);cmd->SetComputeRootShaderResourceView(6,scene.Get(index,4)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(7,previous->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(8,m_history[plan.writeHistoryIndex]->GetGPUVirtualAddress());
            if(m_meshBackend) for(UINT i=0;i<3;++i) cmd->SetComputeRootShaderResourceView(9+i,scene.Get(index,7+i)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(12,frame.workUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(13,frame.remapUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(14,frame.feedback->GetGPUVirtualAddress());
            cmd->SetComputeRootConstantBufferView(15,frame.constantsUpload->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(16,frame.invalidationUpload->GetGPUVirtualAddress());
        };
        const bool initializeCompute=plan.layoutChanged || (!sparse && plan.resetMode!=0);
        RDGPassParameters initialize;
        if(initializeCompute) { initialize.ReadSRV(read);initialize.WriteUAV(write); }else { initialize.ReadCopySrc(read);initialize.WriteCopyDst(write); }
        const auto initializePass=graph.AddPass(initializeCompute?"Probe stable-center remap or reset":"Probe active history copy",
            initializeCompute?ERDGPassFlags::Compute:ERDGPassFlags::Copy,initialize,[&,bind,plan,index,previous,initializeCompute](ID3D12GraphicsCommandList* cmd)
        {
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2);
            if(initializeCompute) { bind(cmd);cmd->SetPipelineState(m_pipelines[plan.layoutChanged?0:3].Get());cmd->Dispatch((c.probeCount+63)/64,1,1);++m_activity.remapDispatches; }
            else { cmd->CopyResource(m_history[plan.writeHistoryIndex].Get(),previous.Get());++m_activity.historyCopies; }
        });
        RDGPassParameters update;update.ReadSRV(read);update.WriteUAV(write);update.WriteUAV(feedback);
        for(const auto handle:geometry) update.ReadSRV(handle);
        const auto updatePass=graph.AddPass("Probe sparse classification and SH",ERDGPassFlags::Compute,update,[&,bind,plan,index](ID3D12GraphicsCommandList* cmd)
        {
            bind(cmd);
            if(dependency.invalidationCount)
            {
                cmd->SetPipelineState(m_pipelines[2].Get());cmd->Dispatch((dependency.invalidationCount+63)/64,1,1);++m_activity.invalidationDispatches;
                const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(m_history[plan.writeHistoryIndex].Get());cmd->ResourceBarrier(1,&barrier);
            }
            cmd->SetPipelineState(m_pipelines[1].Get());
            for(uint32_t base=0;base<plan.count;)
            {
                const uint32_t batch=(std::min)(plan.count-base,65535u);
                cmd->SetComputeRoot32BitConstants(0,1,&base,24);cmd->SetComputeRoot32BitConstants(0,1,&batch,25);
                cmd->Dispatch(batch,1,1);++m_activity.updateDispatches;base+=batch;
            }
            cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2+1);
            cmd->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index*2,2,frame.timestamps.Get(),0);
        });
        graph.AddPassDependency(updatePass,initializePass);
        if(!work.empty())
        {
            const auto dest=graph.RegisterExternalBuffer(frame.readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST,"VXGI compact async results");
            graph.MarkBufferAsOutput(dest);RDGPassParameters capture;capture.ReadCopySrc(feedback);capture.WriteCopyDst(dest);
            const auto pass=graph.AddPass("Probe compact async results",ERDGPassFlags::Copy,capture,[&,feedbackBytes](ID3D12GraphicsCommandList* cmd)
            { cmd->CopyBufferRegion(frame.readback.Get(),0,frame.feedback.Get(),0,feedbackBytes);++m_activity.feedbackCopies; });
            graph.AddPassDependency(pass,updatePass);
        }
        if(frame.legacySnapshot)
        {
            const auto dest=graph.RegisterExternalBuffer(frame.fullReadback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST,"VXGI legacy full snapshot");
            graph.MarkBufferAsOutput(dest);RDGPassParameters capture;capture.ReadCopySrc(write);capture.WriteCopyDst(dest);
            const auto pass=graph.AddPass("Probe legacy async statistics",ERDGPassFlags::Copy,capture,[&,plan](ID3D12GraphicsCommandList* cmd)
            {cmd->CopyResource(frame.fullReadback.Get(),m_history[plan.writeHistoryIndex].Get());++m_activity.legacySnapshotCopies;});
            graph.AddPassDependency(pass,updatePass);
        }
        graph.Execute(device.GetCommandList());if(!graph.GetCompileStats().compileSucceeded) return false;
        frame.work.assign(work.begin(),work.end());frame.domainGeneration=m_domain.generation;frame.sparse=sparse;
        ++m_activity.activeBatches;m_displayHistoryIndex=plan.writeHistoryIndex;m_gpuRecorded=true;m_recorded=true;return true;
    }
    bool OnSubmitted(int index,uint64_t fence,VXGIProbeState& state)
    {
        if(!m_recorded || !state.CommitSubmitted(m_plan)) return false;
        if(m_gpuRecorded)
        {
            auto& frame=m_frames.at(index);frame.submitted=true;frame.fence=fence;frame.frameIndex=m_plan.frameIndex;frame.epoch=m_plan.epoch;
        }
        m_recorded=false;m_gpuRecorded=false;return true;
    }
    ID3D12Resource* GetDisplayHistory() const { return m_history[m_displayHistoryIndex].Get(); }
    uint32_t GetDisplayFrame() const { return m_plan.frameIndex; }
    double GetGPUMilliseconds() const { return m_gpuMilliseconds; }
    uint64_t GetGPUMemoryBytes() const
    {
        uint64_t bytes=m_historyBytes*2;for(const auto& f:m_frames) bytes+=16+f.feedbackCapacity*2+f.workCapacity+f.remapCapacity+f.constantsCapacity+f.invalidationCapacity+f.fullSnapshotBytes;return bytes;
    }
private:
    struct Constants
    {
        DirectX::XMFLOAT3 gridOrigin;float cellSize;std::array<uint32_t,3> cellDimensions;uint32_t probeCount;
        DirectX::XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        DirectX::XMFLOAT3 sunDirectionTowardLight;float materialMipLevel;
        DirectX::XMFLOAT3 sunRadiance;float rayMinDistance;
        float maxRayDistance;uint32_t raysPerProbe,maxHistoryUpdates,frameIndex;
        uint32_t updateStart,updateCount,resetMode,materialCount;
        uint32_t classifyRays;float backfaceThreshold;uint32_t padding[2];
    };
    struct Feedback { VXGIProbeState::GPURecord record;VXGIProbeInvalidation::Mask dependencies; };
    struct DependencyConstants { VXGIProbeInvalidation::Domain domain;uint32_t previousProbeCount=0,layoutChanged=0,invalidationCount=0,padding=0; };
    static_assert(sizeof(Constants)==128 && sizeof(Feedback)==128 && sizeof(DependencyConstants)==64);
    struct Frame
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> timestamps,feedback,readback,workUpload,remapUpload,constantsUpload,invalidationUpload,remapSource,fullReadback;
        std::vector<VXGIProbeState::WorkItem> work;
        uint64_t fence=0,feedbackCapacity=0,workCapacity=0,remapCapacity=0,constantsCapacity=0,invalidationCapacity=0,epoch=0,fullSnapshotBytes=0;
        uint32_t frameIndex=0,domainGeneration=0;bool submitted=false,sparse=false,legacySnapshot=false;
    };
    static bool Check(HRESULT hr,const char* operation)
    { if(SUCCEEDED(hr)) return true;ErrorLog::HRESULT(std::string("VXGI probe: ")+operation,hr);return false; }
    static bool CreateBuffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer)
    {
        const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);
        return Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&buffer)),"create buffer");
    }
    static bool Upload(ID3D12Device* device,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,uint64_t& capacity,
        const void* data,uint64_t bytes,uint64_t minimum=16)
    {
        const auto required=(std::max)(bytes,minimum);
        if(capacity<required)
        {
            if(!CreateBuffer(device,required,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,buffer)) return false;
            capacity=required;
        }
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};if(!Check(buffer->Map(0,&noReads,&mapped),"map batch upload")) return false;
        if(bytes) std::memcpy(mapped,data,static_cast<size_t>(bytes));else std::memset(mapped,0,static_cast<size_t>(minimum));
        const D3D12_RANGE writes{0,static_cast<SIZE_T>(required)};buffer->Unmap(0,&writes);return true;
    }
    ActivityStats m_activity{};
    bool m_meshBackend=false,m_recorded=false,m_gpuRecorded=false;
    uint32_t m_displayHistoryIndex=0;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> m_history;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,4> m_pipelines;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queries;
    std::vector<Frame> m_frames;VXGIProbeState::PendingPlan m_plan;
    VXGIProbeInvalidation::Domain m_domain{};std::vector<CompletedDependency> m_completedDependencies;
    uint64_t m_historyBytes=0,m_frequency=0;double m_gpuMilliseconds=0;
};
