// Independent stage 5 headless acceptance. Real filter/query helpers are tested
// with known signed SH and an independent CPU triangle-visibility reference.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <d3dx12.h>
#include "../Resources/VXGISDF.h"
#include "../Resources/VXGIProbeState.h"
#include "../Resources/VXGIProbeTopology.h"
#include "../Passes/VXGIProbeFilterPass.h"
#include "../Editor/VXGIProbeDebugRenderer.h"
#include "../Editor/VXGISDFDebugRenderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace
{
    using GPURecord=VXGIProbeState::GPURecord;
    void Require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
    void Check(HRESULT value,const char* operation)
    {
        if(FAILED(value)){std::ostringstream message;message<<operation<<": HRESULT 0x"<<std::hex<<uint32_t(value);throw std::runtime_error(message.str());}
    }
    XMFLOAT3 Add(XMFLOAT3 a,XMFLOAT3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
    XMFLOAT3 Sub(XMFLOAT3 a,XMFLOAT3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
    XMFLOAT3 Scale(XMFLOAT3 a,float value){return {a.x*value,a.y*value,a.z*value};}
    float Dot(XMFLOAT3 a,XMFLOAT3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    XMFLOAT3 Normalize(XMFLOAT3 value){const float length=std::sqrt(Dot(value,value));return length>0?Scale(value,1/length):XMFLOAT3{};}
    XMFLOAT3 Color(XMFLOAT4 value){return {value.x,value.y,value.z};}
    float Error(XMFLOAT3 a,XMFLOAT3 b){return std::max({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.z-b.z)});}
    std::string UTF8(const wchar_t* input)
    {
        const int size=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);std::string result(size,'\0');
        WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),size,nullptr,nullptr);if(!result.empty())result.pop_back();return result;
    }
    struct GPUTriangle
    {
        XMFLOAT3 p0;uint32_t triangleId;
        XMFLOAT3 p1;uint32_t flags;
        XMFLOAT3 p2;uint32_t pad0;
        XMFLOAT3 geometricNormal;uint32_t pad1;
        XMFLOAT2 uv0,uv1,uv2;
        XMFLOAT3 shadingNormal0,shadingNormal1,shadingNormal2;
        XMFLOAT3 tangent0,tangent1,tangent2;
        XMFLOAT3 bitangent0,bitangent1,bitangent2;
        uint32_t materialId,instanceId,pad2;
    };
    struct QueryInput
    {
        XMFLOAT3 position;uint32_t triangleIndex;
        XMFLOAT3 geometricNormal;uint32_t backFace;
        XMFLOAT3 mappedNormal;uint32_t valid=1;
        XMFLOAT3 barycentric;uint32_t triangleId;
        float distance=1;uint32_t traceStatus=1,padding0=0,padding1=0;
    };
    static_assert(sizeof(GPUTriangle)==208 && sizeof(QueryInput)==80);
    struct GPUParameters
    {
        XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;uint32_t probeCount;
        XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        float voxelSize;uint32_t updateStart=0,updateCount=0,resetMode=1;
        float neighborWeight=.25f;uint32_t materialCount=0;float materialMipLevel=0;uint32_t padding2=0;
    };
    struct QueryResult
    {
        XMFLOAT3 irradiance;float supportWeight;
        XMFLOAT3 unclamped;uint32_t negativeMask;
        uint32_t candidateCount,acceptedCount,hasSupport,traceFailures;
    };
    static_assert(sizeof(GPUParameters)==80 && sizeof(QueryResult)==48);
    std::vector<GPUTriangle> Triangles(const VXGISDF& sdf)
    {
        std::vector<GPUTriangle> result;
        for(uint32_t index=0;index<sdf.GetTriangles().size();++index)
        {
            const auto& source=sdf.GetTriangles()[index];GPUTriangle value{};
            value.p0=source.positions[0];value.p1=source.positions[1];value.p2=source.positions[2];value.triangleId=index;
            value.flags=source.isCutout;value.geometricNormal=source.geometricNormal;
            value.uv0=source.uv[0];value.uv1=source.uv[1];value.uv2=source.uv[2];
            value.shadingNormal0=source.shadingNormals[0];value.shadingNormal1=source.shadingNormals[1];value.shadingNormal2=source.shadingNormals[2];
            value.tangent0=source.tangents[0];value.tangent1=source.tangents[1];value.tangent2=source.tangents[2];
            value.bitangent0=source.bitangents[0];value.bitangent1=source.bitangents[1];value.bitangent2=source.bitangents[2];
            value.materialId=source.materialId;value.instanceId=source.instanceId;result.push_back(value);
        }
        return result;
    }
    GPURecord ConstantRecord(XMFLOAT3 value)
    {
        GPURecord result;result.sh[0]={value.x/.28209479177387814f,value.y/.28209479177387814f,value.z/.28209479177387814f,0};
        result.state=1;result.updateCount=3;result.sampleCount=24;result.lastUpdateFrame=11;result.classificationRays=8;
        result.minHitDistance=1;return result;
    }
    struct TestResource{void* Get()const{return nullptr;}};
    struct TestTexture{uint32_t type=0;TestResource Resource;std::string path;};
    struct TestVertex{XMFLOAT3 Position,Normal;XMFLOAT2 TexCoords;XMFLOAT3 Tangent,Bitangent;};
    struct TestMesh
    {
        std::vector<TestVertex> vertices;std::vector<std::vector<unsigned>> lodIndices={{}};
        std::vector<TestTexture> textures;uint32_t materialID=0;bool isUnlit=false;
        XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};
    };
    struct TestModel{std::vector<TestMesh> meshes;};
    struct TestInstance
    {
        uint32_t editorId=1;TestModel* pModel=nullptr;XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;bool isTransparent=false,isCutout=false,isVisible=true,isDirty=true;int currentLodLevel=0;
    };
    void Quad(TestMesh& mesh,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d)
    {
        const auto normal=Normalize(Cross(Sub(b,a),Sub(c,a))),tangent=Normalize(Sub(b,a)),bitangent=Cross(normal,tangent);
        const auto first=uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{a,normal,{0,0},tangent,bitangent},{b,normal,{1,0},tangent,bitangent},
            {c,normal,{1,1},tangent,bitangent},{d,normal,{0,1},tangent,bitangent}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    void Build(TestModel& model,VXGIWorld& world,VXGISDF& sdf,float cellSize=1)
    {
        std::vector<TestInstance> instances(1);instances[0].pModel=&model;
        Require(world.Update(instances,cellSize),world.GetLastError());Require(sdf.Update(world,cellSize,128),sdf.GetLastError());
    }
    TestModel ThinSlab()
    {
        TestModel model;model.meshes.resize(2);
        Quad(model.meshes[0],{-.01f,-2,-2},{-.01f,-2,2},{-.01f,2,2},{-.01f,2,-2});
        Quad(model.meshes[1],{.01f,-2,-2},{.01f,2,-2},{.01f,2,2},{.01f,-2,2});return model;
    }
    TestModel ThreePoint()
    {
        TestModel model;model.meshes.resize(2);
        Quad(model.meshes[0],{-1,0,-1},{-1,0,1},{1,0,1},{1,0,-1});
        Quad(model.meshes[1],{0,0,-.6f},{0,.2f,-.6f},{0,.2f,.6f},{0,0,.6f});return model;
    }
    QueryInput SurfaceQuery(const VXGISDF& sdf,XMFLOAT3 position,XMFLOAT3 facingNormal)
    {
        const auto hit=sdf.TraceReference(Add(position,Scale(facingNormal,.25f)),Scale(facingNormal,-1),.5f);
        Require(hit.hit && Error(hit.position,position)<2e-5f,"fixture query must be a real identified surface hit");
        QueryInput result{};result.position=hit.position;result.triangleIndex=hit.triangleIndex;result.triangleId=hit.triangleIndex;
        result.geometricNormal=hit.geometricNormal;result.backFace=hit.backFace;result.mappedNormal=facingNormal;
        result.barycentric=hit.barycentrics;return result;
    }
    ComPtr<IDxcBlob> Compile(const wchar_t* path,const wchar_t* entry)
    {
        ComPtr<IDxcUtils> utils;ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        ComPtr<IDxcBlobEncoding> source;Check(utils->LoadFile(path,nullptr,&source),"load shader");
        ComPtr<IDxcIncludeHandler> includes;Check(utils->CreateDefaultIncludeHandler(&includes),"create include handler");
        const wchar_t* arguments[]={path,L"-E",entry,L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",
#if defined(_DEBUG)
            L"-Od",L"-Zi"
#else
            L"-O3"
#endif
        };
        DxcBuffer input{source->GetBufferPointer(),source->GetBufferSize(),DXC_CP_UTF8};ComPtr<IDxcResult> result;
        Check(compiler->Compile(&input,arguments,UINT(std::size(arguments)),includes.Get(),IID_PPV_ARGS(&result)),"compile shader");
        ComPtr<IDxcBlobUtf8> errors;Check(result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr),"get shader diagnostics");
        if(errors && errors->GetStringLength())std::cerr<<errors->GetStringPointer();
        HRESULT status;Check(result->GetStatus(&status),"get shader status");Check(status,"shader compilation");
        ComPtr<IDxcBlob> object;Check(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&object),nullptr),"get shader object");return object;
    }
    class GPU
    {
    public:
        struct Output{std::vector<uint32_t> visibility;std::vector<GPURecord> filtered;std::vector<QueryResult> queries;};
        GPU()
        {
            ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get debug controller");debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get GPU validation controller");validation->SetEnableGPUBasedValidation(TRUE);
            ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create DXGI factory");ComPtr<IDXGIAdapter1> adapter;
            for(UINT index=0;factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++index)
            {
                DXGI_ADAPTER_DESC1 description{};Check(adapter->GetDesc1(&description),"get adapter description");
                if(!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;
                adapter.Reset();
            }
            Require(device!=nullptr,"hardware GPU unavailable");DXGI_ADAPTER_DESC1 description{};Check(adapter->GetDesc1(&description),"get chosen adapter");
            adapterName=UTF8(description.Description);std::cout<<"Actual device: "<<adapterName<<" (hardware)\n";
            Check(device.As(&info),"get debug message queue");D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
            Check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)),"query shader model");Require(model.HighestShaderModel>=D3D_SHADER_MODEL_6_6,"SM6.6 unavailable");
            D3D12_COMMAND_QUEUE_DESC queueDescription{};Check(device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(&queue)),"create queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create allocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"create command list");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"create fence event failed");
            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(device->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&heap)),"create fixture descriptor heap");
            std::array<CD3DX12_ROOT_PARAMETER,17> roots;roots[0].InitAsConstants(20,0);roots[1].InitAsConstants(4,1);
            for(UINT index=0;index<11;++index)roots[index+2].InitAsShaderResourceView(index);
            for(UINT index=0;index<4;++index)roots[index+13].InitAsUnorderedAccessView(index);
            const CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            CD3DX12_ROOT_SIGNATURE_DESC rootDescription(UINT(roots.size()),roots.data(),1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> blob;Check(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize root signature");
            Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"create root signature");
            reset=Pipeline(L"Shaders/Shaders_For_VXGI_Probe_Filter.hlsl",L"CSResetFilter");
            visibility=Pipeline(L"Shaders/Shaders_For_VXGI_Probe_Filter.hlsl",L"CSVisibility");
            filter=Pipeline(L"Shaders/Shaders_For_VXGI_Probe_Filter.hlsl",L"CSFilter");
            query=Pipeline(L"Shaders/VXGIProbeQueryValidate.hlsl",L"CSQuery");
            queryRadiance=Pipeline(L"Shaders/VXGIProbeQueryValidate.hlsl",L"CSQueryRadiance");
            rawReset=Pipeline(L"Shaders/VXGIProbeQueryValidate.hlsl",L"CSResetRaw");
        }
        ~GPU(){if(event)CloseHandle(event);}
        Output Run(const VXGIWorld& world,const VXGISDF& sdf,const std::vector<GPURecord>& raw,
            const std::vector<QueryInput>& queries,uint32_t resetRaw=0,bool directional=false,
            const VXGISurface::MaterialRecord* materialOverride=nullptr)
        {
            Require(raw.size()==world.GetOccupiedCenters().size() && !raw.empty(),"raw records must match actual world allocation");
            VXGIProbeTopology topology;Require(topology.Update(world),topology.GetLastError());
            const auto& grid=sdf.GetGrid();GPUParameters constants{};constants.gridOrigin=grid.origin;constants.cellSize=grid.cellSize;
            constants.cellDimensions=grid.cellDimensions;constants.probeCount=uint32_t(raw.size());constants.boundsMaximum=grid.boundsMaximum;
            constants.triangleCount=uint32_t(sdf.GetTriangles().size());constants.voxelSize=world.GetVoxelSize();constants.updateCount=constants.probeCount;
            constants.materialCount=1;
            auto nodeBuffer=Upload(sdf.GetNodes()),offsetBuffer=Upload(sdf.GetCellOffsets()),referenceBuffer=Upload(sdf.GetTriangleReferences());
            auto triangleBuffer=Upload(Triangles(sdf)),centerBuffer=Upload(world.GetOccupiedCenters());
            auto lookupBuffer=Upload(topology.GetLookup()),neighborBuffer=Upload(topology.GetNeighbors()),rawBuffer=Upload(raw);
            auto queryBuffer=Upload(queries.empty()?std::vector<QueryInput>(1):queries);
            // Stage 5 golden fixtures are all opaque. Bind a valid material
            // buffer; alpha coverage is independently tested by stage 7.3.
            auto materialBuffer=Upload(std::vector<VXGISurface::MaterialRecord>(1,
                materialOverride?*materialOverride:VXGISurface::MaterialRecord{}));
            auto edgeOutput=Buffer(raw.size()*sizeof(uint32_t),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto filteredOutput=Buffer(raw.size()*sizeof(GPURecord),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto queryOutput=Buffer(std::max(size_t(1),queries.size())*sizeof(QueryResult),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto resetRawOutput=Buffer(raw.size()*sizeof(GPURecord),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            Transition(edgeOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(filteredOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(queryOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(resetRawOutput.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[]={heap.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(root.Get());
            commands->SetComputeRoot32BitConstants(0,20,&constants,0);const std::array<uint32_t,4> extra={uint32_t(queries.size()),resetRaw,0,0};
            commands->SetComputeRoot32BitConstants(1,4,extra.data(),0);
            const std::array<ID3D12Resource*,11> inputs={nodeBuffer.Get(),offsetBuffer.Get(),referenceBuffer.Get(),triangleBuffer.Get(),centerBuffer.Get(),
                lookupBuffer.Get(),neighborBuffer.Get(),rawBuffer.Get(),edgeOutput.Get(),materialBuffer.Get(),queryBuffer.Get()};
            for(UINT index=0;index<inputs.size();++index)commands->SetComputeRootShaderResourceView(index+2,inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(13,edgeOutput->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(14,filteredOutput->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(15,queryOutput->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(16,resetRawOutput->GetGPUVirtualAddress());
            commands->SetPipelineState(reset.Get());commands->Dispatch((constants.probeCount+63)/64,1,1);UAV(edgeOutput.Get());UAV(filteredOutput.Get());
            commands->SetPipelineState(visibility.Get());commands->Dispatch(constants.probeCount,1,1);
            Transition(edgeOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(resetRaw)
            {
                commands->SetPipelineState(rawReset.Get());commands->Dispatch((constants.probeCount+63)/64,1,1);
                Transition(resetRawOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                commands->SetComputeRootShaderResourceView(9,resetRawOutput->GetGPUVirtualAddress());
            }
            commands->SetPipelineState(filter.Get());commands->Dispatch((constants.probeCount+63)/64,1,1);
            Transition(filteredOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(!queries.empty()){commands->SetPipelineState(directional?queryRadiance.Get():query.Get());commands->Dispatch((uint32_t(queries.size())+63)/64,1,1);}
            auto edgeReadback=CopyToReadback(edgeOutput.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto filteredReadback=CopyToReadback(filteredOutput.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto queryReadback=CopyToReadback(queryOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Check(commands->Close(),"close query commands");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
            Check(queue->Signal(fence.Get(),++fenceValue),"signal query fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm query fence");
            Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"query GPU dispatch timed out");Require(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU removed during query tests");
            Output result{Read<uint32_t>(edgeReadback.Get(),raw.size()),Read<GPURecord>(filteredReadback.Get(),raw.size()),Read<QueryResult>(queryReadback.Get(),queries.size())};
            Check(allocator->Reset(),"reset completed query allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset completed query commands");return result;
        }
        uint32_t Warnings()
        {
            uint32_t warnings=0;for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T size=0;Check(info->GetMessage(index,nullptr,&size),"measure debug message");std::vector<char> storage(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(index,message,&size),"read debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<"D3D12: "<<message->pDescription<<'\n';++warnings;}
            }return warnings;
        }
        std::string adapterName;
    private:
        ComPtr<ID3D12PipelineState> Pipeline(const wchar_t* path,const wchar_t* entry)
        {
            auto shader=Compile(path,entry);D3D12_COMPUTE_PIPELINE_STATE_DESC description{};description.pRootSignature=root.Get();description.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            ComPtr<ID3D12PipelineState> result;Check(device->CreateComputePipelineState(&description,IID_PPV_ARGS(&result)),"create query pipeline");return result;
        }
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initialState,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            Require(bytes>0,"empty test resource");const auto properties=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);
            ComPtr<ID3D12Resource> result;Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,initialState,nullptr,IID_PPV_ARGS(&result)),"create test resource");return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& data)
        {
            auto result=Buffer(data.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
            Check(result->Map(0,&noReads,&mapped),"map test upload");std::memcpy(mapped,data.data(),data.size()*sizeof(T));result->Unmap(0,nullptr);return result;
        }
        template<class T> static std::vector<T> Read(ID3D12Resource* resource,size_t count)
        {
            std::vector<T> result(count);if(count==0)return result;void* mapped=nullptr;const D3D12_RANGE reads{0,count*sizeof(T)};
            Check(resource->Map(0,&reads,&mapped),"map query readback");std::memcpy(result.data(),mapped,reads.End);const D3D12_RANGE noWrites{0,0};resource->Unmap(0,&noWrites);return result;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);commands->ResourceBarrier(1,&barrier);}
        void UAV(ID3D12Resource* resource){const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(resource);commands->ResourceBarrier(1,&barrier);}
        ComPtr<ID3D12Resource> CopyToReadback(ID3D12Resource* resource,D3D12_RESOURCE_STATES initialState)
        {
            auto result=Buffer(resource->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(resource,initialState,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(result.Get(),resource);return result;
        }
        ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> reset,visibility,filter,query,queryRadiance,rawReset;
        uint64_t fenceValue=0;HANDLE event=nullptr;
    };
    bool Usable(const GPURecord& record)
    {
        if(record.state!=1 || record.updateCount==0)return false;
        for(const auto& sh:record.sh)if(!std::isfinite(sh.x)||!std::isfinite(sh.y)||!std::isfinite(sh.z)||!std::isfinite(sh.w))return false;
        return true;
    }
    float EndpointTolerance(XMFLOAT3 position,XMFLOAT3 center,float distance,float cellSize)
    {
        const float scale=std::max({std::abs(position.x),std::abs(position.y),std::abs(position.z),std::abs(center.x),std::abs(center.y),std::abs(center.z),distance});
        return std::max({1e-6f,cellSize*1e-5f,8*std::numeric_limits<float>::epsilon()*scale});
    }
    bool SurfaceVisible(const VXGISDF& sdf,const QueryInput& queryInput,XMFLOAT3 center,bool& failure)
    {
        failure=false;const auto delta=Sub(center,queryInput.position);const float length=std::sqrt(Dot(delta,delta));
        const auto normal=Scale(Normalize(queryInput.geometricNormal),queryInput.backFace?-1.0f:1.0f);
        if(!queryInput.valid||queryInput.traceStatus!=1||queryInput.triangleIndex>=sdf.GetTriangles().size()||length<=0){failure=true;return false;}
        if(Dot(normal,delta)<=0)return false;
        const float tolerance=EndpointTolerance(queryInput.position,center,length,sdf.GetGrid().cellSize);
        const auto hit=sdf.TraceReference(center,Scale(delta,-1/length),length+tolerance);
        if(!hit.hit){failure=true;return false;}if(hit.triangleIndex!=queryInput.triangleIndex)return false;
        const auto error=Sub(hit.position,queryInput.position);const bool endpoint=std::abs(hit.distance-length)<=tolerance && std::sqrt(Dot(error,error))<=tolerance;
        failure=!endpoint;return endpoint;
    }
    std::vector<uint32_t> VisibilityReference(const VXGIWorld& world,const VXGISDF& sdf)
    {
        VXGIProbeTopology topology;Require(topology.Update(world),topology.GetLastError());std::vector<uint32_t> result(world.GetCells().size(),0x80000000u);
        for(uint32_t index=0;index<result.size();++index)for(uint32_t slot=0;slot<6;++slot)
        {
            const auto neighbor=topology.GetNeighbors()[index].indices[slot];if(neighbor==UINT32_MAX)continue;
            const auto a=world.GetOccupiedCenters()[std::min(index,neighbor)],b=world.GetOccupiedCenters()[std::max(index,neighbor)];
            const auto delta=Sub(b,a);const float distance=std::sqrt(Dot(delta,delta));
            if(!sdf.TraceReference(a,Scale(delta,1/distance),distance).hit)result[index]|=1u<<slot;
        }return result;
    }
    std::vector<GPURecord> FilterReference(const VXGIWorld& world,const std::vector<GPURecord>& raw,const std::vector<uint32_t>& masks,float neighborWeight=.25f)
    {
        VXGIProbeTopology topology;Require(topology.Update(world),topology.GetLastError());auto result=raw;
        for(uint32_t index=0;index<raw.size();++index)
        {
            if(!Usable(raw[index])){result[index]=GPURecord{};continue;}
            for(auto& sh:result[index].sh)sh={};
            std::array<XMFLOAT3,4> coefficients{};float weight=1;
            for(uint32_t coefficient=0;coefficient<4;++coefficient)coefficients[coefficient]=Color(raw[index].sh[coefficient]);
            for(uint32_t slot=0;slot<6;++slot)
            {
                const auto neighbor=topology.GetNeighbors()[index].indices[slot];if(neighbor==UINT32_MAX || !(masks[index]&(1u<<slot))||!Usable(raw[neighbor]))continue;
                weight+=neighborWeight;for(uint32_t coefficient=0;coefficient<4;++coefficient)coefficients[coefficient]=Add(coefficients[coefficient],Scale(Color(raw[neighbor].sh[coefficient]),neighborWeight));
            }
            for(uint32_t coefficient=0;coefficient<4;++coefficient){const auto value=Scale(coefficients[coefficient],1/weight);result[index].sh[coefficient]={value.x,value.y,value.z,0};}
        }return result;
    }
    QueryResult QueryReference(const VXGIWorld& world,const VXGISDF& sdf,const std::vector<GPURecord>& raw,const QueryInput& input,bool directional=false)
    {
        QueryResult result{};const auto normal=Normalize(input.mappedNormal);
        if((!directional&&(!input.valid||input.traceStatus!=1||input.triangleIndex>=sdf.GetTriangles().size()))||Dot(normal,normal)==0||
            !std::isfinite(input.mappedNormal.x)||!std::isfinite(input.mappedNormal.y)||!std::isfinite(input.mappedNormal.z)){result.traceFailures=1;return result;}
        const XMFLOAT3 coordinate={input.position.x/world.GetVoxelSize()-.5f,input.position.y/world.GetVoxelSize()-.5f,input.position.z/world.GetVoxelSize()-.5f};
        if(!std::isfinite(coordinate.x)||!std::isfinite(coordinate.y)||!std::isfinite(coordinate.z)||
            coordinate.x<-2147483648.0f||coordinate.y<-2147483648.0f||coordinate.z<-2147483648.0f||
            coordinate.x>2147483520.0f||coordinate.y>2147483520.0f||coordinate.z>2147483520.0f){result.traceFailures=1;return result;}
        const VXGIWorld::Coordinate base={int32_t(std::floor(coordinate.x)),int32_t(std::floor(coordinate.y)),int32_t(std::floor(coordinate.z))};
        const XMFLOAT3 fraction={coordinate.x-float(base.x),coordinate.y-float(base.y),coordinate.z-float(base.z)};std::array<XMFLOAT3,4> coefficients{};
        for(uint32_t corner=0;corner<8;++corner)
        {
            const uint32_t x=corner&1,y=(corner>>1)&1,z=(corner>>2)&1;
            const float weight=(x?fraction.x:1-fraction.x)*(y?fraction.y:1-fraction.y)*(z?fraction.z:1-fraction.z);if(weight<=0)continue;
            const int32_t index=world.FindCell(VXGIWorld::Coordinate{base.x+int32_t(x),base.y+int32_t(y),base.z+int32_t(z)});if(index<0)continue;
            ++result.candidateCount;if(!Usable(raw[index]))continue;bool failure=false;
            if(!directional&&!SurfaceVisible(sdf,input,world.GetOccupiedCenters()[index],failure)){result.traceFailures+=failure?1:0;continue;}
            ++result.acceptedCount;result.supportWeight+=weight;
            for(uint32_t coefficient=0;coefficient<4;++coefficient)coefficients[coefficient]=Add(coefficients[coefficient],Scale(Color(raw[index].sh[coefficient]),weight));
        }
        if(result.acceptedCount==0)return result;
        for(auto& coefficient:coefficients)coefficient=Scale(coefficient,1/result.supportWeight);
        result.unclamped=Add(Scale(coefficients[0],.28209479177387814f),Scale(Add(Add(Scale(coefficients[1],normal.x),Scale(coefficients[2],normal.y)),Scale(coefficients[3],normal.z)),(directional?1.0f:(2.0f/3))*.4886025119029199f));
        result.negativeMask=(result.unclamped.x<0?1u:0u)|(result.unclamped.y<0?2u:0u)|(result.unclamped.z<0?4u:0u);
        result.irradiance={std::max(result.unclamped.x,0.0f),std::max(result.unclamped.y,0.0f),std::max(result.unclamped.z,0.0f)};result.hasSupport=1;return result;
    }
    struct Report{uint32_t records=0,queries=0,directionalQueries=0,groups=0,warnings=0;float maximumSHError=0,maximumQueryError=0,maximumDirectionalError=0;};
    void CheckFilters(const std::vector<GPURecord>& actual,const std::vector<GPURecord>& expected,Report& report)
    {
        Require(actual.size()==expected.size(),"filter output count mismatch");for(size_t index=0;index<actual.size();++index)
        {
            auto a=actual[index],b=expected[index];for(uint32_t coefficient=0;coefficient<4;++coefficient)
            {
                const float error=Error(Color(a.sh[coefficient]),Color(b.sh[coefficient]));report.maximumSHError=std::max(report.maximumSHError,error);
                if(!(error<3e-5f && a.sh[coefficient].w==0)){std::ostringstream message;message<<"filter SH at "<<index<<" coefficient "<<coefficient<<" error "<<error<<" actual "<<a.sh[coefficient].x<<','<<a.sh[coefficient].y<<','<<a.sh[coefficient].z<<" expected "<<b.sh[coefficient].x<<','<<b.sh[coefficient].y<<','<<b.sh[coefficient].z;throw std::runtime_error(message.str());}
                a.sh[coefficient]={};b.sh[coefficient]={};
            }
            if(std::memcmp(&a,&b,sizeof(a))!=0)
            {
                std::ostringstream message;message<<"filter metadata at "<<index<<" actual/expected: "<<a.state<<'/'<<b.state<<" updates "<<a.updateCount<<'/'<<b.updateCount
                    <<" samples "<<a.sampleCount<<'/'<<b.sampleCount<<" frame "<<a.lastUpdateFrame<<'/'<<b.lastUpdateFrame<<" class "<<a.classificationRays<<'/'<<b.classificationRays
                    <<" reason "<<a.reason<<'/'<<b.reason<<" backfaces "<<a.backfaceRatio<<'/'<<b.backfaceRatio<<" min "<<a.minHitDistance<<'/'<<b.minHitDistance;
                throw std::runtime_error(message.str());
            }++report.records;
        }
    }
    void CheckQueries(const VXGIWorld& world,const VXGISDF& sdf,const std::vector<GPURecord>& raw,const std::vector<QueryInput>& inputs,
        const std::vector<QueryResult>& actual,Report& report,bool directional=false)
    {
        Require(inputs.size()==actual.size(),"query count mismatch");for(size_t index=0;index<actual.size();++index)
        {
            const auto expected=QueryReference(world,sdf,raw,inputs[index],directional);const auto& result=actual[index];
            Require(result.candidateCount==expected.candidateCount&&result.acceptedCount==expected.acceptedCount&&result.hasSupport==expected.hasSupport&&result.traceFailures==expected.traceFailures&&result.negativeMask==expected.negativeMask,
                "query support/state/negative mask differs from independent CPU reference");
            const float error=std::max({Error(result.irradiance,expected.irradiance),Error(result.unclamped,expected.unclamped),std::abs(result.supportWeight-expected.supportWeight)});
            Require(error<3e-5f,"query weights or SH reconstruction differ from CPU");
            if(directional){report.maximumDirectionalError=std::max(report.maximumDirectionalError,error);++report.directionalQueries;}
            else{report.maximumQueryError=std::max(report.maximumQueryError,error);++report.queries;}
        }
    }
    struct ProductionReport{uint32_t submissions=0,initialCount=0,movedCount=0,restoredCount=0,warnings=0;bool resizeObservedInFlight=false,sameCountGeometryResetVerified=false,coverageRevisionResetVerified=false;};
    class ProductionFixture
    {
    public:
        struct Capture
        {
            ComPtr<ID3D12Resource> records,masks,lookup,neighbors,raw,staging,materials;
            VXGIProbeState::PendingPlan plan;uint64_t fence=0;int slot=0;
        };
        ProductionFixture()
        {
            WNDCLASSW description{};description.lpfnWndProc=DefWindowProcW;description.hInstance=GetModuleHandleW(nullptr);description.lpszClassName=L"VXGIQueryBackgroundAcceptance";
            Require(RegisterClassW(&description)!=0,"register hidden query fixture failed");window=CreateWindowExW(0,description.lpszClassName,L"VXGI background",WS_POPUP,0,0,64,64,nullptr,nullptr,description.hInstance,nullptr);
            Require(window&&!IsWindowVisible(window),"query fixture must remain hidden");Require(device.Initialize(window,64,64,3),"initialize production RenderDevice failed");
            Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"get production debug queue");scene.Initialize(3);Require(pass.Initialize(device,3),"initialize real filter pass failed");
            Require(probeDebug.Initialize(device)&&sdfDebug.Initialize(device,3),"initialize actual diagnostic renderer PSOs failed");
            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(device.GetDevice()->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&heap)),"create production fixture heap");
            Check(device.GetCommandList()->Close(),"close empty production initialization");
        }
        ~ProductionFixture(){for(int slot=0;slot<3;++slot)if(device.GetFence(slot))device.WaitForPreviousFrame(slot);if(window)DestroyWindow(window);UnregisterClassW(L"VXGIQueryBackgroundAcceptance",GetModuleHandleW(nullptr));}
        Capture Submit(const VXGIWorld& world,const VXGISDF& sdf,const std::vector<GPURecord>& raw,float neighborWeight,uint64_t sourceRevision,uint32_t budget,
            float alphaFactor=1,float redFactor=1)
        {
            Require(!IsWindowVisible(window),"background fixture became visible");const int slot=int(submissions%3);device.WaitForPreviousFrame(slot);
            Require(topology.Update(world),topology.GetLastError());parameters.probesPerFrame=budget;
            std::vector<VXGISurface::MaterialRecord> materialRecords(1);
            materialRecords[0].baseColorFactor={redFactor,1,1,alphaFactor};
            VXGISurface::Parameters surfaceParameters;
            Require(surface.Update(materialRecords,world.GetGeometryRevision(),sdf.GetGeometryRevision(),world.GetMaterialRevision(),
                surfaceParameters,world.GetCoverageRevision()),surface.GetLastError());
            const VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),sdf.GetGeometryRevision(),sourceRevision,raw.size(),surface.GetCoverageRevision()};
            Require(state.Prepare(sources,parameters),state.GetLastError());const auto plan=state.GetPendingPlan();
            if(plan.resetMode==1 && submissions)for(int other=0;other<3;++other)resizeObservedInFlight|=device.GetFence(other)->GetCompletedValue()<device.GetFenceValue(other);
            Check(device.GetCommandAllocator(slot)->Reset(),"reset production query allocator");Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset production query list");
            Require(scene.Prepare(device,slot,world,sdf,&topology),"prepare real topology/SDF GPU scene failed");
            Capture capture;capture.plan=plan;capture.slot=slot;capture.staging=Buffer(raw.size()*sizeof(GPURecord),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;const D3D12_RANGE noReads{0,0};Check(capture.staging->Map(0,&noReads,&mapped),"map production raw fixture");std::memcpy(mapped,raw.data(),raw.size()*sizeof(GPURecord));capture.staging->Unmap(0,nullptr);
            capture.raw=Buffer(raw.size()*sizeof(GPURecord),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
            Transition(capture.raw.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);device.GetCommandList()->CopyResource(capture.raw.Get(),capture.staging.Get());
            Transition(capture.raw.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);
            capture.materials=Buffer(sizeof(VXGISurface::MaterialRecord),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            Check(capture.materials->Map(0,&noReads,&mapped),"map production query materials");
            std::memcpy(mapped,materialRecords.data(),sizeof(VXGISurface::MaterialRecord));capture.materials->Unmap(0,nullptr);
            Require(pass.Record(device,heap.Get(),slot,scene,sdf,capture.raw.Get(),state,world.GetVoxelSize(),neighborWeight,surface,capture.materials.Get()),"real filter RDG recording failed");
            Require(pass.GetFilteredRecords()->GetDesc().Width==raw.size()*sizeof(GPURecord)&&pass.GetVisibilityMasks()->GetDesc().Width==raw.size()*sizeof(uint32_t),"production derived buffers did not resize with sparse allocation");
            capture.records=Copy(pass.GetFilteredRecords());capture.masks=Copy(pass.GetVisibilityMasks());capture.lookup=Copy(scene.Get(slot,5));capture.neighbors=Copy(scene.Get(slot,6));
            Check(device.GetCommandList()->Close(),"close production filter commands");ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);
            capture.fence=++device.GetFenceValue(slot);Check(device.GetCommandQueue()->Signal(device.GetFence(slot),capture.fence),"signal production filter fence");
            Require(state.CommitSubmitted(plan),state.GetLastError());Require(pass.OnSubmitted(slot,capture.fence),"filter submission accounting failed");++submissions;return capture;
        }
        GPU::Output Read(const Capture& capture,const VXGIWorld& world)
        {
            Check(device.GetFence(capture.slot)->SetEventOnCompletion(capture.fence,device.GetFenceEvent()),"arm production filter fence");
            Require(WaitForSingleObject(device.GetFenceEvent(),30000)==WAIT_OBJECT_0,"production filter fence timed out");Require(SUCCEEDED(device.GetDevice()->GetDeviceRemovedReason()),"production filter GPU removed");
            VXGIProbeTopology expected;Require(expected.Update(world),expected.GetLastError());const size_t count=world.GetCells().size();
            const auto actualLookup=ReadBuffer<VXGIProbeTopology::LookupRecord>(capture.lookup.Get(),count);const auto actualNeighbors=ReadBuffer<VXGIProbeTopology::NeighborRecord>(capture.neighbors.Get(),count);
            Require(std::memcmp(actualLookup.data(),expected.GetLookup().data(),count*sizeof(actualLookup[0]))==0 &&
                std::memcmp(actualNeighbors.data(),expected.GetNeighbors().data(),count*sizeof(actualNeighbors[0]))==0,"actual scene upload retained stale topology or altered compact indices");
            return {ReadBuffer<uint32_t>(capture.masks.Get(),count),ReadBuffer<GPURecord>(capture.records.Get(),count),{}};
        }
        uint32_t Warnings()
        {
            for(int slot=0;slot<3;++slot)device.WaitForPreviousFrame(slot);uint32_t warnings=0;
            for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T size=0;Check(info->GetMessage(index,nullptr,&size),"measure production debug message");std::vector<char> storage(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(index,message,&size),"read production debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<"Production D3D12: "<<message->pDescription<<'\n';++warnings;}
            }return warnings;
        }
        uint32_t submissions=0;bool resizeObservedInFlight=false;VXGIProbeState state;VXGIProbeState::Parameters parameters;VXGIProbeFilterPass pass;
    private:
        template<class T> static std::vector<T> ReadBuffer(ID3D12Resource* resource,size_t count)
        {
            std::vector<T> result(count);void* mapped=nullptr;const D3D12_RANGE reads{0,count*sizeof(T)};Check(resource->Map(0,&reads,&mapped),"map production filter output");
            std::memcpy(result.data(),mapped,reads.End);const D3D12_RANGE noWrites{0,0};resource->Unmap(0,&noWrites);return result;
        }
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initialState)
        {
            const auto properties=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes);ComPtr<ID3D12Resource> result;
            Check(device.GetDevice()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,initialState,nullptr,IID_PPV_ARGS(&result)),"create production query resource");return result;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);device.GetCommandList()->ResourceBarrier(1,&barrier);}
        ComPtr<ID3D12Resource> Copy(ID3D12Resource* resource)
        {
            auto result=Buffer(resource->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(resource,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            device.GetCommandList()->CopyResource(result.Get(),resource);Transition(resource,D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);return result;
        }
        HWND window=nullptr;RenderDevice device;VXGIGPUScene scene;VXGIProbeTopology topology;VXGISurface surface;VXGIProbeDebugRenderer probeDebug;VXGISDFDebugRenderer sdfDebug;
        ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12DescriptorHeap> heap;
    };
    ProductionReport ProductionAcceptance(Report& report)
    {
        ProductionFixture fixture;ProductionReport result;auto model=ThinSlab();std::vector<TestInstance> instances(1);instances[0].pModel=&model;VXGIWorld world;VXGISDF sdf;
        auto rebuild=[&](){Require(world.Update(instances,1),world.GetLastError());Require(sdf.Update(world,1,128),sdf.GetLastError());};rebuild();
        result.initialCount=uint32_t(world.GetCells().size());const auto initialHash=world.GetStats().geometryFingerprint;
        auto colors=[&](){std::vector<GPURecord> records;for(const auto center:world.GetOccupiedCenters())records.push_back(ConstantRecord(center.x<0?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1}));return records;};
        auto raw=colors();auto capture=fixture.Submit(world,sdf,raw,.25f,1,4096);auto output=fixture.Read(capture,world);const auto originalMasks=VisibilityReference(world,sdf);
        Require(capture.plan.resetMode==1&&output.visibility==originalMasks,"production first frame did not fully initialize edge visibility");CheckFilters(output.filtered,FilterReference(world,raw,originalMasks),report);
        capture=fixture.Submit(world,sdf,raw,.5f,1,1);output=fixture.Read(capture,world);Require(capture.plan.resetMode==0&&output.visibility==originalMasks,"weight change reset geometry-only edges");
        auto expected=std::vector<GPURecord>(raw.size());const auto all=FilterReference(world,raw,originalMasks,.5f);expected[capture.plan.start]=all[capture.plan.start];CheckFilters(output.filtered,expected,report);
        for(auto& record:raw){for(auto& sh:record.sh)sh={};record.updateCount=record.sampleCount=record.lastUpdateFrame=0;}
        capture=fixture.Submit(world,sdf,raw,.5f,2,1);output=fixture.Read(capture,world);Require(capture.plan.resetMode==2&&output.visibility==originalMasks,"lighting reset invalidated geometric visibility cache");
        CheckFilters(output.filtered,std::vector<GPURecord>(raw.size()),report);
        raw.assign(raw.size(),ConstantRecord({.2f,.7f,.4f}));capture=fixture.Submit(world,sdf,raw,.5f,2,4096);output=fixture.Read(capture,world);
        Require(output.visibility==originalMasks,"source recovery changed edge geometry");expected=FilterReference(world,raw,originalMasks,.5f);
        for(uint32_t index=0;index<expected.size();++index)if(index<capture.plan.start||index>=capture.plan.start+capture.plan.count)expected[index]=GPURecord{};
        CheckFilters(output.filtered,expected,report);
        instances[0].translation={.17f,.125f,.11f};rebuild();result.movedCount=uint32_t(world.GetCells().size());Require(result.movedCount!=result.initialCount,"production move did not change actual sparse allocation count");
        raw=colors();capture=fixture.Submit(world,sdf,raw,.5f,2,4096);output=fixture.Read(capture,world);const auto movedMasks=VisibilityReference(world,sdf);
        Require(capture.plan.resetMode==1&&output.visibility==movedMasks,"geometry reset reused obsolete cached edge bits");CheckFilters(output.filtered,FilterReference(world,raw,movedMasks,.5f),report);
        instances[0].translation={};rebuild();result.restoredCount=uint32_t(world.GetCells().size());Require(result.restoredCount==result.initialCount&&world.GetStats().geometryFingerprint==initialHash,"restoration did not recover actual initial allocation");
        raw=colors();capture=fixture.Submit(world,sdf,raw,.5f,2,4096);output=fixture.Read(capture,world);Require(capture.plan.resetMode==1&&output.visibility==originalMasks,"restoration did not rebuild initial edge visibility");CheckFilters(output.filtered,FilterReference(world,raw,originalMasks,.5f),report);
        capture=fixture.Submit(world,sdf,raw,.5f,2,4096);output=fixture.Read(capture,world);Require(capture.plan.resetMode==0&&output.visibility==originalMasks,"stable production edge cache changed on slot reuse");CheckFilters(output.filtered,FilterReference(world,raw,originalMasks,.5f),report);
        // Change geometry inside the existing cells: the capacity and probe IDs
        // stay fixed, while a previously clear edge becomes blocked. This must
        // invalidate cached known bits even without allocating new buffers.
        auto corner=ThreePoint();std::vector<TestInstance> cornerInstances(1);cornerInstances[0].pModel=&corner;
        VXGIWorld cornerWorld;VXGISDF cornerSDF;auto updateCorner=[&](){Require(cornerWorld.Update(cornerInstances,1),cornerWorld.GetLastError());Require(cornerSDF.Update(cornerWorld,1,128),cornerSDF.GetLastError());};updateCorner();
        const auto cornerCount=cornerWorld.GetCells().size();const auto cornerCenters=cornerWorld.GetOccupiedCenters();const auto cornerMasks=VisibilityReference(cornerWorld,cornerSDF);
        const int32_t witness=cornerWorld.FindCell(VXGIWorld::Coordinate{-1,0,0});Require(witness>=0&&(cornerMasks[witness]&2u)!=0,"same-capacity fixture must initially have a clear cross-wall edge");
        std::vector<GPURecord> cornerRaw;for(const auto center:cornerCenters)cornerRaw.push_back(ConstantRecord(center.x<0?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1}));
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,2,4096);output=fixture.Read(capture,cornerWorld);
        Require(output.visibility==cornerMasks,"initial production corner masks incorrect");CheckFilters(output.filtered,FilterReference(cornerWorld,cornerRaw,cornerMasks,.5f),report);
        corner.meshes[1].vertices[1].Position.y=.8f;corner.meshes[1].vertices[2].Position.y=.8f;cornerWorld.Invalidate();updateCorner();
        Require(cornerWorld.GetCells().size()==cornerCount&&std::memcmp(cornerWorld.GetOccupiedCenters().data(),cornerCenters.data(),cornerCount*sizeof(XMFLOAT3))==0,"raising the corner wall must retain allocation capacity and centers");
        const auto blockedMasks=VisibilityReference(cornerWorld,cornerSDF);Require((blockedMasks[witness]&2u)==0,"raised wall must block the clear-edge witness");
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,2,4096);output=fixture.Read(capture,cornerWorld);
        Require(capture.plan.resetMode==1&&output.visibility==blockedMasks,"same-capacity geometry reset retained stale known/visible bits");CheckFilters(output.filtered,FilterReference(cornerWorld,cornerRaw,blockedMasks,.5f),report);
        corner.meshes[1].vertices[1].Position.y=.2f;corner.meshes[1].vertices[2].Position.y=.2f;cornerWorld.Invalidate();updateCorner();
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,2,4096);output=fixture.Read(capture,cornerWorld);
        Require(capture.plan.resetMode==1&&output.visibility==cornerMasks,"same-capacity restoration retained stale blocked edge bits");CheckFilters(output.filtered,FilterReference(cornerWorld,cornerRaw,cornerMasks,.5f),report);
        result.sameCountGeometryResetVerified=true;
        // Opaque geometry isolates automatic coverage-revision scheduling:
        // coverage edits clear every cached known bit, then only this one
        // scheduled edge is restored. Alpha-aware geometry itself has a
        // separate real-texture fixture with known hole/covered layers.
        auto* unchangedEdges=fixture.pass.GetVisibilityMasks();auto* unchangedFiltered=fixture.pass.GetFilteredRecords();
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,2,1,0);output=fixture.Read(capture,cornerWorld);
        Require(capture.plan.resetMode==1&&capture.plan.start==0&&capture.plan.count==1&&
            fixture.pass.GetVisibilityMasks()==unchangedEdges&&fixture.pass.GetFilteredRecords()==unchangedFiltered,
            "coverage revision failed to reset same-capacity actual FilterPass");
        for(size_t index=0;index<output.visibility.size();++index)
            Require(output.visibility[index]==(index==0?cornerMasks[index]:0u),"coverage reset retained an unscheduled cached edge");
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,2,4096,1);output=fixture.Read(capture,cornerWorld);
        Require(capture.plan.resetMode==1&&output.visibility==cornerMasks,"restored coverage failed to rebuild cached actual FilterPass edges");
        capture=fixture.Submit(cornerWorld,cornerSDF,cornerRaw,.5f,3,1,1,.2f);output=fixture.Read(capture,cornerWorld);
        Require(capture.plan.resetMode==2&&output.visibility==cornerMasks,"RGB-only actual FilterPass edit discarded known coverage edges");
        result.coverageRevisionResetVerified=true;
        Require(std::isfinite(fixture.pass.GetGPUMilliseconds())&&fixture.pass.GetGPUMilliseconds()>=0,"production filter GPU timestamps invalid");
        result.submissions=fixture.submissions;result.resizeObservedInFlight=fixture.resizeObservedInFlight;result.warnings=fixture.Warnings();Require(result.warnings==0,"production filter debug/GBV emitted warnings/errors");return result;
    }
}
int main()
{
    try
    {
        std::ofstream("tmp/VXGIProbeQueryGPU-report.json")<<"{\"passed\":false,\"status\":\"running\"}\n";
        GPU gpu;Report report;std::vector<std::string> groups;
        auto success=[&](const char* name){groups.push_back(name);std::cout<<name<<": passed\n";};
        auto slab=ThinSlab(),three=ThreePoint();VXGIWorld slabWorld,threeWorld;VXGISDF slabSDF,threeSDF;Build(slab,slabWorld,slabSDF);Build(three,threeWorld,threeSDF);
        const auto slabMasks=VisibilityReference(slabWorld,slabSDF),threeMasks=VisibilityReference(threeWorld,threeSDF);
        std::vector<GPURecord> raw;for(const auto center:slabWorld.GetOccupiedCenters())raw.push_back(ConstantRecord(center.x<0?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1}));
        const std::vector<QueryInput> slabQueries={SurfaceQuery(slabSDF,{-.01f,.13f,.27f},{-1,0,0}),SurfaceQuery(slabSDF,{.01f,.17f,.29f},{1,0,0})};
        auto output=gpu.Run(slabWorld,slabSDF,raw,slabQueries);Require(output.visibility==slabMasks,"thin slab cached edges differ from CPU closed segments");
        CheckFilters(output.filtered,FilterReference(slabWorld,raw,slabMasks),report);CheckQueries(slabWorld,slabSDF,raw,slabQueries,output.queries,report);
        Require(Error(output.queries[0].irradiance,{1,0,0})<1e-5f&&Error(output.queries[1].irradiance,{0,0,1})<1e-5f,"opposite-side colors crossed the thin wall");
        success("thin_wall_two_colors_closed_edges_and_individual_surface_visibility");
        raw.assign(threeWorld.GetCells().size(),GPURecord{});const auto a=threeWorld.FindCell(VXGIWorld::Coordinate{-1,0,0}),b=threeWorld.FindCell(VXGIWorld::Coordinate{0,0,0});
        Require(a>=0&&b>=0,"three-point probes must be actually allocated");raw[a]=ConstantRecord({1,0,0});raw[b]=ConstantRecord({0,0,1});
        const std::vector<QueryInput> threeQueries={SurfaceQuery(threeSDF,{-.05f,0,.13f},{0,1,0})};output=gpu.Run(threeWorld,threeSDF,raw,threeQueries);
        Require(output.visibility==threeMasks,"three-point cached visibility differs from CPU");CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);
        CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report);
        Require((output.visibility[a]&2u)!=0&&output.filtered[a].sh[0].z>0&&output.queries[0].acceptedCount==1&&Error(output.queries[0].irradiance,{1,0,0})<1e-5f,
            "query borrowed blue through a clear anchor edge even though the surface path was blocked");
        success("three_point_clear_probe_pair_blocked_surface_path_uses_raw_contributors");
        raw.clear();for(const auto center:threeWorld.GetOccupiedCenters())raw.push_back(ConstantRecord(center.x<0?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1}));
        const std::vector<QueryInput> cornerQueries={SurfaceQuery(threeSDF,{.05f,0,.91f},{0,1,0})};
        output=gpu.Run(threeWorld,threeSDF,raw,cornerQueries);CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);CheckQueries(threeWorld,threeSDF,raw,cornerQueries,output.queries,report);
        Require(output.queries[0].hasSupport==1&&output.queries[0].irradiance.x>0&&output.queries[0].irradiance.z>0,"finite wall corner incorrectly rejected legal paths around its edge");
        success("finite_wall_corner_retains_verified_paths_around_the_edge");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({.7f,.4f,.2f}));
        std::vector<QueryInput> boundaries={SurfaceQuery(threeSDF,{-.91f,0,.13f},{0,1,0}),SurfaceQuery(threeSDF,{.5f,0,.13f},{0,1,0}),SurfaceQuery(threeSDF,{-.5f,0,.13f},{0,1,0})};
        auto outside=boundaries[0];outside.position={12,0,.13f};boundaries.push_back(outside);
        output=gpu.Run(threeWorld,threeSDF,raw,boundaries);CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);CheckQueries(threeWorld,threeSDF,raw,boundaries,output.queries,report);
        Require(output.queries[0].hasSupport==1&&Error(output.queries[0].irradiance,{.7f,.4f,.2f})<1e-5f&&output.queries.back().candidateCount==0&&output.queries.back().hasSupport==0,"sparse support failed normalization or borrowed a distant fallback");
        success("negative_coordinates_lattice_boundaries_sparse_renormalization_and_no_fallback");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({0,0,0}));output=gpu.Run(threeWorld,threeSDF,raw,threeQueries);CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report);
        Require(output.queries[0].hasSupport==1&&Error(output.queries[0].irradiance,{0,0,0})==0,"black radiance was treated as missing support");success("valid_black_radiance_has_geometric_support");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({100,100,100}));for(size_t index=0;index<raw.size();++index){raw[index].state=uint32_t(index%3);raw[index].updateCount=0;}
        output=gpu.Run(threeWorld,threeSDF,raw,threeQueries);CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report);
        Require(output.queries[0].hasSupport==0,"disabled/unclassified/unupdated records supplied stale SH");success("disabled_unclassified_and_unupdated_probe_records_are_rejected");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({1,1,1}));for(auto& record:raw)record.sh[0].x=std::numeric_limits<float>::quiet_NaN();
        auto badNormal=threeQueries[0],badPosition=threeQueries[0],invalidHit=threeQueries[0];badNormal.mappedNormal.x=std::numeric_limits<float>::quiet_NaN();badPosition.position.x=1e30f;invalidHit.valid=0;
        const std::vector<QueryInput> invalidQueries={threeQueries[0],badNormal,badPosition,invalidHit};output=gpu.Run(threeWorld,threeSDF,raw,invalidQueries);
        CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);CheckQueries(threeWorld,threeSDF,raw,invalidQueries,output.queries,report);
        Require(output.queries[0].hasSupport==0&&output.queries[1].traceFailures==1&&output.queries[2].traceFailures==1&&output.queries[3].traceFailures==1,"invalid data did not fail closed");
        success("nonfinite_SH_invalid_normal_surface_and_lattice_range_fail_closed");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({.1f,.2f,.3f}));for(auto& record:raw)record.sh[1]={3,.5f,-2,0};
        auto positive=threeQueries[0],negative=threeQueries[0];positive.mappedNormal={1,0,0};negative.mappedNormal={-1,0,0};
        const std::vector<QueryInput> signedQueries={positive,negative};output=gpu.Run(threeWorld,threeSDF,raw,signedQueries);
        CheckFilters(output.filtered,FilterReference(threeWorld,raw,threeMasks),report);CheckQueries(threeWorld,threeSDF,raw,signedQueries,output.queries,report);
        Require(output.queries[0].negativeMask==4&&output.queries[1].negativeMask==1&&output.queries[0].acceptedCount==output.queries[1].acceptedCount,
            "signed SH was clamped before consumption or mapped normal chose the geometric side");success("signed_SH_filter_and_mapped_normal_reconstruction_clamp_only_at_consumption");
        for(const uint32_t mode:{1u,2u})
        {
            output=gpu.Run(threeWorld,threeSDF,raw,threeQueries,mode);std::vector<GPURecord> cleared=raw;
            for(auto& record:cleared){if(mode==1)record=GPURecord{};else{for(auto& sh:record.sh)sh={};record.updateCount=record.sampleCount=record.lastUpdateFrame=0;}}
            CheckFilters(output.filtered,std::vector<GPURecord>(cleared.size()),report);CheckQueries(threeWorld,threeSDF,cleared,threeQueries,output.queries,report);Require(output.queries[0].hasSupport==0,"reset queries saw old SH support");
        }
        output=gpu.Run(threeWorld,threeSDF,raw,threeQueries);CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report);Require(output.queries[0].hasSupport==1,"updated SH did not restore query support");
        success("geometry_and_SH_reset_queries_reject_cleared_history_then_recover");

        // Directional cache reads use all four signed coefficients in the
        // ORIGINAL ray direction. They must not apply cosine convolution or
        // use the hit's mapped/geometric normal as the reconstruction axis.
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({.1f,.2f,.3f}));
        for(auto& record:raw){record.sh[1]={3,.5f,-2,0};record.sh[2]={-.8f,1.25f,.1f,0};record.sh[3]={.2f,-.4f,.6f,0};}
        const std::vector<XMFLOAT3> directions={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{8,0,0},{1,2,-3},{-1,-2,3}};
        std::vector<QueryInput> directionalQueries;
        for(const auto direction:directions){auto input=threeQueries[0];input.mappedNormal=direction;directionalQueries.push_back(input);}
        output=gpu.Run(threeWorld,threeSDF,raw,directionalQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,directionalQueries,output.queries,report,true);
        Require(output.queries[0].negativeMask==4&&output.queries[1].negativeMask==3,
            "directional cache read flipped the original ray or clamped signed coefficients early");
        Require(Error(output.queries[0].unclamped,output.queries[6].unclamped)<1e-6f,
            "directional query did not normalize the finite ray direction");
        const auto convolved=QueryReference(threeWorld,threeSDF,raw,directionalQueries[0]);
        Require(output.queries[0].unclamped.x>1.5f&&Error(output.queries[0].unclamped,convolved.unclamped)>.4f,
            "directional query incorrectly used diffuse irradiance's 2/3 coefficient");
        for(const auto& result:output.queries)Require(result.hasSupport==1&&result.acceptedCount==output.queries[0].acceptedCount,
            "reconstruction direction changed cache support");
        success("directional_signed_four_coefficient_SH_original_ray_axes_oblique_and_no_cosine_convolution");

        std::vector<GPURecord> slabRadiance;
        for(const auto center:slabWorld.GetOccupiedCenters())slabRadiance.push_back(ConstantRecord(center.x<0?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1}));
        output=gpu.Run(slabWorld,slabSDF,slabRadiance,slabQueries,0,true);
        CheckQueries(slabWorld,slabSDF,slabRadiance,slabQueries,output.queries,report,true);
        Require(output.queries[0].irradiance.x>0&&output.queries[0].irradiance.z>0
            &&output.queries[1].irradiance.x>0&&output.queries[1].irradiance.z>0,
            "directional cache lookup incorrectly rejected stored contributions using the thin wall");
        auto changedHitInputs=slabQueries;
        for(auto& input:changedHitInputs)
        {
            input.geometricNormal={std::numeric_limits<float>::quiet_NaN(),0,0};input.valid=0;input.traceStatus=3;
            input.triangleIndex=input.triangleId=UINT32_MAX;input.backFace^=1;input.distance=std::numeric_limits<float>::quiet_NaN();
        }
        // Keep the lattice and cached records, but replace all geometry/SDF
        // resources. This must not affect a geometry-free cache read.
        const auto changedGeometryOutput=gpu.Run(slabWorld,threeSDF,slabRadiance,changedHitInputs,0,true);
        CheckQueries(slabWorld,threeSDF,slabRadiance,changedHitInputs,changedGeometryOutput.queries,report,true);
        Require(std::memcmp(output.queries.data(),changedGeometryOutput.queries.data(),output.queries.size()*sizeof(QueryResult))==0,
            "directional cache lookup depended on geometry, hit identity, normals, or visibility");
        success("directional_cache_ignores_geometry_hit_identity_normals_and_secondary_visibility");

        raw.assign(threeWorld.GetCells().size(),GPURecord{});
        raw[a]=ConstantRecord({1,0,0});raw[b]=ConstantRecord({0,0,1});
        output=gpu.Run(threeWorld,threeSDF,raw,threeQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report,true);
        Require(output.queries[0].acceptedCount==2&&std::abs(output.queries[0].supportWeight-.315f)<2e-6f
            &&Error(output.queries[0].irradiance,{.55f,0,.45f})<2e-6f,
            "sparse cache trilinear weights or renormalization differ from the analytic two-record mixture");
        raw[a].state=2;
        output=gpu.Run(threeWorld,threeSDF,raw,threeQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report,true);
        Require(output.queries[0].acceptedCount==1&&Error(output.queries[0].irradiance,{0,0,1})<1e-6f,
            "disabled sparse contributor was not removed before renormalization");
        raw[b].sh[2].x=std::numeric_limits<float>::quiet_NaN();
        output=gpu.Run(threeWorld,threeSDF,raw,threeQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,threeQueries,output.queries,report,true);
        Require(output.queries[0].candidateCount==8&&output.queries[0].acceptedCount==0&&output.queries[0].hasSupport==0,
            "unclassified, disabled, or nonfinite cache records supplied stale radiance");
        success("directional_sparse_analytic_weights_disabled_nonfinite_states_and_renormalization");

        raw.clear();for(const auto center:threeWorld.GetOccupiedCenters())
        {
            auto record=ConstantRecord(center.x<0?XMFLOAT3{.2f,.1f,.3f}:XMFLOAT3{.1f,.4f,.2f});
            record.sh[1]=center.x<0?XMFLOAT4{3,-2,.3f,0}:XMFLOAT4{-1,.4f,1,0};
            record.sh[2]={.5f,-.1f,.8f,0};record.sh[3]={-.2f,.3f,.1f,0};raw.push_back(record);
        }
        auto directionalCorner=cornerQueries[0];directionalCorner.mappedNormal={1,2,3};
        const std::vector<QueryInput> mixedQueries={directionalCorner};
        output=gpu.Run(threeWorld,threeSDF,raw,mixedQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,mixedQueries,output.queries,report,true);
        Require(output.queries[0].acceptedCount>=2&&output.queries[0].hasSupport==1,
            "directional mix fixture did not retain multiple cached contributors");
        VXGISurface::MaterialRecord changedMaterial{};changedMaterial.baseColorFactor={0,0,0,0};changedMaterial.isUnlit=1;
        changedMaterial.emissiveFactor={1000,2000,3000};changedMaterial.albedoIdx=changedMaterial.normalIdx=changedMaterial.ormIdx=changedMaterial.emissiveIdx=UINT32_MAX;
        const auto changedOutput=gpu.Run(threeWorld,threeSDF,raw,mixedQueries,0,true,&changedMaterial);
        CheckQueries(threeWorld,threeSDF,raw,mixedQueries,changedOutput.queries,report,true);
        Require(std::memcmp(output.queries.data(),changedOutput.queries.data(),sizeof(QueryResult))==0,
            "opaque hit material/albedo/emission affected a cached GI query");
        success("directional_raw_SH_trilinear_mix_ignores_opaque_hit_material_and_emission");

        raw.assign(threeWorld.GetCells().size(),ConstantRecord({0,0,0}));
        output=gpu.Run(threeWorld,threeSDF,raw,mixedQueries,0,true,&changedMaterial);
        CheckQueries(threeWorld,threeSDF,raw,mixedQueries,output.queries,report,true);
        Require(output.queries[0].hasSupport==1&&Error(output.queries[0].irradiance,{0,0,0})==0,
            "black cached GI was replaced by hit emission or treated as missing");
        success("directional_valid_black_cache_retains_support_without_hit_lighting");

        raw.assign(threeWorld.GetCells().size(),ConstantRecord({.7f,.4f,.2f}));
        auto zeroDirection=mixedQueries[0],nanDirection=mixedQueries[0],infiniteDirection=mixedQueries[0];
        zeroDirection.mappedNormal={};nanDirection.mappedNormal.x=std::numeric_limits<float>::quiet_NaN();
        infiniteDirection.mappedNormal.z=std::numeric_limits<float>::infinity();
        auto nonfinitePosition=mixedQueries[0];nonfinitePosition.position.y=std::numeric_limits<float>::quiet_NaN();
        const std::vector<QueryInput> invalidDirections={zeroDirection,nanDirection,infiniteDirection,nonfinitePosition,badPosition,outside};
        output=gpu.Run(threeWorld,threeSDF,raw,invalidDirections,0,true);
        CheckQueries(threeWorld,threeSDF,raw,invalidDirections,output.queries,report,true);
        for(const auto& result:output.queries)Require(result.hasSupport==0,"invalid directional data or missing support produced radiance");
        for(auto& record:raw){record.state=2;record.updateCount=3;}
        output=gpu.Run(threeWorld,threeSDF,raw,mixedQueries,0,true);
        CheckQueries(threeWorld,threeSDF,raw,mixedQueries,output.queries,report,true);
        Require(output.queries[0].hasSupport==0,"disabled records supplied directional radiance");
        raw.assign(threeWorld.GetCells().size(),ConstantRecord({.7f,.4f,.2f}));
        for(const uint32_t mode:{1u,2u})
        {
            output=gpu.Run(threeWorld,threeSDF,raw,mixedQueries,mode,true);std::vector<GPURecord> cleared=raw;
            for(auto& record:cleared){if(mode==1)record=GPURecord{};else{for(auto& sh:record.sh)sh={};record.updateCount=record.sampleCount=record.lastUpdateFrame=0;}}
            CheckQueries(threeWorld,threeSDF,cleared,mixedQueries,output.queries,report,true);
            Require(output.queries[0].hasSupport==0,"reset records supplied directional radiance");
        }
        success("directional_zero_nonfinite_missing_disabled_and_reset_inputs_fail_closed");

        const auto production=ProductionAcceptance(report);success("production_RDG_three_slot_weight_source_resets_geometry_move_resize_restore_and_diagnostic_PSOs");
        report.groups=uint32_t(groups.size());report.warnings=gpu.Warnings();Require(report.warnings==0,"D3D12 debug/GBV emitted warnings or errors");
        std::ofstream file("tmp/VXGIProbeQueryGPU-report.json");file<<std::setprecision(9)<<"{\n  \"adapter\":\""<<gpu.adapterName<<"\",\n  \"debug_layer\":true,\n  \"gpu_based_validation\":true,\n  \"production_source\":\"CSResetFilter / CSVisibility / CSFilter + VXGIProbeQuerySurface + VXGIProbeQueryRadiance\",\n  \"group_count\":"<<report.groups<<",\n  \"filter_records_compared\":"<<report.records<<",\n  \"surface_queries_compared\":"<<report.queries<<",\n  \"directional_radiance_queries_compared\":"<<report.directionalQueries<<",\n  \"maximum_signed_SH_filter_error\":"<<report.maximumSHError<<",\n  \"maximum_query_irradiance_weight_error\":"<<report.maximumQueryError<<",\n  \"maximum_directional_radiance_weight_error\":"<<report.maximumDirectionalError<<",\n  \"d3d12_warnings_errors\":"<<report.warnings<<",\n  \"passed\":true,\n  \"test_groups\":[";
        for(size_t index=0;index<groups.size();++index)file<<(index?",":"")<<'\n'<<"    \""<<groups[index]<<"\"";
        file<<"\n  ],\n  \"production_wrapper\":{\n    \"classes\":\"RenderDevice + VXGIGPUScene + VXGIProbeTopology + VXGIProbeFilterPass + RDGBuilder\",\n    \"frame_slots\":3,\n    \"submissions\":"<<production.submissions
            <<",\n    \"initial_probe_count\":"<<production.initialCount<<",\n    \"moved_probe_count\":"<<production.movedCount<<",\n    \"restored_probe_count\":"<<production.restoredCount
            <<",\n    \"source_input\":\"known signed SH uploaded as immutable raw fixture\",\n    \"diagnostic_renderer_PSOs_created\":true,\n    \"window_visible\":false,\n    \"present_calls\":0,\n    \"resize_observed_in_flight\":"<<(production.resizeObservedInFlight?"true":"false")
            <<",\n    \"same_capacity_geometry_edge_invalidation_verified\":"<<(production.sameCountGeometryResetVerified?"true":"false")
            <<",\n    \"coverage_revision_edge_invalidation_verified\":"<<(production.coverageRevisionResetVerified?"true":"false")
            <<",\n    \"d3d12_warnings_errors\":"<<production.warnings<<"\n  }\n}\n";
        std::cout<<report.groups<<" groups passed; "<<report.records<<" filter records / "<<report.queries<<" surface queries / "<<report.directionalQueries<<" directional queries; errors "<<report.maximumSHError<<" / "<<report.maximumQueryError<<" / "<<report.maximumDirectionalError<<"; 0 warnings/errors.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::ofstream("tmp/VXGIProbeQueryGPU-report.json")<<"{\"passed\":false}\n";return 1;}
}
