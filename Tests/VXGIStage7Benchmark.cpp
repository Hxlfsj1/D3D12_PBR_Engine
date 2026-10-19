// Finite background benchmark of the configured real scene. The window is never
// shown, and no swap-chain Present is called. Performance runs omit debug/GBV.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#pragma warning(push)
#pragma warning(disable:4267 4244 4099 4324)
#include "../Resources/ResourceManager.h"
#pragma warning(pop)
#include "../Passes/VXGIProbePass.h"
#include "../Passes/VXGIProbeFilterPass.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT3;
using Json=nlohmann::json;
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace
{
    using Clock=std::chrono::steady_clock;
    using Record=VXGIProbeState::GPURecord;
    std::string Label;
    std::string ReportStem(){return "tmp/VXGIStage74Benchmark"+(Label.empty()?"":"-"+Label);}
    void Require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
    void Check(HRESULT value,const char* operation)
    {
        if(FAILED(value)) throw std::runtime_error(std::string(operation)+": HRESULT "+std::to_string(uint32_t(value)));
    }
    template<class F> double Time(F&& fn)
    {
        const auto start=Clock::now();fn();
        return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    }
    std::string UTF8(const wchar_t* input)
    {
        const int count=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);
        std::string result(count,'\0');WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),count,nullptr,nullptr);
        result.pop_back();return result;
    }
    XMFLOAT3 Add(XMFLOAT3 a,XMFLOAT3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
    XMFLOAT3 Sub(XMFLOAT3 a,XMFLOAT3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
    XMFLOAT3 Scale(XMFLOAT3 a,float s){return {a.x*s,a.y*s,a.z*s};}
    XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    double Length(XMFLOAT3 a){return std::sqrt(double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z);}
    Json Distribution(const std::vector<double>& values)
    {
        if(values.empty())return Json::object();
        auto sorted=values;std::sort(sorted.begin(),sorted.end());
        return {{"samples",values.size()},{"minimum_ms",sorted.front()},
            {"median_ms",sorted[sorted.size()/2]},{"p95_ms",sorted[size_t(std::ceil(sorted.size()*.95))-1]},
            {"maximum_ms",sorted.back()},{"mean_ms",std::accumulate(sorted.begin(),sorted.end(),0.0)/sorted.size()}};
    }
    class HiddenWindow
    {
    public:
        HiddenWindow()
        {
            WNDCLASSW desc{};desc.lpfnWndProc=DefWindowProcW;desc.hInstance=GetModuleHandleW(nullptr);
            desc.lpszClassName=L"VXGIStage7BackgroundBenchmark";
            Require(RegisterClassW(&desc)!=0,"register hidden benchmark HWND");
            handle=CreateWindowExW(0,desc.lpszClassName,L"VXGI background benchmark",WS_POPUP,
                0,0,32,32,nullptr,nullptr,desc.hInstance,nullptr);
            Require(handle && !IsWindowVisible(handle),"benchmark window must remain hidden");
        }
        ~HiddenWindow(){if(handle)DestroyWindow(handle);UnregisterClassW(L"VXGIStage7BackgroundBenchmark",GetModuleHandleW(nullptr));}
        HWND handle=nullptr;
    };
    struct QueryInput
    {
        XMFLOAT3 position;uint32_t triangleIndex;
        XMFLOAT3 geometricNormal;uint32_t backFace;
        XMFLOAT3 mappedNormal;uint32_t valid=1;
        XMFLOAT3 barycentric={1.f/3,1.f/3,1.f/3};uint32_t triangleId;
        float distance=1;uint32_t traceStatus=1,padding0=0,padding1=0;
    };
    struct QueryResult
    {
        XMFLOAT3 irradiance;float supportWeight;
        XMFLOAT3 unclamped;uint32_t negativeMask;
        uint32_t candidateCount,acceptedCount,hasSupport,traceFailures;
    };
    struct SupportResult
    {
        QueryResult query;
        uint32_t status,coverageStatus,primaryTraceStatus,primaryTriangleIndex;
        uint32_t positiveCorners,missingCorners,disabledCorners,unclassifiedCorners;
        uint32_t neverCapturedCorners,nonfiniteCorners,eligibleCorners,hemisphereRejectedCorners;
        uint32_t visibilityRejectedCorners,visibilityFailures,acceptedCorners,padding;
    };
    struct QueryParameters
    {
        XMFLOAT3 origin;float cellSize;
        std::array<uint32_t,3> cellDimensions;uint32_t probeCount;
        XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        float voxelSize;uint32_t updateStart=0,updateCount=0,resetMode=0;
        float neighborWeight=.25f;uint32_t materialCount=0;float materialMipLevel=0;uint32_t padding=0;
    };
    static_assert(sizeof(QueryInput)==80 && sizeof(QueryResult)==48 && sizeof(SupportResult)==112 && sizeof(QueryParameters)==80);
    class Benchmark
    {
    public:
        Benchmark()
        {
            report["benchmark_stage"]="7.4 post-alpha real-scene acceptance";
            if(!Label.empty())report["label"]=Label;
            settings.LoadAllSettingsFromJson();
            Require(device.Initialize(window.handle,32,32,3),"initialize real hidden RenderDevice");
            ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"create adapter lookup");
            ComPtr<IDXGIAdapter1> adapter;Check(factory->EnumAdapterByLuid(device.GetDevice()->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"lookup real device adapter");
            DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"read real adapter");
            report["adapter"]=UTF8(desc.Description);report["hardware"]=(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)==0;
            Require(report["hardware"].get<bool>(),"benchmark requires actual hardware");
            const auto sceneConfig=SettingsManager::LoadSceneFromJson("Settings/Scene.json");
            Require(!sceneConfig.empty(),"configured scene must contain real assets");
            report["asset_load_wall_ms"]=Time([&]{Require(resources.LoadAssets(&device,sceneConfig,3),"load configured scene assets");
                Require(resources.BuildGlobalMaterialPool(&device),"build actual global material pool");});
            report["scene_paths"]=Json::array();
            for(const auto& instance:sceneConfig)report["scene_paths"].push_back(instance.modelPath);
            const auto& config=settings.vxgi;
            report["configuration"]={{"voxel_size",config.voxelSize},{"sdf_cell_size",config.sdfCellSize},
                {"sdf_node_sizing","automatic"},{"probes_per_frame",config.probeProbesPerFrame},
                {"classify_rays",config.probeClassifyRays},{"rays_per_update",config.probeRaysPerUpdate},
                {"max_history_updates",config.probeMaxHistoryUpdates},{"backface_threshold",config.probeBackfaceThreshold},
                {"filter_neighbor_weight",config.probeFilterNeighborWeight},{"ray_max_distance",config.surfaceMaxRayDistance},
                {"ray_min_distance",config.surfaceRayMinDistance},{"material_mip_level",config.surfaceMaterialMipLevel}};
            report["cpu"]["world_initial_wall_ms"]=Time([&]{Require(world.Update(resources.GetSceneInstances(),config.voxelSize),world.GetLastError());});
            report["cpu"]["sdf_initial_wall_ms"]=Time([&]{Require(sdf.Update(world,config.sdfCellSize,0),sdf.GetLastError());});
            report["cpu"]["topology_initial_wall_ms"]=Time([&]{Require(topology.Update(world),topology.GetLastError());});
            const auto& grid=sdf.GetGrid();
            report["effective_sdf_grid"]={{"cell_size",grid.cellSize},{"node_dimensions",grid.nodeDimensions},
                {"cell_dimensions",grid.cellDimensions},{"origin",{grid.origin.x,grid.origin.y,grid.origin.z}},
                {"bounds_maximum",{grid.boundsMaximum.x,grid.boundsMaximum.y,grid.boundsMaximum.z}}};
            std::vector<double> stable;
            for(int sample=0;sample<16;++sample)stable.push_back(Time([&]{
                Require(world.Update(resources.GetSceneInstances(),config.voxelSize),world.GetLastError());
                Require(sdf.Update(world,config.sdfCellSize,0),sdf.GetLastError());
                Require(topology.Update(world),topology.GetLastError());}));
            report["cpu"]["stable_world_sdf_topology_wall_ms"]=Distribution(stable);
            const auto& stats=world.GetStats();
            report["geometry"]={{"instances",stats.sceneInstances},{"included_instances",stats.includedInstances},
                {"cutout_instances",stats.cutoutInstances},{"triangles",stats.triangles},{"probes",stats.cells},
                {"geometry_fingerprint",stats.geometryFingerprint},{"sdf_nodes",sdf.GetStats().nodes},
                {"sdf_triangle_references",sdf.GetStats().triangleReferences}};
            report["geometry"]["cutout_contract"]="SDF distances and CSR retain complete triangles as conservative geometry. Production probe classification, capture, sun shadows, filter visibility and support queries accept candidate coverage at actual barycentric UV using albedo SampleLevel alpha times baseColorFactor.a >= 0.5. Opaque triangles ignore alpha. Invalid coverage fails closed.";
            report["cpu"]["world_reported_retained_capacity_estimate_bytes"]=stats.memoryBytes;
            report["cpu"]["sdf_reported_retained_capacity_estimate_bytes"]=sdf.GetStats().memoryBytes;
            report["cpu"]["topology_reported_retained_capacity_bytes"]=topology.GetStats().memoryBytes;
            const auto& acceleration=sdf.GetTraceAccelerationStats();
            report["trace_acceleration"]={{"accelerated_cells",acceleration.acceleratedCells},
                {"node_count",acceleration.nodeCount},{"leaf_count",acceleration.leafCount},
                {"maximum_cell_candidates",acceleration.maximumCellCandidates},
                {"packed_words",acceleration.packedWords},{"cpu_build_ms",acceleration.buildMilliseconds}};
            VXGISurface::Parameters source;
            source.lightDirection=settings.lighting.lightDir;source.lightColor=settings.lighting.lightColor;
            source.materialMipLevel=config.surfaceMaterialMipLevel;source.rayMinDistance=config.surfaceRayMinDistance;
            source.maxRayDistance=config.surfaceMaxRayDistance;
            Require(surface.Update(resources.GetGlobalMaterials(),world.GetGeometryRevision(),sdf.GetGeometryRevision(),world.GetMaterialRevision(),source,world.GetCoverageRevision()),surface.GetLastError());
            parameters.probesPerFrame=uint32_t(config.probeProbesPerFrame);parameters.classifyRays=uint32_t(config.probeClassifyRays);
            parameters.raysPerUpdate=uint32_t(config.probeRaysPerUpdate);parameters.maxHistoryUpdates=uint32_t(config.probeMaxHistoryUpdates);
            parameters.backfaceThreshold=config.probeBackfaceThreshold;parameters.rayMinDistance=source.rayMinDistance;parameters.rayMaxDistance=source.maxRayDistance;
            shared.Initialize(3);
            report["pipeline_initialization_wall_ms"]=Time([&]{Require(probes.Initialize(device,3),"initialize production probe pipelines");
                Require(filter.Initialize(device,3),"initialize production filter pipelines");});
            Require(!world.GetOccupiedCenters().empty(),"configured scene must allocate probes");
            std::cout<<"Actual scene: "<<stats.triangles<<" triangles, "<<stats.cells<<" probes; "<<report["adapter"]<<"\n";
        }
        ~Benchmark(){for(int slot=0;slot<3;++slot)if(device.GetFence(slot))device.WaitForPreviousFrame(slot);}
        void Run()
        {
            const uint32_t rounds=uint32_t((world.GetOccupiedCenters().size()+parameters.probesPerFrame-1)/parameters.probesPerFrame);
            // Filter timestamps are read at the next reuse of the same slot.
            // Three additional batches retire the second full round's samples.
            for(uint32_t frame=0;frame<rounds*2+3;++frame)
            {
                Submit(frame<rounds?"first_classification_round":frame<rounds*2?"classified_stable_round":"timestamp_drain");
                if(frame%32==0 || frame+1==rounds || frame+1==rounds*2)std::cout<<"Completed background batch "<<frame+1<<" / "<<rounds*2+3<<"\n";
            }
            report["classification_batches"]=rounds;report["stable_batches"]=rounds;report["timestamp_drain_batches"]=3;
            report["pending_filter_samples"]=3;
            report["filter_phase_contract"]="First classification round discovers selected neighbor edges. Only its first reset_mode=1 sample globally clears derived records/masks. Later batches can reuse edges discovered by reciprocal neighbors. Classified stable round revisits the complete allocation once with cached geometry edges and retains the final partial batch. Three drain submissions retire every stable filter timestamp; their own final three filter timestamps remain pending and are omitted.";
            ReadClassifiedRecords();
            MeasureSupport();
            MeasureMemory();
            std::map<std::string,std::vector<double>> groups;
            for(const auto& sample:report["batch_samples"])
            {
                const std::string phase=sample["phase"].get<std::string>();
                groups[phase+".probe"].push_back(sample["probe_gpu_ms"].get<double>());
                if(sample.contains("filter_gpu_ms"))groups[phase+".filter"].push_back(sample["filter_gpu_ms"].get<double>());
            }
            for(const auto& [name,values]:groups)report["gpu_distributions"][name]=Distribution(values);
            report["window_visible"]=IsWindowVisible(window.handle)!=FALSE;report["present_calls"]=0;
            report["debug_layer"]=false;report["gpu_based_validation"]=false;
            report["timing_scope"]="Production probe: full history copy through batch reset/classify/capture, excluding snapshot. Production filter: optional reset and selected-edge visibility through selected filter batch. CPU submissions wait individually; no full renderer frame, raster GBuffer, UI, or SSGI is measured.";
            report["environment_limitations"]="Finite background samples without locked GPU clocks or power state. The user can work concurrently. Results describe this run and cannot establish full-frame timing or a stable benchmark across machines.";
            report["completed"]=true;
            std::ofstream output(ReportStem()+"-report.json");Require(bool(output),"open benchmark report");output<<std::setw(2)<<report<<'\n';
        }
        Json report=Json::object();
    private:
        void Reset(int slot)
        {
            device.WaitForPreviousFrame(slot);
            Check(device.GetCommandAllocator(slot)->Reset(),"reset benchmark allocator");
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset benchmark commands");
            Require(!IsWindowVisible(window.handle),"benchmark HWND became visible");
        }
        uint64_t Execute(int slot)
        {
            Check(device.GetCommandList()->Close(),"close background benchmark commands");
            ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);
            const auto value=++device.GetFenceValue(slot);Check(device.GetCommandQueue()->Signal(device.GetFence(slot),value),"signal background benchmark fence");
            return value;
        }
        void Wait(int slot)
        {
            Check(device.GetFence(slot)->SetEventOnCompletion(device.GetFenceValue(slot),device.GetFenceEvent()),"arm background benchmark fence");
            Require(WaitForSingleObject(device.GetFenceEvent(),60000)==WAIT_OBJECT_0,"background benchmark GPU fence timeout");
            Check(device.GetDevice()->GetDeviceRemovedReason(),"background benchmark device status");
        }
        void Submit(const char* phase)
        {
            const int slot=int(submissions%3);Reset(slot);
            const VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),sdf.GetGeometryRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};
            Require(state.Prepare(sources,parameters),state.GetLastError());const auto plan=state.GetPendingPlan();
            Require(shared.Prepare(device,slot,world,sdf,&topology),"prepare actual scene geometry resources");
            Require(probes.Record(device,resources.GetMainDescriptorHeap(),slot,shared,sdf,surface,resources.GetMaterialBufferGPUAddress(),state),"record actual production probe graph");
            raw=probes.GetDisplayHistory();Require(raw!=nullptr,"production raw history missing");
            rawHistories.insert(raw.Get());
            Require(filter.Record(device,resources.GetMainDescriptorHeap(),slot,shared,sdf,raw.Get(),state,world.GetVoxelSize(),settings.vxgi.probeFilterNeighborWeight,surface,resources.GetMaterialBuffer()),"record actual production filter graph");
            if(previousFilter[slot]>=0)report["batch_samples"][size_t(previousFilter[slot])]["filter_gpu_ms"]=filter.GetGPUMilliseconds();
            const auto fence=Execute(slot);
            Require(probes.OnSubmitted(slot,fence,state),state.GetLastError());Require(filter.OnSubmitted(slot,fence),"commit actual filter submission");
            Wait(slot);Require(probes.Poll(device,slot,state),"poll actual probe timestamp and snapshot");
            const double ms=probes.GetGPUMilliseconds();Require(std::isfinite(ms)&&ms>=0,"invalid production probe timestamp");
            Json sample={{"frame",plan.frameIndex},{"slot",slot},{"start",plan.start},{"count",plan.count},
                {"reset_mode",plan.resetMode},{"phase",phase},{"probe_gpu_ms",ms},
                {"filter_previous_timestamp_slot",slot},{"filter_timestamp_scope",plan.resetMode==1?"reset_and_new_edges":"selected_edges_and_filter"}};
            report["batch_samples"].push_back(sample);previousFilter[slot]=int(submissions);++submissions;
        }
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);
            ComPtr<ID3D12Resource> result;Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&result)),"create benchmark buffer");return result;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {
            const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);device.GetCommandList()->ResourceBarrier(1,&barrier);
        }
        template<class T> std::vector<T> Read(ID3D12Resource* buffer,size_t count)
        {
            std::vector<T> result(count);void* mapped=nullptr;const D3D12_RANGE reads{0,count*sizeof(T)};
            Check(buffer->Map(0,&reads,&mapped),"map benchmark readback");std::memcpy(result.data(),mapped,reads.End);
            const D3D12_RANGE writes{0,0};buffer->Unmap(0,&writes);return result;
        }
        void ReadClassifiedRecords()
        {
            Reset(0);auto destination=Buffer(raw->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(raw.Get(),VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);device.GetCommandList()->CopyResource(destination.Get(),raw.Get());
            Transition(raw.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);Execute(0);Wait(0);
            const auto values=Read<Record>(destination.Get(),world.GetOccupiedCenters().size());
            uint64_t valid=0,disabled=0,unclassified=0,updated=0,negativeSHComponents=0;std::array<uint64_t,5> reasons={};
            std::map<uint32_t,uint64_t> updateCounts;
            for(const auto& value:values)
            {
                if(value.state==VXGIProbeState::Valid)++valid;else if(value.state==VXGIProbeState::Disabled)++disabled;else ++unclassified;
                if(value.updateCount)++updated;Require(value.reason<reasons.size(),"invalid classified probe reason");++reasons[value.reason];
                Require(std::isfinite(value.backfaceRatio)&&std::isfinite(value.minHitDistance),"nonfinite real-scene classification statistic");
                for(const auto& coefficient:value.sh)
                {
                    const std::array<float,4> components={coefficient.x,coefficient.y,coefficient.z,coefficient.w};
                    for(const auto component:components){Require(std::isfinite(component),"nonfinite real-scene SH coefficient");if(component<0)++negativeSHComponents;}
                }
                ++updateCounts[value.updateCount];
            }
            Require(unclassified==0,"full configured allocation did not complete classification");
            report["classification"]={{"valid",valid},{"disabled",disabled},{"unclassified",unclassified},{"probes_with_capture",updated},
                {"reason_counts_none_backface_origin_trace_sample",reasons},{"valid_fraction",double(valid)/values.size()},
                {"all_classification_and_sh_components_finite",true},{"negative_signed_sh_components",negativeSHComponents}};
            for(const auto& [count,probesWithCount]:updateCounts)report["classification"]["successful_capture_update_count_histogram"].push_back({{"update_count",count},{"probes",probesWithCount}});
        }
        void MeasureSupport()
        {
            std::map<uint32_t,std::vector<uint32_t>> triangles;
            for(uint32_t index=0;index<sdf.GetTriangles().size();++index)triangles[sdf.GetTriangles()[index].instanceId].push_back(index);
            std::vector<QueryInput> inputs;std::vector<std::pair<uint32_t,uint32_t>> groups;
            for(const auto& [instance,indices]:triangles)
            {
                std::vector<double> cumulative;double area=0;
                for(const auto index:indices)
                {
                    const auto& tri=sdf.GetTriangles()[index];area+=.5*Length(Cross(Sub(tri.positions[1],tri.positions[0]),Sub(tri.positions[2],tri.positions[0])));cumulative.push_back(area);
                }
                for(uint32_t sample=0;sample<256;++sample)
                {
                    const double target=area*(double(sample)+.5)/256;
                    const size_t ordinal=size_t(std::lower_bound(cumulative.begin(),cumulative.end(),target)-cumulative.begin());
                    const uint32_t index=indices[std::min(ordinal,indices.size()-1)];const auto& tri=sdf.GetTriangles()[index];
                    for(uint32_t side=0;side<2;++side)
                    {
                        QueryInput input{};input.position=Scale(Add(Add(tri.positions[0],tri.positions[1]),tri.positions[2]),1.f/3);
                        input.triangleIndex=index;input.triangleId=index;input.geometricNormal=tri.geometricNormal;input.backFace=side;
                        input.mappedNormal=Scale(tri.geometricNormal,side?-1.f:1.f);inputs.push_back(input);groups.emplace_back(instance,side);
                    }
                }
            }
            CD3DX12_ROOT_PARAMETER roots[14];roots[0].InitAsConstants(20,0);roots[1].InitAsConstants(4,1);
            for(UINT index=0;index<11;++index)roots[index+2].InitAsShaderResourceView(index);
            roots[13].InitAsUnorderedAccessView(2);
            const CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            const CD3DX12_ROOT_SIGNATURE_DESC desc(14,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> blob;Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize support query root");
            ComPtr<ID3D12RootSignature> root;Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"create support query root");
            const auto shader=ShaderCompiler::CompileFromFile(L"Shaders/VXGIStage74SupportValidate.hlsl",L"CSQuery",L"cs_6_6");Require(shader!=nullptr,"compile production helper real-scene support diagnostics");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};pipelineDesc.pRootSignature=root.Get();pipelineDesc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            ComPtr<ID3D12PipelineState> pipeline;Check(device.GetDevice()->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)),"create real scene support query pipeline");
            Reset(0);auto input=Buffer(inputs.size()*sizeof(QueryInput),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;Check(input->Map(0,nullptr,&mapped),"map geometry support input");std::memcpy(mapped,inputs.data(),inputs.size()*sizeof(QueryInput));input->Unmap(0,nullptr);
            auto output=Buffer(inputs.size()*sizeof(SupportResult),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback=Buffer(inputs.size()*sizeof(SupportResult),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            QueryParameters query;const auto& grid=sdf.GetGrid();query.origin=grid.origin;query.cellSize=grid.cellSize;query.cellDimensions=grid.cellDimensions;
            query.probeCount=uint32_t(world.GetOccupiedCenters().size());query.boundsMaximum=grid.boundsMaximum;query.triangleCount=uint32_t(sdf.GetTriangles().size());query.voxelSize=world.GetVoxelSize();
            query.materialCount=uint32_t(surface.GetMaterials().size());query.materialMipLevel=surface.GetParameters().materialMipLevel;
            auto* cmd=device.GetCommandList();ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};
            cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());
            cmd->SetComputeRoot32BitConstants(0,20,&query,0);const uint32_t constants[4]={uint32_t(inputs.size()),0,0,0};cmd->SetComputeRoot32BitConstants(1,4,constants,0);
            for(UINT index=0;index<7;++index)cmd->SetComputeRootShaderResourceView(index+2,shared.Get(0,index)->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(9,raw->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(10,filter.GetVisibilityMasks()->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(11,resources.GetMaterialBufferGPUAddress());
            cmd->SetComputeRootShaderResourceView(12,input->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(13,output->GetGPUVirtualAddress());
            cmd->Dispatch((uint32_t(inputs.size())+63)/64,1,1);Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(readback.Get(),output.Get());Execute(0);Wait(0);const auto results=Read<SupportResult>(readback.Get(),inputs.size());
            struct SupportCounts
            {
                std::array<uint64_t,8> status={};uint64_t supported=0,traceFailures=0,accepted=0,positive=0,missing=0,disabled=0,
                    unclassified=0,neverCaptured=0,nonfinite=0,eligible=0,hemisphereRejected=0,visibilityRejected=0,visibilityFailures=0,
                    noAllocated=0,noEligible=0,allVisibilityRejected=0,otherUnsupported=0,negativeQueries=0,zeroSupported=0;
            };
            std::map<std::pair<uint32_t,uint32_t>,SupportCounts> counts;
            std::map<std::pair<uint32_t,uint32_t>,std::set<uint32_t>> distinctTriangles;
            std::map<std::pair<uint32_t,uint32_t>,std::set<uint32_t>> verifiedTriangles;
            for(size_t index=0;index<results.size();++index)
            {
                const auto& diagnostic=results[index];const auto& value=diagnostic.query;auto& group=counts[groups[index]];
                Require(diagnostic.status>0 && diagnostic.status<group.status.size(),"invalid real-scene support status");++group.status[diagnostic.status];
                distinctTriangles[groups[index]].insert(inputs[index].triangleIndex);
                Require(value.hasSupport<=1 && value.acceptedCount<=8 && value.candidateCount<=8 &&
                    value.traceFailures<=8 &&
                    std::isfinite(value.supportWeight) && std::isfinite(value.irradiance.x) &&
                    std::isfinite(value.irradiance.y) && std::isfinite(value.irradiance.z),"invalid actual GPU surface support query result");
                if(diagnostic.status!=1)continue;
                verifiedTriangles[groups[index]].insert(inputs[index].triangleIndex);
                Require(diagnostic.positiveCorners<=8 && diagnostic.missingCorners<=diagnostic.positiveCorners &&
                    value.candidateCount==diagnostic.positiveCorners-diagnostic.missingCorners &&
                    diagnostic.eligibleCorners+diagnostic.disabledCorners+diagnostic.unclassifiedCorners+diagnostic.neverCapturedCorners+diagnostic.nonfiniteCorners==value.candidateCount &&
                    diagnostic.acceptedCorners==value.acceptedCount && diagnostic.visibilityFailures==value.traceFailures &&
                    diagnostic.acceptedCorners+diagnostic.hemisphereRejectedCorners+diagnostic.visibilityRejectedCorners+diagnostic.visibilityFailures==diagnostic.eligibleCorners,
                    "support diagnostic partition differs from production RawQuery");
                group.supported+=value.hasSupport?1:0;group.traceFailures+=value.traceFailures;group.accepted+=value.acceptedCount;
                group.positive+=diagnostic.positiveCorners;group.missing+=diagnostic.missingCorners;group.disabled+=diagnostic.disabledCorners;
                group.unclassified+=diagnostic.unclassifiedCorners;group.neverCaptured+=diagnostic.neverCapturedCorners;group.nonfinite+=diagnostic.nonfiniteCorners;
                group.eligible+=diagnostic.eligibleCorners;group.hemisphereRejected+=diagnostic.hemisphereRejectedCorners;
                group.visibilityRejected+=diagnostic.visibilityRejectedCorners;group.visibilityFailures+=diagnostic.visibilityFailures;
                if(value.negativeMask)++group.negativeQueries;
                if(value.hasSupport && value.irradiance.x==0 && value.irradiance.y==0 && value.irradiance.z==0)++group.zeroSupported;
                if(!value.hasSupport)
                {
                    if(value.candidateCount==0)++group.noAllocated;
                    else if(diagnostic.eligibleCorners==0)++group.noEligible;
                    else if(diagnostic.acceptedCorners==0)++group.allVisibilityRejected;
                    else ++group.otherUnsupported;
                }
                report["surface_support"]["verified_query_samples"].push_back({{"instance_id",groups[index].first},{"back_face",groups[index].second},
                    {"triangle_index",inputs[index].triangleIndex},{"position",{inputs[index].position.x,inputs[index].position.y,inputs[index].position.z}},
                    {"has_support",value.hasSupport},{"support_weight",value.supportWeight},{"candidate_count",value.candidateCount},
                    {"eligible_count",diagnostic.eligibleCorners},{"accepted_count",value.acceptedCount},{"trace_failures",value.traceFailures},
                    {"irradiance_over_pi",{value.irradiance.x,value.irradiance.y,value.irradiance.z}},{"negative_mask",value.negativeMask}});
            }
            report["surface_support"]["sample_contract"]="Per instance 256 area-stratified requested triangle centroids, both geometric hemispheres. Actual candidate coverage is checked at centroid UV. Covered targets require a material-aware primary ray from max(voxelSize*0.5,sdfCellSize*0.25) along the chosen geometric hemisphere to identify that exact triangle/endpoint before production RawQuery, which uses actual normal-map shading. Only verified covered targets enter support fractions. Holes, different first surfaces, misses and failures are reported separately. Centroids may repeat, so this is not an unbiased surface-area estimate, camera/raster coverage, or representative interior visibility survey. Alpha uses the configured fixed mip and can differ from raster derivative footprints.";
            report["surface_support"]["primary_origin_distance_world_units"]=std::max(world.GetVoxelSize()*.5f,sdf.GetGrid().cellSize*.25f);
            for(const auto& [key,count]:counts)report["surface_support"]["groups"].push_back({{"instance_id",key.first},{"back_face",key.second},
                {"requested_samples",std::accumulate(count.status.begin(),count.status.end(),uint64_t(0))},{"distinct_requested_triangle_centroids",distinctTriangles[key].size()},
                {"verified_covered_primary_samples",count.status[1]},{"distinct_verified_triangle_centroids",verifiedTriangles[key].size()},
                {"hole_samples",count.status[2]},{"invalid_coverage_samples",count.status[3]},{"primary_miss_samples",count.status[4]},
                {"different_primary_triangle_or_endpoint_samples",count.status[5]},{"primary_trace_failure_samples",count.status[6]},{"invalid_shading_material_samples",count.status[7]},
                {"supported",count.supported},{"support_fraction_among_verified_covered",count.status[1]?Json(double(count.supported)/count.status[1]):Json(nullptr)},
                {"contributor_counts",{{"positive_weight_lattice_corners",count.positive},{"missing_allocated_corners",count.missing},{"disabled_corners",count.disabled},
                    {"unclassified_corners",count.unclassified},{"never_captured_corners",count.neverCaptured},{"nonfinite_record_corners",count.nonfinite},
                    {"eligible_record_corners",count.eligible},{"geometric_hemisphere_rejections",count.hemisphereRejected},
                    {"visibility_rejections",count.visibilityRejected},{"visibility_trace_failures",count.visibilityFailures},{"accepted",count.accepted}}},
                {"unsupported_reason_counts",{{"no_allocated_candidates",count.noAllocated},{"all_allocated_candidates_ineligible",count.noEligible},
                    {"all_eligible_candidates_rejected_by_hemisphere_or_visibility",count.allVisibilityRejected},{"other",count.otherUnsupported}}},
                {"query_trace_failures",count.traceFailures},{"queries_with_negative_reconstruction_component",count.negativeQueries},{"supported_black_queries",count.zeroSupported}});
        }
        void MeasureMemory()
        {
            uint64_t payload=0,allocation=0;
            auto record=[&](ID3D12Resource* resource,const char* kind,int slot)
            {
                Require(resource!=nullptr,"allocation measurement resource missing");const auto desc=resource->GetDesc();
                const auto info=device.GetDevice()->GetResourceAllocationInfo(0,1,&desc);Require(info.SizeInBytes!=UINT64_MAX,"allocation query failed");
                payload+=desc.Width;allocation+=info.SizeInBytes;
                report["gpu_memory"]["public_default_resources"].push_back({{"kind",kind},{"slot",slot},{"payload_bytes",desc.Width},{"allocation_bytes",info.SizeInBytes},{"alignment",info.Alignment}});
            };
            for(int slot=0;slot<3;++slot)for(UINT index=0;index<7;++index)record(shared.Get(slot,index),"shared_scene",slot);
            Require(rawHistories.size()==2,"both actual production history allocations were not observed");
            for(auto* history:rawHistories)record(history,"raw_history",-1);
            record(filter.GetFilteredRecords(),"filtered_history",-1);record(filter.GetVisibilityMasks(),"visibility_edges",-1);
            report["gpu_memory"]["public_default_payload_bytes"]=payload;report["gpu_memory"]["public_default_committed_allocation_bytes"]=allocation;
            report["gpu_memory"]["reported_shared_payload_including_live_staging_bytes"]=shared.GetGPUMemoryBytes();
            report["gpu_memory"]["reported_probe_payload_including_live_snapshots_and_timestamp_bytes"]=probes.GetGPUMemoryBytes();
            report["gpu_memory"]["reported_filter_payload_including_timestamp_bytes"]=filter.GetGPUMemoryBytes();
            report["gpu_memory"]["coverage"]="GetResourceAllocationInfo was called on all 21 actual shared DEFAULT buffers, both actual raw DEFAULT histories, filtered DEFAULT and edges DEFAULT. Allocation includes resource alignment. Private upload staging/readback snapshots/timestamps are reported as payload only; query heap, assets, material pool, RDG pools, swap-chain/depth and benchmark query resources are excluded.";
        }
        HiddenWindow window;RenderDevice device;ResourceManager resources;SettingsManager settings;
        VXGIWorld world;VXGISDF sdf;VXGIProbeTopology topology;VXGISurface surface;VXGIGPUScene shared;
        VXGIProbeState state;VXGIProbeState::Parameters parameters;VXGIProbePass probes;VXGIProbeFilterPass filter;
        ComPtr<ID3D12Resource> raw;std::set<ID3D12Resource*> rawHistories;
        std::array<int,3> previousFilter={-1,-1,-1};uint32_t submissions=0;
    };
}
int main(int argc,char** argv)
{
    try{
        for(int index=1;index<argc;++index){
            Require(std::string(argv[index])=="--label" && index+1<argc,"expected --label value");
            Label=argv[++index];
            Require(!Label.empty() && Label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos,"invalid benchmark label");
        }
        Benchmark benchmark;benchmark.Run();std::cout<<"PASS finite post-alpha real-scene background acceptance; report: "<<ReportStem()<<"-report.json\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAIL stage 7 benchmark: "<<error.what()<<'\n';return 1;}
}
