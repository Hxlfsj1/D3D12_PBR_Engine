// One shader compares the production trace using original CSR and packed BVH.
// Actual alpha textures and hardware DX12 are used; the HWND is never shown.
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
#include "../Resources/VXGITraceAcceleration.h"
#include "../Resources/VXGISurface.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
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
    struct Vertex { XMFLOAT3 Position,Normal={0,0,1};XMFLOAT2 TexCoords;XMFLOAT3 Tangent={1,0,0},Bitangent={0,1,0}; };
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
        uint32_t editorId;Model* pModel;XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;bool isTransparent=false,isCutout=false;
    };
    struct Plane{float z;bool cutout;uint32_t material;bool invalidUV=false;};
    struct Scene
    {
        std::vector<Model> models;std::vector<Instance> instances;VXGIWorld world;VXGISDF sdf;
        explicit Scene(const std::vector<Plane>& planes,XMFLOAT3 translation={})
        {
            models.reserve(planes.size());instances.reserve(planes.size());
            for(size_t index=0;index<planes.size();++index)
            {
                const auto& plane=planes[index];Mesh mesh;mesh.materialID=plane.material;
                mesh.vertices={{{-.4f,-.4f,plane.z},{0,0,1},{0,0}},{{.4f,-.4f,plane.z},{0,0,1},{1,0}},
                    {{.4f,.4f,plane.z},{0,0,1},{1,1}},{{-.4f,.4f,plane.z},{0,0,1},{0,1}}};
                if(plane.invalidUV)for(auto& vertex:mesh.vertices)vertex.TexCoords.x=std::numeric_limits<float>::quiet_NaN();
                mesh.lodIndices={{0,1,2,0,2,3}};models.push_back({{std::move(mesh)}});
                Instance instance{uint32_t(101+index),&models.back()};instance.isCutout=plane.cutout;instance.translation=translation;instances.push_back(instance);
            }
            Require(world.Update(instances,.5f),world.GetLastError());Require(sdf.Update(world,.5f,128),sdf.GetLastError());
        }
    };
    struct Triangle
    {
        XMFLOAT3 p0;uint32_t triangleId;XMFLOAT3 p1;uint32_t flags;XMFLOAT3 p2;uint32_t pad0;
        XMFLOAT3 normal;uint32_t pad1;std::array<XMFLOAT2,3> uv;
        std::array<XMFLOAT3,3> normals,tangents,bitangents;uint32_t materialId,instanceId,pad2;
    };
    struct Parameters
    {
        XMFLOAT3 origin;float cellSize;std::array<uint32_t,3> dimensions;uint32_t triangleCount;
        XMFLOAT3 maximum;uint32_t materialCount;uint32_t rayCount,padding[3]={};
    };
    struct Ray
    {
        XMFLOAT3 origin={.123f,-.187f,-.25f};float minimum=0;
        XMFLOAT3 direction={0,0,1};float maximum=2;
        float mip=0;uint32_t padding[3]={};
    };
    struct Hit
    {
        XMFLOAT3 position;float distance;XMFLOAT3 barycentric;uint32_t triangleIndex;
        XMFLOAT3 normal;uint32_t triangleId;uint32_t backFace,valid,status,reserved;
    };
    struct Pair{Hit linear,accelerated;};
    static_assert(sizeof(Triangle)==208&&sizeof(Parameters)==64&&sizeof(Ray)==48&&sizeof(Hit)==64&&sizeof(Pair)==128);
    using Material=VXGISurface::MaterialRecord;
    std::vector<Material> Materials()
    {
        std::vector<Material> values(5);
        values[0].albedoIdx=0;values[1].albedoIdx=0;values[2].albedoIdx=1;
        values[3].albedoIdx=1;values[3].baseColorFactor.w=std::numeric_limits<float>::quiet_NaN();
        values[4].albedoIdx=2;return values;
    }
    ComPtr<IDxcBlob> Compile()
    {
        ComPtr<IDxcUtils> utils;ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        const wchar_t* path=L"Shaders/VXGIDenseBVHValidate.hlsl";ComPtr<IDxcBlobEncoding> source;Check(utils->LoadFile(path,nullptr,&source),"load dense shader");
        ComPtr<IDxcIncludeHandler> include;Check(utils->CreateDefaultIncludeHandler(&include),"create includes");
        std::vector<const wchar_t*> args={path,L"-E",L"CSMain",L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",L"-WX"};
#if defined(_DEBUG)
        args.insert(args.end(),{L"-Od",L"-Zi",L"-Qembed_debug"});
#else
        args.push_back(L"-O3");
#endif
        DxcBuffer input{source->GetBufferPointer(),source->GetBufferSize(),DXC_CP_UTF8};ComPtr<IDxcResult> result;Check(compiler->Compile(&input,args.data(),UINT(args.size()),include.Get(),IID_PPV_ARGS(&result)),"compile dense shader");
        ComPtr<IDxcBlobUtf8> diagnostics;Check(result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&diagnostics),nullptr),"get DXC diagnostics");
        if(diagnostics&&diagnostics->GetStringLength())std::cerr<<diagnostics->GetStringPointer();HRESULT status;Check(result->GetStatus(&status),"get DXC status");Check(status,"dense shader status");
        ComPtr<IDxcBlob> blob;Check(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&blob),nullptr),"get dense bytecode");return blob;
    }
    class GPU
    {
    public:
        GPU()
        {
            WNDCLASSW window{};window.lpfnWndProc=DefWindowProcW;window.hInstance=GetModuleHandleW(nullptr);window.lpszClassName=L"VXGIDenseBackgroundTest";
            Require(RegisterClassW(&window)!=0,"register hidden HWND");hidden=CreateWindowExW(0,window.lpszClassName,L"Dense background test",WS_POPUP,0,0,16,16,nullptr,nullptr,window.hInstance,nullptr);Require(hidden&&!IsWindowVisible(hidden),"dense HWND must remain hidden");
            ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get D3D debug");debug->EnableDebugLayer();ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get GBV");validation->SetEnableGPUBasedValidation(TRUE);
            ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create DXGI factory");ComPtr<IDXGIAdapter1> adapter;
            for(UINT index=0;factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++index)
            {
                DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"describe adapter");if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;adapter.Reset();
            }
            Require(device!=nullptr,"dense tests require hardware DX12");DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc),"describe selected GPU");adapterName=UTF8(desc.Description);Check(device.As(&info),"get InfoQueue");
            D3D12_COMMAND_QUEUE_DESC queueDesc{};Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"create queue");Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create allocator");Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"create commands");Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"create event");
            CD3DX12_ROOT_PARAMETER roots[10];roots[0].InitAsConstants(sizeof(Parameters)/4,0);for(UINT index=0;index<8;++index)roots[index+1].InitAsShaderResourceView(index);roots[9].InitAsUnorderedAccessView(0);CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            CD3DX12_ROOT_SIGNATURE_DESC rootDesc(10,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);ComPtr<ID3DBlob> serialized;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"serialize root");Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"create root");
            const auto shader=Compile();D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)),"create PSO");std::cout<<"Dense GPU equivalence on "<<adapterName<<"; debug + GBV enabled\n";
        }
        ~GPU(){if(event)CloseHandle(event);if(hidden)DestroyWindow(hidden);UnregisterClassW(L"VXGIDenseBackgroundTest",GetModuleHandleW(nullptr));}
        std::vector<Pair> Dispatch(const Scene& scene,const std::vector<Ray>& rays,const std::vector<Material>& materialValues)
        {
            Require(!IsWindowVisible(hidden),"dense HWND became visible");
            auto packed=VXGITraceAcceleration::Build(scene.sdf.GetCellOffsets(),scene.sdf.GetTriangleReferences(),scene.sdf.GetTriangles(),scene.sdf.GetGrid().cellSize);
            Require(packed.stats.acceleratedCells>0,"fixture must exercise actual dense cells");denseCells+=packed.stats.acceleratedCells;
            UploadTextures();std::vector<Triangle> triangleValues;
            for(uint32_t index=0;index<scene.sdf.GetTriangles().size();++index)
            {
                const auto& source=scene.sdf.GetTriangles()[index];Triangle t{};t.p0=source.positions[0];t.p1=source.positions[1];t.p2=source.positions[2];t.triangleId=index;t.flags=source.isCutout;t.normal=source.geometricNormal;t.uv=source.uv;t.normals=source.shadingNormals;t.tangents=source.tangents;t.bitangents=source.bitangents;t.materialId=source.materialId;t.instanceId=source.instanceId;triangleValues.push_back(t);
            }
            const std::array<ComPtr<ID3D12Resource>,8> inputs={Upload(scene.sdf.GetNodes()),Upload(scene.sdf.GetCellOffsets()),Upload(scene.sdf.GetTriangleReferences()),Upload(packed.offsets),Upload(packed.references),Upload(triangleValues),Upload(materialValues),Upload(rays)};
            const uint64_t bytes=rays.size()*sizeof(Pair);auto output=Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),readback=Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            Parameters p{};const auto& grid=scene.sdf.GetGrid();p.origin=grid.origin;p.cellSize=grid.cellSize;p.dimensions=grid.cellDimensions;p.maximum=grid.boundsMaximum;p.triangleCount=uint32_t(triangleValues.size());p.materialCount=uint32_t(materialValues.size());p.rayCount=uint32_t(rays.size());
            ID3D12DescriptorHeap* heaps[]={textureHeap.Get()};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipeline.Get());commands->SetComputeRoot32BitConstants(0,sizeof(p)/4,&p,0);for(UINT index=0;index<inputs.size();++index)commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());commands->SetComputeRootUnorderedAccessView(9,output->GetGPUVirtualAddress());
            Transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);commands->Dispatch((UINT(rays.size())+63)/64,1,1);Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->CopyResource(readback.Get(),output.Get());Check(commands->Close(),"close dispatch");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),++fenceValue),"signal fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm event");Require(WaitForSingleObject(event,60000)==WAIT_OBJECT_0,"dense GPU timeout");Check(device->GetDeviceRemovedReason(),"device removed reason");
            std::vector<Pair> result(rays.size());void* mapped=nullptr;const D3D12_RANGE range{0,SIZE_T(bytes)};Check(readback->Map(0,&range,&mapped),"map results");std::memcpy(result.data(),mapped,size_t(bytes));const D3D12_RANGE writes{0,0};readback->Unmap(0,&writes);Check(allocator->Reset(),"reset allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset commands");textures.clear();textureUploads.clear();++submissions;return result;
        }
        uint32_t Warnings()
        {
            uint32_t count=0;for(UINT64 index=0;index<info->GetNumStoredMessages();++index){SIZE_T size=0;Check(info->GetMessage(index,nullptr,&size),"measure debug message");std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(index,message,&size),"read debug message");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';++count;}}return count;
        }
        std::string adapterName;uint64_t denseCells=0;uint32_t submissions=0;
    private:
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);ComPtr<ID3D12Resource> resource;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"create buffer");return resource;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            Require(!values.empty(),"fixture input cannot be empty");auto resource=Buffer(values.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;const D3D12_RANGE range{0,0};Check(resource->Map(0,&range,&mapped),"map upload");std::memcpy(mapped,values.data(),values.size()*sizeof(T));resource->Unmap(0,nullptr);return resource;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);commands->ResourceBarrier(1,&barrier);}
        void UploadTextures()
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&textureHeap)),"create texture heap");const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const std::array<XMFLOAT4,3> pixels={XMFLOAT4{1,1,1,0},XMFLOAT4{1,1,1,.5f},XMFLOAT4{1,1,1,std::numeric_limits<float>::quiet_NaN()}};
            for(UINT index=0;index<pixels.size();++index)
            {
                const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,1,1);const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> texture;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"create alpha texture");
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map texture upload");std::memcpy(static_cast<uint8_t*>(mapped)+footprint.Offset,&pixels[index],sizeof(XMFLOAT4));upload->Unmap(0,nullptr);Transition(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprint),destination(texture.Get(),0);commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;const CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor(textureHeap->GetCPUDescriptorHandleForHeapStart(),INT(index),increment);device->CreateShaderResourceView(texture.Get(),&view,descriptor);textures.push_back(texture);textureUploads.push_back(upload);
            }
        }
        HWND hidden=nullptr;HANDLE event=nullptr;uint64_t fenceValue=0;
        ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12DescriptorHeap> textureHeap;std::vector<ComPtr<ID3D12Resource>> textures,textureUploads;
    };
    std::vector<Plane> Layers(bool alpha)
    {
        std::vector<Plane> planes;for(uint32_t index=0;index<48;++index)planes.push_back({.1f+.003f*float(index),alpha,index%5==4?2u:0u});return planes;
    }
    std::vector<Ray> Rays(XMFLOAT3 translation={})
    {
        std::vector<Ray> rays;std::mt19937 random(73031);std::uniform_real_distribution<float> unit(-.35f,.35f);
        for(uint32_t index=0;index<384;++index)
        {
            const float x=index==0?.123f:unit(random),y=index==0?-.187f:unit(random);Ray front;front.origin={x+translation.x,y+translation.y,-.25f+translation.z};front.direction={0,0,1};rays.push_back(front);Ray back=front;back.origin.z=.49f+translation.z;back.direction={0,0,-1};rays.push_back(back);
            Ray angled=front;angled.direction={unit(random)*.15f,unit(random)*.15f,1};rays.push_back(angled);
        }
        for(float x:{-.40001f,-.4f,0.f,.4f,.40001f})for(float y:{-.40001f,-.4f,0.f,.4f,.40001f}){Ray ray;ray.origin={x+translation.x,y+translation.y,-.25f+translation.z};rays.push_back(ray);}
        Ray zeroAxis;zeroAxis.origin={-.4f+translation.x,-.4f+translation.y,-.25f+translation.z};rays.push_back(zeroAxis);Ray grazing=zeroAxis;grazing.direction={1e-8f,0,1};rays.push_back(grazing);
        Ray inside;inside.origin={.123f+translation.x,-.187f+translation.y,.15f+translation.z};inside.minimum=.0001f;rays.push_back(inside);inside.direction={0,0,-1};rays.push_back(inside);
        Ray miss;miss.origin={.48f+translation.x,.48f+translation.y,-.25f+translation.z};rays.push_back(miss);return rays;
    }
    struct Group{std::string name;uint32_t rays=0,hits=0,misses=0,invalid=0;float error=0;uint32_t bitwiseDifferences=0;};
    Group Compare(GPU& gpu,const std::string& name,const std::vector<Plane>& planes,const std::vector<Ray>& rays,XMFLOAT3 translation={},uint32_t expectedFirstTriangle=UINT32_MAX,uint32_t expectedFirstStatus=UINT32_MAX)
    {
        Scene scene(planes,translation);auto pairs=gpu.Dispatch(scene,rays,Materials());Group group;group.name=name;group.rays=uint32_t(rays.size());
        Require(!pairs.empty(),name+" has no result");
        if(expectedFirstTriangle!=UINT32_MAX)Require(pairs[0].linear.valid==1&&pairs[0].linear.triangleIndex==expectedFirstTriangle,name+" independent first-ray triangle mismatch");
        if(expectedFirstStatus!=UINT32_MAX)Require(pairs[0].linear.status==expectedFirstStatus,name+" independent first-ray status mismatch");
        for(size_t index=0;index<pairs.size();++index)
        {
            const auto& a=pairs[index].linear;const auto& b=pairs[index].accelerated;const std::string label=name+" ray "+std::to_string(index);
            Require(a.status==b.status&&a.valid==b.valid&&a.triangleIndex==b.triangleIndex&&a.triangleId==b.triangleId&&a.backFace==b.backFace,label+" hit/status identity mismatch");
            const std::array<float,10> left={a.position.x,a.position.y,a.position.z,a.distance,a.barycentric.x,a.barycentric.y,a.barycentric.z,a.normal.x,a.normal.y,a.normal.z},right={b.position.x,b.position.y,b.position.z,b.distance,b.barycentric.x,b.barycentric.y,b.barycentric.z,b.normal.x,b.normal.y,b.normal.z};
            for(size_t item=0;item<left.size();++item){Require(std::isfinite(left[item])&&std::isfinite(right[item]),label+" nonfinite output");group.error=(std::max)(group.error,std::abs(left[item]-right[item]));Require(std::abs(left[item]-right[item])<=1e-6f,label+" numeric mismatch");}
            group.bitwiseDifferences+=std::memcmp(&a,&b,sizeof(a))?1u:0u;if(a.status==1)++group.hits;else if(a.status==0)++group.misses;else if(a.status==2)++group.invalid;else Require(false,label+" exhausted trace");
        }
        std::cout<<name<<": "<<group.rays<<" equivalent rays; hits="<<group.hits<<", miss="<<group.misses<<", invalid="<<group.invalid<<", max error="<<group.error<<"\n";return group;
    }
}
int main()
{
    try
    {
        GPU gpu;std::vector<Group> groups;const auto rays=Rays();groups.push_back(Compare(gpu,"dense_opaque_front_back_edges",Layers(false),rays,{},0,1));groups.push_back(Compare(gpu,"dense_alpha_holes_cutoff",Layers(true),rays,{},8,1));
        auto planes=Layers(true);planes[4]={planes[4].z,true,0};planes[5]={planes[4].z,false,1};planes[6]={planes[4].z,false,1};groups.push_back(Compare(gpu,"coplanar_hole_opaque_tie",planes,rays,{},10,1));
        planes=Layers(false);planes[40]={planes[40].z,true,3};groups.push_back(Compare(gpu,"invalid_factor_behind_earlier_opaque",planes,rays,{},0,1));planes[0]={planes[0].z,true,3};groups.push_back(Compare(gpu,"invalid_factor_before_opaque",planes,rays,{},UINT32_MAX,2));
        planes=Layers(true);planes[3]={planes[3].z,true,4};groups.push_back(Compare(gpu,"invalid_texture_alpha_original_order",planes,rays,{},UINT32_MAX,2));planes[3]={planes[3].z,true,2,true};groups.push_back(Compare(gpu,"invalid_uv_original_order",planes,rays,{},UINT32_MAX,2));planes[3]={planes[3].z,true,999};groups.push_back(Compare(gpu,"invalid_material_id_original_order",planes,rays,{},UINT32_MAX,2));
        const XMFLOAT3 distantTranslation={10000,-10000,10000};groups.push_back(Compare(gpu,"distant_origin_dense_edges",Layers(true),Rays(distantTranslation),distantTranslation,8,1));
        auto invalidRays=rays;for(auto& ray:invalidRays)ray.mip=std::numeric_limits<float>::quiet_NaN();groups.push_back(Compare(gpu,"invalid_mip_original_order",Layers(true),invalidRays,{},UINT32_MAX,2));
        uint32_t count=0,differences=0;float maximum=0;for(const auto& group:groups){count+=group.rays;differences+=group.bitwiseDifferences;maximum=(std::max)(maximum,group.error);}const uint32_t warnings=gpu.Warnings();Require(warnings==0,"D3D12 warning/error");
        std::ofstream report("tmp/VXGIDenseBVHGPU-report.json");Require(bool(report),"cannot write report");report<<std::setprecision(9)<<"{\n  \"adapter\":\""<<gpu.adapterName<<"\",\n  \"debug_layer\":true,\n  \"gpu_based_validation\":true,\n  \"hidden_hwnd_no_present\":true,\n  \"production_trace\":\"VXGISDFTrace old CSR vs packed dense BVH\",\n  \"groups\":[\n";
        for(size_t index=0;index<groups.size();++index){const auto& group=groups[index];if(index)report<<",\n";report<<"    {\"name\":\""<<group.name<<"\",\"rays\":"<<group.rays<<",\"hits\":"<<group.hits<<",\"misses\":"<<group.misses<<",\"invalid\":"<<group.invalid<<",\"maximum_numeric_error\":"<<group.error<<",\"bitwise_different_pairs\":"<<group.bitwiseDifferences<<'}';}
        report<<"\n  ],\n  \"group_count\":"<<groups.size()<<",\n  \"rays\":"<<count<<",\n  \"dense_cells_exercised\":"<<gpu.denseCells<<",\n  \"submissions\":"<<gpu.submissions<<",\n  \"maximum_numeric_error\":"<<maximum<<",\n  \"bitwise_different_pairs\":"<<differences<<",\n  \"d3d12_warnings_errors\":"<<warnings<<",\n  \"passed\":true\n}\n";
        std::cout<<"Dense BVH equivalence: "<<groups.size()<<" groups / "<<count<<" rays passed; 0 D3D12 warnings/errors.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"Dense BVH GPU FAILED: "<<error.what()<<'\n';return 1;}
}
