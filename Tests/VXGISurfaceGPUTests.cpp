// Standalone D3D12 acceptance: production world-surface source functions,
// bindless texture sampling, SDF shadows and independent CPU checks. No assets required.
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
using DirectX::XMFLOAT4;

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
        XMFLOAT3 boundsMaximum; uint32_t materialCount=0;
    };
    struct GPURay
    {
        XMFLOAT3 origin; float maximumDistance=100;
        XMFLOAT3 direction; float minimumDistance=0;
        XMFLOAT3 sunDirection={0,1,0}; float materialMipLevel=0;
        XMFLOAT3 sunRadiance={3,2,1}; float shadowMinimumDistance=0.0001f;
        float shadowMaximumDistance=1000; uint32_t padding[3]={};
    };
    struct GPUResult
    {
        XMFLOAT3 albedo; float alpha;
        XMFLOAT3 orm; float perceptualRoughness;
        XMFLOAT3 shadingNormal; float metallic;
        XMFLOAT3 uncappedEmissive; uint32_t materialId;
        XMFLOAT3 directDiffuse; float sunVisibility;
        XMFLOAT3 limitedEmissive; uint32_t sourceValid;
        XMFLOAT3 lo0; uint32_t backFace;
        XMFLOAT3 screenLo0; uint32_t instanceId;
        XMFLOAT2 uv; uint32_t hitValid,traceStatus;
        XMFLOAT3 position; float distance;
        XMFLOAT3 geometricNormal; uint32_t shadowTraceStatus;
        float occlusion; uint32_t isUnlit,padding[2];
    };
    struct GPUMaterial
    {
        uint32_t albedoIdx=0,normalIdx=1,ormIdx=2,emissiveIdx=3;
        XMFLOAT4 baseColorFactor={1,1,1,1};
        uint32_t isUnlit=0;XMFLOAT3 emissiveFactor={};
    };
    struct TextureMip {uint32_t width,height;std::vector<XMFLOAT4> pixels;};
    struct TextureImage {std::vector<TextureMip> mips;};
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
    static_assert(sizeof(GPUConstants)==48 && sizeof(GPURay)==80 && sizeof(GPUResult)==192 && sizeof(GPUMaterial)==48);
    static_assert(sizeof(GPUTriangle)==208 && offsetof(GPUTriangle,uv0)==64 && offsetof(GPUTriangle,materialId)==196);

    ComPtr<IDxcBlob> Compile()
    {
        ComPtr<IDxcUtils> utils; ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(L"Shaders/VXGISurfaceValidate.hlsl",nullptr,&source),"load validation shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes),"create include handler");
        const wchar_t* arguments[]={L"Shaders/VXGISurfaceValidate.hlsl",L"-E",L"CSMain",L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",L"-O3"};
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
            std::array<CD3DX12_ROOT_PARAMETER,8> parameters;
            parameters[0].InitAsConstants(sizeof(GPUConstants)/4,0);
            for (UINT i=0;i<6;++i) parameters[i+1].InitAsShaderResourceView(i);
            parameters[7].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            CD3DX12_ROOT_SIGNATURE_DESC rootDescription(UINT(parameters.size()),parameters.data(),1,&sampler,
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> rootBlob;
            Check(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,nullptr),"serialize test root signature");
            Check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"create test root signature");
            auto shader=Compile();
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
            pipelineDescription.pRootSignature=root.Get(); pipelineDescription.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&pipelineDescription,IID_PPV_ARGS(&pipeline)),"create validation PSO");
        }
        ~GPU() { if (event) CloseHandle(event); }

        std::vector<GPUResult> Dispatch(const VXGISDF& sdf,const std::vector<GPURay>& rays,
            const std::vector<GPUMaterial>& materials,const std::vector<TextureImage>& images)
        {
            UploadTextures(images);
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
            auto references=Upload(sdf.GetTriangleReferences());auto triangleBuffer=Upload(triangles);auto materialBuffer=Upload(materials);auto rayBuffer=Upload(rays);
            const UINT64 outputBytes=rays.size()*sizeof(GPUResult);
            auto output=Buffer(outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback=Buffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto& grid=sdf.GetGrid();
            GPUConstants constants{grid.origin,grid.cellSize,grid.cellDimensions,uint32_t(rays.size()),grid.boundsMaximum,uint32_t(materials.size())};
            // HEAP_DIRECTLY_INDEXED signatures capture the currently bound heap.
            ID3D12DescriptorHeap* heaps[]={textureHeap.Get()};commands->SetDescriptorHeaps(1,heaps);
            commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
            const std::array<ID3D12Resource*,6> inputs={nodes.Get(),offsets.Get(),references.Get(),triangleBuffer.Get(),materialBuffer.Get(),rayBuffer.Get()};
            for (UINT index=0;index<inputs.size();++index) commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(7,output->GetGPUVirtualAddress());
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
            textureUploads.clear();
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
        void UploadTextures(const std::vector<TextureImage>& images)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,UINT(images.size()),D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(device->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&textureHeap)),"create bindless test texture heap");
            const UINT descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            textures.clear();textureUploads.clear();
            for(UINT imageIndex=0;imageIndex<images.size();++imageIndex)
            {
                const auto& image=images[imageIndex];Require(!image.mips.empty(),"empty texture fixture");
                const auto description=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,
                    image.mips[0].width,image.mips[0].height,1,UINT16(image.mips.size()));
                const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> texture;
                Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"create known-mip texture");
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(image.mips.size());UINT64 bytes=0;
                device->GetCopyableFootprints(&description,0,UINT(image.mips.size()),0,footprints.data(),nullptr,nullptr,&bytes);
                auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;
                const D3D12_RANGE noReads{0,0};Check(upload->Map(0,&noReads,&mapped),"map texture upload");
                for(UINT mip=0;mip<image.mips.size();++mip)
                {
                    const auto& source=image.mips[mip];Require(source.pixels.size()==size_t(source.width)*source.height,"texture fixture size mismatch");
                    for(UINT row=0;row<source.height;++row) std::memcpy(static_cast<uint8_t*>(mapped)+footprints[mip].Offset+size_t(row)*footprints[mip].Footprint.RowPitch,
                        source.pixels.data()+size_t(row)*source.width,size_t(source.width)*sizeof(XMFLOAT4));
                }
                upload->Unmap(0,nullptr);
                for(UINT mip=0;mip<image.mips.size();++mip)
                {
                    CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprints[mip]),destination(texture.Get(),mip);
                    commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                }
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                commands->ResourceBarrier(1,&barrier);
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=UINT(image.mips.size());
                CD3DX12_CPU_DESCRIPTOR_HANDLE handle(textureHeap->GetCPUDescriptorHandleForHeapStart(),INT(imageIndex),descriptorSize);
                device->CreateShaderResourceView(texture.Get(),&view,handle);
                textures.push_back(std::move(texture));textureUploads.push_back(std::move(upload));
            }
        }
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
        ComPtr<ID3D12DescriptorHeap> textureHeap;std::vector<ComPtr<ID3D12Resource>> textures,textureUploads;
    };

    struct TestResource {void* Get() const{return nullptr;}};
    struct TestTexture {uint32_t type=0;TestResource Resource;std::string path;};
    struct TestVertex {XMFLOAT3 Position,Normal;XMFLOAT2 TexCoords;XMFLOAT3 Tangent,Bitangent;};
    struct TestMesh
    {
        std::vector<TestVertex> vertices;std::vector<std::vector<unsigned>> lodIndices={{0,1,2,0,2,3}};
        std::vector<TestTexture> textures;uint32_t materialID=0;bool isUnlit=false;
        XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};
    };
    struct TestModel {std::vector<TestMesh> meshes={{}};};
    struct TestInstance
    {
        uint32_t editorId=1;TestModel* pModel=nullptr;
        XMFLOAT3 translation={},rotation={},scale={1,1,1};uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=true;int currentLodLevel=0;
    };
    TestModel Plane(float halfExtent=.75f,float height=0)
    {
        TestModel model;
        model.meshes[0].vertices={
            {{-halfExtent,height,-halfExtent},{0,1,0},{0,0},{1,0,0},{0,0,1}},
            {{-halfExtent,height, halfExtent},{0,1,0},{0,1},{1,0,0},{0,0,1}},
            {{ halfExtent,height, halfExtent},{0,1,0},{1,1},{1,0,0},{0,0,1}},
            {{ halfExtent,height,-halfExtent},{0,1,0},{1,0},{1,0,0},{0,0,1}}};
        return model;
    }
    TextureImage ConstantTexture(XMFLOAT4 color)
    {return {{{1,1,{color}}}};}
    TextureMip ConstantMip(uint32_t dimension,XMFLOAT4 color)
    {return {dimension,dimension,std::vector<XMFLOAT4>(size_t(dimension)*dimension,color)};}
    XMFLOAT4 Lerp(XMFLOAT4 a,XMFLOAT4 b,float t)
    {return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t};}
    XMFLOAT4 SampleMip(const TextureMip& mip,XMFLOAT2 uv)
    {
        const float x=uv.x*mip.width-.5f,y=uv.y*mip.height-.5f;
        const int left=int(std::floor(x)),top=int(std::floor(y));
        auto at=[&](int px,int py){px=(px%int(mip.width)+int(mip.width))%int(mip.width);py=(py%int(mip.height)+int(mip.height))%int(mip.height);
            return mip.pixels[size_t(py)*mip.width+px];};
        return Lerp(Lerp(at(left,top),at(left+1,top),x-left),Lerp(at(left,top+1),at(left+1,top+1),x-left),y-top);
    }
    XMFLOAT4 Sample(const TextureImage& texture,XMFLOAT2 uv,float lod)
    {
        lod=std::clamp(lod,0.0f,float(texture.mips.size()-1));const size_t first=size_t(std::floor(lod)),second=std::min(first+1,texture.mips.size()-1);
        return Lerp(SampleMip(texture.mips[first],uv),SampleMip(texture.mips[second],uv),lod-float(first));
    }
    XMFLOAT3 Decode(XMFLOAT4 v)
    {return {std::pow(std::abs(v.x),2.2f),std::pow(std::abs(v.y),2.2f),std::pow(std::abs(v.z),2.2f)};}
    XMFLOAT3 Multiply(XMFLOAT3 a,XMFLOAT3 b){return {a.x*b.x,a.y*b.y,a.z*b.z};}
    XMFLOAT3 Interpolate(const std::array<XMFLOAT3,3>& a,XMFLOAT3 b)
    {return Add(Add(Scale(a[0],b.x),Scale(a[1],b.y)),Scale(a[2],b.z));}
    struct Case {std::string name;GPURay ray;};
    GPURay Query(uint32_t patch,XMFLOAT2 uv={.57f,.63f},float lod=0)
    {
        GPURay ray;ray.origin={float(patch)*3+(uv.x-.5f)*1.5f,.4f,(uv.y-.5f)*1.5f};ray.direction={0,-1,0};ray.materialMipLevel=lod;return ray;
    }
    struct Expected
    {
        VXGISDF::Hit hit;XMFLOAT2 uv={};XMFLOAT3 albedo={},orm={},normal={},emissive={},direct={},limited={},lo0={};
        float alpha=0,roughness=0,metallic=0,visibility=0,occlusion=0;
    };
    Expected Reference(const VXGISDF& sdf,const GPURay& ray,const std::vector<GPUMaterial>& materials,const std::vector<TextureImage>& textures)
    {
        Expected expected;expected.hit=sdf.TraceReference(ray.origin,ray.direction,ray.maximumDistance);if(!expected.hit.hit)return expected;
        const auto& hit=expected.hit;const auto& triangle=sdf.GetTriangles()[hit.triangleIndex];const auto& m=materials[hit.materialId];const auto b=hit.barycentrics;
        expected.uv={triangle.uv[0].x*b.x+triangle.uv[1].x*b.y+triangle.uv[2].x*b.z,triangle.uv[0].y*b.x+triangle.uv[1].y*b.y+triangle.uv[2].y*b.z};
        const auto albedoSample=Sample(textures[m.albedoIdx],expected.uv,ray.materialMipLevel);
        expected.albedo=Multiply(Decode(albedoSample),{m.baseColorFactor.x,m.baseColorFactor.y,m.baseColorFactor.z});expected.alpha=albedoSample.w*m.baseColorFactor.w;
        const auto ormSample=Sample(textures[m.ormIdx],expected.uv,ray.materialMipLevel);expected.orm={ormSample.x,ormSample.y,ormSample.z};
        expected.roughness=std::max(ormSample.y,.005f);expected.metallic=ormSample.z;expected.occlusion=std::max(ormSample.x,.01f);
        const auto n=Normalize(Interpolate(triangle.shadingNormals,b));auto t=Interpolate(triangle.tangents,b);t=Normalize(Sub(t,Scale(n,Dot(n,t))));
        auto bitangent=Cross(n,t);bitangent=Scale(bitangent,Dot(bitangent,Interpolate(triangle.bitangents,b))<0?-1.0f:1.0f);
        const auto normalSample=Sample(textures[m.normalIdx],expected.uv,ray.materialMipLevel);
        const XMFLOAT3 tangentNormal={normalSample.x*2-1,-(normalSample.y*2-1),normalSample.z*2-1};
        expected.normal=Normalize(Add(Add(Scale(t,tangentNormal.x),Scale(bitangent,tangentNormal.y)),Scale(n,tangentNormal.z)));
        if(hit.backFace)expected.normal=Scale(expected.normal,-1);
        if(!m.isUnlit)expected.emissive=Multiply(Decode(Sample(textures[m.emissiveIdx],expected.uv,ray.materialMipLevel)),m.emissiveFactor);
        const auto sunDirection=Normalize(ray.sunDirection);
        const auto blocker=sdf.TraceReference(Add(hit.position,Scale(sunDirection,ray.shadowMinimumDistance)),sunDirection,ray.shadowMaximumDistance-ray.shadowMinimumDistance);
        expected.visibility=blocker.hit?0.0f:1.0f;
        if(!m.isUnlit)
        {
            const float peak=std::max({expected.emissive.x,expected.emissive.y,expected.emissive.z});expected.limited=Scale(expected.emissive,2/std::max(2.0f,peak));
            const float cosine=std::max(Dot(expected.normal,sunDirection),0.0f);const auto& a=expected.albedo;const float metal=expected.metallic;
            const XMFLOAT3 f0={.04f+(a.x-.04f)*metal,.04f+(a.y-.04f)*metal,.04f+(a.z-.04f)*metal};
            const XMFLOAT3 kD={(1-f0.x)*(1-metal),(1-f0.y)*(1-metal),(1-f0.z)*(1-metal)};
            expected.direct=Scale(Multiply(Multiply(kD,a),ray.sunRadiance),cosine*expected.visibility/DirectX::XM_PI);
            expected.lo0=Add(expected.direct,expected.limited);
        }
        return expected;
    }
}

