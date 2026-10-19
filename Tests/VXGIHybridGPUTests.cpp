// Stage 7.7: finite SSGI rays and direct cached GI-only world fallback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <d3dx12.h>
#include "../Resources/VXGISDF.h"
#include "../Resources/VXGISurface.h"
#include "../Resources/VXGIProbeState.h"
#include "../Resources/VXGIProbeTopology.h"
// Existing asset/IBL headers are compiled by the main project at /W3.
// Keep their known conversion/type-tag diagnostics scoped to this include.
#pragma warning(push)
#pragma warning(disable:4267 4244 4099 4324)
#include "../Passes/SSGIPass.h"
#pragma warning(pop)
#include "../Resources/SSGIHybridState.h"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace
{
    using GPURecord=VXGIProbeState::GPURecord;
    using Material=VXGISurface::MaterialRecord;
    constexpr uint32_t Width=16,Height=16;
    void Require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
    void Check(HRESULT value,const char* operation)
    {
        if(FAILED(value)){std::ostringstream message;message<<operation<<": HRESULT 0x"<<std::hex<<uint32_t(value);throw std::runtime_error(message.str());}
    }
    XMFLOAT3 Add(XMFLOAT3 a,XMFLOAT3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
    XMFLOAT3 Sub(XMFLOAT3 a,XMFLOAT3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
    XMFLOAT3 Scale(XMFLOAT3 a,float s){return {a.x*s,a.y*s,a.z*s};}
    float Dot(XMFLOAT3 a,XMFLOAT3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    XMFLOAT3 Normalize(XMFLOAT3 a){return Scale(a,1/std::sqrt(Dot(a,a)));}
    XMFLOAT3 Color(XMFLOAT4 a){return {a.x,a.y,a.z};}
    float Error(XMFLOAT3 a,XMFLOAT3 b){return std::max({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.z-b.z)});}
    std::string UTF8(const wchar_t* input)
    {
        const int n=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);std::string result(n,'\0');
        WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),n,nullptr,nullptr);result.pop_back();return result;
    }
    struct GPUTriangle
    {
        XMFLOAT3 p0;uint32_t triangleId;XMFLOAT3 p1;uint32_t flags;XMFLOAT3 p2;uint32_t pad0;
        XMFLOAT3 normal;uint32_t pad1;XMFLOAT2 uv0,uv1,uv2;
        XMFLOAT3 shadingNormal0,shadingNormal1,shadingNormal2;
        XMFLOAT3 tangent0,tangent1,tangent2,bitangent0,bitangent1,bitangent2;
        uint32_t materialId,instanceId,pad2;
    };
    struct HybridParameters
    {
        XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;uint32_t triangleCount;
        XMFLOAT3 boundsMaximum;uint32_t materialCount;
        XMFLOAT3 reservedLighting0;float materialMipLevel;
        XMFLOAT3 reservedLighting1;float reservedLighting2;
        float reservedWorldRange=1000,voxelSize=1;uint32_t probeCount=0,worldMode=2;
    };
    struct RayInput
    {
        std::array<uint32_t,2> receiver={8,8};uint32_t pad0=0,pad1=0;
        XMFLOAT3 viewOrigin={0,0,2};uint32_t fallbackEnabled=1;
        XMFLOAT3 viewDirection={1,0,0};uint32_t operation=0;
        XMFLOAT3 worldOrigin={0,0,2};float rayLength=.5f;
        XMFLOAT3 worldDirection={1,0,0};uint32_t pad3=0;
    };
    struct RayResult
    {
        XMFLOAT3 radiance;uint32_t hit;
        float distance;uint32_t status,backend,worldTraceStatus;
        float confidence;uint32_t anchorStatus,triangleIndex,probeHasSupport;
        float probeSupportWeight;uint32_t probeAcceptedCount,probeTraceFailures,sourceValid;
    };
    static_assert(sizeof(GPUTriangle)==208&&sizeof(HybridParameters)==96&&sizeof(RayInput)==80&&sizeof(RayResult)==64);
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
        const auto n=Normalize(Cross(Sub(b,a),Sub(c,a))),t=Normalize(Sub(b,a)),bt=Cross(n,t);const auto first=uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{a,n,{0,0},t,bt},{b,n,{1,0},t,bt},{c,n,{1,1},t,bt},{d,n,{0,1},t,bt}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    struct Scene
    {
        TestModel model;VXGIWorld world;VXGISDF sdf;VXGIProbeTopology topology;
        std::vector<Material> materials;std::vector<GPURecord> raw;
        bool isCutout=false;
        void Build(float voxel=1)
        {
            std::vector<TestInstance> instances(1);instances[0].pModel=&model;instances[0].isCutout=isCutout;
            Require(world.Update(instances,voxel),world.GetLastError());Require(sdf.Update(world,.25f,128),sdf.GetLastError());
            Require(topology.Update(world),topology.GetLastError());raw.assign(world.GetCells().size(),Constant({.2f,.4f,.6f}));
        }
        static GPURecord Constant(XMFLOAT3 value)
        {
            GPURecord r;r.sh[0]={value.x/.28209479177387814f,value.y/.28209479177387814f,value.z/.28209479177387814f,0};
            r.state=1;r.updateCount=3;r.sampleCount=24;r.lastUpdateFrame=11;r.classificationRays=8;r.minHitDistance=1;return r;
        }
        HybridParameters Parameters(uint32_t mode=2)const
        {
            const auto& grid=sdf.GetGrid();HybridParameters p{};p.gridOrigin=grid.origin;p.cellSize=grid.cellSize;
            p.cellDimensions=grid.cellDimensions;p.boundsMaximum=grid.boundsMaximum;p.triangleCount=uint32_t(sdf.GetTriangles().size());
            p.materialCount=uint32_t(materials.size());p.voxelSize=world.GetVoxelSize();
            p.probeCount=uint32_t(raw.size());p.worldMode=mode;return p;
        }
    };
    std::vector<GPUTriangle> Triangles(const VXGISDF& sdf)
    {
        std::vector<GPUTriangle> result;
        for(uint32_t index=0;index<sdf.GetTriangles().size();++index)
        {
            const auto& t=sdf.GetTriangles()[index];GPUTriangle r{};r.p0=t.positions[0];r.p1=t.positions[1];r.p2=t.positions[2];r.triangleId=index;
            r.flags=t.isCutout;r.normal=t.geometricNormal;r.uv0=t.uv[0];r.uv1=t.uv[1];r.uv2=t.uv[2];
            r.shadingNormal0=t.shadingNormals[0];r.shadingNormal1=t.shadingNormals[1];r.shadingNormal2=t.shadingNormals[2];
            r.tangent0=t.tangents[0];r.tangent1=t.tangents[1];r.tangent2=t.tangents[2];r.bitangent0=t.bitangents[0];r.bitangent1=t.bitangents[1];r.bitangent2=t.bitangents[2];
            r.materialId=t.materialId;r.instanceId=t.instanceId;result.push_back(r);
        }return result;
    }
    XMFLOAT4X4 Stored(DirectX::FXMMATRIX m){XMFLOAT4X4 r;DirectX::XMStoreFloat4x4(&r,DirectX::XMMatrixTranspose(m));return r;}
    float DeviceDepth(float viewZ){constexpr float nearZ=.1f,farZ=20;return farZ/(farZ-nearZ)-nearZ*farZ/(farZ-nearZ)/viewZ;}
    ComPtr<IDxcBlob> Compile(const wchar_t* path,const wchar_t* entry,const wchar_t* target=L"cs_6_6",const wchar_t* define=nullptr)
    {
        ComPtr<IDxcUtils> utils;ComPtr<IDxcCompiler3> compiler;Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");ComPtr<IDxcBlobEncoding> source;Check(utils->LoadFile(path,nullptr,&source),"load shader");
        ComPtr<IDxcIncludeHandler> includes;Check(utils->CreateDefaultIncludeHandler(&includes),"create include handler");
        std::vector<LPCWSTR> args={path,L"-E",entry,L"-T",target,L"-HV",L"2021",L"-I",L"Shaders",L"-WX"};
        if(define){args.push_back(L"-D");args.push_back(define);}
#if defined(_DEBUG)
        args.insert(args.end(),{L"-Od",L"-Zi"});
#else
        args.push_back(L"-O3");
#endif
        DxcBuffer input{source->GetBufferPointer(),source->GetBufferSize(),DXC_CP_UTF8};ComPtr<IDxcResult> result;
        Check(compiler->Compile(&input,args.data(),UINT(args.size()),includes.Get(),IID_PPV_ARGS(&result)),"compile shader");ComPtr<IDxcBlobUtf8> errors;
        Check(result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr),"get diagnostics");if(errors&&errors->GetStringLength())std::cerr<<errors->GetStringPointer();
        HRESULT status;Check(result->GetStatus(&status),"get shader status");Check(status,"shader compilation");ComPtr<IDxcBlob> blob;
        Check(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&blob),nullptr),"get shader object");return blob;
    }
    void EnableValidation()
    {
        ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get debug layer");debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get GPU validation");validation->SetEnableGPUBasedValidation(TRUE);
    }
    uint32_t WarningCount(ID3D12InfoQueue* info,const char* label)
    {
        uint32_t count=0;for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
        {
            SIZE_T bytes=0;Check(info->GetMessage(index,nullptr,&bytes),"measure validation message");std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(index,message,&bytes),"read validation message");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<label<<": "<<message->pDescription<<'\n';++count;}
        }return count;
    }
    struct Texture{ComPtr<ID3D12Resource> resource;D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;uint32_t width=1,height=1,arraySize=1,bytesPerPixel=16;};
    struct Screen
    {
        std::vector<float> depth=std::vector<float>(Width*Height,1);
        std::vector<XMFLOAT4> normal=std::vector<XMFLOAT4>(Width*Height,{1,.5f,.5f,1});
        std::vector<XMFLOAT4> orm=std::vector<XMFLOAT4>(Width*Height,{1,.6f,0,1});
        std::vector<XMFLOAT4> source=std::vector<XMFLOAT4>(Width*Height,XMFLOAT4{});
        std::vector<XMFLOAT4> albedo=std::vector<XMFLOAT4>(Width*Height,{.5f,.25f,.75f,1});
    };
    class GPU
    {
    public:
        GPU()
        {
            ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create factory");ComPtr<IDXGIAdapter1> adapter;
            for(UINT index=0;factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++index)
            {
                DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"adapter description");
                if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;adapter.Reset();
            }
            Require(device!=nullptr,"hardware GPU unavailable");DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"chosen adapter");adapterName=UTF8(desc.Description);
            Check(device.As(&info),"get validation queue");D3D12_COMMAND_QUEUE_DESC q{};Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"create queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create allocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"create command list");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"create event failed");
            D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,64,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)),"create texture heap");
            descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            std::array<CD3DX12_ROOT_PARAMETER,12> roots;roots[0].InitAsConstantBufferView(0);
            for(UINT i=0;i<9;++i)roots[i+1].InitAsShaderResourceView(i);roots[10].InitAsConstants(4,1);roots[11].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);CD3DX12_ROOT_SIGNATURE_DESC description(UINT(roots.size()),roots.data(),1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> blob;Check(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize root");Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"create root");
            rays=Pipeline(L"Shaders/VXGIHybridValidate.hlsl",L"CSMain_Rays");trace=Pipeline(L"Shaders/Shaders_For_SSGI.hlsl",L"CSMain_Trace",L"SSGI_PASS_TRACE=1");
            pyramid=Pipeline(L"Shaders/Shaders_For_SSGI.hlsl",L"CSMain_Pyramid",L"SSGI_PASS_PYRAMID=1");
            depth=CreateTexture(Width,Height,DXGI_FORMAT_R32_FLOAT,4,0);normal=CreateTexture(Width,Height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,1);
            orm=CreateTexture(Width,Height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,2);source=CreateTexture(Width,Height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,3);
            output=CreateTexture(Width/2,Height/2,DXGI_FORMAT_R32G32B32A32_FLOAT,16,4,true);albedo=CreateTexture(Width,Height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,5);
            noise=CreateTexture(128,128,DXGI_FORMAT_R8G8_UINT,2,6,false,64);std::vector<uint8_t> noiseBytes(128*128*64*2,0);for(size_t i=1;i<noiseBytes.size();i+=2)noiseBytes[i]=128;UploadTexture(noise,noiseBytes.data());
            uint32_t w=Width,hgt=Height;for(UINT i=0;i<4;++i){w=(w+1)/2;hgt=(hgt+1)/2;levels.push_back(CreateTexture(w,hgt,DXGI_FORMAT_R32G32_FLOAT,8,7+i*2,true,1,true));}
            const std::array<XMFLOAT4,4> texels={XMFLOAT4{1,1,1,1},XMFLOAT4{.5f,.5f,1,1},XMFLOAT4{1,.6f,0,1},XMFLOAT4{1,1,1,1}};
            for(UINT i=0;i<4;++i){materialTextures.push_back(CreateTexture(1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,16,40+i));UploadTexture(materialTextures.back(),&texels[i]);}
            Finish();
        }
        ~GPU(){if(event)CloseHandle(event);}
        SSGITraceConstants Constants(const HybridParameters& p)const
        {
            SSGITraceConstants c{};const auto proj=DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2,1,.1f,20);
            c.view=Stored(DirectX::XMMatrixIdentity());c.proj=Stored(proj);c.invProj=Stored(DirectX::XMMatrixInverse(nullptr,proj));
            c.width=Width;c.height=Height;c.outputWidth=Width/2;c.outputHeight=Height/2;c.depthIndex=0;c.normalIndex=1;c.ormIndex=2;c.sourceIndex=3;c.outputIndex=4;
            c.blueNoiseIndex=6;c.radius=.5f;c.thickness=.15f;c.normalBias=.02f;c.rayCount=1;c.maxIterations=96;c.albedoIndex=5;c.traceDownsampleFactor=2;
            c.invView=Stored(DirectX::XMMatrixIdentity());c.invViewProj=c.invProj;
            std::memcpy(&c.hybrid,&p,sizeof(p));return c;
        }
        std::vector<RayResult> Rays(const Scene& scene,const Screen& screen,const std::vector<RayInput>& inputs,SSGITraceConstants constants,bool buildPyramid=true)
        {
            BindScene(scene,screen,constants,buildPyramid);auto input=Upload(inputs);auto result=Buffer(inputs.size()*sizeof(RayResult),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            Transition(result.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->SetComputeRootShaderResourceView(9,input->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(11,result->GetGPUVirtualAddress());const std::array<uint32_t,4> extra={uint32_t(inputs.size()),0,0,0};commands->SetComputeRoot32BitConstants(10,4,extra.data(),0);
            commands->SetPipelineState(rays.Get());commands->Dispatch((uint32_t(inputs.size())+63)/64,1,1);auto readback=CopyBuffer(result.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Finish();return Read<RayResult>(readback.Get(),inputs.size());
        }
        std::vector<XMFLOAT4> Trace(const Scene& scene,const Screen& screen,SSGITraceConstants constants)
        {
            BindScene(scene,screen,constants,true);Transition(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->SetPipelineState(trace.Get());commands->Dispatch(1,1,1);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,bytes=0;const auto desc=output.resource->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&bytes);
            auto readback=Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(output,D3D12_RESOURCE_STATE_COPY_SOURCE);
            CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(),footprint),src(output.resource.Get(),0);commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Finish();
            void* mapped=nullptr;const D3D12_RANGE range{0,SIZE_T(bytes)};Check(readback->Map(0,&range,&mapped),"map trace pixels");std::vector<XMFLOAT4> result(Width*Height/4);
            for(UINT y=0;y<Height/2;++y)std::memcpy(result.data()+y*Width/2,static_cast<const uint8_t*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,Width/2*sizeof(XMFLOAT4));readback->Unmap(0,nullptr);return result;
        }
        uint32_t Warnings(){return WarningCount(info.Get(),"Direct kernel D3D12");}
        void TwoSampleNoise(bool mixed)
        {
            std::vector<uint8_t> bytes(128*128*64*2,0);for(size_t i=1;i<bytes.size();i+=2)bytes[i]=128;
            if(mixed)for(UINT z=0;z<64;++z)bytes[((z*128+76)*128+100)*2]=255;
            UploadTexture(noise,bytes.data());Finish();
        }
        std::string adapterName;
    private:
        ComPtr<ID3D12PipelineState> Pipeline(const wchar_t* path,const wchar_t* entry,const wchar_t* define=nullptr)
        {
            const auto shader=Compile(path,entry,L"cs_6_6",define);D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=root.Get();d.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            ComPtr<ID3D12PipelineState> result;Check(device->CreateComputePipelineState(&d,IID_PPV_ARGS(&result)),"create compute pipeline");return result;
        }
        ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            const auto h=CD3DX12_HEAP_PROPERTIES(type);const auto d=CD3DX12_RESOURCE_DESC::Buffer(std::max(UINT64(4),bytes),flags);ComPtr<ID3D12Resource> r;
            Check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"create buffer");persistent.push_back(r);return r;
        }
        template<class T>ComPtr<ID3D12Resource> Upload(const std::vector<T>& data)
        {
            const auto bytes=data.size()*sizeof(T);auto r=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(r->Map(0,nullptr,&mapped),"map upload");
            if(bytes)std::memcpy(mapped,data.data(),bytes);else std::memset(mapped,0,4);r->Unmap(0,nullptr);return r;
        }
        template<class T>ComPtr<ID3D12Resource> Constant(const T& value)
        {
            auto r=Buffer((sizeof(T)+255)&~UINT64(255),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(r->Map(0,nullptr,&mapped),"map constants");std::memcpy(mapped,&value,sizeof(value));r->Unmap(0,nullptr);return r;
        }
        Texture CreateTexture(UINT w,UINT h,DXGI_FORMAT format,UINT bpp,UINT descriptor,bool uav=false,UINT arraySize=1,bool srvAndUav=false)
        {
            Texture t;t.width=w;t.height=h;t.bytesPerPixel=bpp;t.arraySize=arraySize;const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(format,w,h,UINT16(arraySize),1,1,0,uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
            Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&t.resource)),"create fixture texture");
            if(!uav||srvAndUav){D3D12_SHADER_RESOURCE_VIEW_DESC s{};s.Format=format;s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if(arraySize>1){s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;s.Texture2DArray.MipLevels=1;s.Texture2DArray.ArraySize=arraySize;}else{s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;}
                device->CreateShaderResourceView(t.resource.Get(),&s,Handle(descriptor));}
            if(uav){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(t.resource.Get(),nullptr,&d,Handle(descriptor+(srvAndUav?1:0)));}return t;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE Handle(UINT i)const{return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(),int(i),descriptorSize);}
        void Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){if(before!=after){const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(r,before,after);commands->ResourceBarrier(1,&barrier);}}
        void Transition(Texture& t,D3D12_RESOURCE_STATES after){Transition(t.resource.Get(),t.state,after);t.state=after;}
        void UploadTexture(Texture& t,const void* data)
        {
            const auto desc=t.resource->GetDesc();std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(t.arraySize);UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,t.arraySize,0,footprints.data(),nullptr,nullptr,&bytes);
            auto staging=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(staging->Map(0,nullptr,&mapped),"map texture upload");
            for(UINT z=0;z<t.arraySize;++z)for(UINT y=0;y<t.height;++y)std::memcpy(static_cast<uint8_t*>(mapped)+footprints[z].Offset+y*footprints[z].Footprint.RowPitch,static_cast<const uint8_t*>(data)+(z*t.height+y)*t.width*t.bytesPerPixel,t.width*t.bytesPerPixel);
            staging->Unmap(0,nullptr);Transition(t,D3D12_RESOURCE_STATE_COPY_DEST);for(UINT z=0;z<t.arraySize;++z){CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(),footprints[z]),dst(t.resource.Get(),z);commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}Transition(t,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        void BindScene(const Scene& scene,const Screen& s,SSGITraceConstants& c,bool usePyramid)
        {
            UploadTexture(depth,s.depth.data());UploadTexture(normal,s.normal.data());UploadTexture(orm,s.orm.data());UploadTexture(source,s.source.data());UploadTexture(albedo,s.albedo.data());
            ID3D12DescriptorHeap* heaps[]={heap.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(root.Get());
            const std::array<ComPtr<ID3D12Resource>,8> resources={Upload(scene.sdf.GetNodes()),Upload(scene.sdf.GetCellOffsets()),Upload(scene.sdf.GetTriangleReferences()),Upload(Triangles(scene.sdf)),Upload(scene.materials),Upload(scene.world.GetOccupiedCenters()),Upload(scene.topology.GetLookup()),Upload(scene.raw)};
            for(UINT i=0;i<8;++i)commands->SetComputeRootShaderResourceView(i+1,resources[i]->GetGPUVirtualAddress());
            if(usePyramid){UINT w=Width,h=Height;for(UINT i=0;i<levels.size();++i){SSGIPyramidConstants p{};p.invProj=c.invProj;p.sourceWidth=w;p.sourceHeight=h;p.outputWidth=levels[i].width;p.outputHeight=levels[i].height;p.depthIndex=i?7+(i-1)*2:0;p.outputDepthIndex=8+i*2;p.firstLevel=i==0;
                auto cb=Constant(p);commands->SetComputeRootConstantBufferView(0,cb->GetGPUVirtualAddress());Transition(levels[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->SetPipelineState(pyramid.Get());commands->Dispatch(1,1,1);Transition(levels[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);c.levels[i]={7+i*2,0,0,0};++c.levelCount;w=levels[i].width;h=levels[i].height;}}
            auto cb=Constant(c);commands->SetComputeRootConstantBufferView(0,cb->GetGPUVirtualAddress());
        }
        ComPtr<ID3D12Resource> CopyBuffer(ID3D12Resource* r,D3D12_RESOURCE_STATES before){auto result=Buffer(r->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(r,before,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(result.Get(),r);return result;}
        template<class T>static std::vector<T> Read(ID3D12Resource* r,size_t count){std::vector<T> result(count);void* p=nullptr;Check(r->Map(0,nullptr,&p),"map result");std::memcpy(result.data(),p,count*sizeof(T));r->Unmap(0,nullptr);return result;}
        void Finish(){Check(commands->Close(),"close command list");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),++fenceValue),"signal fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm fence");Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU test timeout");Require(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU removed");Check(allocator->Reset(),"reset allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset command list");}
        ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> rays,trace,pyramid;
        std::vector<ComPtr<ID3D12Resource>> persistent;Texture depth,normal,orm,source,output,albedo,noise;std::vector<Texture> levels,materialTextures;
        UINT descriptorSize=0;UINT64 fenceValue=0;HANDLE event=nullptr;
    };
    Scene SourceScene(float x=.5f,bool receiver=false)
    {
        Scene scene;scene.model.meshes.resize(receiver?2:1);Quad(scene.model.meshes[0],{x,-3,0},{x,-3,4},{x,3,4},{x,3,0});
        if(receiver){scene.model.meshes[1].materialID=1;Quad(scene.model.meshes[1],{-2,-2,2},{-2,2,2},{2,2,2},{2,-2,2});}
        Material material;material.albedoIdx=40;material.normalIdx=41;material.ormIdx=42;material.emissiveIdx=43;
        material.baseColorFactor={.5f,.25f,.75f,1};material.emissiveFactor={.3f,.6f,.9f};scene.materials.assign(scene.model.meshes.size(),material);scene.Build(.5f);return scene;
    }
    XMFLOAT3 FixedDirection(){const float u=.5f/256,v=128.5f/256,phi=DirectX::XM_2PI*v;return {std::sqrt(1-u),std::sqrt(u)*std::cos(phi),std::sqrt(u)*std::sin(phi)};}
    struct Report{uint32_t rays=0,pixels=0,warnings=0;float maximumRadianceError=0,maximumKernelError=0;std::vector<std::string> groups;};
    void CheckColor(const RayResult& result,XMFLOAT3 expected,Report& report,const char* context)
    {
        const float error=Error(result.radiance,expected);report.maximumRadianceError=std::max(report.maximumRadianceError,error);
        if(!(error<4e-5f)){std::ostringstream m;m<<context<<" error "<<error<<" actual "<<result.radiance.x<<','<<result.radiance.y<<','<<result.radiance.z<<" expected "<<expected.x<<','<<expected.y<<','<<expected.z;throw std::runtime_error(m.str());}++report.rays;
    }
    void CheckPixel(XMFLOAT4 actual,XMFLOAT3 expected,float hitFraction,Report& report,const char* context,float tolerance=4e-5f)
    {
        const float error=Error(Color(actual),expected);report.maximumKernelError=std::max(report.maximumKernelError,error);
        Require(actual.w==hitFraction&&error<tolerance,context);++report.pixels;
    }
    void Group(Report& report,const char* name){report.groups.emplace_back(name);std::cout<<"PASS "<<name<<'\n';}
    void NumericalAcceptance(GPU& gpu,Report& report)
    {
        const XMFLOAT3 cache{.2f,.4f,.6f};
        auto scene=SourceScene();Screen screen;RayInput ray;ray.operation=1;
        auto p=scene.Parameters();auto c=gpu.Constants(p);
        auto result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==1&&result.backend==2&&result.confidence==1&&result.worldTraceStatus==1
            &&std::abs(result.distance-.5f)<1e-5f&&result.probeHasSupport==1,"closed finite world endpoint lost cached support");
        CheckColor(result,cache,report,"finite endpoint cached radiance");
        ray.rayLength=.499f;result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==0&&result.worldTraceStatus==0&&result.confidence==0,"world ray extended beyond original segment");CheckColor(result,{},report,"outside finite endpoint");
        p.reservedWorldRange=100000;ray.operation=0;ray.viewOrigin={.001f,0,2};
        result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];
        Require(result.hit==1&&std::abs(result.distance-.499f)<1e-5f,"finite hybrid segment missed an inside surface");CheckColor(result,cache,report,"inside original radius");
        ray.viewOrigin={-.001f,0,2};result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];
        Require(result.hit==0&&result.worldTraceStatus==0,"obsolete world range extended original SSGI radius");CheckColor(result,{},report,"outside original radius");
        p.reservedWorldRange=std::numeric_limits<float>::quiet_NaN();
        result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];Require(result.hit==0&&result.worldTraceStatus==0,"unused legacy range affected finite tracing");
        Group(report,"same_finite_radius_closed_endpoint_inside_outside_and_legacy_world_range_ignored");

        ray=RayInput{};ray.operation=1;ray.worldDirection={};result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==0&&result.worldTraceStatus==2&&result.confidence==0,"invalid direction became a black hit");CheckColor(result,{},report,"invalid direction");
        ray.worldDirection={1,0,0};ray.rayLength=std::numeric_limits<float>::quiet_NaN();result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==0&&result.worldTraceStatus==2,"invalid finite segment was accepted");CheckColor(result,{},report,"invalid finite length");
        ray.rayLength=.5f;ray.worldDirection={0,1,0};result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==0&&result.worldTraceStatus==0&&result.confidence==0,"verified miss status differs");CheckColor(result,{},report,"verified miss");
        Group(report,"finite_world_miss_and_invalid_segments_have_zero_confidence");

        ray=RayInput{};ray.operation=1;p=scene.Parameters();p.reservedLighting0={10,20,30};p.reservedLighting1={1e6f,1e6f,1e6f};
        scene.materials[0].baseColorFactor={1,1,1,1};scene.materials[0].emissiveFactor={1e6f,1e6f,1e6f};scene.materials[0].isUnlit=0;
        for(auto& record:scene.raw)record=Scene::Constant({});result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];
        Require(result.hit==1&&result.probeHasSupport==1&&result.sourceValid==1,"valid black cache lost geometric termination");CheckColor(result,{},report,"no onsite direct light or emission");
        scene.materials[0].baseColorFactor={0,0,0,1};scene.materials[0].isUnlit=1;
        for(auto& record:scene.raw)record=Scene::Constant(cache);result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];
        CheckColor(result,cache,report,"positive cache ignores black hit albedo and unlit flag");
        p.worldMode=1;result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];CheckColor(result,cache,report,"mode one cache alias");
        Group(report,"world_hit_never_shades_sun_emission_or_material_and_both_modes_sample_cache");

        auto behind=scene.materials[0];scene.model.meshes.emplace_back();scene.model.meshes.back().materialID=1;
        Quad(scene.model.meshes.back(),{.75f,-3,0},{.75f,-3,4},{.75f,3,4},{.75f,3,0});scene.materials.push_back(behind);scene.world.Invalidate();scene.Build(.5f);
        for(auto& record:scene.raw){record.updateCount=0;record.sh[0]={100,100,100,0};}
        ray.rayLength=1;p=scene.Parameters();result=gpu.Rays(scene,screen,{ray},gpu.Constants(p))[0];
        Require(result.hit==1&&result.probeHasSupport==0&&result.confidence==1&&std::abs(result.distance-.5f)<1e-5f,"no-support nearest surface was skipped");CheckColor(result,{},report,"no support nearest termination");
        Group(report,"unsupported_nearest_surface_is_black_and_never_searches_behind");

        // Four small blockers cover every front probe->hit segment while
        // leaving the original A->B ray clear. Cache lookup must not emit
        // another set of visibility rays or reject the back-side lattice.
        scene=SourceScene();
        for(float y:{-.125f,.125f})for(float z:{1.875f,2.125f})
            Quad(scene.model.meshes[0],{.375f,y-.04f,z-.04f},{.375f,y-.04f,z+.04f},
                {.375f,y+.04f,z+.04f},{.375f,y+.04f,z-.04f});
        scene.world.Invalidate();scene.Build(.5f);ray=RayInput{};ray.operation=1;
        result=gpu.Rays(scene,screen,{ray},gpu.Constants(scene.Parameters()))[0];
        Require(result.hit==1&&std::abs(result.distance-.5f)<1e-5f&&result.probeHasSupport==1
            &&result.probeAcceptedCount==8&&result.probeTraceFailures==0
            &&std::abs(result.probeSupportWeight-1)<1e-6f,"cached lookup was gated by neighboring probe visibility or geometric hemisphere");
        CheckColor(result,cache,report,"direct cached radiance despite blocked probe segments");
        Group(report,"world_hit_reads_all_valid_local_cache_without_probe_visibility_or_surface_halfspace_tests");

        scene=SourceScene();screen=Screen{};
        for(UINT y=0;y<Height;++y){screen.depth[y*Width+9]=DeviceDepth(2);screen.normal[y*Width+9]={0,.5f,.5f,1};screen.source[y*Width+9]={.3f,.6f,.9f,1};}
        ray=RayInput{};ray.viewOrigin={0,-.125f,2};
        for(uint32_t mode:{0u,1u,2u})
        {
            result=gpu.Rays(scene,screen,{ray},gpu.Constants(scene.Parameters(mode)))[0];
            Require(result.hit==1&&result.backend==1&&result.worldTraceStatus==UINT32_MAX&&result.probeHasSupport==0,"valid screen hit entered cache fallback");
            CheckColor(result,{.3f,.6f,.9f},report,"unchanged original SSGI source");
        }
        for(auto& pixel:screen.source)pixel={};for(auto& record:scene.raw)record=Scene::Constant({100,100,100});
        result=gpu.Rays(scene,screen,{ray},gpu.Constants(scene.Parameters()))[0];
        Require(result.hit==1&&result.backend==1&&result.confidence==1&&result.worldTraceStatus==UINT32_MAX,"black screen hit fell through to world cache");CheckColor(result,{},report,"black screen termination");
        Group(report,"screen_source_is_unchanged_no_probe_addition_and_black_hits_terminate");

        // Exercise CSMain_Trace itself in screen-only mode, rather than only
        // the shared per-ray helper. Fine receiver (8,8) writes trace texel (4,4).
        scene=SourceScene();screen=Screen{};
        screen.depth[8+8*Width]=DeviceDepth(2);
        for(UINT y=0;y<Height;++y)
        {
            // The deterministic cosine ray points slightly toward the camera;
            // place the target's conservative depth slab across that segment.
            screen.depth[y*Width+9]=DeviceDepth(1.99f);
            screen.normal[y*Width+9]={0,.5f,.5f,1};
            screen.source[y*Width+9]={.3f,.6f,.9f,1};
        }
        auto screenConstants=gpu.Constants(scene.Parameters(0));
        auto screenPixels=gpu.Trace(scene,screen,screenConstants);
        CheckPixel(screenPixels[4+4*8],{.3f,.6f,.9f},1,report,"screen-only entrypoint changed the original screen source");
        for(auto& pixel:screen.source)pixel={};
        for(auto& record:scene.raw)record=Scene::Constant({100,100,100});
        screenPixels=gpu.Trace(scene,screen,screenConstants);
        CheckPixel(screenPixels[4+4*8],{},1,report,"screen-only entrypoint lost black screen-hit termination");
        // This flat receiver and nearby world wall are the same geometry as
        // the nonzero world-cache kernel acceptance immediately below.
        screen=Screen{};for(auto& z:screen.depth)z=DeviceDepth(2);
        screenPixels=gpu.Trace(scene,screen,screenConstants);
        CheckPixel(screenPixels[4+4*8],{},0,report,"screen-only entrypoint sampled the bright world cache after a screen miss");
        Group(report,"screen_only_entrypoint_preserves_positive_and_black_screen_hits_and_ends_misses");

        scene=SourceScene();screen=Screen{};for(auto& z:screen.depth)z=DeviceDepth(2);p=scene.Parameters();c=gpu.Constants(p);
        auto pixels=gpu.Trace(scene,screen,c);const auto center=pixels[4+4*8];
        CheckPixel(center,cache,1,report,"actual production trace did not read cached radiance");
        gpu.TwoSampleNoise(true);c.rayCount=2;pixels=gpu.Trace(scene,screen,c);const auto mixed=pixels[4+4*8];
        CheckPixel(mixed,Scale(cache,.5f),.5f,report,"actual trace normalized by hits instead of all finite rays");gpu.TwoSampleNoise(false);
        c=gpu.Constants(p);c.normalBias=.5f;pixels=gpu.Trace(scene,screen,c);
        CheckPixel(pixels[4+4*8],{},0,report,"world ray replaced the original SSGI biased origin");
        c=gpu.Constants(p);for(auto& z:screen.depth)z=DeviceDepth(2.01f);pixels=gpu.Trace(scene,screen,c);
        CheckPixel(pixels[4+4*8],cache,1,report,"obsolete raster-to-SDF receiver proof still gates world fallback");
        Group(report,"production_kernel_uses_original_biased_ray_averages_all_samples_without_receiver_reanchor");

        const auto view=DirectX::XMMatrixLookToLH(DirectX::XMVectorSet(5,1,-3,1),DirectX::XMVectorSet(.8660254f,0,.5f,0),DirectX::XMVectorSet(0,1,0,0));
        const auto inverseView=DirectX::XMMatrixInverse(nullptr,view);
        scene=SourceScene();for(auto& mesh:scene.model.meshes)for(auto& v:mesh.vertices)
        {
            DirectX::XMStoreFloat3(&v.Position,DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&v.Position),inverseView));
            DirectX::XMStoreFloat3(&v.Normal,DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&v.Normal),inverseView));
            DirectX::XMStoreFloat3(&v.Tangent,DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&v.Tangent),inverseView));
            DirectX::XMStoreFloat3(&v.Bitangent,DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&v.Bitangent),inverseView));
        }
        scene.world.Invalidate();scene.Build(.5f);XMFLOAT3 worldNormal;
        DirectX::XMStoreFloat3(&worldNormal,DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(1,0,0,0),inverseView));
        screen=Screen{};for(auto& z:screen.depth)z=DeviceDepth(2);for(auto& n:screen.normal)n={worldNormal.x*.5f+.5f,worldNormal.y*.5f+.5f,worldNormal.z*.5f+.5f,1};
        c=gpu.Constants(scene.Parameters());XMFLOAT4X4 projection;
        DirectX::XMStoreFloat4x4(&projection,DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2,1,.1f,20));projection._31=.017f;projection._32=-.011f;
        const auto jitteredProjection=DirectX::XMLoadFloat4x4(&projection);
        c.view=Stored(view);c.invView=Stored(inverseView);c.proj=Stored(jitteredProjection);c.invProj=Stored(DirectX::XMMatrixInverse(nullptr,jitteredProjection));c.invViewProj=Stored(DirectX::XMMatrixInverse(nullptr,view*jitteredProjection));
        ray=RayInput{};ray.operation=3;ray.viewOrigin={.145f,-.125f,2};ray.viewDirection=FixedDirection();
        result=gpu.Rays(scene,screen,{ray},c)[0];XMFLOAT3 expectedOrigin;
        DirectX::XMStoreFloat3(&expectedOrigin,DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&ray.viewOrigin),inverseView));
        Require(result.hit==1&&std::abs(result.distance-.5f)<1e-6f,"camera transform changed finite ray length");CheckColor(result,expectedOrigin,report,"transformed original ray origin");
        ray.operation=4;result=gpu.Rays(scene,screen,{ray},c)[0];XMFLOAT3 expectedDirection;
        DirectX::XMStoreFloat3(&expectedDirection,DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&ray.viewDirection),inverseView));
        CheckColor(result,expectedDirection,report,"transformed original ray direction");
        ray.operation=2;result=gpu.Rays(scene,screen,{ray},c)[0];pixels=gpu.Trace(scene,screen,c);
        Require(result.hit==1&&result.backend==2&&result.anchorStatus==0,"transformed ray did not enter ungated world fallback");CheckColor(result,cache,report,"camera transformed cache query");
        CheckPixel(pixels[4+4*8],cache,1,report,"actual jittered transformed kernel changed cache result",1e-4f);
        Group(report,"rotation_translation_jitter_preserve_original_world_segment_and_cached_query");

        scene=SourceScene();screen=Screen{};c=gpu.Constants(scene.Parameters());ray=RayInput{};ray.operation=3;ray.viewOrigin={0,0,.2f};ray.viewDirection={0,0,-1};
        result=gpu.Rays(scene,screen,{ray},c)[0];Require(result.hit==1&&std::abs(result.distance-.099f)<1e-6f,"world segment did not preserve original near-plane shortening");
        CheckColor(result,ray.viewOrigin,report,"near-plane shortened original origin");
        ray.viewOrigin={0,0,.11f};result=gpu.Rays(scene,screen,{ray},c)[0];Require(result.hit==1&&std::abs(result.distance-.009f)<1e-6f,"near-plane finite segment changed");
        Group(report,"world_projection_preserves_original_SSGI_near_plane_shortening");

        scene=SourceScene(.25f);scene.isCutout=true;scene.materials[0].baseColorFactor.w=0;
        auto covered=scene.materials[0];covered.baseColorFactor.w=1;scene.materials.push_back(covered);
        scene.model.meshes.emplace_back();scene.model.meshes.back().materialID=1;
        Quad(scene.model.meshes.back(),{.5f,-3,0},{.5f,-3,4},{.5f,3,4},{.5f,3,0});scene.world.Invalidate();scene.Build(.5f);
        c=gpu.Constants(scene.Parameters());ray=RayInput{};ray.operation=1;result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==1&&std::abs(result.distance-.5f)<1e-5f,"alpha hole did not continue within finite segment to rear cache surface");CheckColor(result,cache,report,"alpha hole rear cached radiance");
        scene.materials[0].baseColorFactor.w=.5f;result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==1&&std::abs(result.distance-.25f)<1e-5f,"alpha threshold failed nearest geometric termination");
        scene.materials[0].baseColorFactor.w=std::numeric_limits<float>::quiet_NaN();result=gpu.Rays(scene,screen,{ray},c)[0];
        Require(result.hit==0&&result.confidence==0&&result.worldTraceStatus==2,"invalid alpha leaked cache behind bad coverage");CheckColor(result,{},report,"invalid alpha fail closed");
        Group(report,"finite_alpha_holes_continue_covered_hit_stops_and_invalid_coverage_fails_closed");
    }

}
#include "VXGIHybridRDGAcceptance.h"
int main()
{
    try
    {
        std::cout.setf(std::ios::unitbuf);EnableValidation();Report report;const auto production=RunVXGIHybridRDGAcceptance();Group(report,"production_SSGIPass_RDG_finite_cache_only_three_slots_missing_cache_and_resizes");GPU gpu;NumericalAcceptance(gpu,report);report.warnings=gpu.Warnings();Require(report.warnings==0,"debug/GBV reported warnings/errors");
        std::ofstream file("tmp/VXGIHybridGPU-stage77-report.json");file<<std::setprecision(9)<<"{\n  \"adapter\":\""<<gpu.adapterName<<"\",\n  \"debug_layer\":true,\n  \"gpu_based_validation\":true,\n  \"production_source\":\"TraceHybridRay / SSGIWorldRaySource / CSMain_Pyramid / CSMain_Trace\",\n  \"group_count\":"<<report.groups.size()<<",\n  \"rays_compared\":"<<report.rays<<",\n  \"trace_pixels_compared\":"<<report.pixels<<",\n  \"maximum_radiance_error\":"<<report.maximumRadianceError<<",\n  \"maximum_kernel_error\":"<<report.maximumKernelError<<",\n  \"d3d12_warnings_errors\":"<<report.warnings<<",\n  \"passed\":true,\n  \"test_groups\":[";
        for(size_t i=0;i<report.groups.size();++i)file<<(i?",":"")<<"\n    \""<<report.groups[i]<<'"';file<<"\n  ],\n  \"production_wrapper\":{\n    \"classes\":\"RenderDevice + ResourceManager + PipelineManager + VXGIGPUScene + SSGIPass + RDGBuilder + SSGIHybridState\",\n    \"submissions\":"<<production.submissions<<",\n    \"frame_slots\":"<<production.slots<<",\n    \"resizes\":"<<production.resizes<<",\n    \"maximum_history_error\":"<<production.maxError<<",\n    \"window_visible\":"<<(production.hidden?"false":"true")<<",\n    \"present_calls\":"<<(production.present?1:0)<<",\n    \"d3d12_warnings_errors\":"<<production.warnings<<"\n  }\n}\n";
        std::cout<<report.groups.size()<<" groups passed; radiance/kernel errors "<<report.maximumRadianceError<<" / "<<report.maximumKernelError<<"; 0 warnings/errors.\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::ofstream("tmp/VXGIHybridGPU-stage77-report.json")<<"{\"passed\":false}\n";return 1;}
}
