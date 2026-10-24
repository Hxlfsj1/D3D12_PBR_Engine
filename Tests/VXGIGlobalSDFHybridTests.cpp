// Standalone validation: no application startup, window, scene or settings edits.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "Resources/VXGIGlobalSDF.h"
#include "Resources/VXGIGlobalSDFState.h"
#include "Resources/VXGIMeshSDFThreshold.h"
#include "Resources/SSGIHybridState.h"
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
static Model NarrowHollow()
{
    // At the central ray the exact distance is 0.45: larger than the local
    // Mesh threshold (0.9 * 0.25), but inside the coarse Global SDF shell.
    Mesh mesh;Quad(mesh,{-1,-1,0},{-0.45f,-1,0},{-0.45f,1,0},{-1,1,0});
    Quad(mesh,{0.45f,-1,0},{1,-1,0},{1,1,0},{0.45f,1,0});return {{std::move(mesh)}};
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
struct HybridParameters
{
    XMFLOAT3 gridOrigin{};float cellSize=0;
    std::array<uint32_t,3> cellDimensions{};uint32_t triangleCount=0;
    XMFLOAT3 boundsMaximum{};uint32_t materialCount=0;
    XMFLOAT3 reservedLighting0{};float materialMipLevel=0;
    XMFLOAT3 reservedLighting1{};float reservedLighting2=0;
    float reservedWorldRange=0,voxelSize=1;uint32_t probeCount=0,worldMode=2;
    uint32_t nodeCount=0,assetCount=0,instanceCount=0,bvhNodeCount=0;
};
struct GlobalParameters
{
    uint32_t enabled=1,textureIndex=0;float maxTraceDistance=16;uint32_t maxSteps=512;
    Global::Constants constants{};
};
struct Parameters { HybridParameters mesh; GlobalParameters global; };
struct Query
{
    XMFLOAT3 origin{};float nearLength=1;
    XMFLOAT3 direction={0,0,1};float farLength=14;
    uint32_t enabled=1,maximumSteps=512,invalidNear=0,padding=0;
};
struct Result
{
    XMFLOAT3 radiance;uint32_t hit;
    float distance;uint32_t status,backend,worldTraceStatus;
    float confidence;uint32_t anchorStatus,triangleIndex,probeHasSupport;
    float probeSupportWeight;uint32_t probeAcceptedCount,probeTraceFailures,sourceValid;
};
struct ProbeCell { std::array<int32_t,3> coordinate;uint32_t probeIndex; };
struct Probe
{
    DirectX::XMFLOAT4 sh[4]{};
    uint32_t state=1,updateCount=1,sampleCount=8,lastUpdateFrame=1;
    float backfaceRatio=0,minHitDistance=1;
    uint32_t classificationRays=8,reason=0;
};
static_assert(sizeof(HybridParameters)==112 && offsetof(HybridParameters,nodeCount)==96);
static_assert(sizeof(GlobalParameters)==192 && offsetof(GlobalParameters,constants)==16);
static_assert(sizeof(Parameters)==304 && offsetof(Parameters,global)==112);
static_assert(sizeof(Query)==48 && sizeof(Result)==64 && offsetof(Result,sourceValid)==60);
static_assert(sizeof(ProbeCell)==16 && sizeof(Probe)==96);

struct ProbeField
{
    std::vector<ProbeCell> cells;
    std::vector<Probe> records;
    // The CPU assigns deterministic coefficients only; production shader does
    // all hit-point lookup, interpolation and SH evaluation. No CPU shading.
    void Fill(uint32_t mode)
    {
        for(int32_t x=-16;x<=16;++x) for(int32_t y=-16;y<=16;++y) for(int32_t z=-16;z<=16;++z)
        {
            if(mode==2 && z<4) continue; // no support near the first near wall
            if(mode==4 && z<9) continue; // no support near the first far wall
            if(mode==7 && z>4) continue; // coarse point supported, fine hit unsupported
            if(mode==8 && z<6) continue; // fine hit supported, coarse point unsupported
            Probe probe;
            const bool black=(mode==1&&z<3)||(mode==3&&z<8);
            const float coefficient=1.0f/0.28209479177387814f;
            probe.sh[0]=black?DirectX::XMFLOAT4{}:DirectX::XMFLOAT4{2*coefficient,3*coefficient,4*coefficient,0};
            if(mode==5 && z<8) probe.updateCount=0;
            if(mode==6 && z<8) probe.sh[0].x=std::numeric_limits<float>::quiet_NaN();
            cells.push_back({{x,y,z},uint32_t(records.size())});records.push_back(probe);
        }
    }
};
class HybridPipeline
{
public:
    HybridPipeline(Device& gpu,const std::vector<uint8_t>& hybrid,const std::vector<uint8_t>& legacy):gpu(gpu)
    {
        D3D12_ROOT_PARAMETER roots[10]{};
        roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;roots[0].Descriptor={0,0};
        const std::array<std::array<UINT,2>,7> bindings={{{0,0},{0,1},{1,1},{3,1},{6,0},{7,0},{8,0}}};
        for(UINT i=0;i<bindings.size();++i)
        {roots[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;roots[i+1].Descriptor={bindings[i][0],bindings[i][1]};}
        roots[8].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;roots[8].Descriptor={0,0};
        roots[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[9].Constants={1,0,1};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=10;desc.pParameters=roots;
        desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> bytes,error;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error),"Hybrid root serialization");
        Check(gpu.device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&signature)),"Hybrid root signature");
        auto create=[&](const std::vector<uint8_t>& bytecode,ComPtr<ID3D12PipelineState>& target)
        {D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=signature.Get();pso.CS={bytecode.data(),bytecode.size()};Check(gpu.device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&target)),"Hybrid compute pipeline");};
        create(hybrid,pipeline);create(legacy,legacyPipeline);
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors=1;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(gpu.device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"Hybrid descriptor heap");
    }
    std::vector<Result> Run(VXGIGlobalSDFPass& pass,const Global::Layout& layout,const Fixture& fixture,
        const ProbeField& probes,const std::vector<Query>& queries,bool legacy=false,bool invalidLayout=false)
    {
        auto nodes=gpu.Upload(fixture.geometry.nodes),assets=gpu.Upload(fixture.geometry.assets),instances=gpu.Upload(fixture.instances.instances);
        const auto acceleration=GPUData::BuildInstanceAcceleration(fixture.scene);
        auto bvh=gpu.Upload(acceleration.nodes),lookup=gpu.Upload(probes.cells),records=gpu.Upload(probes.records),inputs=gpu.Upload(queries);
        Parameters parameters;
        parameters.mesh.nodeCount=uint32_t(fixture.geometry.nodes.size());parameters.mesh.assetCount=uint32_t(fixture.geometry.assets.size());
        parameters.mesh.instanceCount=uint32_t(fixture.instances.instances.size());parameters.mesh.bvhNodeCount=acceleration.nodeCount;
        parameters.mesh.probeCount=uint32_t(probes.records.size());parameters.global.constants=layout.constants;
        if(invalidLayout) parameters.global.constants.resolution=0;
        std::array<uint8_t,512> constants{};std::memcpy(constants.data(),&parameters,sizeof(parameters));
        auto constantBuffer=gpu.Upload(std::vector<uint8_t>(constants.begin(),constants.end()));
        const uint64_t bytes=uint64_t(queries.size())*sizeof(Result);
        auto outputs=gpu.Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.device->CopyDescriptorsSimple(1,heap->GetCPUDescriptorHandleForHeapStart(),pass.GetSRVCPUHandle(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        gpu.Begin();auto barrier=Transition(outputs.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.list->ResourceBarrier(1,&barrier);
        ID3D12DescriptorHeap* heaps[]={heap.Get()};gpu.list->SetDescriptorHeaps(1,heaps);
        gpu.list->SetComputeRootSignature(signature.Get());gpu.list->SetPipelineState(legacy?legacyPipeline.Get():pipeline.Get());
        gpu.list->SetComputeRootConstantBufferView(0,constantBuffer->GetGPUVirtualAddress());
        const std::array<ID3D12Resource*,7> resources={nodes.Get(),assets.Get(),instances.Get(),bvh.Get(),lookup.Get(),records.Get(),inputs.Get()};
        for(UINT i=0;i<resources.size();++i)gpu.list->SetComputeRootShaderResourceView(i+1,resources[i]->GetGPUVirtualAddress());
        gpu.list->SetComputeRootUnorderedAccessView(8,outputs->GetGPUVirtualAddress());
        gpu.list->SetComputeRoot32BitConstant(9,UINT(queries.size()),0);gpu.list->Dispatch(UINT((queries.size()+63)/64),1,1);
        barrier=Transition(outputs.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);gpu.list->ResourceBarrier(1,&barrier);
        gpu.list->CopyBufferRegion(readback.Get(),0,outputs.Get(),0,bytes);gpu.Submit();
        std::vector<Result> result(queries.size());void* address=nullptr;const D3D12_RANGE readRange={0,size_t(bytes)},empty{};
        Check(readback->Map(0,&readRange,&address),"Hybrid readback map");std::memcpy(result.data(),address,size_t(bytes));readback->Unmap(0,&empty);return result;
    }
private:
    Device& gpu;ComPtr<ID3D12RootSignature> signature;ComPtr<ID3D12PipelineState> pipeline,legacyPipeline;ComPtr<ID3D12DescriptorHeap> heap;
};
static Query Ray(XMFLOAT3 origin={0,0,0},XMFLOAT3 direction={0,0,1},float nearLength=1,float farLength=14,uint32_t enabled=1)
{Query value;value.origin=origin;value.direction=direction;value.nearLength=nearLength;value.farLength=farLength;value.enabled=enabled;return value;}
static Query Coarse(Query value){value.padding=1;return value;}
static Json ResultJson(const Result& r)
{return {{"hit",r.hit},{"backend",r.backend},{"status",r.status},{"world_trace_status",r.worldTraceStatus},{"distance",r.distance},
    {"radiance",{r.radiance.x,r.radiance.y,r.radiance.z}},{"probe_support",r.probeHasSupport},{"source_valid",r.sourceValid},
    {"probe_weight",r.probeSupportWeight},{"accepted_probes",r.probeAcceptedCount},{"probe_failures",r.probeTraceFailures}};}
static void Bright(const Result& r,const char* message)
{Require(r.sourceValid&&r.probeHasSupport&&std::abs(r.radiance.x-2)<2e-5f&&std::abs(r.radiance.y-3)<2e-5f&&std::abs(r.radiance.z-4)<2e-5f,message);}
static void Black(const Result& r,const char* message)
{Require(r.radiance.x==0&&r.radiance.y==0&&r.radiance.z==0,message);}

static Json ValidateHistoryIdentity()
{
    SSGIHybridState state;SSGIHybridState::Key key;
    key.effectiveWorldMode=2;key.globalEnabled=true;key.globalTraceDistance=128;
    key.globalCellSize=0.5f;key.globalMaxSteps=512;key.globalCells=64;key.globalClipmaps=3;
    Require(state.NeedsReset(key),"Initial source needs history reset");state.Commit(key);
    Require(!state.NeedsReset(key),"Committed source must preserve history across ordinary camera updates");
    uint32_t changedFields=0;
    auto check=[&](const SSGIHybridState::Key& changed)
    {Require(state.NeedsReset(changed),"Changed GDF source must invalidate accumulated GI");++changedFields;};
    auto changed=key;changed.globalEnabled=false;check(changed);
    changed=key;changed.globalTraceDistance+=1;check(changed);
    changed=key;changed.globalCellSize*=2;check(changed);
    changed=key;++changed.globalMaxSteps;check(changed);
    changed=key;changed.globalCells+=2;check(changed);
    changed=key;--changed.globalClipmaps;check(changed);
    changed=key;changed.globalLayerCells={4,8,8};check(changed);
    key.effectiveWorldMode=0;state.Commit(key);changed=key;
    changed.globalEnabled=false;changed.globalTraceDistance=0;changed.globalCellSize=2;
    changed.globalMaxSteps=1;changed.globalCells=16;changed.globalClipmaps=1;
    changed.globalLayerCells={4,8,8};
    Require(!state.NeedsReset(changed),"Screen-only mode must ignore GDF source settings");
    return {{"changed_fields_reset",changedFields},{"unchanged_key_preserved",true},{"screen_only_ignores_global_settings",true},
        {"camera_scroll","Camera position and clipmap offsets are absent from the source key; identical keys preserve history"}};
}

int main(int argc,char** argv)
{
    Json report;std::string output=argc>4?argv[4]:"hybrid-report.json";
    std::unique_ptr<Device> validationDevice;
    try
    {
        Require(argc>=5,"Usage: test compose.dxil hybrid.dxil legacy.dxil report.json [--gbv]");
        const bool gbv=argc>5&&std::string(argv[5])=="--gbv";
        const auto compose=ReadBytes(argv[1]),hybridBytes=ReadBytes(argv[2]),legacyBytes=ReadBytes(argv[3]);
        validationDevice=std::make_unique<Device>(gbv);Device& gpu=*validationDevice;
        report["adapter"]=gpu.adapterName;report["scope"]="Production GDF GPU composition and production SSGIWorldRaySource, deterministic SH fixtures; near-only shader compiled independently. No application or settings writes.";
        report["history_identity"]=ValidateHistoryIdentity();
        report["contracts"]={{"hybrid_parameters",sizeof(HybridParameters)},{"global_parameters",sizeof(GlobalParameters)},
            {"combined_constants",sizeof(Parameters)},{"query",sizeof(Query)},{"result",sizeof(Result)},{"probe",sizeof(Probe)}};
        VXGIGlobalSDFPass pass;Require(pass.Initialize(gpu.device.Get(),compose.data(),compose.size()),pass.GetLastError());
        HybridPipeline pipeline(gpu,hybridBytes,legacyBytes);report["cases"]=Json::array();
        Model solid=SolidXY(),hollow=Hollow(),narrowHollow=NarrowHollow(),box=Box(),ground=Plane();
        uint64_t queryCount=0,legacyCompared=0,rollingCompared=0;
        auto run=[&](const std::string& name,const std::vector<Instance>& sources,const std::vector<Query>& queries,uint32_t probeMode=0,
            bool compareLegacy=false,bool rolling=false,bool invalidLayout=false,bool compact=false)
        {
            Fixture fixture;fixture.Build(sources);ProbeField probes;probes.Fill(probeMode);
            Global::Layout layout;std::string error;
            Global::Settings settings{16,3,0.5f};
            if(compact){settings.baseCellSize=.25f;settings.layerCellsPerAxis={4,8,8};}
            Require(Global::BuildLayout({},settings,uint32_t(fixture.geometry.assets.size()),
                uint32_t(fixture.instances.instances.size()),uint32_t(fixture.geometry.nodes.size()),layout,&error),error);
            if(rolling)for(uint32_t i=0;i<layout.constants.clipmapCount;++i)
            {
                const auto n=Global::Resolution(layout.constants,i);
                layout.constants.clipmaps[i].scrollOffset={3%n,5%n,7%n};
            }
            Require(pass.Create(gpu.device.Get(),layout),pass.GetLastError());
            auto nodes=gpu.Upload(fixture.geometry.nodes),assets=gpu.Upload(fixture.geometry.assets),instances=gpu.Upload(fixture.instances.instances);
            gpu.Begin();VXGIGlobalSDFPass::Inputs inputs{nodes.Get(),assets.Get(),instances.Get()};
            Require(pass.Record(gpu.list.Get(),layout,inputs),pass.GetLastError());gpu.Submit();pass.OnSubmitted();
            const auto results=pipeline.Run(pass,layout,fixture,probes,queries,false,invalidLayout);
            Json item={{"name",name},{"rolling",rolling},{"compact",compact},{"invalid_layout",invalidLayout},{"probe_mode",probeMode},{"queries",Json::array()}};
            for(size_t i=0;i<queries.size();++i)
            {
                auto entry=ResultJson(results[i]);entry["origin"]={queries[i].origin.x,queries[i].origin.y,queries[i].origin.z};
                entry["direction"]={queries[i].direction.x,queries[i].direction.y,queries[i].direction.z};
                entry["near_length"]=queries[i].nearLength;entry["far_length"]=std::isfinite(queries[i].farLength)?Json(queries[i].farLength):Json("NaN");
                entry["coarse_diagnostic"]=queries[i].padding!=0;
                const auto exact=fixture.scene.TraceGeometry(queries[i].origin,queries[i].direction,0,queries[i].farLength);
                entry["exact_triangle_hit"]=exact.hit;entry["exact_distance"]=exact.distance;entry["exact_instance_id"]=exact.instanceId;
                if(queries[i].padding==0&&results[i].hit)
                {
                    Require(exact.hit,name+": verified geometric hit has no exact fixture intersection");
                    const auto source=std::find_if(sources.begin(),sources.end(),[&](const auto& instance){return instance.editorId==exact.instanceId;});
                    Require(source!=sources.end(),name+": exact hit instance is absent from the fixture");
                    const float maximumScale=(std::max)({std::abs(source->scale.x),std::abs(source->scale.y),std::abs(source->scale.z)});
                    const float tolerance=0.9f*0.25f*maximumScale+1e-4f;
                    const float fineError=std::abs(results[i].distance-exact.distance);
                    entry["fine_distance_error"]=fineError;entry["fine_mesh_world_threshold_tolerance"]=tolerance;
                    Require(fineError<=tolerance,name+": refined hit is farther from the exact surface than the fine Mesh SDF threshold permits");
                }
                item["queries"].push_back(std::move(entry));
            }
            if(compareLegacy)
            {
                const auto legacy=pipeline.Run(pass,layout,fixture,probes,queries,true);
                for(size_t i=0;i<results.size();++i)Require(std::memcmp(&legacy[i],&results[i],sizeof(Result))==0,name+": disabled result differs from independently compiled legacy shader");
                legacyCompared+=results.size();item["independent_legacy_byte_comparisons"]=results.size();
            }
            if(name=="third_target_after_two_hollow_candidates")
            {
                const auto old=VXGIMeshSDFThreshold::Trace(fixture.scene,queries[0].origin,queries[0].direction,0,queries[0].farLength);
                const auto exact=fixture.scene.TraceGeometry(queries[0].origin,queries[0].direction,0,queries[0].farLength);
                Require(old.status==VXGIMeshSDFThreshold::Status::Miss&&exact.hit&&exact.instanceId==30,"Third target fixture does not isolate nearest-two candidate omission");
                item["old_full_range_mesh_status"]=uint32_t(old.status);item["exact_target_id"]=exact.instanceId;
            }
            queryCount+=results.size();report["cases"].push_back(std::move(item));std::cout<<"PASS dispatch "<<name<<" ("<<results.size()<<" queries)\n";return results;
        };
        const std::vector<Instance> farWall={Make(solid,30,{0,0,6})};
        const std::vector<Instance> compactWall={Make(solid,30,{0,0,3})};
        const std::vector<Query> compactRays={Ray(),Ray({0,0,-6})};
        const auto compactHits=run("compact_far_hits_from_inside_and_outside",compactWall,compactRays,0,false,false,false,true);
        const auto compactRolled=run("compact_rolling_keeps_fine_hits",compactWall,compactRays,0,false,true,false,true);
        for(size_t i=0;i<compactHits.size();++i)
        {
            Require(compactHits[i].hit&&compactHits[i].backend==3,"Compact atlas must confirm the far surface");
            Bright(compactHits[i],"Compact atlas must read existing SH at the fine hit");
            Require(std::memcmp(&compactHits[i],&compactRolled[i],sizeof(Result))==0,
                "Compact rolling must preserve fine hit and SH results");
            ++rollingCompared;
        }
        const auto compactOutside=run("compact_does_not_reach_beyond_outer_box",farWall,{Ray()},0,false,false,false,true);
        Require(!compactOutside[0].hit&&compactOutside[0].worldTraceStatus==0,
            "An 8m outer box must stop before the surface at z=6");
        const auto farResult=run("far_color",farWall,{Ray()});
        Require(farResult[0].hit&&farResult[0].backend==3&&farResult[0].distance>1&&farResult[0].distance<=6,"Near miss must continue to far GDF target");Bright(farResult[0],"Far hit must evaluate existing SH to the expected RGB");
        Require(farResult[0].distance>=5.99f,"Far source must be evaluated at the refined Mesh SDF surface, not the coarse shell");
        const auto atFine=run("fine_hit_has_support_coarse_candidate_has_none",farWall,{Ray(),Coarse(Ray())},8);
        Require(atFine[0].hit&&atFine[0].backend==3&&atFine[0].distance>=5.99f,"Fine-supported target must retain its true geometry endpoint");
        Bright(atFine[0],"Fine hit must query cache at the refined hit position");
        Require(atFine[1].hit&&!atFine[1].sourceValid,"Probe fixture must leave the coarse shell unsupported");
        const auto atCoarse=run("coarse_support_does_not_relocate_fine_hit",farWall,{Ray(),Coarse(Ray())},7);
        Require(atCoarse[0].hit&&atCoarse[0].backend==3&&!atCoarse[0].sourceValid,"Coarse support must not relocate an unsupported fine surface");
        Black(atCoarse[0],"Unsupported fine hit must add zero");Bright(atCoarse[1],"Probe fixture must support the coarse shell independently");
        const auto finite=run("finite_far_range",farWall,{Ray({}, {0,0,1},1,2)});
        Require(!finite[0].hit&&finite[0].backend==3&&finite[0].sourceValid==0,"Finite far trace must not reach the remote target");Black(finite[0],"Miss must not inject radiance");
        const Query beforeSurface=Ray({}, {0,0,1},1,5.8f);
        const auto finiteShell=run("finite_end_inside_coarse_shell_before_real_surface",farWall,{beforeSurface,Coarse(beforeSurface)});
        Require(!finiteShell[0].hit&&finiteShell[0].worldTraceStatus==0&&!finiteShell[0].sourceValid,"A finite endpoint before the real surface must be a proven miss");
        Bright(finiteShell[1],"Finite-end fixture must expose the original coarse-shell false hit");Black(finiteShell[0],"Finite endpoint leaked coarse-shell radiance");
        const auto atEndpoint=run("finite_end_on_real_surface",farWall,{Ray({}, {0,0,1},1,6)});
        Require(atEndpoint[0].hit&&atEndpoint[0].distance<=6&&atEndpoint[0].distance>=5.99f,"Inclusive finite endpoint on the true surface must hit within range");
        const auto zeroNear=run("near_range_consumed_by_bias",farWall,{Ray({}, {0,0,1},0,14)});
        Require(zeroNear[0].hit&&zeroNear[0].backend==3,"Consumed near range must retain far eligibility");Bright(zeroNear[0],"Consumed near range lost valid far radiance");
        const std::vector<Instance> nearFar={Make(solid,10,{0,0,0.75f}),Make(solid,30,{0,0,6})};
        const auto nearBlack=run("near_black_stops_before_bright_far",nearFar,{Ray({}, {0,0,1},1.5f,14)},1);
        Require(nearBlack[0].hit&&nearBlack[0].backend==2&&nearBlack[0].sourceValid,"Black near hit must terminate as a valid source");Black(nearBlack[0],"Black near hit leaked rear radiance");
        const auto nearUnsupported=run("near_unsupported_stops_before_bright_far",nearFar,{Ray({}, {0,0,1},1.5f,14)},2);
        Require(nearUnsupported[0].hit&&nearUnsupported[0].backend==2&&!nearUnsupported[0].sourceValid&&!nearUnsupported[0].probeHasSupport,"Unsupported near geometry must still terminate");Black(nearUnsupported[0],"Unsupported near hit leaked rear radiance");
        const std::vector<Instance> farTwo={Make(solid,10,{0,0,6}),Make(solid,30,{0,0,10})};
        for(uint32_t mode:{3u,4u,5u,6u})
        {
            const auto result=run("far_first_surface_probe_mode_"+std::to_string(mode),farTwo,{Ray()},mode);
            Require(result[0].hit&&result[0].backend==3&&result[0].distance<8,"Far first geometry must terminate before rear bright wall");
            Black(result[0],"Far first surface leaked radiance from the wall behind it");
            Require(mode==3?result[0].sourceValid!=0:result[0].sourceValid==0,"Far black/unsupported/invalid SH source validity mismatch");
        }
        const auto fullMiss=run("empty_field_zero_contribution",{}, {Ray(),Ray({}, {0,1,0},0,14)});
        for(const auto& result:fullMiss){Require(!result.hit&&result.backend==3&&!result.sourceValid,"Empty field must remain a miss");Black(result,"Full miss must preserve existing IBL by adding zero");}
        std::vector<Query> grazingRays;
        for(const auto direction:std::vector<XMFLOAT3>{{1,0,0},{1,0.01f,0},{1,0.05f,0},{0.5f,0.04f,0.5f}})
        {const Query ray=Ray({0,0.1f,0},direction,1,12);grazingRays.push_back(ray);grazingRays.push_back(Coarse(ray));}
        const auto grazing=run("ground_upward_and_parallel_grazing",{Make(ground,10,{}, {4,1,4})},grazingRays);
        for(size_t i=0;i<grazing.size();i+=2)
        {
            Require(!grazing[i].hit&&grazing[i].worldTraceStatus==0&&!grazing[i].sourceValid,"Ground ray directed away or parallel must not hit its coarse shell");
            Black(grazing[i],"Ground coarse shell injected false radiance");Bright(grazing[i+1],"Ground grazing fixture must first demonstrate a supported coarse false candidate");
        }
        const Query hollowRay=Ray({}, {0,0,1},1,5);
        const auto coarseOnly=run("coarse_supported_candidate_fine_geometry_miss",{Make(narrowHollow,10,{0,0,2})},{hollowRay,Coarse(hollowRay)});
        Require(!coarseOnly[0].hit&&coarseOnly[0].worldTraceStatus==0&&!coarseOnly[0].probeHasSupport&&!coarseOnly[0].sourceValid,
            "A supported coarse candidate with no Mesh SDF surface must remain a complete miss");
        Black(coarseOnly[0],"Unverified coarse candidate must not query radiance");Bright(coarseOnly[1],"Hollow fixture did not expose a supported coarse false candidate");
        const auto falseThenReal=run("false_coarse_candidate_then_real_surface",
            {Make(narrowHollow,10,{0,0,2}),Make(solid,30,{0,0,6})},{Ray(),Coarse(Ray())});
        Require(falseThenReal[0].hit&&falseThenReal[0].distance>=5.99f&&falseThenReal[0].distance<=6,
            "A clear fine window must continue to the later true surface");
        Bright(falseThenReal[0],"Later true surface lost SH after a rejected coarse candidate");
        Require(falseThenReal[1].hit&&falseThenReal[1].distance<2,"Fixture must encounter the false coarse candidate before the true surface");
        const auto thirdInWindow=run("fine_window_third_instance_after_two_hollow_aabbs",
            {Make(narrowHollow,10,{0,0,2}),Make(narrowHollow,20,{0,0,2.5f}),Make(solid,30,{0,0,3.25f})},{Ray(),Coarse(Ray())});
        Require(thirdInWindow[0].hit&&thirdInWindow[0].backend==3&&thirdInWindow[0].distance>=3.24f&&thirdInWindow[0].distance<=3.25f,
            "Fine verification must retain the third instance after the first two AABBs contain empty ray intervals");
        Bright(thirdInWindow[0],"Third fine candidate lost SH");
        const auto third=run("third_target_after_two_hollow_candidates",{Make(hollow,10,{0,0,2}),Make(hollow,20,{0,0,4}),Make(solid,30,{0,0,6})},{Ray({}, {0,0,1},5,10)});
        Require(third[0].hit&&third[0].backend==3&&third[0].distance>=5&&third[0].distance<=6,"GDF must retain the third target after a clear near interval");Bright(third[0],"Third target lost cached SH");
        const std::vector<Instance> seamSource={Make(box,10,{7,0,0},{0.5f,0.5f,0.5f})};
        const std::vector<Query> seamRays={Ray({}, {1,0,0},2,12),Ray({0,0.2f,0.1f},{1,0,0},2,12),Ray({0,-0.2f,-0.1f},{1,0,0},2,12)};
        const auto seam=run("cross_clipmap_seam",seamSource,seamRays);
        const auto rolled=run("cross_clipmap_seam_rolling_storage",seamSource,seamRays,0,false,true);
        for(size_t i=0;i<seam.size();++i)
        {Require(seam[i].hit&&seam[i].backend==3&&seam[i].distance>4,"Far ray must cross from fine to coarse clipmap");Bright(seam[i],"Clipmap seam lost SH");Require(std::memcmp(&seam[i],&rolled[i],sizeof(Result))==0,"Rolling storage changed hybrid trace result");++rollingCompared;}
        const auto disabled=run("disabled_matches_independent_legacy",nearFar,
            {Ray({}, {0,0,1},1.5f,14,0),Ray({}, {0,0,1},0.1f,14,0),Ray({}, {1,0,0},1,14,0),Ray({}, {},1,14,0),Ray({}, {0,0,1},0,14,0)},0,true);
        (void)disabled;
        Query invalidNear=Ray({}, {0,0,1},7,14);invalidNear.invalidNear=1;
        const auto badNear=run("invalid_near_does_not_continue",farWall,{invalidNear});
        Require(!badNear[0].hit&&badNear[0].backend==2&&badNear[0].worldTraceStatus>=2,"Invalid near query must not continue into far tracing");
        const auto badLayout=run("invalid_global_layout",farWall,{Ray()},0,false,false,true);
        Require(!badLayout[0].hit&&badLayout[0].backend==3&&badLayout[0].worldTraceStatus==2,"Invalid GDF layout must be explicit and contribute zero");
        Query exhausted=Ray();exhausted.maximumSteps=1;Query zeroSteps=Ray();zeroSteps.maximumSteps=0;
        Query nanDistance=Ray();nanDistance.farLength=std::numeric_limits<float>::quiet_NaN();
        const auto failure=run("global_exhaustion_and_invalid_parameters",
            {Make(narrowHollow,10,{0,0,2}),Make(solid,30,{0,0,6})},{exhausted,zeroSteps,nanDistance});
        Require(failure[0].worldTraceStatus==3&&!failure[0].hit,"Far exhaustion must be explicit");
        for(size_t i=1;i<failure.size();++i)Require(failure[i].worldTraceStatus==2&&!failure[i].hit,"Invalid far parameters must contribute zero");
        std::vector<Instance> manyEmptyCandidates;
        for(uint32_t index=0;index<520;++index)manyEmptyCandidates.push_back(Make(narrowHollow,index+1,{0,0,2}));
        manyEmptyCandidates.push_back(Make(solid,1000,{0,0,3}));
        const auto fineExhaustion=run("shared_fine_budget_exhaustion_is_not_a_miss",manyEmptyCandidates,{Ray()});
        Require(!fineExhaustion[0].hit&&fineExhaustion[0].backend==3&&fineExhaustion[0].worldTraceStatus==3&&fineExhaustion[0].status==5,
            "Shared fine sample budget exhaustion must remain explicit EXHAUSTED/INVALID, not MISS or a farther hit");
        Black(fineExhaustion[0],"Unresolved fine query must contribute zero");
        report["totals"]={{"cases",report["cases"].size()},{"queries",queryCount},{"independent_legacy_byte_comparisons",legacyCompared},{"rolling_byte_comparisons",rollingCompared}};
        report["diagnostics"]=gpu.Diagnostics();Require(report["diagnostics"]["errors"]==0&&report["diagnostics"]["warnings"]==0,"D3D12 debug/GBV emitted warnings or errors");
        report["passed"]=true;
    }
    catch(const std::exception& error)
    {report["passed"]=false;report["failure"]=error.what();std::cerr<<"FAIL "<<error.what()<<'\n';if(validationDevice)try{report["diagnostics"]=validationDevice->Diagnostics();}catch(...) {}}
    std::ofstream file(output);file<<std::setw(2)<<report<<'\n';if(!file)return 2;return report.value("passed",false)?0:1;
}
