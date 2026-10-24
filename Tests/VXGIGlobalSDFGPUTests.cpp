// Standalone validation: no application startup, window, scene or settings edits.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "Resources/VXGIGlobalSDF.h"
#include "Resources/VXGIGlobalSDFState.h"
#include "Resources/VXGIMeshSDFThreshold.h"
#include "Passes/VXGIGlobalSDFPass.h"
#include "ThirdParty/json.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT3;
using Json = nlohmann::json;
using Global = VXGIGlobalSDF;
using GPUData = VXGIMeshSDFGPUData;

static void Require(bool condition, const std::string& message)
{ if (!condition) throw std::runtime_error(message); }
static void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
    {
        char code[32]; std::snprintf(code,sizeof(code),"0x%08X",unsigned(hr));
        throw std::runtime_error(std::string(operation)+" failed: "+code);
    }
}
static std::vector<uint8_t> ReadBytes(const char* path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Require(bool(file),std::string("Cannot read ")+path);
    const auto size=file.tellg(); Require(size>0,"Shader file is empty");
    std::vector<uint8_t> result(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(result.data()),size);
    Require(bool(file),"Cannot read full shader bytecode"); return result;
}
static D3D12_RESOURCE_DESC BufferDescription(uint64_t bytes,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC description{}; description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width=(std::max)(uint64_t(16),bytes); description.Height=1;
    description.DepthOrArraySize=1; description.MipLevels=1;
    description.SampleDesc.Count=1; description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags=flags; return description;
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource; b.Transition.StateBefore=before;
    b.Transition.StateAfter=after; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

class Device
{
public:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12InfoQueue> info;
    std::string adapterName;
    bool gpuValidation=false;
    explicit Device(bool gbv)
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer");
        debug->EnableDebugLayer();
        if(gbv)
        {
            ComPtr<ID3D12Debug1> debug1; Check(debug.As(&debug1),"GPU validation interface");
            debug1->SetEnableGPUBasedValidation(TRUE); gpuValidation=true;
        }
        ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT index=0;;++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const auto status=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
            if(status==DXGI_ERROR_NOT_FOUND) break;
            Check(status,"Hardware adapter enumeration");
            DXGI_ADAPTER_DESC1 description{}; Check(adapter->GetDesc1(&description),"Adapter description");
            if(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
            {
                for(const auto character:description.Description) { if(!character) break; adapterName.push_back(character<128?char(character):'?'); }
                break;
            }
        }
        Require(bool(device),"No D3D12 hardware adapter; WARP is intentionally not used for this GPU test");
        Check(device.As(&info),"D3D12 information queue");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Command queue");
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Command allocator");
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Command list");
        Check(list->Close(),"Initial command list close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"Fence event creation failed");
    }
    ~Device(){ if(event) CloseHandle(event); }
    void Begin(){ Check(allocator->Reset(),"Allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"Command list reset"); }
    void Abandon(){ Check(list->Close(),"Abandoned command list close"); }
    void Submit()
    {
        Check(list->Close(),"Command list close"); ID3D12CommandList* lists[]={list.Get()};
        queue->ExecuteCommandLists(1,lists); Check(queue->Signal(fence.Get(),++fenceValue),"Queue fence signal");
        if(fence->GetCompletedValue()<fenceValue)
        {
            Check(fence->SetEventOnCompletion(fenceValue,event),"Fence event");
            Require(WaitForSingleObject(event,60000)==WAIT_OBJECT_0,"GPU timeout");
        }
        Check(device->GetDeviceRemovedReason(),"Device removal check");
    }
    ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_HEAP_PROPERTIES properties{}; properties.Type=heap;
        const auto description=BufferDescription(bytes,flags); ComPtr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,state,nullptr,IID_PPV_ARGS(&resource)),"Buffer allocation");
        return resource;
    }
    template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
    {
        const uint64_t bytes=uint64_t(values.size())*sizeof(T);
        auto resource=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* address=nullptr; const D3D12_RANGE empty{};
        Check(resource->Map(0,&empty,&address),"Upload buffer map");
        if(bytes) std::memcpy(address,values.data(),size_t(bytes)); else std::memset(address,0,16);
        resource->Unmap(0,nullptr); return resource;
    }
    std::vector<float> ReadTexture(ID3D12Resource* texture,const Global::Layout& layout)
    {
        const auto description=texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows=0; UINT64 rowBytes=0,totalBytes=0;
        device->GetCopyableFootprints(&description,0,1,0,&footprint,&rows,&rowBytes,&totalBytes);
        Require(rows==layout.height && rowBytes==uint64_t(layout.width)*sizeof(float),"Unexpected texture readback footprint");
        auto readback=Buffer(totalBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Begin(); auto barrier=Transition(texture,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=texture; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource=readback.Get(); target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint=footprint;
        list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        barrier=Transition(texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1,&barrier); Submit();
        void* address=nullptr; const D3D12_RANGE readRange={0,size_t(totalBytes)};
        Check(readback->Map(0,&readRange,&address),"Texture readback map");
        std::vector<float> result(size_t(layout.totalNodeCount));
        for(UINT z=0;z<layout.depth;++z) for(UINT y=0;y<layout.height;++y)
            std::memcpy(result.data()+size_t(layout.width)*(y+size_t(layout.height)*z),
                static_cast<const uint8_t*>(address)+footprint.Offset+size_t(footprint.Footprint.RowPitch)*(y+size_t(rows)*z),size_t(rowBytes));
        const D3D12_RANGE empty{};readback->Unmap(0,&empty); return result;
    }
    Json Diagnostics()
    {
        Json messages=Json::array(); uint32_t warnings=0,errors=0;
        for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
        {
            SIZE_T bytes=0; Check(info->GetMessage(index,nullptr,&bytes),"Debug message size");
            std::vector<uint8_t> storage(bytes); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(index,message,&bytes),"Debug message");
            if(message->Severity==D3D12_MESSAGE_SEVERITY_WARNING) ++warnings;
            else if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) ++errors;
            else continue;
            messages.push_back({{"id",uint32_t(message->ID)},{"severity",uint32_t(message->Severity)},{"description",message->pDescription}});
        }
        return {{"debug_layer",true},{"gpu_based_validation",gpuValidation},{"warnings",warnings},{"errors",errors},{"messages",messages}};
    }
private:
    ComPtr<ID3D12Fence> fence; HANDLE event=nullptr; uint64_t fenceValue=0;
};

