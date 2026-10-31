// Tests real WIC uploads, DirectXTK mip generation and D3D12 texture filtering.
// The role policy comes from the production loader, not a test copy.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <ResourceUploadBatch.h>
#include "Resources/MaterialTextureLoader.h"
#include "ThirdParty/json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
struct Vector { float x, y, z, w; };
struct Output { Vector midpoint, mip, firstMip, secondMip; };
struct Case
{
    std::string name;
    TextureType role;
    bool gray, tagged, legacy;
    ComPtr<ID3D12Resource> texture;
    int shape = 0; // 0=2x2, 1=3x1, 2=1x3, 3=8x8 checker, 4=1x1 gray.
    bool srgbAutogen = false;
};

static void Require(bool condition, const std::string& message)
{ if (!condition) throw std::runtime_error(message); }
static void Check(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result)) return;
    char code[32]; std::snprintf(code,sizeof(code),"0x%08X",unsigned(result));
    throw std::runtime_error(std::string(operation) + " failed: " + code);
}
static Json Components(Vector value) { return {value.x,value.y,value.z,value.w}; }

// Both PNGs are 2x2 RGBA. BlackWhite has black/white columns with alpha 64/192;
// Gray has RGBA 128 everywhere. The sRGB chunk below changes metadata only.
static const uint8_t BlackWhitePNG[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x06,0x00,0x00,0x00,0x72,0xb6,0x0d,0x24,
    0x00,0x00,0x00,0x14,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x60,0x60,0x60,0x70,0xf8,
    0xff,0xff,0xff,0x01,0x06,0x18,0x03,0x00,0x39,0xe5,0x07,0xfb,0xbe,0x6f,0x90,0x71,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
static const uint8_t GrayPNG[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x06,0x00,0x00,0x00,0x72,0xb6,0x0d,0x24,
    0x00,0x00,0x00,0x0e,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x68,0x80,0x02,0x06,0x18,
    0x03,0x00,0x48,0x12,0x08,0x01,0x4f,0xf7,0x68,0x54,0x00,0x00,0x00,0x00,0x49,0x45,
    0x4e,0x44,0xae,0x42,0x60,0x82};
static std::vector<uint8_t> PNG(bool gray, bool tagged)
{
    std::vector<uint8_t> data = gray ? std::vector<uint8_t>(std::begin(GrayPNG),std::end(GrayPNG))
        : std::vector<uint8_t>(std::begin(BlackWhitePNG),std::end(BlackWhitePNG));
    if (tagged)
    {
        const uint8_t chunk[] = {0,0,0,1,'s','R','G','B',0,0xae,0xce,0x1c,0xe9};
        data.insert(data.begin()+33,std::begin(chunk),std::end(chunk));
    }
    return data;
}
static std::vector<uint8_t> ShapePNG(int shape)
{
    // Minimal PNG encoder for deterministic test fixtures. A stored DEFLATE block
    // avoids adding a compression library to this standalone GPU regression.
    const uint32_t width=shape==1?3u:shape==3?8u:1u;
    const uint32_t height=shape==2?3u:shape==3?8u:1u;
    std::vector<uint8_t> png={137,80,78,71,13,10,26,10};
    const auto bigEndian=[](std::vector<uint8_t>& out,uint32_t value) {
        for (int shift: {24,16,8,0}) out.push_back(static_cast<uint8_t>(value>>shift));
    };
    const auto chunk=[&](const char* type,const std::vector<uint8_t>& data) {
        bigEndian(png,static_cast<uint32_t>(data.size()));
        const size_t start=png.size();
        for (int c=0;c<4;++c) png.push_back(static_cast<uint8_t>(type[c]));
        png.insert(png.end(),data.begin(),data.end());
        uint32_t crc=0xffffffffu;
        for (size_t i=start;i<png.size();++i)
        {
            crc^=png[i];
            for (int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320u:0u);
        }
        bigEndian(png,~crc);
    };
    std::vector<uint8_t> header;
    bigEndian(header,width); bigEndian(header,height);
    header.insert(header.end(),{8,6,0,0,0}); chunk("IHDR",header);
    std::vector<uint8_t> raw;
    for (uint32_t y=0;y<height;++y)
    {
        raw.push_back(0);
        for (uint32_t x=0;x<width;++x)
        {
            const uint8_t value=shape==4?128u:shape==3?((x+y)%2?255u:0u):
                ((shape==1?x:y)==2?255u:0u);
            raw.insert(raw.end(),{value,value,value,128});
        }
    }
    const auto length=static_cast<uint16_t>(raw.size());
    const auto inverse=static_cast<uint16_t>(~length);
    std::vector<uint8_t> compressed={0x78,0x01,0x01,
        static_cast<uint8_t>(length),static_cast<uint8_t>(length>>8),
        static_cast<uint8_t>(inverse),static_cast<uint8_t>(inverse>>8)};
    compressed.insert(compressed.end(),raw.begin(),raw.end());
    uint32_t a=1,b=0;
    for (uint8_t value: raw) { a=(a+value)%65521; b=(b+a)%65521; }
    bigEndian(compressed,(b<<16)|a); chunk("IDAT",compressed); chunk("IEND",{});
    return png;
}
static std::vector<uint8_t> ReadShader(const char* path)
{
    std::ifstream file(path,std::ios::binary | std::ios::ate);
    Require(bool(file),std::string("Cannot read shader: ")+path);
    const auto size=file.tellg(); Require(size>0,"Empty shader");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),size);
    Require(bool(file),"Cannot read shader bytes"); return bytes;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes,
    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES properties{}; properties.Type=heap;
    D3D12_RESOURCE_DESC description{}; description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width=bytes; description.Height=1; description.DepthOrArraySize=1;
    description.MipLevels=1; description.SampleDesc.Count=1;
    description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; description.Flags=flags;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,
        state,nullptr,IID_PPV_ARGS(&resource)),"Buffer allocation"); return resource;
}

