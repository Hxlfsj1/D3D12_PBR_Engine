// Standalone material regression: execute the production ORM helper and
// G-buffer material layout, with the real CPU MaterialData upload ABI.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "ThirdParty/json.hpp"
#pragma warning(push)
#pragma warning(disable:4324) // Production constant buffers intentionally align to 256 bytes.
#include "Passes/RenderStructs.h"
#pragma warning(pop)
#include <cstddef>
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
struct Vector { float x = 0, y = 0, z = 0, w = 0; };
struct Output { Vector orm; MaterialData material; };
static_assert(sizeof(MaterialData) == 80 && sizeof(Output) == 96);
static_assert(offsetof(MaterialData, albedoIdx) == 0 && offsetof(MaterialData, normalIdx) == 4);
static_assert(offsetof(MaterialData, ormIdx) == 8 && offsetof(MaterialData, emissiveIdx) == 12);
static_assert(offsetof(MaterialData, baseColorFactor) == 16 && offsetof(MaterialData, isUnlit) == 32);
static_assert(offsetof(MaterialData, emissiveFactor) == 36 && offsetof(MaterialData, metallicFactor) == 48);
static_assert(offsetof(MaterialData, roughnessFactor) == 52 && offsetof(MaterialData, occlusionIdx) == 56);
static_assert(offsetof(MaterialData, occlusionStrength) == 60 && offsetof(MaterialData, alphaCutoff) == 64);
static_assert(offsetof(MaterialData, materialPadding) == 68);
struct Case { std::string name; MaterialData material; Vector expected; };

static void Require(bool value, const std::string& message)
{ if (!value) throw std::runtime_error(message); }
static void Check(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result)) return;
    char code[32]; std::snprintf(code, sizeof(code), "0x%08X", unsigned(result));
    throw std::runtime_error(std::string(operation) + " failed: " + code);
}
static Json Components(Vector value) { return {value.x, value.y, value.z}; }

// Shared sample records model a texture result once; distinct material records
// index the same sample so material factors cannot be baked into shared assets.
static const std::array<Vector, 7> Samples = {{{1,1,1}, {.3f,.8f,.6f}, {.7f,2,-1},
    {-.2f,.5f,.5f}, {.5f,1,1}, {0,1,1}, {.9f,.1f,.2f}}};
static std::vector<Case> Cases()
{
    std::vector<Case> cases;
    const auto add = [&](const char* name, UINT sample, float metallic, float roughness, Vector expected,
        UINT occlusion = UINT32_MAX, float strength = 1) {
        const auto id = static_cast<UINT>(cases.size());
        MaterialData material{};
        material.albedoIdx = 101 + id; material.normalIdx = 201 + id;
        material.ormIdx = sample; material.emissiveIdx = 301 + id;
        material.baseColorFactor = {float(id)+.125f, .25f, .375f, .5f};
        material.isUnlit = 401 + id;
        material.emissiveFactor = {.625f, float(id)+.75f, .875f};
        material.metallicFactor = metallic; material.roughnessFactor = roughness;
        material.occlusionIdx = occlusion == UINT32_MAX ? sample : occlusion;
        material.occlusionStrength = strength;
        material.alphaCutoff = .1f + float(id) * .01f;
        material.materialPadding = {float(id)+11, float(id)+17, float(id)+23};
        cases.push_back({name, material, expected});
    };
    add("white-no-texture-diffuse", 0, 0, 1, {1,1,0});
    add("white-no-texture-smooth-metal", 0, 1, 0, {1,0,1});
    add("white-no-texture-scalars", 0, .25f, .5f, {1,.5f,.25f});
    add("texture-times-material", 1, .25f, .5f, {.3f,.4f,.15f});
    add("same-texture-different-material", 1, .75f, .25f, {.3f,.2f,.45f});
    add("zero-factors", 1, 0, 0, {.3f,0,0});
    add("unit-factors", 1, 1, 1, {.3f,.8f,.6f});
    add("factor-product-clamp-high", 0, 2, 3, {1,1,1});
    add("factor-product-clamp-low", 0, -1, -2, {1,0,0});
    add("sample-product-clamping", 2, 1, 1, {.7f,1,0});
    add("invalid-occlusion-clamped-without-changing-mr", 3, .5f, .5f, {0,.25f,.25f});
    // Separate AO has deliberately unrelated G/B values. Those channels must
    // never replace MR, including when both textures are shared by materials.
    add("separate-ao-preserves-mr", 1, .25f, .5f, {.5f,.4f,.15f}, 4);
    add("separate-ao-strength-zero", 1, .25f, .5f, {1,.4f,.15f}, 4, 0);
    add("separate-ao-strength-half", 1, .25f, .5f, {.75f,.4f,.15f}, 4, .5f);
    add("separate-black-ao", 1, .25f, .5f, {0,.4f,.15f}, 5);
    add("missing-ao-uses-white", 1, .25f, .5f, {1,.4f,.15f}, 0);
    add("missing-mr-keeps-scalars-and-independent-ao", 0, .25f, .5f, {.5f,.5f,.25f}, 4);
    add("packed-orm-shares-one-sample", 1, .25f, .5f, {.3f,.4f,.15f}, 1);
    add("different-ao-keeps-shared-mr", 1, .25f, .5f, {.9f,.4f,.15f}, 6);
    add("ao-strength-clamped-high", 1, .25f, .5f, {.5f,.4f,.15f}, 4, 2);
    add("ao-strength-clamped-low", 1, .25f, .5f, {1,.4f,.15f}, 4, -1);
    return cases;
}