struct Vertex{XMFLOAT3 Position;};
struct Mesh{std::vector<Vertex> vertices;std::vector<std::vector<uint32_t>> lodIndices={{}};uint32_t materialID=0;};
struct Model{std::vector<Mesh> meshes;};
struct Instance
{
    uint32_t editorId=0;Model* pModel=nullptr;
    XMFLOAT3 translation={},rotation={},scale={1,1,1};
    uint32_t customMaterialID=UINT32_MAX;bool isTransparent=false,isCutout=false;
};
static void Quad(Mesh& mesh,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d)
{
    const uint32_t first=uint32_t(mesh.vertices.size());mesh.vertices.insert(mesh.vertices.end(),{{a},{b},{c},{d}});
    mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
}
static Model Plane()
{Mesh mesh;Quad(mesh,{-3,0,-3},{3,0,-3},{3,0,3},{-3,0,3});return {{std::move(mesh)}};}
static Model SolidXY()
{Mesh mesh;Quad(mesh,{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0});return {{std::move(mesh)}};}
static Model Hollow()
{
    Mesh mesh;Quad(mesh,{-6,-6,0},{-4,-6,0},{-4,6,0},{-6,6,0});
    Quad(mesh,{4,-6,0},{6,-6,0},{6,6,0},{4,6,0});return {{std::move(mesh)}};
}
static Model Box()
{
    Mesh mesh;
    Quad(mesh,{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1});
    Quad(mesh,{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1});
    Quad(mesh,{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1});
    Quad(mesh,{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1});
    Quad(mesh,{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1});
    Quad(mesh,{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1});return {{std::move(mesh)}};
}
static Instance Make(Model& model,uint32_t id,XMFLOAT3 translation={},XMFLOAT3 scale={1,1,1},XMFLOAT3 rotation={})
{Instance value;value.editorId=id;value.pModel=&model;value.translation=translation;value.scale=scale;value.rotation=rotation;return value;}
struct Fixture
{
    VXGIMeshSDFCache cache; VXGIMeshSDFScene scene;
    GPUData::Geometry geometry; GPUData::Instances instances;
    void Build(const std::vector<Instance>& sources,float localCell=0.25f)
    {
        Require(scene.UpdateInstances(sources,cache,{localCell,0}),scene.GetLastError());
        const auto assets=GPUData::CollectAssets(scene);
        geometry=GPUData::BuildAssets(assets);instances=GPUData::BuildInstances(scene,assets);
        std::string error;Require(Global::ValidateSources(geometry,instances,&error),error);
    }
};
struct Query
{
    XMFLOAT3 origin;float minimum;XMFLOAT3 direction;float maximum;
    uint32_t maximumSteps=256,mode=1,pad0=0,pad1=0;
};
struct Result
{
    XMFLOAT3 position;uint32_t status;
    float distance,fieldDistance,cellSize;uint32_t clipmapIndex;
    uint32_t sampleCount,valid;float safeDistance;uint32_t padding;
};
static_assert(sizeof(Query)==48 && sizeof(Result)==48);
static Query Ray(XMFLOAT3 origin,XMFLOAT3 direction,float maximum,float minimum=0,uint32_t steps=256)
{return {origin,minimum,direction,maximum,steps,1};}
static Query Sample(XMFLOAT3 position){Query result{};result.origin=position;result.mode=0;return result;}