int main()
{
    try
    {
        std::ofstream("tmp/VXGISurfaceGPU-report.json") << "{\"passed\":false,\"status\":\"running\"}\n";
        std::vector<TextureImage> textures={ConstantTexture({1,1,1,1}),ConstantTexture({.5f,.5f,1,1}),
            ConstantTexture({1,.7f,0,1}),ConstantTexture({0,0,0,1})};
        textures.push_back({{ConstantMip(4,{.5f,.25f,.75f,.8f}),ConstantMip(2,{1,.5f,.25f,.6f}),ConstantMip(1,{.25f,1,.5f,.4f})}});
        textures.push_back(ConstantTexture({.25f,.45f,.35f,1}));textures.push_back(ConstantTexture({.75f,.25f,.8535533906f,1}));
        textures.push_back(ConstantTexture({1,.5f,.25f,1}));
        TextureImage spatial;TextureMip image{4,4,{}};
        const std::array<XMFLOAT4,4> colors={{{.2f,.3f,.4f,1},{.8f,.6f,.2f,1},{.4f,.9f,.1f,1},{.9f,.2f,.7f,1}}};
        for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x)image.pixels.push_back(colors[(x>=2?1:0)+(y>=2?2:0)]);
        spatial.mips.push_back(std::move(image));textures.push_back(std::move(spatial));
        textures.push_back(ConstantTexture({1,.7f,1,1}));textures.push_back(ConstantTexture({0,0,0,1}));
        std::vector<GPUMaterial> materials(9);
        materials[0].baseColorFactor={.7f,.2f,.1f,.9f};materials[0].ormIdx=10;
        materials[1].albedoIdx=4;materials[1].ormIdx=5;materials[1].baseColorFactor={.6f,.4f,.2f,.5f};
        materials[2].ormIdx=9;materials[2].baseColorFactor={.8f,.6f,.3f,1};
        materials[3].baseColorFactor={0,0,0,1};materials[3].emissiveIdx=7;materials[3].emissiveFactor={8,4,2};
        materials[4].normalIdx=6;materials[4].baseColorFactor={.4f,.5f,.6f,1};
        materials[5]=materials[1];materials[5].normalIdx=6;materials[5].emissiveIdx=7;materials[5].emissiveFactor={8,4,2};materials[5].isUnlit=1;
        materials[6].albedoIdx=8;materials[6].baseColorFactor={.8f,.9f,1,1};
        materials[7].albedoIdx=4;materials[7].baseColorFactor={.3f,.6f,.9f,1};
        materials[8].baseColorFactor={0,0,0,1};
        auto plane=Plane();auto roof=Plane(.3f,1);roof.meshes[0].baseColorFactor={0,0,0,1};
        std::vector<TestInstance> instances(9);
        for(uint32_t i=0;i<8;++i){instances[i].editorId=100+i;instances[i].pModel=&plane;instances[i].translation={float(i)*3,0,0};instances[i].customMaterialID=i;}
        instances[8].editorId=108;instances[8].pModel=&roof;instances[8].customMaterialID=8;
        VXGIWorld world;VXGISDF sdf;Require(world.Update(instances,.5f),world.GetLastError());Require(sdf.Update(world,.5f,128),sdf.GetLastError());
        std::vector<Case> cases;
        auto add=[&](std::string label,GPURay ray){cases.push_back({std::move(label),ray});};
        auto pure=Query(0);pure.sunDirection={1,1,0};add("pure_albedo_factor_roughness_occlusion_floor",pure);
        add("sunlight_occluded_by_black_surface",Query(0));add("changed_sun_direction_unoccluded",pure);
        for(float lod:{-1.0f,0.0f,.5f,1.0f,2.0f,9.0f})add("encoded_sRGB_ORM_explicit_mip_"+std::to_string(lod),Query(1,{.31f,.73f},lod));
        add("metallic_one_zero_diffuse",Query(2));add("emission_peak_cap_preserves_RGB",Query(3));
        auto normal=Query(4);normal.sunDirection={0,0,1};add("normal_map_Y_flip_TBN",normal);
        auto backside=Query(4);backside.origin.y=-.4f;backside.direction={0,1,0};backside.sunDirection={0,-1,0};add("normal_map_backface_flip",backside);
        add("unlit_zero_GI_despite_emissive_texture",Query(5));
        for(auto uv:{XMFLOAT2{.125f,.125f},XMFLOAT2{.875f,.125f},XMFLOAT2{.125f,.875f},XMFLOAT2{.875f,.875f},XMFLOAT2{.5f,.5f}})
            add("spatial_hit_UV_texture_"+std::to_string(cases.size()),Query(6,uv));
        add("instance_material_override",Query(7));
        auto cameraA=Query(1,{.27f,.67f},1);add("offscreen_same_surface_camera_A",cameraA);
        auto cameraB=cameraA;const auto target=Sub(cameraA.origin,{0,.4f,0});cameraB.origin=Add(cameraA.origin,{.13f,.1f,-.17f});cameraB.direction=Normalize(Sub(target,cameraB.origin));add("offscreen_same_surface_camera_B",cameraB);
        auto brighter=Query(1);brighter.sunRadiance={7,11,13};add("changed_sun_radiance",brighter);
        auto black=Query(0);black.origin.y=1.5f;add("black_hit_still_terminates_primary_ray",black);
        auto miss=Query(1);miss.origin.z=4;add("world_surface_miss",miss);
        std::vector<GPURay> rays;for(const auto& test:cases)rays.push_back(test.ray);
        GPU gpu;const auto results=gpu.Dispatch(sdf,rays,materials,textures);
        float maximumMaterialError=0,maximumRadianceError=0,maximumPositionError=0,maximumScreenParityError=0;uint32_t hitCount=0,blockedCount=0;
        for(size_t i=0;i<cases.size();++i)
        {
            const auto& test=cases[i];const auto& actual=results[i];const auto expected=Reference(sdf,test.ray,materials,textures);const auto& hit=expected.hit;
            auto require=[&](bool condition,const char* detail){Require(condition,test.name+": "+detail);};
            require(bool(actual.hitValid)==hit.hit,"primary hit validity mismatch");require(actual.traceStatus==(hit.hit?1u:0u),"primary trace status mismatch");
            if(!hit.hit){std::cout << test.name << ": verified miss\n";continue;}++hitCount;
            require(actual.sourceValid==1,"surface/shadow query invalid");require(actual.materialId==hit.materialId && actual.instanceId==hit.instanceId,"material override/instance identity mismatch");
            require(bool(actual.backFace)==hit.backFace,"backface mismatch");
            const float positionError=std::max(std::abs(actual.distance-hit.distance),Error(actual.position,hit.position));maximumPositionError=std::max(maximumPositionError,positionError);require(positionError<3e-5f,"offscreen hit position mismatch");
            const float materialError=std::max({Error(actual.albedo,expected.albedo),std::abs(actual.alpha-expected.alpha),Error(actual.orm,expected.orm),std::abs(actual.perceptualRoughness-expected.roughness),
                std::abs(actual.metallic-expected.metallic),std::abs(actual.occlusion-expected.occlusion),Error(actual.shadingNormal,expected.normal),Error(actual.uncappedEmissive,expected.emissive),
                std::abs(actual.uv.x-expected.uv.x),std::abs(actual.uv.y-expected.uv.y)});
            maximumMaterialError=std::max(maximumMaterialError,materialError);require(materialError<2e-5f,"sRGB/factor/alpha/ORM/mip/normal/emission mismatch");
            require(actual.sunVisibility==expected.visibility,"SDF sunlight visibility mismatch");blockedCount+=actual.sunVisibility==0;
            require(actual.shadowTraceStatus==(actual.sunVisibility==0?1u:0u),"shadow trace status mismatch");
            require(actual.isUnlit==materials[hit.materialId].isUnlit,"unlit flag mismatch");
            const float radianceError=std::max({Error(actual.directDiffuse,expected.direct),Error(actual.limitedEmissive,expected.limited),Error(actual.lo0,expected.lo0)});
            maximumRadianceError=std::max(maximumRadianceError,radianceError);require(radianceError<2e-5f,"direct/limited emission/Lo0 mismatch");
            const float screenError=Error(actual.screenLo0,actual.lo0);maximumScreenParityError=std::max(maximumScreenParityError,screenError);require(screenError<1e-7f,"screen/world shared GI formula mismatch");
            std::cout << test.name << ": material=" << actual.materialId << ", visibility=" << actual.sunVisibility << ", Lo0=(" << actual.lo0.x << ',' << actual.lo0.y << ',' << actual.lo0.z << ")\n";
        }
        auto find=[&](const char* name)->const GPUResult&{for(size_t i=0;i<cases.size();++i)if(cases[i].name==name)return results[i];throw std::runtime_error("case not found");};
        const auto& a=find("offscreen_same_surface_camera_A");const auto& b=find("offscreen_same_surface_camera_B");
        Require(Error(a.lo0,b.lo0)<1e-6f && std::abs(a.uv.x-b.uv.x)<1e-6f && std::abs(a.uv.y-b.uv.y)<1e-6f,"camera changed explicit-mip world source");
        Require(find("sunlight_occluded_by_black_surface").sunVisibility==0 && find("changed_sun_direction_unoccluded").sunVisibility==1,"sun direction did not update occlusion");
        Require(find("black_hit_still_terminates_primary_ray").materialId==8 && Error(find("black_hit_still_terminates_primary_ray").lo0,{})==0,"black surface failed to terminate ray");
        const uint32_t warnings=gpu.Warnings();
        std::ofstream report("tmp/VXGISurfaceGPU-report.json");Require(bool(report),"cannot write surface acceptance report");
        report << std::setprecision(9) << "{\n  \"adapter\":\"" << gpu.adapterName << "\",\n  \"warp\":" << (gpu.warp?"true":"false") << ",\n  \"debug_layer\":" << (gpu.debugLayer?"true":"false")
            << ",\n  \"gpu_based_validation\":" << (gpu.gpuValidation?"true":"false") << ",\n  \"production_source\":\"VXGISurfaceCommon.hlsli::VXGIEvaluateSurfaceSource\",\n  \"texture_filter\":\"linear wrap, explicit SampleLevel\",\n  \"cases\":[\n";
        for(size_t i=0;i<cases.size();++i){if(i)report << ",\n";report << "    {\"name\":\"" << cases[i].name << "\",\"hit\":" << results[i].hitValid << ",\"sun_visibility\":" << results[i].sunVisibility << '}';}
        report << "\n  ],\n  \"query_count\":" << cases.size() << ",\n  \"hit_count\":" << hitCount << ",\n  \"blocked_sun_queries\":" << blockedCount
            << ",\n  \"maximum_material_error\":" << maximumMaterialError << ",\n  \"maximum_radiance_error\":" << maximumRadianceError << ",\n  \"maximum_position_error\":" << maximumPositionError
            << ",\n  \"screen_world_formula_maximum_error\":" << maximumScreenParityError << ",\n  \"d3d12_warnings_errors\":" << warnings << ",\n  \"passed\":" << (warnings?"false":"true") << "\n}\n";
        Require(warnings==0,"D3D12 validation emitted warnings/errors");
        std::cout << "VXGI surface GPU acceptance: " << cases.size() << " queries passed; material error " << maximumMaterialError << ", radiance error " << maximumRadianceError << ", screen/world formula error " << maximumScreenParityError << "; 0 warnings/errors.\n";
        return 0;
    }
    catch(const std::exception& error)
    {std::cerr << "VXGI surface GPU acceptance FAILED: " << error.what() << '\n';return 1;}
}


