// Explicit alpha fixtures test the production material-aware traversal/helpers.
// No CPU geometric trace is used as an alpha golden. No window is ever shown.
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
#include "../Resources/VXGISurface.h"
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
    void Require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
    void Check(HRESULT value,const char* operation)
    {
        if(FAILED(value)){std::ostringstream text;text<<operation<<": HRESULT 0x"<<std::hex<<uint32_t(value);throw std::runtime_error(text.str());}
    }
    std::string UTF8(const wchar_t* input)
    {
        const int count=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);std::string text(count,'\0');
        WideCharToMultiByte(CP_UTF8,0,input,-1,text.data(),count,nullptr,nullptr);text.pop_back();return text;
    }
    struct Vertex
    {
        XMFLOAT3 Position,Normal={0,0,1};XMFLOAT2 TexCoords;
        XMFLOAT3 Tangent={1,0,0},Bitangent={0,1,0};
    };
    struct TextureResource{void* Get() const{return nullptr;}};
    struct Texture{uint32_t type=0;TextureResource Resource;std::string path;};
    struct Mesh
    {
        std::vector<Vertex> vertices;std::vector<std::vector<uint32_t>> lodIndices;uint32_t materialID=0;
        bool isUnlit=false;XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};std::vector<Texture> textures;
    };
    struct Model{std::vector<Mesh> meshes;};
    struct Instance
    {
        uint32_t editorId;Model* pModel;
        XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false;
    };
    struct Plane{float z;bool cutout;uint32_t material;bool invalidUV=false;};
    struct Scene
    {
        std::vector<Model> models;std::vector<Instance> instances;VXGIWorld world;VXGISDF sdf;
        explicit Scene(std::vector<Plane> planes)
        {
            models.reserve(planes.size());instances.reserve(planes.size());
            for(size_t index=0;index<planes.size();++index)
            {
                const auto& plane=planes[index];Mesh mesh;mesh.materialID=plane.material;
                mesh.vertices={{{-2,-2,plane.z},{0,0,1},{0,0}},{{2,-2,plane.z},{0,0,1},{1,0}},
                    {{2,2,plane.z},{0,0,1},{1,1}},{{-2,2,plane.z},{0,0,1},{0,1}}};
                if(plane.invalidUV)for(auto& vertex:mesh.vertices)vertex.TexCoords.x=std::numeric_limits<float>::quiet_NaN();
                mesh.lodIndices={{0,1,2,0,2,3}};models.push_back({{std::move(mesh)}});
                Instance instance{uint32_t(101+index),&models.back()};instance.isCutout=plane.cutout;instances.push_back(instance);
            }
            Require(world.Update(instances,.5f),world.GetLastError());Require(sdf.Update(world,.5f,128),sdf.GetLastError());
        }
    };
    struct Triangle
    {
        XMFLOAT3 p0;uint32_t triangleId;XMFLOAT3 p1;uint32_t flags;XMFLOAT3 p2;uint32_t pad0;
        XMFLOAT3 geometricNormal;uint32_t pad1;std::array<XMFLOAT2,3> uv;
        std::array<XMFLOAT3,3> normals,tangents,bitangents;uint32_t materialId,instanceId,pad2;
    };
    struct Parameters
    {
        XMFLOAT3 origin;float cellSize;std::array<uint32_t,3> cellDimensions;uint32_t probeCount=1;
        XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        float voxelSize=.5f;uint32_t updateStart=0,updateCount=0,resetMode=0;
        float neighborWeight=.25f;uint32_t materialCount;float materialMipLevel=0;uint32_t padding2=0;
        uint32_t testCount,padding[3]={};
    };
    struct Ray
    {
        XMFLOAT3 origin={.2f,-.4f,0};float maximumDistance=5;
        XMFLOAT3 direction={0,0,1};float minimumDistance=.0001f;
        XMFLOAT3 target={.2f,-.4f,2};uint32_t operation=0;
        XMFLOAT3 sunDirection={0,0,1};float mipLevel=0;
        XMFLOAT3 sunRadiance={DirectX::XM_PI,2*DirectX::XM_PI,.5f*DirectX::XM_PI};float shadowMinimum=.0001f;
        float shadowMaximum=5;uint32_t targetTriangle=2,targetBackFace=1,probeIndex=0;
        XMFLOAT3 barycentric={.45f,.15f,.4f};uint32_t reserved=0;
    };
    struct Result
    {
        XMFLOAT3 position;float distance;uint32_t triangleIndex,traceStatus,hitValid,materialId;
        XMFLOAT2 uv;float alpha;uint32_t visible;XMFLOAT3 lo0;float sunVisibility;
        uint32_t sourceValid,shadowTraceStatus,probeState,probeReason;
        float backfaceRatio,minimumHitDistance;uint32_t classificationRays,traceFailures;
        XMFLOAT3 irradiance;float supportWeight;uint32_t acceptedCount,hasSupport,queryFailures,reserved;
    };
    using Material=VXGISurface::MaterialRecord;
    struct Mip{UINT width,height;std::vector<XMFLOAT4> pixels;};
    struct Image{std::vector<Mip> mips;};
    static_assert(sizeof(Triangle)==208 && sizeof(Parameters)==96 && sizeof(Ray)==112 && sizeof(Result)==128);
    Image Constant(XMFLOAT4 color){return {{{1,1,{color}}}};}
    std::vector<Image> Images(float alpha=0)
    {
        return {Constant({1,1,1,alpha}),Constant({.5f,.5f,1,1}),Constant({1,.7f,0,1}),
            Constant({0,0,0,1}),Constant({1,1,1,1})};
    }
    std::vector<Material> Materials(size_t count=2)
    {
        std::vector<Material> values(count);for(auto& material:values){material.albedoIdx=4;material.normalIdx=1;material.ormIdx=2;material.emissiveIdx=3;}
        values[0].albedoIdx=0;return values;
    }
    ComPtr<IDxcBlob> Compile(const wchar_t* path=L"Shaders/VXGIAlphaCutoutValidate.hlsl",const wchar_t* entry=L"CSMain")
    {
        ComPtr<IDxcUtils> utils;ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create alpha DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create alpha DXC compiler");
        ComPtr<IDxcBlobEncoding> source;Check(utils->LoadFile(path,nullptr,&source),"load alpha helper shader");
        ComPtr<IDxcIncludeHandler> include;Check(utils->CreateDefaultIncludeHandler(&include),"create alpha includes");
        std::vector<const wchar_t*> arguments={path,L"-E",entry,L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",L"-WX"};
#if defined(_DEBUG)
        arguments.push_back(L"-Od");arguments.push_back(L"-Zi");arguments.push_back(L"-Qembed_debug");
#else
        arguments.push_back(L"-O3");
#endif
        DxcBuffer input{source->GetBufferPointer(),source->GetBufferSize(),DXC_CP_UTF8};ComPtr<IDxcResult> compiled;
        Check(compiler->Compile(&input,arguments.data(),UINT(arguments.size()),include.Get(),IID_PPV_ARGS(&compiled)),"compile alpha validation shader");
        ComPtr<IDxcBlobUtf8> errors;Check(compiled->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr),"get alpha DXC diagnostics");
        if(errors&&errors->GetStringLength())std::cerr<<errors->GetStringPointer();HRESULT status;Check(compiled->GetStatus(&status),"get alpha compile status");Check(status,"alpha test shader");
        ComPtr<IDxcBlob> blob;Check(compiled->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&blob),nullptr),"get alpha shader bytecode");return blob;
    }
    class GPU
    {
    public:
        GPU()
        {
            WNDCLASSW window{};window.lpfnWndProc=DefWindowProcW;window.hInstance=GetModuleHandleW(nullptr);window.lpszClassName=L"VXGIAlphaBackgroundTest";
            Require(RegisterClassW(&window)!=0,"register hidden alpha HWND");
            hidden=CreateWindowExW(0,window.lpszClassName,L"Alpha background test",WS_POPUP,0,0,16,16,nullptr,nullptr,window.hInstance,nullptr);
            Require(hidden && !IsWindowVisible(hidden),"alpha fixture must remain hidden");
            ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get required D3D debug layer");debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get required GPU validation");validation->SetEnableGPUBasedValidation(TRUE);
            ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create alpha DXGI factory");
            ComPtr<IDXGIAdapter1> adapter;
            for(UINT index=0;factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++index)
            {
                DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"describe alpha adapter");
                if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;
                adapter.Reset();
            }
            Require(device!=nullptr,"alpha tests require hardware DX12");DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"describe selected alpha GPU");adapterName=UTF8(desc.Description);
            Check(device.As(&info),"get alpha InfoQueue");D3D12_COMMAND_QUEUE_DESC queueDesc{};
            Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"create alpha queue");Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create alpha allocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"create alpha commands");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create alpha fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"create alpha event");
            CD3DX12_ROOT_PARAMETER roots[11];roots[0].InitAsConstants(sizeof(Parameters)/4,0);for(UINT index=0;index<9;++index)roots[index+1].InitAsShaderResourceView(index);roots[10].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(11,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> serialized;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"serialize alpha root");
            Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"create alpha root");
            const auto shader=Compile();D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};pipelineDesc.pRootSignature=root.Get();pipelineDesc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)),"create alpha PSO");
            std::cout<<"Actual alpha GPU: "<<adapterName<<"; debug + GBV enabled\n";
        }
        ~GPU(){if(event)CloseHandle(event);if(hidden)DestroyWindow(hidden);UnregisterClassW(L"VXGIAlphaBackgroundTest",GetModuleHandleW(nullptr));}
        std::vector<Result> Dispatch(const Scene& scene,const std::vector<Ray>& rays,const std::vector<Material>& materials,const std::vector<Image>& images,uint32_t materialCount=UINT32_MAX)
        {
            Require(!IsWindowVisible(hidden),"alpha HWND became visible");UploadTextures(images);
            std::vector<Triangle> packed;for(uint32_t index=0;index<scene.sdf.GetTriangles().size();++index)
            {
                const auto& source=scene.sdf.GetTriangles()[index];Triangle t{};t.p0=source.positions[0];t.p1=source.positions[1];t.p2=source.positions[2];t.triangleId=index;t.flags=source.isCutout;
                t.geometricNormal=source.geometricNormal;t.uv=source.uv;t.normals=source.shadingNormals;t.tangents=source.tangents;t.bitangents=source.bitangents;t.materialId=source.materialId;t.instanceId=source.instanceId;packed.push_back(t);
            }
            std::vector<XMFLOAT3> centers={{.25f,-.25f,1.75f}};std::vector<VXGIProbeTopology::LookupRecord> lookup={{{0,-1,3},0}};
            VXGIProbeState::GPURecord record;record.state=VXGIProbeState::Valid;record.updateCount=1;record.sh[0]={.2f/.28209479177387814f,.4f/.28209479177387814f,.6f/.28209479177387814f,0};
            auto nodes=Upload(scene.sdf.GetNodes()),offsets=Upload(scene.sdf.GetCellOffsets()),references=Upload(scene.sdf.GetTriangleReferences());
            auto triangles=Upload(packed),materialBuffer=Upload(materials),rayBuffer=Upload(rays),centerBuffer=Upload(centers),lookupBuffer=Upload(lookup),recordBuffer=Upload(std::vector<VXGIProbeState::GPURecord>{record});
            const uint64_t bytes=rays.size()*sizeof(Result);auto output=Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);auto readback=Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            Parameters p;const auto& grid=scene.sdf.GetGrid();p.origin=grid.origin;p.cellSize=grid.cellSize;p.cellDimensions=grid.cellDimensions;p.boundsMaximum=grid.boundsMaximum;p.triangleCount=uint32_t(packed.size());p.testCount=uint32_t(rays.size());p.materialCount=materialCount==UINT32_MAX?uint32_t(materials.size()):materialCount;
            ID3D12DescriptorHeap* heaps[]={textureHeap.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipeline.Get());commands->SetComputeRoot32BitConstants(0,sizeof(p)/4,&p,0);
            const std::array<ID3D12Resource*,9> inputs={nodes.Get(),offsets.Get(),references.Get(),triangles.Get(),materialBuffer.Get(),rayBuffer.Get(),centerBuffer.Get(),lookupBuffer.Get(),recordBuffer.Get()};
            for(UINT index=0;index<inputs.size();++index)commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(10,output->GetGPUVirtualAddress());
            Transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->Dispatch((UINT(rays.size())+63)/64,1,1);
            Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(readback.Get(),output.Get());
            Check(commands->Close(),"close alpha dispatch");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),++fenceValue),"signal alpha fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm alpha fence");
            Require(WaitForSingleObject(event,60000)==WAIT_OBJECT_0,"alpha GPU timeout");Check(device->GetDeviceRemovedReason(),"alpha GPU removed reason");
            std::vector<Result> result(rays.size());void* mapped=nullptr;const D3D12_RANGE reads{0,SIZE_T(bytes)};Check(readback->Map(0,&reads,&mapped),"map alpha results");std::memcpy(result.data(),mapped,size_t(bytes));const D3D12_RANGE writes{0,0};readback->Unmap(0,&writes);
            Check(allocator->Reset(),"reset alpha allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset alpha commands");textureUploads.clear();++submissions;raysCompared+=uint32_t(rays.size());return result;
        }
        uint32_t Warnings()
        {
            uint32_t count=0;for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T size=0;Check(info->GetMessage(index,nullptr,&size),"measure alpha debug message");std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(index,message,&size),"read alpha debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';++count;}
            }return count;
        }
        void TestFilterCache()
        {
            // Persistent output allocations survive every material edit. Only
            // the requested reset mode changes; geometry, centers and count do
            // not. CPU revision scheduling is covered by its separate tests.
            Scene scene({{1,true,0}});std::vector<Triangle> packed;
            for(uint32_t index=0;index<scene.sdf.GetTriangles().size();++index)
            {
                const auto& source=scene.sdf.GetTriangles()[index];Triangle t{};t.p0=source.positions[0];t.p1=source.positions[1];t.p2=source.positions[2];t.triangleId=index;t.flags=source.isCutout;
                t.geometricNormal=source.geometricNormal;t.uv=source.uv;t.normals=source.shadingNormals;t.tangents=source.tangents;t.bitangents=source.bitangents;t.materialId=source.materialId;t.instanceId=source.instanceId;packed.push_back(t);
            }
            auto nodes=Upload(scene.sdf.GetNodes()),offsets=Upload(scene.sdf.GetCellOffsets()),references=Upload(scene.sdf.GetTriangleReferences()),triangles=Upload(packed);
            auto centers=Upload(std::vector<XMFLOAT3>{{.25f,-.25f,.75f},{.25f,-.25f,1.25f}});
            auto lookup=Upload(std::vector<VXGIProbeTopology::LookupRecord>{{{0,-1,1},0},{{0,-1,2},1}});
            std::vector<VXGIProbeTopology::NeighborRecord> neighbors(2);neighbors[0].indices[5]=1;neighbors[1].indices[4]=0;
            auto neighborBuffer=Upload(neighbors);std::vector<VXGIProbeState::GPURecord> raw(2);
            for(auto& record:raw){record.state=1;record.updateCount=1;record.sampleCount=64;}
            raw[0].sh[0]={1,0,0,0};raw[1].sh[0]={0,1,0,0};auto rawBuffer=Upload(raw);
            auto edges=Buffer(8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto filtered=Buffer(2*sizeof(VXGIProbeState::GPURecord),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            Transition(edges.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Transition(filtered.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            CD3DX12_ROOT_PARAMETER roots[13];roots[0].InitAsConstants(20,0);for(UINT index=0;index<10;++index)roots[index+1].InitAsShaderResourceView(index);
            roots[11].InitAsUnorderedAccessView(0);roots[12].InitAsUnorderedAccessView(1);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(13,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> serialized;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"serialize actual filter root");
            ComPtr<ID3D12RootSignature> filterRoot;Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&filterRoot)),"create actual filter root");
            std::array<ComPtr<ID3D12PipelineState>,3> pipelines;const wchar_t* entries[]={L"CSResetFilter",L"CSVisibility",L"CSFilter"};
            for(UINT index=0;index<3;++index)
            {
                const auto shader=Compile(L"Shaders/Shaders_For_VXGI_Probe_Filter.hlsl",entries[index]);D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};pipelineDesc.pRootSignature=filterRoot.Get();pipelineDesc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
                Check(device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipelines[index])),"create actual alpha filter PSO");
            }
            Parameters p;const auto& grid=scene.sdf.GetGrid();p.origin=grid.origin;p.cellSize=grid.cellSize;p.cellDimensions=grid.cellDimensions;p.boundsMaximum=grid.boundsMaximum;p.triangleCount=uint32_t(packed.size());p.probeCount=2;p.materialCount=1;p.updateCount=2;
            std::array<uint32_t,2> previousMask={};
            for(uint32_t edit=0;edit<4;++edit)
            {
                auto values=Materials(1);if(edit==3)values[0].baseColorFactor={.2f,.3f,.4f,1};
                auto materials=Upload(values);UploadTextures(Images(edit==1?.5f:0));p.resetMode=edit==3?2u:1u;
                auto resetMasks=Buffer(8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST),finalMasks=Buffer(8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                auto resetRecords=Buffer(2*sizeof(VXGIProbeState::GPURecord),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST),finalRecords=Buffer(2*sizeof(VXGIProbeState::GPURecord),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                ID3D12DescriptorHeap* heaps[]={textureHeap.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(filterRoot.Get());commands->SetComputeRoot32BitConstants(0,20,&p,0);
                const std::array<ID3D12Resource*,10> inputs={nodes.Get(),offsets.Get(),references.Get(),triangles.Get(),centers.Get(),lookup.Get(),neighborBuffer.Get(),rawBuffer.Get(),edges.Get(),materials.Get()};
                for(UINT index=0;index<inputs.size();++index)commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());
                commands->SetComputeRootUnorderedAccessView(11,edges->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(12,filtered->GetGPUVirtualAddress());
                commands->SetPipelineState(pipelines[0].Get());commands->Dispatch(1,1,1);
                const D3D12_RESOURCE_BARRIER barriers[]={CD3DX12_RESOURCE_BARRIER::UAV(edges.Get()),CD3DX12_RESOURCE_BARRIER::UAV(filtered.Get())};commands->ResourceBarrier(2,barriers);
                Transition(edges.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(resetMasks.Get(),edges.Get());Transition(edges.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(filtered.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(resetRecords.Get(),filtered.Get());Transition(filtered.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commands->SetPipelineState(pipelines[1].Get());commands->Dispatch(2,1,1);Transition(edges.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                commands->SetPipelineState(pipelines[2].Get());commands->Dispatch(1,1,1);
                Transition(edges.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(finalMasks.Get(),edges.Get());Transition(edges.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Transition(filtered.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(finalRecords.Get(),filtered.Get());Transition(filtered.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Check(commands->Close(),"close actual alpha filter commands");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),++fenceValue),"signal actual alpha filter fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm actual alpha filter fence");Require(WaitForSingleObject(event,60000)==WAIT_OBJECT_0,"actual alpha filter timeout");Check(device->GetDeviceRemovedReason(),"actual alpha filter removed status");
                auto read=[&]<class T>(ID3D12Resource* resource,size_t count)
                {
                    std::vector<T> result(count);void* mapped=nullptr;const D3D12_RANGE reads{0,count*sizeof(T)};Check(resource->Map(0,&reads,&mapped),"map actual alpha filter capture");std::memcpy(result.data(),mapped,reads.End);const D3D12_RANGE writes{0,0};resource->Unmap(0,&writes);return result;
                };
                const auto afterReset=read.operator()<uint32_t>(resetMasks.Get(),2),afterFilter=read.operator()<uint32_t>(finalMasks.Get(),2);
                const auto cleared=read.operator()<VXGIProbeState::GPURecord>(resetRecords.Get(),2),output=read.operator()<VXGIProbeState::GPURecord>(finalRecords.Get(),2);
                for(size_t probe=0;probe<2;++probe)
                {
                    Require(afterReset[probe]==(edit==3?previousMask[probe]:0),"actual CSResetFilter did not invalidate coverage edges or preserve RGB-only edges");
                    Require(cleared[probe].state==0&&cleared[probe].updateCount==0&&cleared[probe].sh[0].x==0&&cleared[probe].sh[0].y==0,"actual filter reset retained stale SH");
                    const uint32_t expected=0x80000000u|(edit==1?0u:(1u<<(probe==0?5:4)));
                    Require(afterFilter[probe]==expected,"actual CSVisibility did not recompute hole-covered-hole edge");
                    Require(output[probe].state==1&&output[probe].updateCount==1,"actual CSFilter lost valid raw metadata");
                    const float expectedRed=edit==1?(probe==0?1.f:0.f):(probe==0?.8f:.2f);
                    const float expectedGreen=edit==1?(probe==0?0.f:1.f):(probe==0?.2f:.8f);
                    Require(std::abs(output[probe].sh[0].x-expectedRed)<2e-6f&&std::abs(output[probe].sh[0].y-expectedGreen)<2e-6f,"actual CSFilter mixed SH across covered cutout edge");
                    previousMask[probe]=afterFilter[probe];
                }
                Check(allocator->Reset(),"reset actual alpha filter allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset actual alpha filter commands");textureUploads.clear();++submissions;++filterEdits;
            }
        }
        std::string adapterName;uint32_t submissions=0,raysCompared=0,filterEdits=0;
    private:
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);ComPtr<ID3D12Resource> result;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&result)),"create alpha buffer");return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            Require(!values.empty(),"alpha fixture input buffer cannot be empty");auto buffer=Buffer(values.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;const D3D12_RANGE reads{0,0};Check(buffer->Map(0,&reads,&mapped),"map alpha upload");std::memcpy(mapped,values.data(),values.size()*sizeof(T));buffer->Unmap(0,nullptr);return buffer;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {
            const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);commands->ResourceBarrier(1,&barrier);
        }
        void UploadTextures(const std::vector<Image>& images)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,UINT(images.size()),D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&textureHeap)),"create actual alpha bindless heap");
            textures.clear();textureUploads.clear();const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for(UINT imageIndex=0;imageIndex<images.size();++imageIndex)
            {
                const auto& image=images[imageIndex];Require(!image.mips.empty(),"alpha image must contain a mip");const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,image.mips[0].width,image.mips[0].height,1,UINT16(image.mips.size()));const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> texture;
                Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"create actual alpha texture");
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(image.mips.size());UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,UINT(image.mips.size()),0,footprints.data(),nullptr,nullptr,&bytes);auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map actual alpha texture upload");
                for(UINT mip=0;mip<image.mips.size();++mip)
                {
                    const auto& source=image.mips[mip];Require(source.pixels.size()==size_t(source.width)*source.height,"alpha texture fixture pixel size mismatch");
                    for(UINT row=0;row<source.height;++row)std::memcpy(static_cast<uint8_t*>(mapped)+footprints[mip].Offset+size_t(row)*footprints[mip].Footprint.RowPitch,source.pixels.data()+size_t(row)*source.width,size_t(source.width)*sizeof(XMFLOAT4));
                }
                upload->Unmap(0,nullptr);Transition(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
                for(UINT mip=0;mip<image.mips.size();++mip){const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprints[mip]),destination(texture.Get(),mip);commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}
                Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=desc.MipLevels;
                const CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor(textureHeap->GetCPUDescriptorHandleForHeapStart(),INT(imageIndex),increment);device->CreateShaderResourceView(texture.Get(),&view,descriptor);textures.push_back(texture);textureUploads.push_back(upload);
            }
        }
        HWND hidden=nullptr;HANDLE event=nullptr;uint64_t fenceValue=0;
        ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12DescriptorHeap> textureHeap;std::vector<ComPtr<ID3D12Resource>> textures,textureUploads;
    };
    void Hit(const Result& value,uint32_t material,float z,const char* label)
    {
        Require(value.traceStatus==1 && value.hitValid==1 && value.materialId==material && std::abs(value.position.z-z)<2e-5f,label);
    }
    void Invalid(const Result& value,const char* label){Require(value.traceStatus>=2 && value.hitValid==0,label);}
    uint32_t groups=0;
    void Passed(const char* label){++groups;std::cout<<"PASS "<<label<<'\n';}
    void TestTrace(GPU& gpu)
    {
        Scene layers({{1,true,0},{2,false,1}});auto materials=Materials();
        Hit(gpu.Dispatch(layers,{Ray{}},materials,Images(0))[0],1,2,"alpha-zero hole must reveal rear opaque");Passed("alpha-zero hole reveals rear opaque");
        std::vector<Plane> multi;for(uint32_t index=0;index<8;++index)multi.push_back({.2f+.2f*index,true,0});multi.push_back({2.5f,false,1});Scene multilayer(multi);
        Hit(gpu.Dispatch(multilayer,{Ray{}},materials,Images(0))[0],1,2.5f,"eight holes must reveal rear opaque");Passed("multiple holes retain rear surface");
        Hit(gpu.Dispatch(layers,{Ray{}},materials,Images(.499f))[0],1,2,"alpha .499 must be hole");Hit(gpu.Dispatch(layers,{Ray{}},materials,Images(.5f))[0],0,1,"alpha exactly .5 must block");
        auto factor=materials;factor[0].baseColorFactor.w=.499f;Hit(gpu.Dispatch(layers,{Ray{}},factor,Images(1))[0],1,2,"factor .499 must be hole");factor[0].baseColorFactor.w=.5f;Hit(gpu.Dispatch(layers,{Ray{}},factor,Images(1))[0],0,1,"factor .5 must block");Passed("sample and factor alpha .499/.5 boundary");
        Scene opaque({{1,false,0},{2,false,1}});Hit(gpu.Dispatch(opaque,{Ray{}},materials,Images(0))[0],0,1,"opaque alpha-zero must still block");Passed("opaque alpha-zero still terminates");
        Scene coplanar({{1,true,0},{1,false,1},{2,false,1}});const auto coincident=gpu.Dispatch(coplanar,{Ray{}},materials,Images(0))[0];Hit(coincident,1,1,"coplanar hole cannot skip coincident opaque");Require(coincident.triangleIndex==2,"coplanar accepted triangle must be opaque identity");Passed("coplanar hole preserves opaque layer identity");
    }
    void TestUVAndMip(GPU& gpu)
    {
        Scene layers({{1,true,0},{2,false,1}});auto images=Images();images[0]={{{4,1,{{1,1,1,0},{1,1,1,0},{1,1,1,1},{1,1,1,1}}}}};
        Ray left,right;left.origin.x=-1;right.origin.x=1;const auto results=gpu.Dispatch(layers,{left,right},Materials(),images);
        Hit(results[0],1,2,"barycentric left UV must traverse hole");Hit(results[1],0,1,"barycentric right UV must retain covered texel");
        Require(std::abs(results[0].uv.x-.25f)<2e-6f&&std::abs(results[1].uv.x-.75f)<2e-6f,"real barycentric hit UV mismatch");Passed("real barycentric UV changes cutout coverage");
        images[0]={{{4,1,std::vector<XMFLOAT4>(4,{1,1,1,0})},{2,1,std::vector<XMFLOAT4>(2,{1,1,1,1})},{1,1,{{1,1,1,0}}}}};
        Ray base,mip1,mip2;mip1.mipLevel=1;mip2.mipLevel=2;const auto mips=gpu.Dispatch(layers,{base,mip1,mip2},Materials(),images);
        Hit(mips[0],1,2,"mip0 hole must traverse");Hit(mips[1],0,1,"mip1 covered must terminate");Hit(mips[2],1,2,"mip2 hole must traverse");Passed("explicit fixed material mip changes coverage");
    }
    void TestInvalid(GPU& gpu)
    {
        const float nan=std::numeric_limits<float>::quiet_NaN();Scene layers({{1,true,0},{2,false,1}});auto materials=Materials();
        Scene badID({{1,true,99},{2,false,1}});Invalid(gpu.Dispatch(badID,{Ray{}},materials,Images(0))[0],"invalid cutout material index must fail closed");
        Invalid(gpu.Dispatch(layers,{Ray{}},materials,Images(0),0)[0],"zero materialCount must fail closed");
        auto badFactor=materials;badFactor[0].baseColorFactor.w=nan;Invalid(gpu.Dispatch(layers,{Ray{}},badFactor,Images(0))[0],"nonfinite alpha factor must fail closed");
        Invalid(gpu.Dispatch(layers,{Ray{}},materials,Images(nan))[0],"nonfinite texture alpha must fail closed");
        Scene badUV({{1,true,0,true},{2,false,1}});Invalid(gpu.Dispatch(badUV,{Ray{}},materials,Images(0))[0],"nonfinite barycentric UV must fail closed");
        Ray badMip;badMip.mipLevel=nan;Invalid(gpu.Dispatch(layers,{badMip},materials,Images(0))[0],"nonfinite mip must fail closed");badMip.mipLevel=-1;Invalid(gpu.Dispatch(layers,{badMip},materials,Images(0))[0],"negative mip must fail closed");
        Passed("invalid material/count/alpha/UV/mip fails closed");
    }
    void TestShadow(GPU& gpu)
    {
        Scene shadow({{0,false,1},{1,true,0}});Ray ray;ray.operation=1;ray.origin.z=.5f;ray.direction={0,0,-1};
        const auto clear=gpu.Dispatch(shadow,{ray},Materials(),Images(0))[0];Hit(clear,1,0,"shadow fixture receiver hit must be preserved");
        Require(clear.sourceValid==1&&clear.sunVisibility==1&&clear.shadowTraceStatus==0&&std::abs(clear.lo0.x-.96f)<3e-5f&&std::abs(clear.lo0.y-1.92f)<3e-5f&&std::abs(clear.lo0.z-.48f)<3e-5f,"hole must transmit correct Lo0 sunlight");
        const auto blocked=gpu.Dispatch(shadow,{ray},Materials(),Images(.5f))[0];Require(blocked.sourceValid==1&&blocked.sunVisibility==0&&blocked.shadowTraceStatus==1&&blocked.lo0.x==0&&blocked.lo0.y==0&&blocked.lo0.z==0,"covered cutout must shadow Lo0");
        const auto invalid=gpu.Dispatch(shadow,{ray},Materials(),Images(std::numeric_limits<float>::quiet_NaN()))[0];Require(invalid.sourceValid==0&&invalid.sunVisibility==0&&invalid.shadowTraceStatus>=2,"invalid cutout shadow must fail closed");Passed("Lo0 sunlight shadow honors holes, coverage and invalid data");
    }
    void TestVisibilityAndClassification(GPU& gpu)
    {
        Scene layers({{1,true,0},{2,false,1}});Ray target;target.operation=3;
        const auto clear=gpu.Dispatch(layers,{target},Materials(),Images(0))[0];Require(clear.visible==1&&clear.traceFailures==0,"strict endpoint must see rear target through hole");
        const auto blocked=gpu.Dispatch(layers,{target},Materials(),Images(.5f))[0];Require(blocked.visible==0&&blocked.traceFailures==0,"strict endpoint must reject earlier covered triangle");
        Scene opaque({{1,false,0},{2,false,1}});const auto black=gpu.Dispatch(opaque,{target},Materials(),Images(0))[0];Require(black.visible==0&&black.traceFailures==0,"strict endpoint must reject opaque alpha-zero blocker");
        const auto invalid=gpu.Dispatch(layers,{target},Materials(),Images(std::numeric_limits<float>::quiet_NaN()))[0];Require(invalid.visible==0&&invalid.traceFailures==1,"strict endpoint invalid coverage must fail closed");
        Ray holeTarget=target;holeTarget.target.z=1;holeTarget.targetTriangle=0;const auto absent=gpu.Dispatch(layers,{holeTarget},Materials(),Images(0))[0];Require(absent.visible==0&&absent.traceFailures==1,"hole cannot serve as verified endpoint");Passed("strict endpoint visibility preserves target identity and blockers");
        Scene hole({{1,true,0}});Ray edge;edge.operation=2;const auto open=gpu.Dispatch(hole,{edge},Materials(),Images(0))[0];Require(open.visible==1&&open.traceFailures==0,"hole must permit center edge");
        const auto closed=gpu.Dispatch(hole,{edge},Materials(),Images(.5f))[0];Require(closed.visible==0&&closed.traceFailures==0,"covered texel must block center edge");
        const auto edgeInvalid=gpu.Dispatch(hole,{edge},Materials(),Images(std::numeric_limits<float>::quiet_NaN()))[0];Require(edgeInvalid.visible==0&&edgeInvalid.traceFailures==1,"invalid center edge must fail closed");Passed("probe center edge visibility honors alpha");
        Ray classify;classify.operation=4;classify.origin.z=1;const auto empty=gpu.Dispatch(hole,{classify},Materials(),Images(0))[0];Require(empty.probeState==1&&empty.probeReason==0&&empty.classificationRays==64&&empty.backfaceRatio==0,"hole origin must remain classified valid");
        const auto unsafe=gpu.Dispatch(hole,{classify},Materials(),Images(.5f))[0];Require(unsafe.probeState==2&&unsafe.probeReason==2&&unsafe.minimumHitDistance<=classify.minimumDistance,"covered exact origin must disable fixed probe");
        const auto broken=gpu.Dispatch(hole,{classify},Materials(),Images(std::numeric_limits<float>::quiet_NaN()))[0];Require(broken.probeState==2&&broken.probeReason==3,"invalid classification coverage must disable fixed probe");Passed("64-ray fixed-probe classification honors cutout coverage");
        Scene queryScene({{1.9f,true,0},{2,false,1}});Ray query;query.operation=5;query.origin={.25f,-.25f,1.75f};query.target={.25f,-.25f,2};query.barycentric={.4375f,.125f,.4375f};
        const auto supported=gpu.Dispatch(queryScene,{query},Materials(),Images(0))[0];Require(supported.hasSupport==1&&supported.acceptedCount==1&&supported.queryFailures==0&&supported.supportWeight>0&&std::abs(supported.irradiance.x-.2f)<3e-6f&&std::abs(supported.irradiance.y-.4f)<3e-6f&&std::abs(supported.irradiance.z-.6f)<3e-6f,"production raw query must consume SH through alpha hole");
        const auto noSupport=gpu.Dispatch(queryScene,{query},Materials(),Images(.5f))[0];Require(noSupport.hasSupport==0&&noSupport.acceptedCount==0&&noSupport.queryFailures==0,"production raw query must reject covered blocker");
        const auto failedQuery=gpu.Dispatch(queryScene,{query},Materials(),Images(std::numeric_limits<float>::quiet_NaN()))[0];Require(failedQuery.hasSupport==0&&failedQuery.queryFailures==1,"production raw query invalid coverage must fail closed");Passed("production raw probe query consumes only endpoint-visible SH");
    }
}
int main()
{
    try
    {
        GPU gpu;TestTrace(gpu);TestUVAndMip(gpu);TestInvalid(gpu);TestShadow(gpu);TestVisibilityAndClassification(gpu);
        gpu.TestFilterCache();Passed("actual reset/visibility/filter kernels invalidate coverage cache and preserve RGB-only edges");
        const auto warnings=gpu.Warnings();Require(warnings==0,"alpha debug layer or GBV emitted warnings/errors");
        std::ofstream report("tmp/VXGIAlphaCutoutGPU-report.json");Require(bool(report),"open alpha report");
        report<<"{\n  \"adapter\": \""<<gpu.adapterName<<"\",\n  \"debug_layer\": true,\n  \"gpu_based_validation\": true,\n  \"window_visible\": false,\n  \"present_calls\": 0,\n  \"group_count\": "<<groups<<",\n  \"submissions\": "<<gpu.submissions<<",\n  \"rays_compared\": "<<gpu.raysCompared<<",\n  \"actual_filter_edit_phases\": "<<gpu.filterEdits<<",\n  \"d3d12_warnings_errors\": "<<warnings<<",\n  \"passed\": true\n}\n";
        std::cout<<"PASS "<<groups<<" alpha groups, "<<gpu.submissions<<" submissions, "<<gpu.raysCompared<<" rays; no D3D12 warnings/errors\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAIL alpha GPU fixture: "<<error.what()<<'\n';return 1;}
}