class QueryPipeline
{
public:
    QueryPipeline(Device& gpu,const std::vector<uint8_t>& bytecode):gpu(gpu)
    {
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,UINT(sizeof(Global::Constants)/sizeof(uint32_t))};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor={1,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor={0,0};
        parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[4].Constants={1,0,1};
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=5;root.pParameters=parameters;
        ComPtr<ID3DBlob> serialized,error;Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"Query root signature serialization");
        Check(gpu.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature)),"Query root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};description.pRootSignature=signature.Get();description.CS={bytecode.data(),bytecode.size()};
        Check(gpu.device->CreateComputePipelineState(&description,IID_PPV_ARGS(&pipeline)),"Query pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};heapDescription.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.NumDescriptors=1;heapDescription.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(gpu.device->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&heap)),"Query descriptor heap");
    }
    std::vector<Result> Run(VXGIGlobalSDFPass& pass,const Global::Layout& layout,const std::vector<Query>& queries)
    {
        Require(!queries.empty(),"Query input is empty");
        const uint64_t bytes=queries.size()*sizeof(Result);
        auto inputs=gpu.Upload(queries);
        auto outputs=gpu.Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.device->CopyDescriptorsSimple(1,heap->GetCPUDescriptorHandleForHeapStart(),pass.GetSRVCPUHandle(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        gpu.Begin();auto barrier=Transition(outputs.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.list->ResourceBarrier(1,&barrier);
        ID3D12DescriptorHeap* heaps[]={heap.Get()};gpu.list->SetDescriptorHeaps(1,heaps);
        gpu.list->SetComputeRootSignature(signature.Get());gpu.list->SetPipelineState(pipeline.Get());
        gpu.list->SetComputeRoot32BitConstants(0,UINT(sizeof(Global::Constants)/sizeof(uint32_t)),&layout.constants,0);
        gpu.list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
        gpu.list->SetComputeRootShaderResourceView(2,inputs->GetGPUVirtualAddress());
        gpu.list->SetComputeRootUnorderedAccessView(3,outputs->GetGPUVirtualAddress());
        gpu.list->SetComputeRoot32BitConstant(4,UINT(queries.size()),0);gpu.list->Dispatch(UINT((queries.size()+63)/64),1,1);
        barrier=Transition(outputs.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);gpu.list->ResourceBarrier(1,&barrier);
        gpu.list->CopyBufferRegion(readback.Get(),0,outputs.Get(),0,bytes);gpu.Submit();
        std::vector<Result> result(queries.size());void* address=nullptr;const D3D12_RANGE range={0,size_t(bytes)};
        Check(readback->Map(0,&range,&address),"Query readback map");std::memcpy(result.data(),address,size_t(bytes));
        const D3D12_RANGE empty{};readback->Unmap(0,&empty);return result;
    }
private:
    Device& gpu;ComPtr<ID3D12RootSignature> signature;ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12DescriptorHeap> heap;
};

static Json CompareNodes(const Fixture& fixture,const Global::Layout& layout,const std::vector<float>& values)
{
    Require(values.size()==layout.totalNodeCount,"Texture node count mismatch");
    float maximumAbsolute=0,maximumRelative=0;uint64_t mismatches=0,compared=0;Json first=Json::array();
    for(uint32_t layer=0;layer<layout.constants.clipmapCount;++layer)
    {
        const auto& clip=layout.constants.clipmaps[layer];
        const uint32_t n=Global::Resolution(layout.constants,layer);
        for(uint32_t z=0;z<n;++z)for(uint32_t y=0;y<n;++y)for(uint32_t x=0;x<n;++x)
        {
        const auto position=Global::NodePosition(clip,x,y,z);
        const float expected=Global::ComposeReferenceNode(fixture.geometry,fixture.instances,position,clip.distanceLimit);
        const size_t index=Global::PhysicalNodeIndex(layout,layer,x,y,z);
        const float actual=values[index],error=std::abs(actual-expected);
        maximumAbsolute=(std::max)(maximumAbsolute,error);maximumRelative=(std::max)(maximumRelative,error/(std::max)(1.0f,std::abs(expected)));
        if(!std::isfinite(actual)||actual<0||error>2e-4f*(std::max)(1.0f,std::abs(expected)))
        {++mismatches;if(first.size()<4)first.push_back({{"index",index},{"actual",actual},{"expected",expected},{"position",{position.x,position.y,position.z}}});}
        ++compared;
        }
    }
    Require(mismatches==0,"GPU composition disagrees with CPU reference: "+first.dump());
    return {{"compared_nodes",compared},{"allocated_nodes",values.size()},{"mismatches",mismatches},{"maximum_absolute_error",maximumAbsolute},{"maximum_relative_error",maximumRelative}};
}
static Json ResultJson(const Query& q,const Result& r,const VXGIMeshSDFScene& scene)
{
    Json result={{"mode",q.mode},{"origin",{q.origin.x,q.origin.y,q.origin.z}},
        {"direction",{q.direction.x,q.direction.y,q.direction.z}},{"range",{q.minimum,q.maximum}},
        {"status",r.status},{"distance",r.distance},{"position",{r.position.x,r.position.y,r.position.z}},
        {"field_distance",r.fieldDistance},{"clipmap",r.clipmapIndex},{"samples",r.sampleCount},
        {"sample_valid",r.valid},{"sample_cell",r.cellSize},{"safe_distance",r.safeDistance}};
    if(q.mode==1)
    {
        const auto exact=scene.TraceGeometry(q.origin,q.direction,q.minimum,q.maximum);
        result["exact_triangles"]={{"hit",exact.hit},{"distance",exact.distance},{"instance_id",exact.instanceId}};
    }
    return result;
}

// Each update is checked against an independently rebuilt texture with zero
// scrolling. Deferred layers are expected to retain their previous contents;
// they are compared byte-for-byte instead of pretending they contain new data.
static Json RunUpdates(Device& gpu,const std::vector<uint8_t>& compose,QueryPipeline& queries)
{
    VXGIGlobalSDFPass incremental,reference;
    Require(incremental.Initialize(gpu.device.Get(),compose.data(),compose.size()),incremental.GetLastError());
    Require(reference.Initialize(gpu.device.Get(),compose.data(),compose.size()),reference.GetLastError());
    VXGIGlobalSDFState state;Fixture fixture;Model box=Box(),plane=Plane(),solid=SolidXY();
    Global::Settings settings={16,3,0.5f};XMFLOAT3 center={};
    std::vector<Instance> sources={Make(box,1,{0,1,0}),Make(plane,2,{0,-1,0})};
    std::vector<float> previous;uint64_t frame=0,totalCompared=0,totalUnchanged=0,totalQueries=0,totalUpdated=0;
    Json cases=Json::array();
    auto run=[&](const std::string& name,bool mustIdle=false,bool mustRecreate=false,bool retry=false)
    {
        fixture.Build(sources);std::string error;
        auto prepare=[&](){return state.Prepare(center,settings,fixture.scene,frame,
            uint32_t(fixture.geometry.assets.size()),uint32_t(fixture.geometry.nodes.size()),&error);};
        Require(prepare(),name+": "+error);
        auto plan=state.GetPendingPlan();
        if(retry)
        {
            const auto first=plan;state.Cancel();Require(!state.HasPendingPlan(),"Cancelled plan must not remain pending");
            Require(prepare(),name+": retry "+error);plan=state.GetPendingPlan();
            Require(first.updatedNodes==plan.updatedNodes&&first.recreate==plan.recreate&&first.regions.size()==plan.regions.size()&&
                std::memcmp(&first.layout.constants,&plan.layout.constants,sizeof(Global::Constants))==0,
                "Unsubmitted cancellation must produce the same retry plan");
            for(size_t i=0;i<plan.regions.size();++i)
                Require(std::memcmp(&first.regions[i],&plan.regions[i],sizeof(Global::UpdateRegion))==0,"Cancelled update region changed");
        }
        Require(!mustIdle||plan.updatedNodes==0,name+": unchanged geometry must not schedule GPU work");
        Require(!mustRecreate||plan.recreate,name+": size change must request a new texture");
        auto layout=plan.layout,fullLayout=layout;
        for(uint32_t layer=0;layer<fullLayout.constants.clipmapCount;++layer)fullLayout.constants.clipmaps[layer].scrollOffset={};
        ComPtr<ID3D12Resource> retired;
        if(plan.recreate)
        {
            // All earlier work is already fenced by Device::Submit. Retain the
            // old allocation while descriptors and the texture are replaced.
            retired=incremental.GetTexture();
            Require(incremental.Create(gpu.device.Get(),layout),incremental.GetLastError());
            Require(reference.Create(gpu.device.Get(),fullLayout),reference.GetLastError());
            Require(!retired||retired.Get()!=incremental.GetTexture(),"Resize reused a live old texture object");
            previous.clear();
        }
        auto nodes=gpu.Upload(fixture.geometry.nodes),assets=gpu.Upload(fixture.geometry.assets),instances=gpu.Upload(fixture.instances.instances);
        const VXGIGlobalSDFPass::Inputs input={nodes.Get(),assets.Get(),instances.Get()};
        if(retry)
        {
            gpu.Begin();Require(incremental.RecordRegions(gpu.list.Get(),layout,input,plan.regions),incremental.GetLastError());
            gpu.Abandon();incremental.CancelRecording();state.Cancel();
            Require(prepare(),name+": recorded cancellation retry "+error);
            Require(std::memcmp(&plan.layout.constants,&state.GetPendingPlan().layout.constants,sizeof(Global::Constants))==0&&
                plan.updatedNodes==state.GetPendingPlan().updatedNodes,"Discarded command list changed the committed scheduling state");
        }
        gpu.Begin();Require(incremental.RecordRegions(gpu.list.Get(),layout,input,plan.regions),incremental.GetLastError());
        Require(reference.Record(gpu.list.Get(),fullLayout,input),reference.GetLastError());gpu.Submit();
        incremental.OnSubmitted();reference.OnSubmitted();
        Require(state.Commit(),name+": state commit failed after submission");Require(!state.HasPendingPlan(),"Committed plan must not remain pending");
        const auto values=gpu.ReadTexture(incremental.GetTexture(),layout),expected=gpu.ReadTexture(reference.GetTexture(),fullLayout);
        uint64_t compared=0,unchanged=0;float maximumError=0;
        for(uint32_t layer=0;layer<layout.constants.clipmapCount;++layer)
        {
            const auto& clip=layout.constants.clipmaps[layer];
            const uint32_t n=Global::Resolution(layout.constants,layer);
            for(uint32_t z=0;z<n;++z)for(uint32_t y=0;y<n;++y)for(uint32_t x=0;x<n;++x)
            {
                const size_t logical=x+size_t(layout.width)*(y+size_t(layout.height)*(z+clip.nodeOffsetZ));
                if(!plan.updated[layer])
                {
                    Require(previous.size()==values.size(),name+": deferred clipmap has no prior contents");
                    Require(std::memcmp(&values[logical],&previous[logical],sizeof(float))==0,name+": deferred clipmap was modified");++unchanged;continue;
                }
                const size_t physical=Global::PhysicalNodeIndex(layout,layer,x,y,z);
                const float delta=std::abs(values[physical]-expected[logical]);maximumError=(std::max)(maximumError,delta);
                Require(std::isfinite(values[physical])&&delta<=2e-4f*(std::max)(1.0f,std::abs(expected[logical])),
                    name+": incremental node differs from full GPU reconstruction at layer "+std::to_string(layer)+" logical "+std::to_string(x)+","+std::to_string(y)+","+std::to_string(z));
                const auto position=Global::NodePosition(clip,x,y,z);
                const float cpu=Global::ComposeReferenceNode(fixture.geometry,fixture.instances,position,clip.distanceLimit);
                Require(std::abs(values[physical]-cpu)<=2e-4f*(std::max)(1.0f,std::abs(cpu)),name+": incremental node differs from CPU oracle");++compared;
            }
        }
        std::vector<Query> sampleQueries;
        for(uint32_t layer=0;layer<layout.constants.clipmapCount;++layer)
        {
            const auto& clip=layout.constants.clipmaps[layer];
            const float n=float(Global::Resolution(layout.constants,layer)-1);
            for(float z:{0.0f,0.37f,0.999f,1.0f})for(float y:{0.0f,0.61f,0.999f,1.0f})for(float x:{0.0f,0.23f,0.999f,1.0f})
                sampleQueries.push_back(Sample({clip.origin.x+x*n*clip.cellSize,clip.origin.y+y*n*clip.cellSize,clip.origin.z+z*n*clip.cellSize}));
        }
        const auto observed=queries.Run(incremental,layout,sampleQueries),rebuilt=queries.Run(reference,fullLayout,sampleQueries);
        uint64_t checkedQueries=0;
        for(size_t i=0;i<observed.size();++i)
        {
            Require(observed[i].valid==rebuilt[i].valid&&observed[i].clipmapIndex==rebuilt[i].clipmapIndex,name+": scrolling sample selected a different clipmap");
            if(!observed[i].valid||!plan.updated[observed[i].clipmapIndex])continue;
            Require(std::abs(observed[i].distance-rebuilt[i].distance)<2e-4f&&std::abs(observed[i].safeDistance-rebuilt[i].safeDistance)<2e-4f,
                name+": scrolling interpolation differs from complete reconstruction");++checkedQueries;
        }
        cases.push_back({{"name",name},{"frame",frame},{"regions",plan.regions.size()},{"updated_nodes",plan.updatedNodes},
            {"full_node_count",layout.totalNodeCount},{"updated_layers",plan.updated},{"full_layers",plan.fullUpdate},{"recreated",plan.recreate},
            {"gpu_and_cpu_node_comparisons",compared},{"deferred_nodes_unchanged",unchanged},{"sample_comparisons",checkedQueries},{"maximum_absolute_error",maximumError},{"cancel_retry",retry}});
        totalCompared+=compared;totalUnchanged+=unchanged;totalQueries+=checkedQueries;totalUpdated+=plan.updatedNodes;previous=values;++frame;
        std::cout<<"PASS update "<<name<<" ("<<plan.updatedNodes<<" / "<<layout.totalNodeCount<<" nodes)\n";
        return plan;
    };
    run("initial",false,true,true);run("stationary",true);
    sources[0].customMaterialID=17;run("material_only",true);
    center.x=0.5f;const auto one=run("camera_positive_one_cell");
    Require(one.updatedNodes<one.layout.totalNodeCount,"One-cell scroll must reuse previous field contents");
    center.x=0;run("camera_negative_one_cell");center={-0.5f,0.5f,-0.5f};run("camera_diagonal",false,false,true);
    for(int i=0;i<38;++i){center.x+=0.5f;run("positive_wrap_"+std::to_string(i));}
    for(int i=0;i<38;++i){center.x-=0.5f;run("negative_wrap_"+std::to_string(i));}
    center={};run("return_to_origin");
    sources[0].translation={2.5f,2,-1};run("object_move");
    sources[0].translation={-2.5f,0,1};run("object_move_while_far_deferred");
    sources.erase(sources.begin());run("object_delete");
    sources.push_back(Make(box,3,{-1,1,1},{-1.5f,0.5f,2},{0.2f,0.4f,0.1f}));run("object_add_nonuniform_mirror");
    for(int i=0;i<4;++i)run("dirty_flush_"+std::to_string(i));
    sources.clear();run("all_objects_removed");for(int i=0;i<4;++i)run("empty_flush_"+std::to_string(i));
    sources={Make(solid,4,{2,1,-1},{1,2,1})};run("empty_to_populated");
    center={-23,-7,-11};run("camera_teleport");for(int i=0;i<4;++i)run("teleport_flush_"+std::to_string(i));
    settings.cellsPerAxis=24;run("resolution_resize",false,true);settings.clipmapCount=2;run("clipmap_count_resize",false,true);
    settings.baseCellSize=0.75f;run("cell_size_change");run("final_idle",true);
    settings={64,3,0.5f};center={};sources={Make(box,5,{0,0,0},{0.5f,0.5f,0.5f})};run("default_resolution_initial",false,true);
    while(frame%4!=1)run("default_resolution_cadence_align",true);
    sources[0].translation={0.5f,0,0};const auto smallMove=run("default_resolution_local_dirty");
    Require(smallMove.updatedNodes>0&&smallMove.updatedNodes<uint64_t(65)*65*65,
        "A local edit in the default field must update less than one full clipmap");
    sources[0].translation={-3,0.5f,1};run("default_resolution_accumulated_dirty_2");
    sources[0].translation={3,-0.5f,-1};run("default_resolution_accumulated_dirty_3");
    run("default_resolution_accumulated_dirty_flush");run("default_resolution_final_idle",true);
    settings={};settings.baseCellSize=.25f;settings.layerCellsPerAxis={4,8,8};center={};
    sources={Make(box,6,{0,0,0},{0.25f,0.25f,0.25f}),Make(plane,7,{0,-2.5f,0})};
    const auto compact=run("compact_1_4_8_initial",false,true,true);
    Require(compact.updatedNodes==1583&&compact.layout.width==9&&compact.layout.depth==23,
        "Compact layer layout must dispatch 125+729+729 active nodes in a 9x9x23 atlas");
    while(frame%4!=1)run("compact_cadence_align",true);
    center.x=.25f;const auto compactPlane=run("compact_positive_near_plane");
    Require(compactPlane.updatedNodes==25&&compactPlane.layout.constants.clipmaps[0].scrollOffset[0]==1,
        "Compact near scroll must update one 5x5 plane");
    center.x=-.25f;const auto compactNegative=run("compact_negative_distinct_moduli");
    Require(compactNegative.layout.constants.clipmaps[0].scrollOffset[0]==4&&
        compactNegative.layout.constants.clipmaps[1].scrollOffset[0]==8,
        "Compact near and middle layers must wrap independently modulo five and nine");
    for(int i=0;i<18;++i){center.x+=.25f;run("compact_positive_wrap_"+std::to_string(i));}
    for(int i=0;i<18;++i){center.x-=.25f;run("compact_negative_wrap_"+std::to_string(i));}
    sources[0].translation={.5f,.125f,-.5f};run("compact_object_move");
    sources[0].translation={-.5f,-.125f,.5f};run("compact_object_move_deferred");
    for(int i=0;i<4;++i)run("compact_dirty_flush_"+std::to_string(i));
    settings.layerCellsPerAxis={6,6,8};center={};const auto reshaped=run("compact_same_atlas_reshape");
    Require(!reshaped.recreate&&reshaped.fullUpdate[0]&&reshaped.fullUpdate[1]&&reshaped.fullUpdate[2]&&
        reshaped.updatedNodes==1415,"Same-atlas layer reshaping must rebuild all layers with new packed boundaries");
    run("compact_same_atlas_idle",true);
    return {{"cases",cases},{"totals",{{"frames",cases.size()},{"gpu_and_cpu_node_comparisons",totalCompared},
        {"deferred_nodes_unchanged",totalUnchanged},{"sample_comparisons",totalQueries},{"updated_nodes",totalUpdated}}},
        {"reference","Independent complete GPU rebuild with zero ring offsets plus per-node CPU composition; unupdated layers remain byte-identical"}};
}

int main(int argc,char** argv)
{
    Json report;std::string output=argc>3?argv[3]:"report.json";
    std::unique_ptr<Device> validationDevice;
    try
    {
        Require(argc>=4,"Usage: test compose.dxil query.dxil report.json [--gbv]");
        const bool gbv=argc>4&&std::string(argv[4])=="--gbv";
        const auto compose=ReadBytes(argv[1]),queryBytes=ReadBytes(argv[2]);
        validationDevice=std::make_unique<Device>(gbv);Device& gpu=*validationDevice;report["adapter"]=gpu.adapterName;
        report["scope"]="Standalone hardware GPU composition/sample/trace validation; production pass and shader, no window or settings modifications. CPU reference checks nodes; analytic fixtures independently constrain rays.";
        VXGIGlobalSDFPass pass;Require(pass.Initialize(gpu.device.Get(),compose.data(),compose.size()),pass.GetLastError());
        QueryPipeline queryPipeline(gpu,queryBytes);report["cases"]=Json::array();
        Model plane=Plane(),box=Box(),hollow=Hollow(),solid=SolidXY();
        auto run=[&](const char* name,const std::vector<Instance>& sources,XMFLOAT3 center,Global::Settings settings,const std::vector<Query>& queries)
        {
            Fixture fixture;fixture.Build(sources);Global::Layout layout;std::string error;
            Require(Global::BuildLayout(center,settings,uint32_t(fixture.geometry.assets.size()),uint32_t(fixture.instances.instances.size()),uint32_t(fixture.geometry.nodes.size()),layout,&error),error);
            Require(pass.Create(gpu.device.Get(),layout),pass.GetLastError());
            auto nodes=gpu.Upload(fixture.geometry.nodes),assets=gpu.Upload(fixture.geometry.assets),instances=gpu.Upload(fixture.instances.instances);
            gpu.Begin();VXGIGlobalSDFPass::Inputs inputs{nodes.Get(),assets.Get(),instances.Get()};
            Require(pass.Record(gpu.list.Get(),layout,inputs),pass.GetLastError());gpu.Submit();
            const auto values=gpu.ReadTexture(pass.GetTexture(),layout);
            Json item={{"name",name},{"shape",{layout.width,layout.height,layout.depth}},{"texture_bytes",layout.totalNodeCount*sizeof(float)},
                {"source_assets",fixture.geometry.assets.size()},{"source_instances",fixture.instances.instances.size()},
                {"node_comparison",CompareNodes(fixture,layout,values)}};
            const auto results=queryPipeline.Run(pass,layout,queries);item["queries"]=Json::array();
            for(size_t i=0;i<queries.size();++i)item["queries"].push_back(ResultJson(queries[i],results[i],fixture.scene));
            if(std::string(name)=="third_overlapping_aabb_target")
            {
                const auto mesh=VXGIMeshSDFThreshold::Trace(fixture.scene,queries[0].origin,queries[0].direction,queries[0].minimum,queries[0].maximum);
                const auto exact=fixture.scene.TraceGeometry(queries[0].origin,queries[0].direction,queries[0].minimum,queries[0].maximum);
                Require(mesh.status==VXGIMeshSDFThreshold::Status::Miss&&exact.hit&&exact.instanceId==30,"Third-target fixture must isolate nearest-two omission");
                item["nearest_two_mesh_trace_status"]=uint32_t(mesh.status);item["exact_target_instance_id"]=exact.instanceId;
            }
            item["query_count"]=queries.size();
            // Reuse this resource after a read/trace, with changed constants.
            // This exercises NPS->UAV->NPS and complete node replacement.
            Global::Layout shifted;const XMFLOAT3 shiftedCenter={center.x+settings.baseCellSize,center.y-settings.baseCellSize,center.z+settings.baseCellSize};
            Require(Global::BuildLayout(shiftedCenter,settings,uint32_t(fixture.geometry.assets.size()),uint32_t(fixture.instances.instances.size()),uint32_t(fixture.geometry.nodes.size()),shifted,&error),error);
            gpu.Begin();Require(pass.Record(gpu.list.Get(),shifted,inputs),pass.GetLastError());gpu.Submit();
            item["second_build_same_texture"]=CompareNodes(fixture,shifted,gpu.ReadTexture(pass.GetTexture(),shifted));
            report["cases"].push_back(std::move(item));std::cout<<"PASS nodes "<<name<<" ("<<layout.totalNodeCount<<")\n";
            return results;
        };
        const Global::Settings smallLayout={16,3,0.5f};
        const auto empty=run("empty",{}, {},smallLayout,{Ray({0,0,0},{0,1,0},10),Sample({0,0,0}),Sample({100,100,100}),Ray({0,0,0},{0,0,0},10),Ray({0,0,0},{0,1,0},1,2),Ray({0,0,0},{0,1,0},10,0,0)});
        Require(empty[0].status==0,"Empty field ray must miss");Require(empty[1].valid==1&&empty[2].valid==0,"Inside/outside sampling validity mismatch");
        Require(empty[3].status==2&&empty[4].status==2&&empty[5].status==2,"Invalid rays must return INVALID");
        std::vector<Query> planeQueries={Ray({0,3,0},{0,-7,0},6),Ray({0,3,0},{0,-7,0},0.5f),Ray({0,3,0},{0,1,0},6),Sample({0,2,0}),
            Ray({0,20,0},{0,-1,0},40),Ray({0,20,0},{0,-1,0},3),Ray({0,-20,0},{0,1,0},40),Ray({0,3,0},{0,-1e30f,0},6),Ray({0,3,0},{0,-1,0},6,0,1)};
        for(int y=-12;y<=12;++y)for(int x=-2;x<=2;++x)for(int z=-2;z<=2;++z)
            planeQueries.push_back(Sample({float(x),float(y)*0.25f,float(z)}));
        const auto flat=run("plane",{Make(plane,1)}, {},smallLayout,planeQueries);
        Require(flat[0].status==1&&flat[0].distance<=3&&flat[0].distance>=1,"Plane downward ray must hit the conservative surface neighborhood");
        Require(flat[1].status==0&&flat[2].status==0,"Finite ray before plane and ray directed away must miss");
        Require(flat[3].valid&&flat[3].distance>1&&flat[3].distance<=2,"Plane sample must be a conservative nonzero distance");
        Require(flat[4].status==1&&flat[4].clipmapIndex==0&&flat[5].status==0&&flat[5].sampleCount==0&&flat[6].status==1&&flat[6].clipmapIndex==0,"Outside-to-inside plane coverage failed");
        Require(flat[7].status==1&&flat[8].status==3,"Robust direction normalization or explicit exhaustion failed");
        float largestAnalyticExcess=0;
        for(size_t i=9;i<flat.size();++i)
        {
            const float exactDistance=std::abs(planeQueries[i].origin.y);
            Require(flat[i].valid&&std::isfinite(flat[i].safeDistance)&&flat[i].safeDistance>=0,"Analytic plane sample invalid");
            largestAnalyticExcess=(std::max)(largestAnalyticExcess,flat[i].safeDistance-exactDistance);
            Require(flat[i].safeDistance<=exactDistance+1e-5f,"GPU safe distance exceeds analytic plane distance");
        }
        report["cases"].back()["analytic_safe_distance"]={{"samples",flat.size()-9},{"maximum_excess",largestAnalyticExcess},{"geometry","finite XZ quad; all projection points inside quad"}};
        const auto negative=run("negative_world_coordinates",{Make(plane,2,{-23,-7,-11})},{-23,-7,-11},smallLayout,{Ray({-23,-4,-11},{0,-1,0},6),Ray({-23,-4,-11},{0,-1,0},0.5f)});
        Require(negative[0].status==1&&negative[1].status==0,"Negative coordinate plane queries failed");
        const auto scaled=run("nonuniform_negative_scale",{Make(box,3,{-2,1,-1},{-2,0.5f,1.5f},{0,0.3f,0})},{-2,1,-1},smallLayout,{Ray({-2,5,-1},{0,-1,0},8),Ray({-2,5,-1},{0,-1,0},0.5f)});
        Require(scaled[0].status==1&&scaled[1].status==0,"Nonuniform signed-scale box queries failed");
        const auto third=run("third_overlapping_aabb_target",{Make(hollow,10,{0,0,2}),Make(hollow,20,{0,0,4}),Make(solid,30,{0,0,6})},{0,0,3},smallLayout,{Ray({0,0,0},{0,0,7},10)});
        Require(third[0].status==1&&third[0].distance>4&&third[0].distance<=6,"Global field did not retain third AABB's target");
        const auto seam=run("clipmap_seam",{Make(box,4,{7,0,0},{0.5f,0.5f,0.5f})},{},smallLayout,
            {Ray({0,0,0},{1,0,0},10),Ray({0,0,0},{1,0,0},3),Sample({3.999f,0,0}),Sample({4.001f,0,0}),Sample({8.001f,0,0}),Sample({16.001f,0,0})});
        Require(seam[0].status==1&&seam[0].clipmapIndex>=1,"Trace must cross a clipmap boundary to the remote box");
        Require(seam[1].status==0,"Finite seam ray before target must miss");
        Require(seam[2].valid&&seam[2].clipmapIndex==0&&seam[3].valid&&seam[3].clipmapIndex==1&&seam[4].valid&&seam[4].clipmapIndex==2&&!seam[5].valid,"Clipmap seam selection failed");
        const auto inward=run("outside_coarse_to_fine_box",{Make(box,5,{},{0.5f,0.5f,0.5f})},{},smallLayout,
            {Ray({-20,0,0},{1,0,0},40),Ray({20,0,0},{-1,0,0},40),Ray({-20,0,0},{1,0,0},3)});
        Require(inward[0].status==1&&inward[0].clipmapIndex==0&&inward[1].status==1&&inward[1].clipmapIndex==0&&inward[2].status==0&&inward[2].sampleCount==0,"Coarse-to-fine entry skipped the small box");
        const auto defaults=run("default_65_nodes_three_clipmaps",{Make(plane,1)},{},{},{Ray({0,3,0},{0,-1,0},6),Sample({0,0,0})});
        Require(defaults[0].status==1,"Default layout plane trace failed");
        Global::Settings compactLayout;compactLayout.baseCellSize=.25f;compactLayout.layerCellsPerAxis={4,8,8};
        const auto compactSeams=run("compact_1_4_8_seams",{}, {},compactLayout,
            {Sample({.499f,0,0}),Sample({.501f,0,0}),Sample({1.999f,0,0}),Sample({2.001f,0,0}),
             Sample({3.999f,0,0}),Sample({4.001f,0,0}),Ray({-6,0,0},{1,0,0},12),Ray({-6,0,0},{1,0,0},1)});
        Require(compactSeams[0].valid&&compactSeams[0].clipmapIndex==0&&compactSeams[0].cellSize==.25f&&
            compactSeams[1].valid&&compactSeams[1].clipmapIndex==1&&compactSeams[1].cellSize==.5f&&
            compactSeams[2].valid&&compactSeams[2].clipmapIndex==1&&
            compactSeams[3].valid&&compactSeams[3].clipmapIndex==2&&compactSeams[3].cellSize==1&&
            compactSeams[4].valid&&compactSeams[4].clipmapIndex==2&&!compactSeams[5].valid,
            "Compact sampling must select .25/.5/1m cells at the 1/4/8m cube boundaries");
        Require(compactSeams[6].status==0&&compactSeams[7].status==0&&compactSeams[7].sampleCount==0,
            "Compact empty ray must cross every layer boundary and respect the outer finite entry");
        const auto compactInward=run("compact_1_4_8_near_target",{Make(box,8,{},{.1f,.1f,.1f})},{},compactLayout,
            {Ray({-6,0,0},{1,0,0},12),Ray({6,0,0},{-1,0,0},12),Ray({-6,0,0},{1,0,0},1),Ray({-.4f,0,0},{1,0,0},.8f)});
        // Coarse hits identify a conservative neighborhood; they can precede
        // entry to the tiny near cube and are refined by the hybrid path.
        Require(compactInward[0].status==1&&compactInward[0].clipmapIndex<=1&&
            compactInward[1].status==1&&compactInward[1].clipmapIndex<=1&&
            compactInward[2].status==0&&compactInward[2].sampleCount==0&&
            compactInward[3].status==1&&compactInward[3].clipmapIndex==0,
            "Compact target rays must remain valid through packed layers and use the fine layer for an inside origin");
        report["updates"]=RunUpdates(gpu,compose,queryPipeline);
        report["diagnostics"]=gpu.Diagnostics();
        uint64_t totalNodes=0,totalQueries=0;
        for(const auto& item:report["cases"])
        {totalNodes+=item["node_comparison"]["compared_nodes"].get<uint64_t>()+item["second_build_same_texture"]["compared_nodes"].get<uint64_t>();totalQueries+=item["query_count"].get<uint64_t>();}
        report["totals"]={{"cases",report["cases"].size()},{"node_comparisons",totalNodes},{"queries",totalQueries},{"rebuilds_on_existing_texture",report["cases"].size()}};
        Require(report["diagnostics"]["errors"]==0&&report["diagnostics"]["warnings"]==0,"D3D12 debug layer reported warnings or errors");
        report["passed"]=true;
    }
    catch(const std::exception& exception)
    {
        report["passed"]=false;report["failure"]=exception.what();std::cerr<<"FAIL "<<exception.what()<<'\n';
        if(validationDevice) try {report["diagnostics"]=validationDevice->Diagnostics();} catch(...) {}
    }
    std::ofstream file(output);file<<std::setw(2)<<report<<'\n';
    if(!file){std::cerr<<"Cannot write report "<<output<<'\n';return 2;}
    return report.value("passed",false)?0:1;
}
