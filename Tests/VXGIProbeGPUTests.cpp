// Standalone D3D12 acceptance: the production fixed-probe classification/update
// passes, paired SH projection, temporal history and independent CPU references.
// No scene assets or renderer startup required; hardware GPU validation is enabled.
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
#include "../Passes/VXGIProbePass.h"
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
        XMFLOAT3 origin;float cellSize;
        std::array<uint32_t,3> cellDimensions;uint32_t probeCount;
        XMFLOAT3 boundsMaximum;uint32_t triangleCount;
        XMFLOAT3 sunDirection={0,1,0};float materialMipLevel=0;
        XMFLOAT3 sunRadiance={};float rayMinDistance=.0001f;
        float maxRayDistance=100;uint32_t raysPerProbe=64,maxHistoryUpdates=4,frameIndex=1;
        uint32_t updateStart=0,updateCount=0,resetMode=0,materialCount=0;
        uint32_t classifyRays=64;float backfaceThreshold=.25f;uint32_t padding0=0,padding1=0;
    };
    struct GPURecord
    {
        std::array<XMFLOAT4,4> sh={};
        uint32_t state=0,updateCount=0,sampleCount=0,lastUpdateFrame=0;
        float backfaceRatio=0,minHitDistance=0;
        uint32_t classificationRays=0,reason=0;
    };
    struct NumericalResult
    {
        std::array<XMFLOAT4,4> sh;
        std::array<XMFLOAT4,6> irradiance;
        XMFLOAT4 firstDirection;
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
    static_assert(sizeof(GPUConstants)==128 && sizeof(GPURecord)==96 && sizeof(NumericalResult)==176 && sizeof(GPUMaterial)==48 && sizeof(GPUTriangle)==208);
    ComPtr<IDxcBlob> Compile(const wchar_t* path,const wchar_t* entry)
    {
        ComPtr<IDxcUtils> utils; ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils,IID_PPV_ARGS(&utils)),"create DXC utils");
        Check(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler)),"create DXC compiler");
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(path,nullptr,&source),"load validation shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes),"create include handler");
        const wchar_t* arguments[]={path,L"-E",entry,L"-T",L"cs_6_6",L"-HV",L"2021",L"-I",L"Shaders",L"-O3"};
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
            std::array<CD3DX12_ROOT_PARAMETER,9> parameters;
            parameters[0].InitAsConstants(sizeof(GPUConstants)/4,0);
            for (UINT i=0;i<7;++i) parameters[i+1].InitAsShaderResourceView(i);
            parameters[8].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            CD3DX12_ROOT_SIGNATURE_DESC rootDescription(UINT(parameters.size()),parameters.data(),1,&sampler,
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> rootBlob;
            Check(D3D12SerializeRootSignature(&rootDescription,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,nullptr),"serialize test root signature");
            Check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"create test root signature");
            updatePipeline=CreatePipeline(L"Shaders/Shaders_For_VXGI_Probe_Update.hlsl",L"CSUpdateParallel");
            referencePipeline=CreatePipeline(L"Shaders/Shaders_For_VXGI_Probe_Update.hlsl",L"CSUpdate");
            resetPipeline=CreatePipeline(L"Shaders/Shaders_For_VXGI_Probe_Update.hlsl",L"CSReset");
            numericalPipeline=CreatePipeline(L"Shaders/VXGIProbeValidate.hlsl",L"CSMain");
        }
        ~GPU() { if (event) CloseHandle(event); }

        std::vector<GPURecord> Dispatch(const VXGISDF& sdf,const std::vector<XMFLOAT3>& centers,
            const std::vector<GPURecord>& previous,const std::vector<GPUMaterial>& materials,
            GPUConstants constants,bool runUpdate=true)
        {
            Require(centers.size()==previous.size(),"probe fixtures have different sizes");
            constants.probeCount=uint32_t(centers.size());
            const auto parallel=DispatchInternal<GPURecord>(sdf,centers,previous,materials,constants,false,runUpdate,false);
            const auto serial=DispatchInternal<GPURecord>(sdf,centers,previous,materials,constants,false,runUpdate,true);
            for(size_t index=0;index<parallel.size();++index)
            {
                const auto& a=parallel[index];const auto& b=serial[index];
                Require(a.state==b.state && a.reason==b.reason && a.updateCount==b.updateCount && a.sampleCount==b.sampleCount &&
                    a.lastUpdateFrame==b.lastUpdateFrame && a.classificationRays==b.classificationRays && a.backfaceRatio==b.backfaceRatio,
                    "parallel/reference probe classification, counters or frame metadata differ");
                const float distanceError=std::abs(a.minHitDistance-b.minHitDistance);
                maximumParallelReferenceDistanceError=std::max(maximumParallelReferenceDistanceError,distanceError);
                Require(distanceError<3e-5f,"parallel/reference classification minimum distances differ");
                for(size_t coefficient=0;coefficient<4;++coefficient)
                {
                    const auto& av=a.sh[coefficient];const auto& bv=b.sh[coefficient];
                    const float error=std::max({std::abs(av.x-bv.x),std::abs(av.y-bv.y),std::abs(av.z-bv.z)});
                    maximumParallelReferenceSHError=std::max(maximumParallelReferenceSHError,error);
                    Require(error<3e-5f && av.w==bv.w,"parallel/reference SH projection or history differs");
                }
                ++parallelReferenceComparisons;
            }
            return parallel;
        }
        std::vector<NumericalResult> Numerical(const VXGISDF& sdf)
        {
            GPUConstants constants{};constants.probeCount=7;constants.raysPerProbe=8192;
            return DispatchInternal<NumericalResult>(sdf,std::vector<XMFLOAT3>(7),std::vector<GPURecord>(7),
                std::vector<GPUMaterial>(1),constants,true,false,false);
        }
        template<class Result> std::vector<Result> DispatchInternal(const VXGISDF& sdf,const std::vector<XMFLOAT3>& centers,
            const std::vector<GPURecord>& previous,const std::vector<GPUMaterial>& materials,
            GPUConstants constants,bool numerical,bool runUpdate,bool referenceKernel)
        {
            UploadTextures({{{{1,1,{{1,1,1,1}}}}},{{{1,1,{{.5f,.5f,1,1}}}}},
                {{{1,1,{{1,.7f,0,1}}}}},{{{1,1,{{1,1,1,1}}}}}});
            std::vector<GPUTriangle> triangles;
            for(uint32_t index=0;index<sdf.GetTriangles().size();++index)
            {
                const auto& source=sdf.GetTriangles()[index];GPUTriangle t{};
                t.p0=source.positions[0];t.p1=source.positions[1];t.p2=source.positions[2];
                t.triangleId=index;t.flags=source.isCutout;t.geometricNormal=source.geometricNormal;
                t.uv0=source.uv[0];t.uv1=source.uv[1];t.uv2=source.uv[2];
                t.shadingNormal0=source.shadingNormals[0];t.shadingNormal1=source.shadingNormals[1];t.shadingNormal2=source.shadingNormals[2];
                t.tangent0=source.tangents[0];t.tangent1=source.tangents[1];t.tangent2=source.tangents[2];
                t.bitangent0=source.bitangents[0];t.bitangent1=source.bitangents[1];t.bitangent2=source.bitangents[2];
                t.materialId=source.materialId;t.instanceId=source.instanceId;triangles.push_back(t);
            }
            auto nodes=Upload(sdf.GetNodes());auto offsets=Upload(sdf.GetCellOffsets());
            auto references=Upload(sdf.GetTriangleReferences());auto triangleBuffer=Upload(triangles);
            auto materialBuffer=Upload(materials);auto centersBuffer=Upload(centers);auto previousBuffer=Upload(previous);
            const UINT64 outputBytes=centers.size()*sizeof(Result);
            auto output=Buffer(outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback=Buffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            if(!numerical)
            {
                const auto beginCopy=CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
                commands->ResourceBarrier(1,&beginCopy);commands->CopyBufferRegion(output.Get(),0,previousBuffer.Get(),0,outputBytes);
            }
            const auto& grid=sdf.GetGrid();constants.origin=grid.origin;constants.cellSize=grid.cellSize;
            constants.cellDimensions=grid.cellDimensions;constants.boundsMaximum=grid.boundsMaximum;
            constants.triangleCount=uint32_t(triangles.size());
            ID3D12DescriptorHeap* heaps[]={textureHeap.Get()};commands->SetDescriptorHeaps(1,heaps);
            commands->SetComputeRootSignature(root.Get());
            commands->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
            const std::array<ID3D12Resource*,7> inputs={nodes.Get(),offsets.Get(),references.Get(),triangleBuffer.Get(),materialBuffer.Get(),centersBuffer.Get(),previousBuffer.Get()};
            for(UINT index=0;index<inputs.size();++index)commands->SetComputeRootShaderResourceView(index+1,inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(8,output->GetGPUVirtualAddress());
            auto beginWrite=CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),numerical?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->ResourceBarrier(1,&beginWrite);
            if(numerical)
            {
                commands->SetPipelineState(numericalPipeline.Get());commands->Dispatch((constants.probeCount+63)/64,1,1);
            }
            else
            {
                if(constants.resetMode)
                {
                    commands->SetPipelineState(resetPipeline.Get());commands->Dispatch((constants.probeCount+63)/64,1,1);
                    const auto resetBarrier=CD3DX12_RESOURCE_BARRIER::UAV(output.Get());commands->ResourceBarrier(1,&resetBarrier);
                }
                if(runUpdate && constants.updateCount)
                {
                    commands->SetPipelineState(referenceKernel?referencePipeline.Get():updatePipeline.Get());
                    if(referenceKernel)commands->Dispatch((constants.updateCount+63)/64,1,1);
                    else for(uint32_t base=0;base<constants.updateCount;)
                    {
                        const uint32_t chunk=std::min(65535u,constants.updateCount-base);
                        const std::array<uint32_t,2> chunkConstants={constants.updateStart+base,chunk};
                        commands->SetComputeRoot32BitConstants(0,2,chunkConstants.data(),24);
                        commands->Dispatch(chunk,1,1);base+=chunk;
                    }
                }
            }
            auto endWrite=CD3DX12_RESOURCE_BARRIER::Transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1,&endWrite);commands->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);
            Check(commands->Close(),"close probe commands");ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
            Check(queue->Signal(fence.Get(),++fenceValue),"signal probe fence");Check(fence->SetEventOnCompletion(fenceValue,event),"arm probe fence");
            Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"probe GPU dispatch timed out");
            Require(SUCCEEDED(device->GetDeviceRemovedReason()),"GPU device removed during probe dispatch");
            std::vector<Result> result(centers.size());void* mapped=nullptr;const D3D12_RANGE reads{0,SIZE_T(outputBytes)};
            Check(readback->Map(0,&reads,&mapped),"map probe result");std::memcpy(result.data(),mapped,size_t(outputBytes));
            const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);
            Check(allocator->Reset(),"reset completed probe allocator");Check(commands->Reset(allocator.Get(),nullptr),"reset completed probe commands");
            textureUploads.clear();return result;
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
        float maximumParallelReferenceSHError=0,maximumParallelReferenceDistanceError=0;uint32_t parallelReferenceComparisons=0;
    private:
        ComPtr<ID3D12PipelineState> CreatePipeline(const wchar_t* path,const wchar_t* entry)
        {
            auto shader=Compile(path,entry);D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature=root.Get();description.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            ComPtr<ID3D12PipelineState> result;Check(device->CreateComputePipelineState(&description,IID_PPV_ARGS(&result)),"create probe PSO");return result;
        }
        void UploadTextures(const std::vector<TextureImage>& images)
        {
            // Every dispatch uses these same four immutable fixture textures.
            // Keep their heap/resources for the whole paired-kernel acceptance
            // run; only staging is released after the first completed fence.
            if(textureHeap) return;
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
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> updatePipeline,referencePipeline,resetPipeline,numericalPipeline;UINT64 fenceValue=0;HANDLE event=nullptr;
        ComPtr<ID3D12DescriptorHeap> textureHeap;std::vector<ComPtr<ID3D12Resource>> textures,textureUploads;
    };

    struct TestResource {void* Get() const{return nullptr;}};
    struct TestTexture {uint32_t type=0;TestResource Resource;std::string path;};
    struct TestVertex {XMFLOAT3 Position,Normal;XMFLOAT2 TexCoords;XMFLOAT3 Tangent,Bitangent;};
    struct TestMesh
    {
        std::vector<TestVertex> vertices;std::vector<std::vector<unsigned>> lodIndices={{}};
        std::vector<TestTexture> textures;uint32_t materialID=0;bool isUnlit=false;
        XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};
    };
    struct TestModel {std::vector<TestMesh> meshes;};
    struct TestInstance
    {
        uint32_t editorId=1;TestModel* pModel=nullptr;
        XMFLOAT3 translation={},rotation={},scale={1,1,1};uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=true;int currentLodLevel=0;
    };
    void Quad(TestMesh& mesh,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d,bool inward=false)
    {
        if(inward)std::swap(b,d);
        const auto normal=Normalize(Cross(Sub(b,a),Sub(c,a))),tangent=Normalize(Sub(b,a)),bitangent=Cross(normal,tangent);
        const auto first=uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{a,normal,{0,0},tangent,bitangent},{b,normal,{1,0},tangent,bitangent},
            {c,normal,{1,1},tangent,bitangent},{d,normal,{0,1},tangent,bitangent}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    TestModel Box(bool inward)
    {
        TestModel model;model.meshes.resize(6);
        for(uint32_t i=0;i<6;++i)model.meshes[i].materialID=i;
        Quad(model.meshes[0],{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},inward);
        Quad(model.meshes[1],{-1,-1,-1},{-1,1,-1},{1,1,-1},{1,-1,-1},inward);
        Quad(model.meshes[2],{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1},inward);
        Quad(model.meshes[3],{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1},inward);
        Quad(model.meshes[4],{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1},inward);
        Quad(model.meshes[5],{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1},inward);
        return model;
    }
    TestModel Plane()
    {
        TestModel model;model.meshes.resize(1);
        Quad(model.meshes[0],{-4,0,-4},{-4,0,4},{4,0,4},{4,0,-4});return model;
    }
    void Build(TestModel& model,VXGIWorld& world,VXGISDF& sdf,bool cutout=false)
    {
        std::vector<TestInstance> instances(1);instances[0].pModel=&model;
        instances[0].isCutout=cutout;
        Require(world.Update(instances,.5f),world.GetLastError());Require(sdf.Update(world,.5f,128),sdf.GetLastError());
    }
    XMFLOAT3 Color3(XMFLOAT4 value){return {value.x,value.y,value.z};}
    float SHError(const GPURecord& a,const std::array<XMFLOAT3,4>& b)
    {
        float result=0;for(size_t coefficient=0;coefficient<4;++coefficient)result=std::max(result,Error(Color3(a.sh[coefficient]),b[coefficient]));return result;
    }
    bool ZeroSH(const GPURecord& record)
    {
        for(const auto& coefficient:record.sh)if(coefficient.x!=0 || coefficient.y!=0 || coefficient.z!=0 || coefficient.w!=0)return false;return true;
    }
    std::vector<GPUMaterial> EmissiveMaterials(const std::array<XMFLOAT3,6>& colors)
    {
        std::vector<GPUMaterial> materials(6);
        for(size_t i=0;i<6;++i){materials[i].baseColorFactor={0,0,0,1};materials[i].emissiveFactor=colors[i];}return materials;
    }
    std::vector<GPUMaterial> UniformMaterials(XMFLOAT3 color)
    {
        std::array<XMFLOAT3,6> colors;colors.fill(color);return EmissiveMaterials(colors);
    }
    std::array<XMFLOAT3,4> ConstantSH(XMFLOAT3 color)
    {
        return {Scale(color,std::sqrt(4*DirectX::XM_PI)),XMFLOAT3{},XMFLOAT3{},XMFLOAT3{}};
    }
    uint32_t Hash(uint32_t value)
    {
        value^=value>>16;value*=0x7feb352du;value^=value>>15;value*=0x846ca68bu;value^=value>>16;return value;
    }
    XMFLOAT3 SampleDirection(uint32_t pair,uint32_t pairCount,uint32_t probe,uint32_t update)
    {
        // Independent CPU copy of the deterministic sampling contract; all SH
        // projection and hit/source expectations below are separately evaluated.
        const float z=(float(pair)+.5f)/float(pairCount),phi=2.39996322972865332f*float(pair);
        const float radial=std::sqrt(std::max(0.0f,1-z*z));const XMFLOAT3 base={radial*std::cos(phi),radial*std::sin(phi),z};
        const uint32_t seed=probe*0x9e3779b9u ^ update*0x85ebca6bu ^ 0x68bc21ebu;
        const float u1=float(Hash(seed+0x9e3779b9u)&0xffffffu)/16777216.0f;
        const float u2=float(Hash(seed+0x3c6ef372u)&0xffffffu)/16777216.0f;
        const float u3=float(Hash(seed+0xdaa66d2bu)&0xffffffu)/16777216.0f;
        const float a=std::sqrt(1-u1),b=std::sqrt(u1),p2=2*DirectX::XM_PI*u2,p3=2*DirectX::XM_PI*u3;
        const XMFLOAT3 q={a*std::sin(p2),a*std::cos(p2),b*std::sin(p3)};const float qw=b*std::cos(p3);
        return Normalize(Add(base,Scale(Cross(q,Add(Cross(q,base),Scale(base,qw))),2)));
    }
    std::array<XMFLOAT3,4> ReferenceProjection(const VXGISDF& sdf,XMFLOAT3 center,uint32_t probe,uint32_t update,
        uint32_t rays,const std::vector<GPUMaterial>& materials)
    {
        std::array<XMFLOAT3,4> result={};const float weight=4*DirectX::XM_PI/float(rays);
        for(uint32_t pair=0;pair<rays/2;++pair)
        {
            const auto positive=SampleDirection(pair,rays/2,probe,update);
            for(uint32_t side=0;side<2;++side)
            {
                const auto direction=Scale(positive,side==0?1.0f:-1.0f);const auto hit=sdf.TraceReference(center,direction,100);
                XMFLOAT3 source={};if(hit.hit)source=materials[hit.materialId].emissiveFactor;
                const std::array<float,4> basis={.28209479177387814f,.4886025119029199f*direction.x,.4886025119029199f*direction.y,.4886025119029199f*direction.z};
                for(uint32_t coefficient=0;coefficient<4;++coefficient)result[coefficient]=Add(result[coefficient],Scale(source,weight*basis[coefficient]));
            }
        }
        return result;
    }
    struct ProductionReport
    {
        uint32_t initialCount=0,movedCount=0,restoredCount=0,frames=0,recordsChecked=0,warnings=0;
        uint64_t acceptedReadbacks=0;float maximumSHError=0;bool resizeObservedInFlight=false;
    };
    class HiddenWindow
    {
    public:
        HiddenWindow()
        {
            WNDCLASSW description{};description.lpfnWndProc=DefWindowProcW;description.hInstance=GetModuleHandleW(nullptr);
            description.lpszClassName=L"VXGIBackgroundAcceptance";
            Require(RegisterClassW(&description)!=0,"cannot register hidden background fixture window");
            handle=CreateWindowExW(0,description.lpszClassName,L"VXGI background fixture",WS_POPUP,
                0,0,64,64,nullptr,nullptr,description.hInstance,nullptr);
            Require(handle!=nullptr && !IsWindowVisible(handle),"fixture window must exist and remain hidden");
        }
        ~HiddenWindow(){if(handle)DestroyWindow(handle);UnregisterClassW(L"VXGIBackgroundAcceptance",GetModuleHandleW(nullptr));}
        HWND handle=nullptr;
    };
    class ProductionFixture
    {
    public:
        struct Capture
        {
            ComPtr<ID3D12Resource> records,centers;
            std::vector<XMFLOAT3> expectedCenters;
            VXGIProbeState::PendingPlan plan;
            uint64_t fence=0;int slot=0;
        };
        ProductionFixture()
        {
            Require(device.Initialize(window.handle,64,64,3),"production RenderDevice hidden initialization failed");
            Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"get production debug message queue");
            Require(pass.Initialize(device,3),"production probe pass initialization failed");scene.Initialize(3);
            parameters.probesPerFrame=7;parameters.classifyRays=8;parameters.raysPerUpdate=8;parameters.maxHistoryUpdates=4;
            parameters.rayMaxDistance=100;
            sourceParameters.lightColor={};sourceParameters.maxRayDistance=100;
            materials=UniformMaterials({.4f,.2f,.1f});materialBuffer=Upload(materials);
            D3D12_DESCRIPTOR_HEAP_DESC heapDescription{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            Check(device.GetDevice()->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&heap)),"create production fixture texture heap");
            const std::array<XMFLOAT4,4> pixels={XMFLOAT4{1,1,1,1},XMFLOAT4{.5f,.5f,1,1},XMFLOAT4{1,.7f,0,1},XMFLOAT4{1,1,1,1}};
            const UINT descriptorSize=device.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for(UINT index=0;index<4;++index)
            {
                const auto description=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,1,1);
                const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> texture;
                Check(device.GetDevice()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"create production fixture texture");
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
                device.GetDevice()->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&bytes);
                auto staging=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;
                const D3D12_RANGE noReads{0,0};Check(staging->Map(0,&noReads,&mapped),"map production texture staging");
                std::memcpy(static_cast<uint8_t*>(mapped)+footprint.Offset,&pixels[index],sizeof(XMFLOAT4));staging->Unmap(0,nullptr);
                const CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(),footprint),dst(texture.Get(),0);
                device.GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);
                device.GetCommandList()->ResourceBarrier(1,&barrier);
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
                const CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor(heap->GetCPUDescriptorHandleForHeapStart(),INT(index),descriptorSize);
                device.GetDevice()->CreateShaderResourceView(texture.Get(),&view,descriptor);textures.push_back(texture);textureUploads.push_back(staging);
            }
            Check(device.GetCommandList()->Close(),"close production fixture initialization");
            ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);
            Check(device.GetCommandQueue()->Signal(device.GetFence(0),++device.GetFenceValue(0)),"signal production fixture initialization");
            device.WaitForPreviousFrame(0);textureUploads.clear();
        }
        ~ProductionFixture(){for(int slot=0;slot<3;++slot)if(device.GetFence(slot))device.WaitForPreviousFrame(slot);}
        Capture Submit(VXGIWorld& world,VXGISDF& sdf)
        {
            Require(!IsWindowVisible(window.handle),"background acceptance window became visible");
            const int slot=int(submissions%3);device.WaitForPreviousFrame(slot);
            Require(surface.Update(materials,world.GetGeometryRevision(),sdf.GetGeometryRevision(),world.GetMaterialRevision(),sourceParameters,world.GetCoverageRevision()),surface.GetLastError());
            const VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),sdf.GetGeometryRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};
            Require(state.Prepare(sources,parameters),state.GetLastError());const auto plan=state.GetPendingPlan();
            if(plan.resetMode==1 && submissions)
                for(int other=0;other<3;++other)if(device.GetFence(other)->GetCompletedValue()<device.GetFenceValue(other))resizeObservedInFlight=true;
            Check(device.GetCommandAllocator(slot)->Reset(),"reset production fixture allocator");
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset production fixture command list");
            Require(scene.Prepare(device,slot,world,sdf),"production GPU scene preparation failed");
            Require(pass.Record(device,heap.Get(),slot,scene,sdf,surface,materialBuffer->GetGPUVirtualAddress(),state),"production probe graph recording failed");
            Capture capture;capture.expectedCenters=world.GetOccupiedCenters();capture.plan=plan;capture.slot=slot;
            const uint64_t count=capture.expectedCenters.size();auto* history=pass.GetDisplayHistory();
            Require(history && history->GetDesc().Width==count*sizeof(GPURecord),"production history size does not match changed allocation");
            Require(scene.Get(slot,4)->GetDesc().Width==count*sizeof(XMFLOAT3),"production fixed center buffer size does not match changed allocation");
            capture.records=Buffer(count*sizeof(GPURecord),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            capture.centers=Buffer(count*sizeof(XMFLOAT3),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            for(const auto pair:std::array<std::pair<ID3D12Resource*,ID3D12Resource*>,2>{{{history,capture.records.Get()},{scene.Get(slot,4),capture.centers.Get()}}})
            {
                const auto before=CD3DX12_RESOURCE_BARRIER::Transition(pair.first,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);
                device.GetCommandList()->ResourceBarrier(1,&before);device.GetCommandList()->CopyResource(pair.second,pair.first);
                const auto after=CD3DX12_RESOURCE_BARRIER::Transition(pair.first,D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);
                device.GetCommandList()->ResourceBarrier(1,&after);
            }
            Check(device.GetCommandList()->Close(),"close production probe graph commands");ID3D12CommandList* lists[]={device.GetCommandList()};
            device.GetCommandQueue()->ExecuteCommandLists(1,lists);capture.fence=++device.GetFenceValue(slot);
            Check(device.GetCommandQueue()->Signal(device.GetFence(slot),capture.fence),"signal production probe submission");
            Require(pass.OnSubmitted(slot,capture.fence,state),state.GetLastError());++submissions;return capture;
        }
        std::vector<GPURecord> Read(const Capture& capture)
        {
            Wait(capture);const auto positions=ReadBuffer<XMFLOAT3>(capture.centers.Get(),capture.expectedCenters.size());
            Require(std::memcmp(positions.data(),capture.expectedCenters.data(),positions.size()*sizeof(XMFLOAT3))==0,"production GPU centers moved or used stale allocation data");
            return ReadBuffer<GPURecord>(capture.records.Get(),capture.expectedCenters.size());
        }
        void Poll(const Capture& capture){Wait(capture);Require(pass.Poll(device,capture.slot,state),"production snapshot polling failed");}
        uint32_t Warnings()
        {
            for(int slot=0;slot<3;++slot)device.WaitForPreviousFrame(slot);uint32_t warnings=0;
            for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T bytes=0;Check(info->GetMessage(index,nullptr,&bytes),"measure production debug message");std::vector<char> storage(bytes);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(index,message,&bytes),"read production debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr << "Production D3D12: " << message->pDescription << '\n';++warnings;}
            }
            return warnings;
        }
        uint32_t submissions=0;bool resizeObservedInFlight=false;
        VXGIProbeState state;VXGIProbeState::Parameters parameters;VXGIProbePass pass;
        std::vector<GPUMaterial> materials;
    private:
        void Wait(const Capture& capture)
        {
            Check(device.GetFence(capture.slot)->SetEventOnCompletion(capture.fence,device.GetFenceEvent()),"arm production capture fence");
            Require(WaitForSingleObject(device.GetFenceEvent(),30000)==WAIT_OBJECT_0,"production capture fence timed out");
            Require(SUCCEEDED(device.GetDevice()->GetDeviceRemovedReason()),"device removed during production graph acceptance");
        }
        template<class T> static std::vector<T> ReadBuffer(ID3D12Resource* resource,size_t count)
        {
            std::vector<T> result(count);void* mapped=nullptr;const D3D12_RANGE range{0,count*sizeof(T)};
            Check(resource->Map(0,&range,&mapped),"map production capture");std::memcpy(result.data(),mapped,range.End);
            const D3D12_RANGE noWrites{0,0};resource->Unmap(0,&noWrites);return result;
        }
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initialState)
        {
            const auto properties=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes);ComPtr<ID3D12Resource> result;
            Check(device.GetDevice()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,initialState,nullptr,IID_PPV_ARGS(&result)),"create production fixture buffer");return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            auto result=Buffer(values.size()*sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;
            const D3D12_RANGE noReads{0,0};Check(result->Map(0,&noReads,&mapped),"map production material fixture");
            std::memcpy(mapped,values.data(),values.size()*sizeof(T));result->Unmap(0,nullptr);return result;
        }
        HiddenWindow window;RenderDevice device;VXGIGPUScene scene;VXGISurface surface;VXGISurface::Parameters sourceParameters;
        ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12Resource> materialBuffer;
        std::vector<ComPtr<ID3D12Resource>> textures,textureUploads;
    };
    GPURecord ClassificationReference(const VXGISDF& sdf,XMFLOAT3 center,uint32_t index,uint32_t rays)
    {
        GPURecord result;result.state=1;result.classificationRays=rays;result.minHitDistance=100;uint32_t backfaces=0;bool any=false;
        for(uint32_t pair=0;pair<rays/2;++pair)
        {
            const auto direction=SampleDirection(pair,rays/2,index,0);
            for(const float side:{1.0f,-1.0f})
            {
                const auto hit=sdf.TraceReference(center,Scale(direction,side),100);
                if(hit.hit){any=true;backfaces+=hit.backFace?1:0;result.minHitDistance=std::min(result.minHitDistance,hit.distance);}
            }
        }
        result.backfaceRatio=float(backfaces)/float(rays);
        if(any && result.minHitDistance<=.0001f){result.state=2;result.reason=2;}
        else if(result.backfaceRatio>.25f){result.state=2;result.reason=1;}return result;
    }
    void CheckProductionCapture(const ProductionFixture::Capture& capture,const std::vector<GPURecord>& actual,
        const std::vector<GPURecord>& previous,const VXGISDF& sdf,const ProductionFixture& fixture,ProductionReport& report)
    {
        for(uint32_t index=0;index<actual.size();++index)
        {
            GPURecord expected=capture.plan.resetMode==1?GPURecord{}:previous.at(index);
            const bool selected=index>=capture.plan.start && index<capture.plan.start+capture.plan.count;
            if(selected)
            {
                if(expected.state==0)expected=ClassificationReference(sdf,capture.expectedCenters[index],index,fixture.parameters.classifyRays);
                if(expected.state==1)
                {
                    const auto estimate=ReferenceProjection(sdf,capture.expectedCenters[index],index,expected.updateCount+1,fixture.parameters.raysPerUpdate,fixture.materials);
                    const float weight=1.0f/float(std::min(expected.updateCount+1,fixture.parameters.maxHistoryUpdates));
                    for(uint32_t coefficient=0;coefficient<4;++coefficient)
                    {
                        const auto old=Color3(expected.sh[coefficient]);const auto combined=Add(Scale(old,1-weight),Scale(estimate[coefficient],weight));
                        expected.sh[coefficient]={combined.x,combined.y,combined.z,0};
                    }
                    ++expected.updateCount;expected.sampleCount+=fixture.parameters.raysPerUpdate;expected.lastUpdateFrame=capture.plan.frameIndex;
                }
            }
            const auto& result=actual[index];Require(result.state==expected.state && result.reason==expected.reason &&
                result.updateCount==expected.updateCount && result.sampleCount==expected.sampleCount && result.lastUpdateFrame==expected.lastUpdateFrame &&
                result.classificationRays==expected.classificationRays && result.backfaceRatio==expected.backfaceRatio,
                "production RDG classification/metadata differs from CPU triangles and selected frame batch");
            Require(std::abs(result.minHitDistance-expected.minHitDistance)<3e-5f,"production classification minimum distance differs from CPU triangles");
            for(uint32_t coefficient=0;coefficient<4;++coefficient)
            {
                const float error=Error(Color3(result.sh[coefficient]),Color3(expected.sh[coefficient]));report.maximumSHError=std::max(report.maximumSHError,error);
                Require(error<4e-5f && result.sh[coefficient].w==0,"production RDG SH source/history differs from independent CPU reference");
            }
            if(!selected && capture.plan.resetMode==0)Require(std::memcmp(&result,&previous[index],sizeof(result))==0,"production history ping-pong corrupted an unselected probe");
            if(!selected && capture.plan.resetMode==1){const GPURecord zero;Require(std::memcmp(&result,&zero,sizeof(result))==0,"production geometry reset left old or uninitialized probe data");}
            ++report.recordsChecked;
        }
    }
    ProductionReport ProductionAcceptance()
    {
        ProductionReport report;ProductionFixture fixture;TestModel model;model.meshes.resize(1);
        Quad(model.meshes[0],{-.6f,0,-.6f},{-.6f,0,.6f},{.6f,0,.6f},{.6f,0,-.6f});
        std::vector<TestInstance> instances(1);instances[0].pModel=&model;VXGIWorld world;VXGISDF sdf;
        auto update=[&](){Require(world.Update(instances,.5f),world.GetLastError());Require(sdf.Update(world,.5f,128),sdf.GetLastError());};update();
        const auto originalCenters=world.GetOccupiedCenters();const auto originalHash=world.GetStats().geometryFingerprint;
        const VXGISDF originalSDF=sdf;report.initialCount=uint32_t(originalCenters.size());
        const auto first=fixture.Submit(world,sdf),second=fixture.Submit(world,sdf);
        Require(first.plan.resetMode==1 && second.plan.resetMode==0,"production initial/reset scheduling incorrect");
        instances[0].translation={.17f,.125f,.11f};update();report.movedCount=uint32_t(world.GetOccupiedCenters().size());
        Require(report.movedCount!=report.initialCount,"moved fixture did not change actual sparse allocation size");
        const auto moved=fixture.Submit(world,sdf);Require(moved.plan.resetMode==1 && moved.plan.epoch==first.plan.epoch+1,"moved geometry did not reset/revise production history");
        auto previous=fixture.Read(first);CheckProductionCapture(first,previous,{},originalSDF,fixture,report);
        auto actual=fixture.Read(second);CheckProductionCapture(second,actual,previous,originalSDF,fixture,report);
        actual=fixture.Read(moved);CheckProductionCapture(moved,actual,{},sdf,fixture,report);previous=actual;
        const auto acceptedBefore=fixture.state.GetStats().acceptedReadbacks;const auto movedTail=fixture.Submit(world,sdf);
        Require(fixture.state.GetStats().acceptedReadbacks==acceptedBefore,"pre-move epoch snapshot was accepted into resized production allocation");
        Require(movedTail.plan.resetMode==0 && movedTail.plan.count<fixture.parameters.probesPerFrame,"production moved allocation tail batch was not bounded");
        actual=fixture.Read(movedTail);CheckProductionCapture(movedTail,actual,previous,sdf,fixture,report);
        instances[0].translation={};update();report.restoredCount=uint32_t(world.GetOccupiedCenters().size());
        Require(report.restoredCount==report.initialCount && world.GetStats().geometryFingerprint==originalHash &&
            std::memcmp(world.GetOccupiedCenters().data(),originalCenters.data(),originalCenters.size()*sizeof(XMFLOAT3))==0,
            "restored geometry did not recover original allocation/hash/centers");
        const auto restored=fixture.Submit(world,sdf);Require(restored.plan.resetMode==1 && restored.plan.epoch==moved.plan.epoch+1,"restoration did not reset/revise production history");
        previous=fixture.Read(restored);CheckProductionCapture(restored,previous,{},sdf,fixture,report);
        const auto acceptedAfterRestore=fixture.state.GetStats().acceptedReadbacks;const auto discardMoved=fixture.Submit(world,sdf);
        Require(fixture.state.GetStats().acceptedReadbacks==acceptedAfterRestore,"moved epoch snapshot was accepted after restoration");
        actual=fixture.Read(discardMoved);CheckProductionCapture(discardMoved,actual,previous,sdf,fixture,report);previous=actual;
        ProductionFixture::Capture completed=discardMoved;bool tailObserved=false;
        while(fixture.submissions<32)
        {
            completed=fixture.Submit(world,sdf);tailObserved|=completed.plan.count<fixture.parameters.probesPerFrame;
            actual=fixture.Read(completed);CheckProductionCapture(completed,actual,previous,sdf,fixture,report);previous=actual;
        }
        Require(tailObserved,"production restored scene did not exercise final partial update batch");fixture.Poll(completed);
        const auto& stats=fixture.state.GetStats();uint64_t valid=0,disabled=0,updates=0,samples=0;
        for(const auto& record:actual){valid+=record.state==1;disabled+=record.state==2;updates+=record.updateCount;samples+=record.sampleCount;}
        Require(stats.hasCompletedReadback && stats.epoch==restored.plan.epoch && stats.completedFrameIndex==32 && stats.probeCount==report.restoredCount &&
            stats.valid==valid && stats.disabled==disabled && stats.unclassified==0 && stats.totalUpdates==updates && stats.totalSamples==samples,
            "production asynchronous snapshot stats do not match actual restored GPU history");
        Require(std::isfinite(fixture.pass.GetGPUMilliseconds()) && fixture.pass.GetGPUMilliseconds()>=0,"production timestamps were invalid");
        report.frames=fixture.submissions;report.acceptedReadbacks=stats.acceptedReadbacks;report.resizeObservedInFlight=fixture.resizeObservedInFlight;
        report.warnings=fixture.Warnings();Require(report.warnings==0,"production D3D12 validation emitted warnings/errors");return report;
    }
}