static Json Run(const char* shaderPath, bool gbv)
{
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM initialization");
    struct COMOwner { ~COMOwner() { CoUninitialize(); } } comOwner;
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer");
    debug->EnableDebugLayer();
    if (gbv)
    {
        ComPtr<ID3D12Debug1> debug1; Check(debug.As(&debug1),"GPU validation interface");
        debug1->SetEnableGPUBasedValidation(TRUE);
    }
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<ID3D12Device> device; std::string adapterName;
    for (UINT index=0; ; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const auto status=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
        if (status==DXGI_ERROR_NOT_FOUND) break;
        Check(status,"Adapter enumeration");
        DXGI_ADAPTER_DESC1 description{}; Check(adapter->GetDesc1(&description),"Adapter description");
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) continue;
        for (const auto character: description.Description)
        { if (!character) break; adapterName.push_back(character<128 ? char(character) : '?'); }
        break;
    }
    Require(bool(device),"No D3D12 hardware adapter available");
    ComPtr<ID3D12InfoQueue> info; Check(device.As(&info),"D3D12 information queue");
    D3D12_COMMAND_QUEUE_DESC queueDescription{}; queueDescription.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(&queue)),"Command queue");

    std::vector<Case> cases;
    const std::array<std::pair<TextureType,const char*>,5> roles={{{TextureType::Albedo,"albedo"},
        {TextureType::Emissive,"emissive"},{TextureType::Normal,"normal"},
        {TextureType::MetallicRoughness,"mr"},{TextureType::Occlusion,"ao"}}};
    for (bool gray: {false,true})
        for (const auto& role: roles)
            for (bool tagged: {false,true})
                cases.push_back({std::string(gray?"gray-":"black-white-")+role.second+
                    (tagged?"-tagged":"-plain"),role.first,gray,tagged,false,{}});
    cases.push_back({"negative-control-old-unorm-then-pow",TextureType::Albedo,false,false,true,{}});
    cases.push_back({"negative-control-srgb-autogen",TextureType::Albedo,false,false,false,{},0,true});
    cases.push_back({"odd-3x1-includes-last-white-pixel",TextureType::Albedo,false,false,false,{},1});
    cases.push_back({"odd-1x3-includes-last-white-pixel",TextureType::Emissive,false,false,false,{},2});
    cases.push_back({"checker-8x8-preserves-all-mips",TextureType::Albedo,false,false,false,{},3});
    cases.push_back({"single-pixel-no-extra-mip",TextureType::Albedo,true,false,false,{},4});
    DirectX::ResourceUploadBatch upload(device.Get()); upload.Begin();
    for (auto& item: cases)
    {
        const auto png=item.shape?ShapePNG(item.shape):PNG(item.gray,item.tagged);
        if (item.legacy || item.srgbAutogen)
        {
            const auto flags=static_cast<DirectX::WIC_LOADER_FLAGS>(
                (item.srgbAutogen?DirectX::WIC_LOADER_FORCE_SRGB:DirectX::WIC_LOADER_IGNORE_SRGB) |
                DirectX::WIC_LOADER_FORCE_RGBA32 | DirectX::WIC_LOADER_MIP_AUTOGEN);
            Check(DirectX::CreateWICTextureFromMemoryEx(device.Get(),upload,png.data(),png.size(),
                0,D3D12_RESOURCE_FLAG_NONE,flags,item.texture.GetAddressOf()),"Negative control WIC load");
            upload.Transition(item.texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else
            Check(CreateEmbeddedMaterialTexture(device.Get(),upload,png.data(),png.size(),
                item.role,item.texture.GetAddressOf()),"Production texture load");
    }
    upload.End(queue.Get()).get();

    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(queueDescription.Type,IID_PPV_ARGS(&allocator)),"Command allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandList(0,queueDescription.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Command list");
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors=static_cast<UINT>(cases.size());
    heapDescription.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(device->CreateDescriptorHeap(&heapDescription,IID_PPV_ARGS(&heap)),"Texture heap");
    const UINT increment=device->GetDescriptorHandleIncrementSize(heapDescription.Type);
    for (size_t index=0; index<cases.size(); ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=cases[index].texture->GetDesc().Format;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels=cases[index].texture->GetDesc().MipLevels;
        auto handle=heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=index*increment;
        device->CreateShaderResourceView(cases[index].texture.Get(),&view,handle);
    }
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors=1; range.BaseShaderRegister=0;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={1,&range};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[1].Descriptor.ShaderRegister=0;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants={0,0,3};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderRegister=0; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters=3; root.pParameters=parameters;
    root.NumStaticSamplers=1; root.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> serialized, errors;
    Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"Root signature serialization");
    ComPtr<ID3D12RootSignature> signature;
    Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature)),"Root signature");
    const auto shader=ReadShader(shaderPath);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature=signature.Get(); pipelineDescription.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device->CreateComputePipelineState(&pipelineDescription,IID_PPV_ARGS(&pipeline)),"Compute pipeline");
    const UINT64 outputBytes=cases.size()*sizeof(Output);
    auto output=Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback=Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER initial{}; initial.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    initial.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1,&initial);
    ID3D12DescriptorHeap* heaps[]={heap.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(signature.Get()); list->SetPipelineState(pipeline.Get());
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    for (UINT index=0; index<static_cast<UINT>(cases.size()); ++index)
    {
        auto handle=heap->GetGPUDescriptorHandleForHeapStart(); handle.ptr+=UINT64(index)*increment;
        list->SetComputeRootDescriptorTable(0,handle);
        const UINT constants[]={index,cases[index].legacy?1u:0u,
            UINT(cases[index].texture->GetDesc().MipLevels-1)};
        list->SetComputeRoot32BitConstants(2,3,constants,0); list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER uav{}; uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource=output.Get(); list->ResourceBarrier(1,&uav);
    }
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier); list->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);
    Check(list->Close(),"Command list close");
    ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"Fence event creation failed");
    struct EventOwner { HANDLE value; ~EventOwner() { CloseHandle(value); } } eventOwner{event};
    Check(queue->Signal(fence.Get(),1),"Fence signal");
    Check(fence->SetEventOnCompletion(1,event),"Fence event");
    Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU timeout");
    Check(device->GetDeviceRemovedReason(),"Device removal check");
    void* mapped=nullptr; const D3D12_RANGE readRange={0,static_cast<SIZE_T>(outputBytes)}, empty{};
    Check(readback->Map(0,&readRange,&mapped),"Output map");
    std::vector<Output> results(cases.size());
    std::memcpy(results.data(),mapped,static_cast<size_t>(outputBytes)); readback->Unmap(0,&empty);

    Json caseResults=Json::array(); size_t failures=0;
    for (size_t index=0; index<cases.size(); ++index)
    {
        const auto& item=cases[index]; const auto& actual=results[index];
        const auto description=item.texture->GetDesc(); const bool color=IsColorTexture(item.role) && !item.legacy;
        const float grayLinear=std::pow(((128.0f/255.0f)+.055f)/1.055f,2.4f);
        const float expected=item.legacy?std::pow(.5f,2.2f):(item.gray?(color?grayLinear:128.0f/255.0f):.5f);
        const float expectedMidpoint=(item.shape==1 || item.shape==2)?0.0f:expected;
        const float expectedMip=(item.shape==1 || item.shape==2)?1.0f/3.0f:
            item.srgbAutogen?std::pow(((127.0f/255.0f)+.055f)/1.055f,2.4f):expected;
        const float expectedAlpha=128.0f/255.0f;
        std::vector<std::string> reasons;
        const auto expect=[&](bool value,const char* reason) { if (!value) reasons.push_back(reason); };
        expect(description.Format==(color?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM),"Incorrect color/data resource format");
        const UINT expectedMips=item.shape==4?1u:item.shape==3?4u:2u;
        expect(description.MipLevels==expectedMips,"Generated mip count differs");
        const Vector values[]={actual.midpoint,actual.mip,actual.firstMip,actual.secondMip};
        for (size_t sample=0; sample<4; ++sample)
        {
            const Vector value=values[sample];
            const float expectedSample=sample==0?expectedMidpoint:expectedMip;
            expect(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w),"Nonfinite sample");
            // The 1x1 mip is stored in 8 bits. sRGB .5 rounds to code 188,
            // decoded as .502886; tolerate quantization, not the old .218.
            expect(std::abs(value.x-expectedSample)<.004f && std::abs(value.y-expectedSample)<.004f && std::abs(value.z-expectedSample)<.004f,"RGB filtering or mip differs");
            expect(std::abs(value.w-expectedAlpha)<.0001f,"sRGB conversion changed alpha");
        }
        if (index<20 && index%2==1)
            expect(std::memcmp(&actual,&results[index-1],sizeof(Output))==0,"PNG sRGB metadata changed role-defined values");
        if (item.legacy)
            expect(std::abs(actual.midpoint.x-results[0].midpoint.x)>.25f,"Negative control does not distinguish old ordering");
        if (item.srgbAutogen)
            expect(std::abs(actual.mip.x-results[0].mip.x)>.25f,"Negative control does not distinguish encoded mip filtering");
        if (!reasons.empty()) ++failures;
        caseResults.push_back({{"name",item.name},{"passed",reasons.empty()},{"failures",reasons},
            {"resource_format",description.Format},{"mip_levels",description.MipLevels},
            {"expected_midpoint_rgb",expectedMidpoint},{"expected_mip_rgb",expectedMip},
            {"midpoint_mip0",Components(actual.midpoint)},
            {"generated_last_mip",Components(actual.mip)},{"generated_mip1",Components(actual.firstMip)},
            {"generated_mip2",Components(actual.secondMip)}});
    }
    Json messages=Json::array(); uint32_t warnings=0, errorCount=0;
    for (UINT64 index=0; index<info->GetNumStoredMessages(); ++index)
    {
        SIZE_T bytes=0; Check(info->GetMessage(index,nullptr,&bytes),"Debug message size");
        std::vector<uint8_t> storage(bytes); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(index,message,&bytes),"Debug message");
        if (message->Severity>D3D12_MESSAGE_SEVERITY_WARNING) continue;
        if (message->Severity==D3D12_MESSAGE_SEVERITY_WARNING) ++warnings; else ++errorCount;
        messages.push_back({{"id",static_cast<uint32_t>(message->ID)},{"description",message->pDescription}});
    }
    return {{"passed",failures==0 && warnings==0 && errorCount==0},{"adapter",adapterName},
        {"case_count",cases.size()},{"failed_cases",failures},{"cases",caseResults},
        {"diagnostics",{{"debug_layer",true},{"gpu_based_validation",gbv},{"warnings",warnings},
            {"errors",errorCount},{"messages",messages}}}};
}

int main(int argc, char** argv)
{
    const std::string reportPath=argc>2?argv[2]:"report.json"; Json report;
    try
    {
        Require(argc>=3,"Usage: TextureColorSpaceGPUTests shader.dxil report.json [--gbv]");
        report=Run(argv[1],argc>3 && std::string(argv[3])=="--gbv");
    }
    catch (const std::exception& error) { report={{"passed",false},{"error",error.what()}}; }
    std::ofstream file(reportPath); file<<report.dump(2)<<'\n';
    Json summary=report; summary.erase("cases"); summary["report"]=reportPath;
    if (report.contains("cases"))
        for (const auto& item: report["cases"])
            if (!item.value("passed",false)) summary["failed_case_details"].push_back(item);
    std::cout<<summary.dump(2)<<'\n'; return file && report.value("passed",false)?0:1;
}
