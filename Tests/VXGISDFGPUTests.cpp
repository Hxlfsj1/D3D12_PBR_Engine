// Standalone D3D12 acceptance: production HLSL tracing, real device dispatch,
// readback, and independent CPU BVH comparisons. No application/assets required.
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
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}

namespace
{
    void Require(bool value, const std::string& message)
    { if (!value) throw std::runtime_error(message); }
    void Check(HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            std::ostringstream message;
            message << operation << " failed: HRESULT 0x" << std::hex << uint32_t(result);
            throw std::runtime_error(message.str());
        }
    }
    XMFLOAT3 Add(XMFLOAT3 a, XMFLOAT3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    XMFLOAT3 Sub(XMFLOAT3 a, XMFLOAT3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    XMFLOAT3 Scale(XMFLOAT3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
    float Dot(XMFLOAT3 a, XMFLOAT3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    XMFLOAT3 Cross(XMFLOAT3 a, XMFLOAT3 b)
    { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    XMFLOAT3 Normalize(XMFLOAT3 a)
    { const float length=std::sqrt(Dot(a,a)); return length>0 ? Scale(a,1/length) : XMFLOAT3{}; }
    float Error(XMFLOAT3 a, XMFLOAT3 b)
    { return std::max({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.z-b.z)}); }
    std::string UTF8(const wchar_t* input)
    {
        const int size=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);
        std::string result(size,'\0');
        WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),size,nullptr,nullptr);
        if (!result.empty()) result.pop_back();
        return result;
    }

    struct GPUConstants
    {
        XMFLOAT3 origin; float cellSize;
        std::array<uint32_t,3> cellDimensions; uint32_t rayCount;
        XMFLOAT3 boundsMaximum; uint32_t padding=0;
    };
    struct GPURay
    {
        XMFLOAT3 origin; float maximumDistance=100;
        XMFLOAT3 direction; float minimumDistance=0;
    };
    struct GPUResult
    {
        XMFLOAT3 position; float distance;
        XMFLOAT3 geometricNormal; uint32_t valid;
        XMFLOAT3 barycentric; uint32_t triangleId;
        uint32_t triangleIndex,backFace,materialId,instanceId;
        XMFLOAT2 uv; XMFLOAT2 uvPadding;
        XMFLOAT3 shadingNormal; uint32_t traceStatus;
        XMFLOAT3 tangent; float tangentPadding;
        XMFLOAT3 bitangent; float bitangentPadding;
    };
    struct GPUTriangle
    {
        XMFLOAT3 p0; uint32_t triangleId;
        XMFLOAT3 p1; uint32_t flags;
        XMFLOAT3 p2; uint32_t pad0;
        XMFLOAT3 geometricNormal; uint32_t pad1;
        XMFLOAT2 uv0,uv1,uv2;
        XMFLOAT3 shadingNormal0,shadingNormal1,shadingNormal2;
        XMFLOAT3 tangent0,tangent1,tangent2;
        XMFLOAT3 bitangent0,bitangent1,bitangent2;
        uint32_t materialId,instanceId,pad2;
    };
    static_assert(sizeof(GPUConstants)==48 && sizeof(GPURay)==32 && sizeof(GPUResult)==128);
    static_assert(sizeof(GPUTriangle)==208 && offsetof(GPUTriangle,uv0)==64 && offsetof(GPUTriangle,materialId)==196);

    ComPtr<IDxcBlob> Compile()
    {
        ComPtr<IDxcUtils> utils; ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(L"Shaders/VXGISDFValidate.hlsl",nullptr,&source),"load validation shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes),"create include handler");
        const wchar_t* arguments[]={L"Shaders/VXGISDFValidate.hlsl",L"-E",L"CSMain",L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",L"-O3"};
        DxcBuffer input{source->GetBufferPointer(),source->GetBufferSize(),DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        Check(compiler->Compile(&input,arguments,UINT(std::size(arguments)),includes.Get(),IID_PPV_ARGS(&result)),"compile validation shader");
        ComPtr<IDxcBlobUtf8> errors;
        Check(result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr),"read DXC diagnostics");
        if (errors && errors->GetStringLength()) std::cerr << errors->GetStringPointer();
        HRESULT status; Check(result->GetStatus(&status),"read compile status"); Check(status,"validation shader");
        ComPtr<IDxcBlob> object;
        Check(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&object),nullptr),"read shader object");
        return object;
    }

    class GPU
    {
    public:
        GPU()
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer(); debugLayer=true;
                ComPtr<ID3D12Debug1> validation;
                if (SUCCEEDED(debug.As(&validation))) { validation->SetEnableGPUBasedValidation(TRUE); gpuValidation=true; }
            }
            ComPtr<IDXGIFactory6> factory;
            Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create DXGI factory");
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index=0;factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++index)
            {
                DXGI_ADAPTER_DESC1 description; Check(adapter->GetDesc1(&description),"get adapter description");
                if (!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) break;
                adapter.Reset();
            }
            if (!device)
            {
                Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"get WARP adapter");
                Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"create WARP device");
                warp=true;
            }
            DXGI_ADAPTER_DESC1 description; Check(adapter->GetDesc1(&description),"get selected adapter");
            adapterName=UTF8(description.Description);
            std::cout << "Actual device: " << adapterName << (warp ? " (WARP software)" : " (hardware)") << '\n';
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
            Check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)),"query shader model");
            Require(model.HighestShaderModel>=D3D_SHADER_MODEL_6_6,"Selected adapter lacks Shader Model 6.6.");
            if (debugLayer) Check(device.As(&info),"get D3D12 debug info queue");
            D3D12_COMMAND_QUEUE_DESC queueDescription{};
            Check(device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(&queue)),"create command queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create command allocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"create command list");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create fence");
            event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"create fence event failed");
            std::array<CD3DX12_ROOT_PARAMETER,7> parameters;
            parameters[0].InitAsConstants(sizeof(GPUConstants)/4,0);
            for (UINT i=0;i<5;++i) parameters[i+1].InitAsShaderResourceView(i);
            parameters[6].InitAsUnorderedAccessView(0);
            CD3DX12_ROOT_SIGNATURE_DESC rootDescription(UINT(parameters.size()),parameters.data());
            ComPtr<ID3DBlob> rootBlob;
            Check(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,nullptr),"serialize test root signature");
            Check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"create test root signature");
            auto shader=Compile();
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
            pipelineDescription.pRootSignature=root.Get(); pipelineDescription.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&pipelineDescription,IID_PPV_ARGS(&pipeline)),"create validation PSO");
        }
        ~GPU() { if (event) CloseHandle(event); }

        std::vector<GPUResult> Dispatch(const VXGISDF& sdf,const std::vector<GPURay>& rays)
        {
            std::vector<GPUTriangle> triangles;
            for (uint32_t index=0;index<sdf.GetTriangles().size();++index)
            {
                const auto& source=sdf.GetTriangles()[index];
                GPUTriangle t{};
                t.p0=source.positions[0];t.p1=source.positions[1];t.p2=source.positions[2];
                t.triangleId=index;t.flags=source.isCutout;t.geometricNormal=source.geometricNormal;
                t.uv0=source.uv[0];t.uv1=source.uv[1];t.uv2=source.uv[2];
                t.shadingNormal0=source.shadingNormals[0];t.shadingNormal1=source.shadingNormals[1];t.shadingNormal2=source.shadingNormals[2];
                t.tangent0=source.tangents[0];t.tangent1=source.tangents[1];t.tangent2=source.tangents[2];
                t.bitangent0=source.bitangents[0];t.bitangent1=source.bitangents[1];t.bitangent2=source.bitangents[2];
                t.materialId=source.materialId;t.instanceId=source.instanceId;
                triangles.push_back(t);
            }
            auto nodes=Upload(sdf.GetNodes());auto offsets=Upload(sdf.GetCellOffsets());
            auto references=Upload(sdf.GetTriangleReferences());auto triangleBuffer=Upload(triangles);auto rayBuffer=Upload(rays);
            const UINT64 outputBytes=rays.size()*sizeof(GPUResult);
            auto output=Buffer(outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback=Buffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto& grid=sdf.GetGrid();
            GPUConstants constants{grid.origin,grid.cellSize,grid.cellDimensions,uint32_t(rays.size()),grid.boundsMaximum};
            commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
            const std::array<ID3D12Resource*,5> inputs={nodes.Get(),offsets.Get(),references.Get(),triangleBuffer.Get(),rayBuffer.Get()};
            for (UINT index=0;index<inputs.size();++index) commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(6,output->GetGPUVirtualAddress());
            auto beginWrite=CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1,&beginWrite);
            commands->Dispatch((UINT(rays.size())+63)/64,1,1);
            auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1,&barrier);commands->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);
            Check(commands->Close(),"close command list");
            ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
            Check(queue->Signal(fence.Get(),++fenceValue),"signal compute fence");
            Check(fence->SetEventOnCompletion(fenceValue,event),"arm compute fence");
            Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU dispatch did not complete in 30 seconds.");
            Require(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU device removed during dispatch.");
            std::vector<GPUResult> result(rays.size()); void* mapped=nullptr;
            const D3D12_RANGE reads{0,SIZE_T(outputBytes)};Check(readback->Map(0,&reads,&mapped),"map GPU result");
            std::memcpy(result.data(),mapped,size_t(outputBytes)); const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);
            Check(allocator->Reset(),"reset completed allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset completed commands");
            return result;
        }
        uint32_t Warnings()
        {
            uint32_t warnings=0;
            if (info) for (UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T size=0;Check(info->GetMessage(index,nullptr,&size),"measure debug message");
                std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(info->GetMessage(index,message,&size),"get debug message");
                if (message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) {std::cerr << "D3D12: " << message->pDescription << '\n';++warnings;}
            }
            return warnings;
        }
        std::string adapterName;bool warp=false,debugLayer=false,gpuValidation=false;
    private:
        ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            Require(bytes>0,"Empty GPU test resource.");ComPtr<ID3D12Resource> result;
            const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,state,nullptr,IID_PPV_ARGS(&result)),"create test buffer");
            return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& data)
        {
            auto result=Buffer(data.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;const D3D12_RANGE noReads{0,0};Check(result->Map(0,&noReads,&mapped),"map test upload");
            std::memcpy(mapped,data.data(),data.size()*sizeof(T));result->Unmap(0,nullptr);return result;
        }
        ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;UINT64 fenceValue=0;HANDLE event=nullptr;
    };

    struct TestResource {void* Get() const{return nullptr;}};
    struct TestTexture {uint32_t type=0;TestResource Resource;std::string path;};
    struct TestVertex {XMFLOAT3 Position,Normal;XMFLOAT2 TexCoords;XMFLOAT3 Tangent,Bitangent;};
    struct TestMesh
    {
        std::vector<TestVertex> vertices;std::vector<std::vector<unsigned>> lodIndices={{}};
        std::vector<TestTexture> textures;uint32_t materialID=7;bool isUnlit=false;
        DirectX::XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};
    };
    struct TestModel {std::vector<TestMesh> meshes={{}};};
    struct TestInstance
    {
        uint32_t editorId=17;TestModel* pModel=nullptr;
        XMFLOAT3 translation={},rotation={},scale={1,1,1};uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=true;int currentLodLevel=0;
    };
    void Triangle(TestModel& model,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c)
    {
        auto& mesh=model.meshes[0];const uint32_t start=uint32_t(mesh.vertices.size());
        const auto normal=Normalize(Cross(Sub(b,a),Sub(c,a)));const auto tangent=Normalize(Sub(b,a));
        const auto bitangent=Normalize(Cross(normal,tangent));
        mesh.vertices.push_back({a,normal,{.13f,.21f},tangent,bitangent});
        mesh.vertices.push_back({b,normal,{.83f,.17f},tangent,bitangent});
        mesh.vertices.push_back({c,normal,{.29f,.91f},tangent,bitangent});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{start,start+1,start+2});
    }
    void Quad(TestModel& model,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d)
    {Triangle(model,a,b,c);Triangle(model,a,c,d);}
    TestModel Box()
    {
        TestModel model;
        Quad(model,{-1,-1,-1},{-1,1,-1},{1,1,-1},{1,-1,-1});
        Quad(model,{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1});
        Quad(model,{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1});
        Quad(model,{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1});
        Quad(model,{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1});
        Quad(model,{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1});
        return model;
    }
    GPURay Ray(XMFLOAT3 origin,XMFLOAT3 direction,float maximumDistance=100)
    {return {origin,maximumDistance,Normalize(direction),0};}
    struct Group {std::string name;uint32_t rays=0,hits=0,misses=0,backFaces=0;float maxDistanceError=0,maxPositionError=0,maxBarycentricError=0,maxAttributeError=0;};
    XMFLOAT3 Interpolate(const std::array<XMFLOAT3,3>& a,XMFLOAT3 b)
    {return Add(Add(Scale(a[0],b.x),Scale(a[1],b.y)),Scale(a[2],b.z));}

    Group Compare(GPU& gpu,const std::string& name,TestModel& model,TestInstance instance,float cellSize,std::vector<GPURay> rays)
    {
        instance.pModel=&model;std::vector<TestInstance> instances={instance};VXGIWorld world;VXGISDF sdf;
        Require(world.Update(instances,.5f),"world build: "+world.GetLastError());
        Require(sdf.Update(world,cellSize,128),"SDF bake: "+sdf.GetLastError());
        auto actual=gpu.Dispatch(sdf,rays);Group group;group.name=name;group.rays=uint32_t(rays.size());
        for (size_t i=0;i<rays.size();++i)
        {
            const auto& ray=rays[i];const auto reference=sdf.TraceReference(ray.origin,ray.direction,ray.maximumDistance);const auto& result=actual[i];
            const std::string label=name+" ray "+std::to_string(i);
            const bool invalidDirection=Dot(ray.direction,ray.direction)==0;
            Require(result.traceStatus==(invalidDirection?2u:(reference.hit?1u:0u)),label+": trace status mismatch/exhaustion");
            Require(bool(result.valid)==reference.hit,label+": GPU/BVH hit mismatch");
            if (!reference.hit) {++group.misses;continue;} ++group.hits;group.backFaces+=result.backFace;
            const float distanceError=std::abs(result.distance-reference.distance);
            group.maxDistanceError=std::max(group.maxDistanceError,distanceError);
            group.maxPositionError=std::max(group.maxPositionError,Error(result.position,reference.position));
            group.maxBarycentricError=std::max(group.maxBarycentricError,Error(result.barycentric,reference.barycentrics));
            const float tolerance=std::max(3e-5f,cellSize*2e-4f);
            Require(std::isfinite(result.distance) && distanceError<tolerance,label+": hit distance mismatch");
            Require(Error(result.position,reference.position)<tolerance,label+": hit position mismatch");
            Require(result.triangleId==reference.triangleIndex && result.triangleIndex==reference.triangleIndex,label+": triangle identity mismatch");
            Require(Error(result.barycentric,reference.barycentrics)<2e-4f,label+": barycentrics mismatch");
            Require(Error(result.geometricNormal,reference.geometricNormal)<2e-5f,label+": geometric normal mismatch");
            Require(bool(result.backFace)==reference.backFace,label+": backface mismatch");
            Require(result.materialId==reference.materialId && result.instanceId==reference.instanceId,label+": material/instance identity mismatch");
            const auto& triangle=sdf.GetTriangles()[reference.triangleIndex];const auto b=reference.barycentrics;
            const XMFLOAT2 uv={triangle.uv[0].x*b.x+triangle.uv[1].x*b.y+triangle.uv[2].x*b.z,
                triangle.uv[0].y*b.x+triangle.uv[1].y*b.y+triangle.uv[2].y*b.z};
            const auto n=Normalize(Interpolate(triangle.shadingNormals,b));auto t=Interpolate(triangle.tangents,b);
            t=Normalize(Sub(t,Scale(n,Dot(n,t))));const auto rawB=Interpolate(triangle.bitangents,b);const auto crossNT=Cross(n,t);
            const auto bitangent=Scale(crossNT,Dot(crossNT,rawB)<0 ? -1.0f : 1.0f);
            const float attributeError=std::max({std::abs(result.uv.x-uv.x),std::abs(result.uv.y-uv.y),Error(result.shadingNormal,n),Error(result.tangent,t),Error(result.bitangent,bitangent)});
            group.maxAttributeError=std::max(group.maxAttributeError,attributeError);
            Require(attributeError<3e-4f,label+": recovered UV/TBN mismatch");
            Require(std::abs(Dot(result.shadingNormal,result.tangent))<1e-4f && std::abs(Dot(result.shadingNormal,result.bitangent))<1e-4f &&
                std::abs(Dot(result.tangent,result.bitangent))<1e-4f,label+": recovered TBN is not orthogonal");
        }
        std::cout << name << ": " << group.rays << " rays, " << group.hits << " hits, " << group.misses << " misses, " << group.backFaces << " backside hits, maximum distance error " << group.maxDistanceError << '\n';
        return group;
    }
}