static D3D12_RESOURCE_DESC BufferDescription(UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes; description.Height = 1;
    description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags; return description;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes,
    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
    auto description = BufferDescription(bytes,flags);
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,
        &description,state,nullptr,IID_PPV_ARGS(&resource)),"Buffer allocation");
    return resource;
}
static std::vector<uint8_t> ReadShader(const char* path)
{
    std::ifstream file(path,std::ios::binary | std::ios::ate);
    Require(bool(file),std::string("Cannot read shader: ") + path);
    const auto size = file.tellg(); Require(size > 0,"Empty shader");
    std::vector<uint8_t> result(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(result.data()),size);
    Require(bool(file),"Cannot read shader bytes"); return result;
}

static Json Run(const char* shaderPath, bool gbv)
{
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer");
    debug->EnableDebugLayer();
    if (gbv)
    {
        ComPtr<ID3D12Debug1> debug1; Check(debug.As(&debug1),"GPU validation interface");
        debug1->SetEnableGPUBasedValidation(TRUE);
    }
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<ID3D12Device> device;
    std::string adapterName;
    for (UINT index = 0; ; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        auto status = factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
        if (status == DXGI_ERROR_NOT_FOUND) break;
        Check(status,"Hardware adapter enumeration");
        DXGI_ADAPTER_DESC1 description{}; Check(adapter->GetDesc1(&description),"Adapter description");
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) continue;
        for (const auto character : description.Description)
        { if (!character) break; adapterName.push_back(character < 128 ? char(character) : '?'); }
        break;
    }
    Require(bool(device),"No D3D12 hardware adapter available");
    ComPtr<ID3D12InfoQueue> info; Check(device.As(&info),"D3D12 information queue");
    D3D12_COMMAND_QUEUE_DESC queueDescription{}; queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDescription,IID_PPV_ARGS(&queue)),"Command queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(queueDescription.Type,IID_PPV_ARGS(&allocator)),"Command allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandList(0,queueDescription.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Command list");

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 7; // Actual gMaterialData register.
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor.ShaderRegister = 0;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants.ShaderRegister = 2;
    parameters[3].Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 4; root.pParameters = parameters;
    ComPtr<ID3DBlob> serialized,errors;
    Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"Root signature serialization");
    ComPtr<ID3D12RootSignature> signature;
    Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&signature)),"Root signature");
    const auto shader = ReadShader(shaderPath);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = signature.Get();
    pipelineDescription.CS = {shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(device->CreateComputePipelineState(&pipelineDescription,IID_PPV_ARGS(&pipeline)),"Compute pipeline");

    const auto cases = Cases();
    std::vector<MaterialData> inputs; for (const auto& item : cases) inputs.push_back(item.material);
    const UINT64 inputBytes = inputs.size() * sizeof(MaterialData);
    const UINT64 outputBytes = cases.size() * sizeof(Output);
    auto input = Buffer(device.Get(),inputBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto samples = Buffer(device.Get(),sizeof(Samples),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr; const D3D12_RANGE empty{};
    Check(input->Map(0,&empty,&mapped),"Input map");
    std::memcpy(mapped,inputs.data(),static_cast<size_t>(inputBytes)); input->Unmap(0,nullptr);
    Check(samples->Map(0,&empty,&mapped),"Sample map");
    std::memcpy(mapped,Samples.data(),sizeof(Samples)); samples->Unmap(0,nullptr);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1,&barrier);
    list->SetComputeRootSignature(signature.Get()); list->SetPipelineState(pipeline.Get());
    list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1,samples->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(3,static_cast<UINT>(cases.size()),0);
    list->Dispatch(static_cast<UINT>((cases.size()+63)/64),1,1);
    barrier.Transition = {output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);
    list->CopyBufferRegion(readback.Get(),0,output.Get(),0,outputBytes);
    Check(list->Close(),"Command list close");
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
    HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event != nullptr,"Fence event creation failed");
    struct EventOwner { HANDLE value; ~EventOwner() { CloseHandle(value); } } eventOwner{event};
    Check(queue->Signal(fence.Get(),1),"Fence signal");
    Check(fence->SetEventOnCompletion(1,event),"Fence event");
    Require(WaitForSingleObject(event,30000) == WAIT_OBJECT_0,"GPU timeout");
    Check(device->GetDeviceRemovedReason(),"Device removal check");
    const D3D12_RANGE range = {0,static_cast<SIZE_T>(outputBytes)};
    Check(readback->Map(0,&range,&mapped),"Output map");
    std::vector<Output> outputs(cases.size());
    std::memcpy(outputs.data(),mapped,static_cast<size_t>(outputBytes)); readback->Unmap(0,&empty);

    Json caseResults = Json::array(); size_t failures = 0;
    constexpr float tolerance = 1e-6f;
    for (size_t index = 0; index < cases.size(); ++index)
    {
        const auto& test = cases[index]; const auto& actual = outputs[index];
        std::vector<std::string> reasons;
        const auto expect = [&](bool condition, const char* reason) { if (!condition) reasons.push_back(reason); };
        expect(std::isfinite(actual.orm.x) && std::isfinite(actual.orm.y) && std::isfinite(actual.orm.z), "nonfinite output");
        expect(std::abs(actual.orm.x-test.expected.x) < tolerance, "occlusion changed");
        expect(std::abs(actual.orm.y-test.expected.y) < tolerance, "roughness factor result differs");
        expect(std::abs(actual.orm.z-test.expected.z) < tolerance, "metallic factor result differs");
        expect(std::memcmp(&actual.material,&test.material,sizeof(MaterialData)) == 0, "CPU/GPU material ABI mismatch");
        if (!reasons.empty()) ++failures;
        caseResults.push_back({{"name",test.name},{"passed",reasons.empty()},{"failures",reasons},
            {"sample",Components(Samples[test.material.ormIdx])},{"metallic_factor",test.material.metallicFactor},
            {"roughness_factor",test.material.roughnessFactor},
            {"occlusion_sample",Samples[test.material.occlusionIdx].x},{"occlusion_strength",test.material.occlusionStrength},
            {"expected_orm",Components(test.expected)},
            {"actual_orm",Components(actual.orm)}});
    }
    Json messages = Json::array(); uint32_t warningCount = 0,errorCount = 0;
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
    {
        SIZE_T bytes = 0; Check(info->GetMessage(index,nullptr,&bytes),"Debug message size");
        std::vector<uint8_t> storage(bytes); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(index,message,&bytes),"Debug message");
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++warningCount; else ++errorCount;
        messages.push_back({{"id",static_cast<uint32_t>(message->ID)},{"description",message->pDescription}});
    }
    return {{"passed",failures == 0 && warningCount == 0 && errorCount == 0},
        {"adapter",adapterName},{"material_stride",sizeof(MaterialData)},
        {"abi_offsets",{{"metallic_factor",offsetof(MaterialData,metallicFactor)},
            {"roughness_factor",offsetof(MaterialData,roughnessFactor)},
            {"occlusion_index",offsetof(MaterialData,occlusionIdx)},
            {"occlusion_strength",offsetof(MaterialData,occlusionStrength)},
            {"alpha_cutoff",offsetof(MaterialData,alphaCutoff)},{"padding",offsetof(MaterialData,materialPadding)}}},
        {"case_count",cases.size()},{"failed_cases",failures},
        {"diagnostics",{{"debug_layer",true},{"gpu_based_validation",gbv},
            {"warnings",warningCount},{"errors",errorCount},{"messages",messages}}},
        {"cases",caseResults}};
}

int main(int argc, char** argv)
{
    const std::string reportPath = argc > 2 ? argv[2] : "report.json";
    Json report;
    try
    {
        Require(argc >= 3,"Usage: MaterialFactorsGPUTests shader.dxil report.json [--gbv]");
        report = Run(argv[1],argc > 3 && std::string(argv[3]) == "--gbv");
    }
    catch (const std::exception& error) { report = {{"passed",false},{"error",error.what()}}; }
    std::ofstream file(reportPath); file << report.dump(2) << '\n';
    Json summary = report; summary.erase("cases"); summary["report"] = reportPath;
    if (report.contains("cases"))
        for (const auto& item : report["cases"])
            if (!item.value("passed",false))
                summary["failed_case_details"].push_back({{"name",item["name"]},{"failures",item["failures"]}});
    std::cout << summary.dump(2) << '\n';
    return file && report.value("passed",false) ? 0 : 1;
}
