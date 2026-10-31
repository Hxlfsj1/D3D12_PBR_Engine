// Standalone GPU regression: execute the production HLSL helper without a
// window, application startup, model import, settings edits or CPU math mirror.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "ThirdParty/json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
struct Vector { float x = 0, y = 0, z = 0, w = 0; };
struct Input { Vector normal, tangent, bitangent; };
struct Output { Vector normal, tangent, bitangent, mappedNormal; };
static_assert(sizeof(Input) == 48 && sizeof(Output) == 64);
struct Case
{
    std::string name;
    Input input;
    Vector expectedNormal;
    Vector expectedTangent;
    int handedness = 0;
};

static void Require(bool value, const std::string& message)
{ if (!value) throw std::runtime_error(message); }
static void Check(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result)) return;
    char code[32]; std::snprintf(code, sizeof(code), "0x%08X", unsigned(result));
    throw std::runtime_error(std::string(operation) + " failed: " + code);
}
static float Dot(Vector a, Vector b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vector Cross(Vector a, Vector b)
{ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static Vector Unit(Vector value)
{
    const float length = std::sqrt(Dot(value, value));
    return {value.x/length, value.y/length, value.z/length};
}
static bool Finite(Vector value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
static Json Components(Vector value) { return {value.x, value.y, value.z}; }

static std::vector<Case> Cases()
{
    std::vector<Case> cases;
    const std::array<Vector, 6> axes = {{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    for (size_t index = 0; index < axes.size(); ++index)
    {
        const auto normal = axes[index];
        cases.push_back({"axis-" + std::to_string(index) + "-importer-defaults",
            {normal,{1,0,0},{0,1,0}},normal});
        cases.push_back({"axis-" + std::to_string(index) + "-missing-tangent-and-bitangent",
            {normal,{},{}},normal});
    }
    cases.push_back({"parallel-tangent",{{1,0,0},{3,0,0},{0,1,0}},{1,0,0}});
    cases.push_back({"antiparallel-tangent",{{1,0,0},{-3,0,0},{0,1,0}},{1,0,0}});
    cases.push_back({"nearly-parallel-tangent",{{1,0,0},{1,1e-8f,0},{0,1,0}},{1,0,0}});
    cases.push_back({"zero-bitangent",{{0,0,1},{1,0,0},{}},{0,0,1},{1,0,0}});
    cases.push_back({"skew-right-handed",{{0,0,4},{2,1,7},{-1,2,8}},{0,0,1},{2,1,0},1});
    cases.push_back({"skew-mirrored",{{0,0,4},{2,1,7},{1,-2,-8}},{0,0,1},{2,1,0},-1});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    cases.push_back({"zero-normal",{{},{1,0,0},{0,1,0}},{0,0,1}});
    cases.push_back({"nan-normal",{{nan,0,1},{1,0,0},{0,1,0}},{0,0,1}});
    cases.push_back({"infinite-normal",{{0,inf,1},{1,0,0},{0,1,0}},{0,0,1}});
    cases.push_back({"nan-tangent",{{0,0,1},{nan,0,0},{0,1,0}},{0,0,1}});
    cases.push_back({"infinite-tangent",{{0,0,1},{0,inf,0},{0,1,0}},{0,0,1}});
    cases.push_back({"nan-bitangent",{{0,0,1},{1,0,0},{0,nan,0}},{0,0,1},{1,0,0}});
    cases.push_back({"infinite-bitangent",{{0,0,1},{1,0,0},{0,inf,0}},{0,0,1},{1,0,0}});
    cases.push_back({"all-nonfinite",{{nan,inf,nan},{inf,nan,inf},{nan,inf,nan}},{0,0,1}});
    const std::array<Vector, 4> rotated = {{{1,2,3},{-2,1,3},{2,-3,1},{-3,-2,-1}}};
    for (size_t index = 0; index < rotated.size(); ++index)
    {
        const Vector normal = rotated[index];
        const Vector tangent = {normal.y,-normal.x,0};
        const Vector bitangent = Cross(normal,tangent);
        cases.push_back({"rotated-" + std::to_string(index),
            {normal,tangent,bitangent},normal,tangent,1});
        cases.push_back({"rotated-" + std::to_string(index) + "-missing-tangent",
            {normal,{},{}},normal});
    }
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

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 3; root.pParameters = parameters;
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
    std::vector<Input> inputs; for (const auto& item : cases) inputs.push_back(item.input);
    const UINT64 inputBytes = inputs.size() * sizeof(Input);
    const UINT64 outputBytes = cases.size() * sizeof(Output);
    auto input = Buffer(device.Get(),inputBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = Buffer(device.Get(),outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr; const D3D12_RANGE empty{};
    Check(input->Map(0,&empty,&mapped),"Input map");
    std::memcpy(mapped,inputs.data(),static_cast<size_t>(inputBytes)); input->Unmap(0,nullptr);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1,&barrier);
    list->SetComputeRootSignature(signature.Get()); list->SetPipelineState(pipeline.Get());
    list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(2,static_cast<UINT>(cases.size()),0);
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
    constexpr float tolerance = 1e-4f;
    for (size_t index = 0; index < cases.size(); ++index)
    {
        const auto& test = cases[index]; const auto& actual = outputs[index];
        std::vector<std::string> reasons;
        const auto expect = [&](bool condition, const char* reason) { if (!condition) reasons.push_back(reason); };
        expect(Finite(actual.normal) && Finite(actual.tangent) && Finite(actual.bitangent) && Finite(actual.mappedNormal),"nonfinite output");
        expect(std::abs(Dot(actual.normal,actual.normal)-1) < tolerance,"normal is not unit length");
        expect(std::abs(Dot(actual.tangent,actual.tangent)-1) < tolerance,"tangent is not unit length");
        expect(std::abs(Dot(actual.bitangent,actual.bitangent)-1) < tolerance,"bitangent is not unit length");
        expect(std::abs(Dot(actual.normal,actual.tangent)) < tolerance &&
            std::abs(Dot(actual.normal,actual.bitangent)) < tolerance &&
            std::abs(Dot(actual.tangent,actual.bitangent)) < tolerance,"basis is not orthogonal");
        expect(std::abs(Dot(actual.mappedNormal,actual.mappedNormal)-1) < tolerance &&
            Dot(actual.mappedNormal,actual.normal) > 0.9999f,"dummy normal mapping is invalid");
        if (Dot(test.expectedNormal,test.expectedNormal) > 0)
            expect(Dot(actual.normal,Unit(test.expectedNormal)) > 1-tolerance,"valid normal direction changed");
        if (Dot(test.expectedTangent,test.expectedTangent) > 0)
            expect(Dot(actual.tangent,Unit(test.expectedTangent)) > 1-tolerance,"valid tangent direction changed");
        const float handedness = Dot(Cross(actual.normal,actual.tangent),actual.bitangent);
        if (test.handedness)
            expect(handedness * static_cast<float>(test.handedness) > 1-tolerance,"handedness changed");
        if (!reasons.empty()) ++failures;
        caseResults.push_back({{"name",test.name},{"passed",reasons.empty()},{"failures",reasons},
            {"normal",Components(actual.normal)},{"tangent",Components(actual.tangent)},
            {"bitangent",Components(actual.bitangent)},{"mapped_normal",Components(actual.mappedNormal)},
            {"handedness",handedness}});
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
        {"adapter",adapterName},{"case_count",cases.size()},{"failed_cases",failures},
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
        Require(argc >= 3,"Usage: TangentBasisGPUTests shader.dxil report.json [--gbv]");
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