int main()
{
    try
    {
        GPU gpu;std::vector<Group> groups;TestInstance instance;
        TestModel ground;Quad(ground,{-2,0,-2},{-2,0,2},{2,0,2},{2,0,-2});
        groups.push_back(Compare(gpu,"open_ground_both_sides",ground,instance,.25f,
            {Ray({.23f,2,.47f},{0,-1,0}),Ray({.23f,-2,.47f},{0,1,0}),Ray({3,2,0},{0,-1,0}),Ray({.2f,.4f,.7f},{1,0,0}),Ray({.1f,2,.7f},{0,-1,0},1),
             {{.23f,2,.47f},100,{0,-17,0},0},Ray({.2f,2,.7f},{0,0,0}),Ray({.3f,2,.3f},{0,-1,0})}));
        TestModel wall;Quad(wall,{.013f,-2,-2},{.013f,2,-2},{.013f,2,2},{.013f,-2,2});wall.meshes[0].baseColorFactor={0,0,0,1};
        groups.push_back(Compare(gpu,"thin_black_wall_and_grazing",wall,instance,.25f,
            {Ray({-2,.11f,.43f},{1,0,0}),Ray({2,.11f,.43f},{-1,0,0}),Ray({-.0001f,-1,.3f},{.003f,1,0}),Ray({-2,3,.4f},{1,0,0}),Ray({-1,.25f,.5f},{1,0,0})}));
        auto box=Box();std::vector<GPURay> boxRays={Ray({0,.19f,.37f},{1,0,0}),Ray({3,.19f,.37f},{-1,0,0}),Ray({0,.19f,.37f},{0,0,1}),Ray({3,3,3},{1,1,1}),Ray({0,0,0},{.13f,.41f,1})};
        std::mt19937 random(0x534446);std::uniform_real_distribution<float> unit(-1,1);
        for (uint32_t i=0;i<256;++i)
        {
            const XMFLOAT3 target={unit(random)*.8f,unit(random)*.8f,unit(random)*.8f};
            const XMFLOAT3 origin=Scale(Normalize({unit(random),unit(random),unit(random)}),4.0f);
            boxRays.push_back(Ray(origin,Sub(target,origin)));
            boxRays.push_back(Ray(target,{unit(random),unit(random),unit(random)}));
        }
        groups.push_back(Compare(gpu,"closed_box_inside_outside_random",box,instance,.25f,boxRays));
        instance.scale={-1.7f,.63f,1.23f};instance.rotation={.17f,.31f,-.23f};instance.translation={.37f,-.21f,.18f};instance.customMaterialID=29;
        std::vector<GPURay> transformed;
        for (uint32_t i=0;i<128;++i)
        {
            const XMFLOAT3 origin=Add(instance.translation,Scale(Normalize({unit(random),unit(random),unit(random)}),4.0f));
            transformed.push_back(Ray(origin,Sub(instance.translation,origin)));
            transformed.push_back(Ray(instance.translation,{unit(random),unit(random),unit(random)}));
        }
        groups.push_back(Compare(gpu,"negative_nonuniform_rotated_box",box,instance,.2f,transformed));
        TestModel boundary;Triangle(boundary,{0,-2,-2},{0,2,-2},{0,0,3});instance={};
        groups.push_back(Compare(gpu,"cell_faces_edges_corners",boundary,instance,.25f,
            {Ray({-1,.25f,.5f},{1,0,0}),Ray({1,.25f,.5f},{-1,0,0}),Ray({-1,-1,-1},{1,1,1}),Ray({1,-1,-1},{-1,1,1}),Ray({-.5f,.2f,.3f},{1,0,0}),Ray({.5f,.2f,.3f},{-1,0,0}),Ray({0,.2f,.3f},{1,0,0})}));
        TestModel shortEntry;
        Triangle(shortEntry,{-5e-7f,-2,-2},{-5e-7f,2,-2},{-5e-7f,0,3});
        Triangle(shortEntry,{5e-7f,-2,-2},{5e-7f,2,-2},{5e-7f,0,3});
        groups.push_back(Compare(gpu,"sub_epsilon_initial_cell_thin_shell",shortEntry,instance,.25f,
            {Ray({-1e-6f,.2f,.3f},{1,0,0}),Ray({1e-6f,.2f,.3f},{-1,0,0}),Ray({0,.2f,.3f},{1,0,0}),Ray({0,.2f,.3f},{-1,0,0})}));
        instance.scale={.35f,.31f,.29f};instance.translation={10000,-10000,10000};
        std::vector<GPURay> distantRays;
        for (uint32_t i=0;i<64;++i)
        {
            const XMFLOAT3 origin=Add(instance.translation,Scale(Normalize({unit(random),unit(random),unit(random)}),2.0f));
            const XMFLOAT3 target=Add(instance.translation,{.07f,-.11f,.03f});
            distantRays.push_back(Ray(origin,Sub(target,origin)));
            distantRays.push_back(Ray(target,{unit(random),unit(random),unit(random)}));
        }
        groups.push_back(Compare(gpu,"distant_origin_fractional_grid",box,instance,.03f,distantRays));
        const uint32_t warnings=gpu.Warnings();uint32_t rays=0;for(const auto& group:groups) rays+=group.rays;
        std::ofstream report("tmp/VXGISDFGPU-report.json");Require(bool(report),"cannot write GPU acceptance report");
        report << std::setprecision(9) << "{\n  \"adapter\": \"" << gpu.adapterName << "\",\n  \"warp\": " << (gpu.warp?"true":"false") << ",\n  \"debug_layer\": " << (gpu.debugLayer?"true":"false")
            << ",\n  \"gpu_based_validation\": " << (gpu.gpuValidation?"true":"false") << ",\n  \"production_trace\": \"VXGISDFCommon.hlsli::VXGISDFTrace\",\n  \"groups\": [\n";
        for (size_t index=0;index<groups.size();++index)
        {
            const auto& group=groups[index];if(index)report << ",\n";
            report << "    {\"name\":\"" << group.name << "\",\"rays\":" << group.rays << ",\"hits\":" << group.hits << ",\"misses\":" << group.misses << ",\"back_faces\":" << group.backFaces
                << ",\"max_distance_error\":" << group.maxDistanceError << ",\"max_position_error\":" << group.maxPositionError << ",\"max_barycentric_error\":" << group.maxBarycentricError << ",\"max_attribute_error\":" << group.maxAttributeError << '}';
        }
        report << "\n  ],\n  \"rays\": " << rays << ",\n  \"d3d12_warnings_errors\": " << warnings << ",\n  \"passed\": " << (warnings?"false":"true") << "\n}\n";
        Require(warnings==0,"D3D12 validation emitted warnings/errors");
        std::cout << "VXGISDF GPU acceptance: all " << groups.size() << " groups / " << rays << " rays passed; 0 D3D12 warnings/errors.\n";
        return 0;
    }
    catch(const std::exception& error)
    {std::cerr << "VXGISDF GPU acceptance FAILED: " << error.what() << '\n';return 1;}
}