int main()
{
    try
    {
        std::ofstream("tmp/VXGIProbeGPU-report.json") << "{\"passed\":false,\"status\":\"running\"}\n";
        std::vector<std::string> passed;auto success=[&](const char* name){passed.push_back(name);std::cout << name << ": passed\n";};
        auto inward=Box(true),outward=Box(false),plane=Plane();
        VXGIWorld inwardWorld,outwardWorld,planeWorld;VXGISDF inwardSDF,outwardSDF,planeSDF;
        Build(inward,inwardWorld,inwardSDF);Build(outward,outwardWorld,outwardSDF);Build(plane,planeWorld,planeSDF);
        GPU gpu;float maximumNumericalError=0,maximumProjectionError=0,maximumHistoryError=0;
        const auto numerical=gpu.Numerical(inwardSDF);const XMFLOAT3 uniform={1,.6f,.25f},amplitude={.4f,.2f,.1f};
        for(uint32_t index=0;index<numerical.size();++index)
        {
            const uint32_t axis=index==0?0:(index-1)/2;const float sign=index==0?0:((index&1)?1.0f:-1.0f);
            auto expected=ConstantSH(uniform);expected[axis+1]=Scale(amplitude,sign*std::sqrt(4*DirectX::XM_PI/3));
            for(uint32_t coefficient=0;coefficient<4;++coefficient)
            {
                const float error=Error(Color3(numerical[index].sh[coefficient]),expected[coefficient]);maximumNumericalError=std::max(maximumNumericalError,error);
                Require(error<.0015f,"SH constant/axis projection has wrong coefficient, scale or sign");
            }
            for(uint32_t normal=0;normal<6;++normal)
            {
                const float cosine=normal/2==axis?((normal&1)?-1.0f:1.0f):0;
                const auto expectedIrradiance=Add(uniform,Scale(amplitude,sign*cosine*2/3));
                const float error=Error(Color3(numerical[index].irradiance[normal]),expectedIrradiance);maximumNumericalError=std::max(maximumNumericalError,error);
                Require(error<.0015f,"irradiance/pi reconstruction missed the 2/3 l1 convolution");
            }
        }
        for(uint32_t coefficient=1;coefficient<4;++coefficient)
            Require(Error(Color3(numerical[0].sh[coefficient]),{})==0,"paired constant radiance did not yield exact zero l1 coefficients");
        success("constant_sphere_and_signed_X_Y_Z_SH_projection_and_E_over_pi");
        auto materials=UniformMaterials({.4f,.2f,.1f});GPUConstants constants;constants.materialCount=6;constants.updateCount=5;
        const std::vector<XMFLOAT3> centers={{0,0,0},{3,.13f,.19f},{1.0002f,.13f,.19f},{1,.13f,.19f},{1.00005f,.13f,.19f}};
        std::vector<GPURecord> previous(centers.size());
        const auto classified=gpu.Dispatch(outwardSDF,centers,previous,materials,constants);
        Require(classified[0].state==2 && classified[0].reason==1 && classified[0].backfaceRatio==1,"closed outward box interior did not disable by real backfaces");
        Require(classified[1].state==1 && classified[1].reason==0,"box exterior incorrectly disabled");
        Require(classified[2].state==1 && classified[2].minHitDistance>constants.rayMinDistance,"safe surface gap incorrectly disabled");
        Require(classified[3].state==2 && classified[3].reason==2 && classified[3].minHitDistance==0,"zero-distance surface origin was not disabled");
        Require(classified[4].state==2 && classified[4].reason==2 && classified[4].minHitDistance<=constants.rayMinDistance,"unsafe short origin gap was not disabled");
        for(const auto index:{0,3,4})Require(ZeroSH(classified[index]) && classified[index].updateCount==0,"disabled probe retained lighting");
        for(const auto& record:classified)Require(record.classificationRays==64,"classification ray count incorrect");
        for(const uint32_t boundary:{8u,256u})
        {
            auto boundaryConstants=constants;boundaryConstants.classifyRays=boundary;boundaryConstants.raysPerProbe=boundary;boundaryConstants.updateCount=1;
            const auto boundaryInterior=gpu.Dispatch(outwardSDF,{{0,0,0}},{GPURecord{}},materials,boundaryConstants);
            Require(boundaryInterior[0].state==2 && boundaryInterior[0].reason==1 && boundaryInterior[0].backfaceRatio==1 &&
                boundaryInterior[0].classificationRays==boundary && ZeroSH(boundaryInterior[0]),"8/256-ray grouped classification did not preserve all-backface disable");
        }
        success("closed_box_backface_disable_safe_gap_and_zero_distance_origin");
        const std::vector<XMFLOAT3> planeCenters={{0,.25f,0},{0,-.25f,0},{0,0,0},{0,.0002f,0}};
        constants.updateCount=uint32_t(planeCenters.size());
        const auto thin=gpu.Dispatch(planeSDF,planeCenters,std::vector<GPURecord>(planeCenters.size()),materials,constants);
        Require(thin[0].state==1 && thin[1].state==2 && thin[1].reason==1,"thin wall failed front/back classification");
        Require(thin[2].state==2 && thin[2].reason==2 && thin[3].state==1,"thin wall surface origin/gap classification failed");
        success("thin_wall_fixed_centers_front_back_surface_and_safe_gap");
        const std::vector<XMFLOAT3> captureCenters={{0,0,0},{.12f,-.17f,.21f}};
        const std::array<XMFLOAT3,6> colors={XMFLOAT3{.2f,.4f,1.6f},XMFLOAT3{.8f,.9f,.2f},XMFLOAT3{1.5f,.2f,.1f},
            XMFLOAT3{.1f,.6f,.4f},XMFLOAT3{.5f,1,.2f},XMFLOAT3{.7f,.1f,1}};
        const auto colored=EmissiveMaterials(colors);constants.raysPerProbe=128;constants.updateCount=2;constants.frameIndex=19;
        const auto captured=gpu.Dispatch(inwardSDF,captureCenters,std::vector<GPURecord>(2),colored,constants);
        for(uint32_t index=0;index<captured.size();++index)
        {
            Require(captured[index].state==1 && captured[index].reason==0,"front-facing inward enclosure did not capture");
            const auto expected=ReferenceProjection(inwardSDF,captureCenters[index],index,1,constants.raysPerProbe,colored);
            const float error=SHError(captured[index],expected);maximumProjectionError=std::max(maximumProjectionError,error);
            Require(error<3e-4f,"actual hit material Lo0 SH projection differs from CPU triangles and 4pi/N reference");
            Require(captured[index].updateCount==1 && captured[index].sampleCount==128 && captured[index].lastUpdateFrame==19,"capture counters/frame stamp incorrect");
            for(const auto& coefficient:captured[index].sh)Require(coefficient.w==0,"SH padding was not zero");
        }
        for(const uint32_t boundary:{8u,256u})
        {
            auto boundaryConstants=constants;boundaryConstants.classifyRays=boundary;boundaryConstants.raysPerProbe=boundary;
            const auto boundaryCapture=gpu.Dispatch(inwardSDF,captureCenters,std::vector<GPURecord>(2),colored,boundaryConstants);
            for(uint32_t index=0;index<boundaryCapture.size();++index)
            {
                const auto& record=boundaryCapture[index];
                const auto expected=ReferenceProjection(inwardSDF,captureCenters[index],index,1,boundary,colored);
                const float error=SHError(record,expected);maximumProjectionError=std::max(maximumProjectionError,error);
                Require(error<3e-4f && record.state==1 && record.reason==0 && record.classificationRays==boundary &&
                    record.updateCount==1 && record.sampleCount==boundary && record.lastUpdateFrame==19,"8/256-ray grouped capture projection/metadata incorrect");
            }
        }
        success("actual_hit_UV_emission_source_directional_SH_CPU_reference");
        {
            // Alpha expectations are explicit, independent of CPU geometry:
            // all six inward shell faces have the same alpha and known Lo0.
            // The existing white albedo texture (heap index 0) supplies alpha 1.
            auto alphaShell=Box(true);VXGIWorld alphaWorld;VXGISDF alphaSDF;
            Build(alphaShell,alphaWorld,alphaSDF,true);
            Require(std::all_of(alphaSDF.GetTriangles().begin(),alphaSDF.GetTriangles().end(),
                [](const auto& triangle){return triangle.isCutout;}),"alpha fixture lost its cutout flags");
            const XMFLOAT3 source={.4f,.2f,.1f};auto alphaMaterials=UniformMaterials(source);
            for(auto& material:alphaMaterials)material.baseColorFactor.w=0;
            GPUConstants alphaConstants=constants;alphaConstants.updateStart=0;
            alphaConstants.updateCount=uint32_t(captureCenters.size());alphaConstants.resetMode=1;
            alphaConstants.frameIndex=900;alphaConstants.materialCount=uint32_t(alphaMaterials.size());
            alphaConstants.sunRadiance={};alphaConstants.maxRayDistance=100;
            const auto holes=gpu.Dispatch(alphaSDF,captureCenters,std::vector<GPURecord>(captureCenters.size()),alphaMaterials,alphaConstants);
            for(const auto& record:holes)
                Require(record.state==1&&record.reason==0&&record.backfaceRatio==0&&
                    record.classificationRays==alphaConstants.classifyRays&&record.minHitDistance==alphaConstants.maxRayDistance&&
                    record.updateCount==1&&record.sampleCount==alphaConstants.raysPerProbe&&record.lastUpdateFrame==900&&ZeroSH(record),
                    "alpha-zero shell must classify fixed centers valid and capture exactly zero radiance");

            // The same alpha-zero factors on the original opaque shell must
            // still terminate geometry and capture its known uniform emission.
            const auto opaque=gpu.Dispatch(inwardSDF,captureCenters,std::vector<GPURecord>(captureCenters.size()),alphaMaterials,alphaConstants);
            const auto expected=ConstantSH(source);
            for(const auto& record:opaque)
            {
                const float error=SHError(record,expected);maximumProjectionError=std::max(maximumProjectionError,error);
                Require(record.state==1&&record.reason==0&&record.minHitDistance<alphaConstants.maxRayDistance&&error<3e-5f,
                    "opaque alpha-zero shell must retain geometry and uniform Lo0 capture");
            }

            for(auto& material:alphaMaterials)material.baseColorFactor.w=1;
            alphaConstants.frameIndex=901;
            // A coverage edit requires reset mode 1, replacing the old miss
            // classification as well as its already-updated zero SH history.
            const auto covered=gpu.Dispatch(alphaSDF,captureCenters,holes,alphaMaterials,alphaConstants);
            for(const auto& record:covered)
            {
                const float error=SHError(record,expected);maximumProjectionError=std::max(maximumProjectionError,error);
                Require(record.state==1&&record.reason==0&&record.backfaceRatio==0&&
                    record.classificationRays==alphaConstants.classifyRays&&record.minHitDistance>alphaConstants.rayMinDistance&&
                    record.minHitDistance<alphaConstants.maxRayDistance&&record.updateCount==1&&
                    record.sampleCount==alphaConstants.raysPerProbe&&record.lastUpdateFrame==901&&error<3e-5f,
                    "covered shell reset must reclassify and capture known uniform Lo0 once");
            }
            for(auto& material:alphaMaterials)material.baseColorFactor.w=0;
            alphaConstants.frameIndex=902;
            const auto holesAgain=gpu.Dispatch(alphaSDF,captureCenters,covered,alphaMaterials,alphaConstants);
            for(const auto& record:holesAgain)
                Require(record.state==1&&record.reason==0&&record.backfaceRatio==0&&
                    record.minHitDistance==alphaConstants.maxRayDistance&&record.updateCount==1&&
                    record.sampleCount==alphaConstants.raysPerProbe&&record.lastUpdateFrame==902&&ZeroSH(record),
                    "restored holes retained covered-shell classification or stale captured Lo0");
        }
        success("alpha_shell_parallel_serial_classification_capture_full_reset_and_opaque_alpha_zero");
        constants.raysPerProbe=64;constants.updateCount=1;constants.updateStart=1;constants.frameIndex=41;
        std::vector<GPURecord> historical(3);std::vector<XMFLOAT3> historyCenters(3);
        for(uint32_t index=0;index<3;++index)
        {
            auto& record=historical[index];record.state=1;record.updateCount=2;record.sampleCount=128;record.lastUpdateFrame=12;
            record.classificationRays=64;record.minHitDistance=1;const auto sh=ConstantSH({.5f,.25f,.125f});
            for(size_t coefficient=0;coefficient<4;++coefficient)record.sh[coefficient]={sh[coefficient].x,sh[coefficient].y,sh[coefficient].z,0};
        }
        const auto history=gpu.Dispatch(inwardSDF,historyCenters,historical,UniformMaterials({1,.5f,.25f}),constants);
        Require(std::memcmp(&history[0],&historical[0],sizeof(GPURecord))==0 && std::memcmp(&history[2],&historical[2],sizeof(GPURecord))==0,"full copy plus selected update corrupted unselected probes");
        auto expectedHistory=ConstantSH({2.0f/3,1.0f/3,1.0f/6});
        maximumHistoryError=SHError(history[1],expectedHistory);Require(maximumHistoryError<2e-5f,"history averaging does not weight existing updates");
        Require(history[1].updateCount==3 && history[1].sampleCount==192 && history[1].lastUpdateFrame==41,"selected update accounting wrong");
        success("full_previous_copy_selected_batch_preserves_unselected_and_temporal_mean");
        GPUConstants scheduleConstants=constants;scheduleConstants.raysPerProbe=128;scheduleConstants.updateStart=0;
        scheduleConstants.updateCount=2;scheduleConstants.frameIndex=500;
        const auto alternateFrame=gpu.Dispatch(inwardSDF,captureCenters,std::vector<GPURecord>(2),colored,scheduleConstants);
        for(size_t index=0;index<captured.size();++index)
        {
            for(size_t coefficient=0;coefficient<4;++coefficient)
                Require(Error(Color3(alternateFrame[index].sh[coefficient]),Color3(captured[index].sh[coefficient]))==0,"frame scheduling changed directional probe sample result");
            Require(alternateFrame[index].lastUpdateFrame==500 && alternateFrame[index].classificationRays==captured[index].classificationRays,"alternate frame tag/classification wrong");
        }
        success("capture_directions_depend_on_probe_update_count_and_ignore_frame_schedule");
        constants.updateStart=2;constants.updateCount=2;constants.frameIndex=501;
        const auto wrapped=gpu.Dispatch(inwardSDF,historyCenters,historical,UniformMaterials({1,.5f,.25f}),constants);
        Require(std::memcmp(&wrapped[1],&historical[1],sizeof(GPURecord))==0 && wrapped[0].updateCount==3 && wrapped[2].updateCount==3
            && wrapped[0].lastUpdateFrame==501 && wrapped[2].lastUpdateFrame==501,"cyclic batch did not wrap exactly once while retaining unselected record");
        Require(SHError(wrapped[0],expectedHistory)<2e-5f && SHError(wrapped[2],expectedHistory)<2e-5f,"cyclic batch sample results wrong");
        success("cyclic_selected_batch_wraps_and_preserves_one_writer_per_probe");
        constants.updateCount=1;
        constants.updateStart=0;constants.frameIndex=42;
        const auto fourth=gpu.Dispatch(inwardSDF,{{0,0,0}},{history[1]},UniformMaterials({1.5f,.75f,.375f}),constants);
        expectedHistory=ConstantSH({.875f,.4375f,.21875f});const float fourthError=SHError(fourth[0],expectedHistory);maximumHistoryError=std::max(maximumHistoryError,fourthError);
        Require(fourthError<2e-5f && fourth[0].updateCount==4 && fourth[0].sampleCount==256,"fourth running mean/counters wrong");
        constants.frameIndex=43;
        const auto fifth=gpu.Dispatch(inwardSDF,{{0,0,0}},fourth,UniformMaterials({1.875f,.9375f,.46875f}),constants);
        expectedHistory=ConstantSH({1.125f,.5625f,.28125f});const float fifthError=SHError(fifth[0],expectedHistory);maximumHistoryError=std::max(maximumHistoryError,fifthError);
        Require(fifthError<2e-5f && fifth[0].updateCount==5 && fifth[0].sampleCount==320,"history cap did not become fixed 1/maxHistoryUpdates blend");
        success("history_limit_switches_to_fixed_weight_and_keeps_total_sample_accounting");
        std::vector<GPURecord> resetInputs={fifth[0],classified[0],classified[3],fifth[0],fifth[0]};
        resetInputs[3].state=2;resetInputs[3].reason=3;resetInputs[4].state=2;resetInputs[4].reason=4;
        constants.resetMode=2;constants.updateCount=0;
        const auto resetLighting=gpu.Dispatch(inwardSDF,std::vector<XMFLOAT3>(resetInputs.size()),resetInputs,materials,constants,false);
        Require(resetLighting[0].state==1 && resetLighting[1].state==2 && resetLighting[1].reason==1 && resetLighting[2].state==2 && resetLighting[2].reason==2,"lighting reset lost reliable classification");
        Require(resetLighting[3].state==0 && resetLighting[3].reason==0 && resetLighting[4].state==0 && resetLighting[4].reason==0,"lighting reset did not retry trace/source failures");
        for(const auto& record:resetLighting)Require(ZeroSH(record) && record.updateCount==0 && record.sampleCount==0 && record.lastUpdateFrame==0,"lighting reset retained stale SH/counters");
        success("lighting_reset_clears_SH_preserves_reliable_classification_retries_failures");
        constants.resetMode=1;
        const auto resetGeometry=gpu.Dispatch(inwardSDF,std::vector<XMFLOAT3>(resetInputs.size()),resetInputs,materials,constants,false);
        const GPURecord zero{};for(const auto& record:resetGeometry)Require(std::memcmp(&record,&zero,sizeof(record))==0,"geometry reset did not zero full record");
        success("geometry_reset_zeros_full_probe_record");
        constants.resetMode=0;constants.updateCount=1;constants.materialCount=0;
        const auto failedSource=gpu.Dispatch(inwardSDF,{{0,0,0}},{GPURecord{}},materials,constants);
        Require(failedSource[0].state==2 && failedSource[0].reason==4 && ZeroSH(failedSource[0]),"source failure did not disable/clear probe");
        constants.resetMode=2;constants.materialCount=6;constants.frameIndex=55;
        const auto retried=gpu.Dispatch(inwardSDF,{{0,0,0}},failedSource,materials,constants);
        Require(retried[0].state==1 && retried[0].reason==0 && retried[0].updateCount==1 && retried[0].lastUpdateFrame==55,"lighting reset failed to retry source failure in same selected dispatch");
        success("real_source_failure_disables_then_same_frame_reset_retry_succeeds");
        constants.resetMode=0;constants.maxRayDistance=-1;
        const auto failedTrace=gpu.Dispatch(inwardSDF,{{0,0,0}},{GPURecord{}},materials,constants);
        Require(failedTrace[0].state==2 && failedTrace[0].reason==3 && ZeroSH(failedTrace[0]),"invalid traversal did not disable classification");
        success("real_classification_trace_failure_disables_and_clears_SH");
        const auto production=ProductionAcceptance();
        success("production_RDG_three_slot_pingpong_geometry_move_resize_restore_and_epoch_snapshots");
        const auto warnings=gpu.Warnings();Require(warnings==0,"D3D12 validation emitted warnings/errors");
        std::ofstream report("tmp/VXGIProbeGPU-report.json");Require(bool(report),"cannot write probe GPU report");
        report << std::setprecision(9) << "{\n  \"adapter\":\"" << gpu.adapterName << "\",\n  \"warp\":" << (gpu.warp?"true":"false")
            << ",\n  \"debug_layer\":" << (gpu.debugLayer?"true":"false") << ",\n  \"gpu_based_validation\":" << (gpu.gpuValidation?"true":"false")
            << ",\n  \"production_source\":\"Shaders_For_VXGI_Probe_Update.hlsl CSReset/CSUpdateParallel + VXGIProbeCommon.hlsli\",\n  \"reference_source\":\"Shaders_For_VXGI_Probe_Update.hlsl CSUpdate\",\n  \"test_groups\":[\n";
        for(size_t index=0;index<passed.size();++index){if(index)report << ",\n";report << "    \"" << passed[index] << '\"';}
        report << "\n  ],\n  \"group_count\":" << passed.size() << ",\n  \"production_ray_counts_tested\":[8,64,128,256],\n  \"maximum_numerical_SH_irradiance_error\":" << maximumNumericalError
            << ",\n  \"maximum_actual_surface_SH_error\":" << maximumProjectionError << ",\n  \"maximum_history_error\":" << maximumHistoryError
            << ",\n  \"parallel_reference_record_comparisons\":" << gpu.parallelReferenceComparisons
            << ",\n  \"parallel_reference_maximum_SH_error\":" << gpu.maximumParallelReferenceSHError
            << ",\n  \"parallel_reference_maximum_distance_error\":" << gpu.maximumParallelReferenceDistanceError
            << ",\n  \"parallel_reference_classification_counters_exact\":true"
            << ",\n  \"production_wrapper\":{\n    \"classes\":\"RenderDevice + VXGIGPUScene + VXGIProbePass + RDGBuilder\",\n    \"window_visible\":false,\n    \"present_calls\":0,\n    \"frame_slots\":3,\n    \"submissions\":" << production.frames
            << ",\n    \"initial_probe_count\":" << production.initialCount << ",\n    \"moved_probe_count\":" << production.movedCount << ",\n    \"restored_probe_count\":" << production.restoredCount
            << ",\n    \"records_checked_against_CPU\":" << production.recordsChecked << ",\n    \"maximum_SH_source_history_error\":" << production.maximumSHError
            << ",\n    \"accepted_production_snapshots\":" << production.acceptedReadbacks << ",\n    \"resize_observed_in_flight\":" << (production.resizeObservedInFlight?"true":"false")
            << ",\n    \"old_epoch_snapshots_discarded\":true,\n    \"d3d12_warnings_errors\":" << production.warnings << "\n  }"
            << ",\n  \"d3d12_warnings_errors\":" << warnings << ",\n  \"passed\":true\n}\n";
        std::cout << "VXGI probe GPU acceptance: " << passed.size() << " groups passed; SH/error " << maximumNumericalError << ", actual surface SH " << maximumProjectionError << ", history " << maximumHistoryError
            << "; parallel/reference SH " << gpu.maximumParallelReferenceSHError << " over " << gpu.parallelReferenceComparisons << " records; exact classification/counters; 0 warnings/errors.\n";
        std::cout << "Production wrapper: " << production.initialCount << " -> " << production.movedCount << " -> " << production.restoredCount
            << " probes; " << production.frames << " submissions on 3 slots; " << production.recordsChecked << " CPU-checked records; SH error " << production.maximumSHError
            << "; stale epoch snapshots discarded; hidden window, no Present; 0 warnings/errors.\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr << "VXGI probe GPU acceptance FAILED: " << error.what() << '\n';return 1;}
}
