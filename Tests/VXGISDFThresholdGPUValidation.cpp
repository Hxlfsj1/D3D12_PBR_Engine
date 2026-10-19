// Standalone node-distance threshold queries. No window, swap chain or Present.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d12shader.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <d3dx12.h>
#include "../Resources/VXGISDFThreshold.h"
#include "../Resources/VXGIMeshSDF.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Float3 = DirectX::XMFLOAT3;
using Threshold = VXGISDFThreshold;
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
    __declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";
}
namespace
{
    void Require(bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void Check(HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            std::ostringstream message;
            message << operation << ": HRESULT 0x" << std::hex << uint32_t(result);
            throw std::runtime_error(message.str());
        }
    }
    std::string UTF8(const wchar_t* input)
    {
        const int count = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
        std::string result(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, input, -1, result.data(), count, nullptr, nullptr);
        result.pop_back();
        return result;
    }
    struct Ray
    {
        Threshold::Grid grid;
        Threshold::Parameters parameters;
        Float3 origin; float minimum;
        Float3 direction; float maximum;
    };
    struct Hit
    {
        Float3 position; float distance;
        float fieldDistance; uint32_t status, sampleCount, padding;
    };
    struct Constants { uint32_t rayCount, nodeCount, padding[2]; };
    static_assert(sizeof(Ray) == 96 && sizeof(Hit) == 32 && sizeof(Constants) == 16);

    ComPtr<IDxcBlob> Compile()
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create DXC utilities");
        Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create DXC compiler");
        const wchar_t* path = L"Shaders/VXGISDFThresholdValidate.hlsl";
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(path, nullptr, &source), "load threshold shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes), "create shader include handler");
        std::vector<const wchar_t*> arguments = { path, L"-E", L"CSMain", L"-T", L"cs_6_0",
            L"-HV", L"2021", L"-I", L"Shaders", L"-WX" };
#if defined(_DEBUG)
        arguments.insert(arguments.end(), { L"-Od", L"-Zi", L"-Qembed_debug" });
        const char* dumpPath = "tmp/VXGISDFThresholdValidate-Debug.ll";
#else
        arguments.push_back(L"-O3");
        const char* dumpPath = "tmp/VXGISDFThresholdValidate-O2.ll";
#endif
        DxcBuffer input = { source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8 };
        ComPtr<IDxcResult> result;
        Check(compiler->Compile(&input, arguments.data(), UINT(arguments.size()), includes.Get(), IID_PPV_ARGS(&result)), "compile threshold shader");
        ComPtr<IDxcBlobUtf8> diagnostics;
        Check(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr), "read DXC diagnostics");
        if (diagnostics && diagnostics->GetStringLength()) std::cerr << diagnostics->GetStringPointer();
        HRESULT status;
        Check(result->GetStatus(&status), "get DXC status");
        Check(status, "threshold shader compilation");
        ComPtr<IDxcBlob> bytecode;
        Check(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&bytecode), nullptr), "read threshold shader object");
        DxcBuffer disassemblyInput = { bytecode->GetBufferPointer(), bytecode->GetBufferSize(), 0 };
        ComPtr<IDxcResult> disassemblyResult;
        Check(compiler->Disassemble(&disassemblyInput, IID_PPV_ARGS(&disassemblyResult)), "disassemble threshold DXIL");
        ComPtr<IDxcBlobUtf8> disassembly;
        Check(disassemblyResult->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disassembly), nullptr), "read threshold DXIL disassembly");
        Require(disassembly != nullptr, "DXIL disassembly missing");
        const std::string text(disassembly->GetStringPointer(), disassembly->GetStringLength());
        std::ofstream(dumpPath, std::ios::binary) << text;
        Require(text.find("double") == std::string::npos && text.find(".f64") == std::string::npos,
            "threshold shader contains FP64 operations");
        ComPtr<IDxcBlob> reflectionBlob;
        Check(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflectionBlob), nullptr), "read DXIL reflection");
        DxcBuffer reflectionInput = { reflectionBlob->GetBufferPointer(), reflectionBlob->GetBufferSize(), 0 };
        ComPtr<ID3D12ShaderReflection> reflection;
        Check(utils->CreateReflection(&reflectionInput, IID_PPV_ARGS(&reflection)), "create threshold reflection");
        D3D12_SHADER_DESC description = {};
        Check(reflection->GetDesc(&description), "describe threshold resources");
        std::set<std::string> names;
        for (UINT index = 0; index < description.BoundResources; ++index)
        {
            D3D12_SHADER_INPUT_BIND_DESC binding = {};
            Check(reflection->GetResourceBindingDesc(index, &binding), "describe threshold resource");
            names.insert(binding.Name);
        }
        Require(names == std::set<std::string>{ "TestConstants", "nodes", "rays", "results" },
            "threshold shader unexpectedly binds legacy geometry or material resources");
        std::cout << "DXIL verified: FP32 only; constants + nodes + rays + results; " << dumpPath << '\n';
        return bytecode;
    }
    class GPU
    {
    public:
        GPU()
        {
            ComPtr<ID3D12Debug> debug;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "get D3D12 debug interface");
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;
            Check(debug.As(&validation), "get GPU validation interface");
            validation->SetEnableGPUBasedValidation(TRUE);
            ComPtr<IDXGIFactory6> factory;
            Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "create DXGI factory");
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index = 0; factory->EnumAdapterByGpuPreference(index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index)
            {
                DXGI_ADAPTER_DESC1 description = {};
                Check(adapter->GetDesc1(&description), "get adapter description");
                if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
                adapter.Reset();
            }
            Require(device != nullptr, "hardware D3D12 device required");
            DXGI_ADAPTER_DESC1 description = {};
            Check(adapter->GetDesc1(&description), "describe selected adapter");
            adapterName = UTF8(description.Description);
            Check(device.As(&info), "get D3D12 info queue");
            D3D12_COMMAND_QUEUE_DESC queueDescription = {};
            Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "create compute queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create allocator");
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "create command list");
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Require(event != nullptr, "create fence event");
            CD3DX12_ROOT_PARAMETER roots[4];
            roots[0].InitAsConstants(sizeof(Constants) / 4, 0);
            roots[1].InitAsShaderResourceView(0);
            roots[2].InitAsShaderResourceView(1);
            roots[3].InitAsUnorderedAccessView(0);
            CD3DX12_ROOT_SIGNATURE_DESC signature(4, roots);
            ComPtr<ID3DBlob> serialized;
            Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "serialize threshold root signature");
            Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "create threshold root signature");
            const auto shader = Compile();
            D3D12_COMPUTE_PIPELINE_STATE_DESC state = {};
            state.pRootSignature = root.Get();
            state.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            Check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)), "create threshold compute pipeline");
            std::cout << "Threshold GPU validation on " << adapterName << "; debug + GBV; no HWND / Present\n";
        }
        ~GPU() { if (event) CloseHandle(event); }
        std::vector<Hit> Dispatch(const std::vector<float>& nodes, const std::vector<Ray>& rays)
        {
            const auto nodeInput = Upload(nodes), rayInput = Upload(rays);
            const uint64_t bytes = rays.size() * sizeof(Hit);
            auto output = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Constants constants = { uint32_t(rays.size()), uint32_t(nodes.size()), {} };
            commands->SetComputeRootSignature(root.Get());
            commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            commands->SetComputeRootShaderResourceView(1, nodeInput->GetGPUVirtualAddress());
            commands->SetComputeRootShaderResourceView(2, rayInput->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress());
            Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->Dispatch((UINT(rays.size()) + 63) / 64, 1, 1);
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyResource(readback.Get(), output.Get());
            Check(commands->Close(), "close threshold dispatch");
            ID3D12CommandList* lists[] = { commands.Get() };
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), 1), "signal threshold fence");
            Check(fence->SetEventOnCompletion(1, event), "arm threshold fence");
            Require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "threshold GPU timeout");
            Check(device->GetDeviceRemovedReason(), "threshold device removed reason");
            std::vector<Hit> results(rays.size());
            void* mapped = nullptr;
            const D3D12_RANGE range = { 0, SIZE_T(bytes) };
            Check(readback->Map(0, &range, &mapped), "map threshold readback");
            std::memcpy(results.data(), mapped, size_t(bytes));
            const D3D12_RANGE writes = { 0, 0 };
            readback->Unmap(0, &writes);
            return results;
        }
        uint32_t Warnings()
        {
            uint32_t warnings = 0;
            for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0;
                Check(info->GetMessage(index, nullptr, &bytes), "measure D3D12 message");
                std::vector<char> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(info->GetMessage(index, message, &bytes), "read D3D12 message");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                {
                    std::cerr << message->pDescription << '\n'; ++warnings;
                }
            }
            return warnings;
        }
        std::string adapterName;
    private:
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes, D3D12_HEAP_TYPE type,
            D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(type);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                state, nullptr, IID_PPV_ARGS(&result)), "create threshold buffer");
            return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            Require(!values.empty(), "GPU input cannot be empty");
            auto resource = Buffer(values.size() * sizeof(T), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            const D3D12_RANGE range = { 0, 0 };
            Check(resource->Map(0, &range, &mapped), "map threshold upload");
            std::memcpy(mapped, values.data(), values.size() * sizeof(T));
            resource->Unmap(0, nullptr);
            return resource;
        }
        void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            commands->ResourceBarrier(1, &barrier);
        }
        HANDLE event = nullptr;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> info;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
    };
    struct Batch
    {
        std::vector<float> nodes = std::vector<float>(7, -1000);
        std::vector<Ray> rays;
        template<class Distance>
        Threshold::Grid Field(Float3 origin, float cellSize, std::array<uint32_t, 3> cells, Distance distance)
        {
            Threshold::Grid grid = {};
            grid.origin = origin; grid.cellSize = cellSize; grid.cellDimensions = cells;
            grid.boundsMaximum = { origin.x + float(cells[0]) * cellSize,
                origin.y + float(cells[1]) * cellSize, origin.z + float(cells[2]) * cellSize };
            grid.nodeOffset = uint32_t(nodes.size());
            for (uint32_t z = 0; z <= cells[2]; ++z)
                for (uint32_t y = 0; y <= cells[1]; ++y)
                    for (uint32_t x = 0; x <= cells[0]; ++x)
                        nodes.push_back(distance(Float3{ origin.x + float(x) * cellSize,
                            origin.y + float(y) * cellSize, origin.z + float(z) * cellSize }));
            return grid;
        }
        Threshold::Grid Baked(const VXGISDF& sdf)
        {
            const auto& source = sdf.GetGrid();
            Threshold::Grid grid = {};
            grid.origin = source.origin; grid.cellSize = source.cellSize;
            grid.cellDimensions = source.cellDimensions; grid.boundsMaximum = source.boundsMaximum;
            grid.nodeOffset = uint32_t(nodes.size());
            nodes.insert(nodes.end(), sdf.GetNodes().begin(), sdf.GetNodes().end());
            return grid;
        }
        void Add(Threshold::Grid grid, Float3 origin, Float3 direction, float minimum, float maximum,
            Threshold::Parameters parameters = {})
        {
            rays.push_back({ grid, parameters, origin, minimum, direction, maximum });
        }
    };
    struct Vertex { Float3 Position; };
    struct Mesh { std::vector<Vertex> vertices; std::vector<std::vector<uint32_t>> lodIndices = { {} }; };
    struct Model { std::vector<Mesh> meshes; };
    void Quad(Mesh& mesh, Float3 a, Float3 b, Float3 c, Float3 d)
    {
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), { { a }, { b }, { c }, { d } });
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(), { first, first + 1, first + 2, first, first + 2, first + 3 });
    }
    Model Box()
    {
        Mesh mesh;
        Quad(mesh, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 });
        Quad(mesh, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 });
        Quad(mesh, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 });
        Quad(mesh, { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 });
        Quad(mesh, { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 });
        Quad(mesh, { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 });
        return { { std::move(mesh) } };
    }
    Batch TestData()
    {
        Batch batch;
        const auto plane = batch.Field({ -2, -2, -4 }, .25f, { 16, 16, 32 }, [](Float3 p) { return std::abs(p.z - .125f); });
        std::mt19937 random(0x53444647u);
        std::uniform_real_distribution<float> coordinate(-.75f, .75f), slope(-.15f, .15f);
        for (uint32_t index = 0; index < 768; ++index)
        {
            const Float3 origin = { coordinate(random), coordinate(random), -3.5f };
            const Float3 direction = { slope(random), slope(random), index % 2 ? 7.f : 1.f };
            batch.Add(plane, origin, direction, 0, index % 5 ? 4.f : 2.f);
        }
        batch.Add(plane, { 0, 0, -1 }, { 0, 0, 1 }, 1, 1);
        batch.Add(plane, { 0, 0, -1 }, { 0, 0, 1 }, .875f, .875f);
        batch.Add(plane, { 5, 0, 0 }, { 1, 0, 0 }, 0, 1);
        batch.Add(plane, {}, {}, 0, 1);
        batch.Add(plane, {}, { 0, 0, 1 }, 2, 1);
        batch.Add(plane, { std::numeric_limits<float>::quiet_NaN(), 0, 0 }, { 0, 0, 1 }, 0, 1);
        auto invalidGrid = plane;
        invalidGrid.nodeOffset = UINT32_MAX;
        batch.Add(invalidGrid, {}, { 0, 0, 1 }, 0, 1);
        Threshold::Parameters parameters;
        parameters.maxSteps = 1;
        batch.Add(plane, { 0, 0, -3.5f }, { 0, 0, 1 }, 0, 4, parameters);
        parameters = {}; parameters.hitThresholdCells = .8f;
        batch.Add(plane, {}, { 0, 0, 1 }, 0, 1, parameters);
        const auto point = batch.Field({}, 1, { 1, 1, 1 }, [](Float3 p) { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); });
        batch.Add(point, { .5f, .5f, .5f }, { 0, 0, 1 }, 0, 0);
        const auto subcell = batch.Field({}, 1, { 1, 1, 1 }, [](Float3 p)
            { p.x -= .5f; p.y -= .5f; p.z -= .5f; return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); });
        batch.Add(subcell, { .5f, .5f, .5f }, { 0, 0, 1 }, 0, 0);
        const auto thin = batch.Field({}, 1, { 1, 1, 1 }, [](Float3 p) { return std::abs(p.z - .4f); });
        batch.Add(thin, { .5f, .5f, .4f }, { 0, 0, 1 }, 0, 0);
        const auto equal = batch.Field({}, 1, { 1, 1, 1 }, [](Float3) { return .9f; });
        batch.Add(equal, {}, { 1, 0, 0 }, 0, 0);
        const auto above = batch.Field({}, 1, { 1, 1, 1 }, [](Float3) { return std::nextafter(.9f, 1.f); });
        batch.Add(above, {}, { 1, 0, 0 }, 0, 0);
        const auto constant = batch.Field({}, 1, { 32, 1, 1 }, [](Float3) { return 2.f; });
        batch.Add(constant, { -100000000.f, .5f, .5f }, { 1, 0, 0 }, 100000000.f, 100000032.f);
        auto boxModel = Box();
        Mesh planeMesh;
        Quad(planeMesh, { -2, -2, .1375f }, { 2, -2, .1375f }, { 2, 2, .1375f }, { -2, 2, .1375f });
        Model planeModel = { { std::move(planeMesh) } };
        VXGIMeshSDFCache cache;
        const auto boxAsset = cache.GetOrCreate(boxModel, { .25f, 128 });
        const auto planeAsset = cache.GetOrCreate(planeModel, { .5f, 128 });
        Require(boxAsset && planeAsset, cache.GetLastError());
        const auto box = batch.Baked(boxAsset->GetSDF()), bakedPlane = batch.Baked(planeAsset->GetSDF());
        for (uint32_t index = 0; index < 512; ++index)
        {
            batch.Add(box, { coordinate(random), coordinate(random), -3 }, { slope(random), slope(random), 1 }, 0, 6);
            batch.Add(bakedPlane, { coordinate(random), coordinate(random), -2 }, { slope(random), slope(random), 1 }, 0, 4);
        }
        return batch;
    }
}
int main()
{
    try
    {
        const auto batch = TestData();
        GPU gpu;
        const auto results = gpu.Dispatch(batch.nodes, batch.rays);
        std::array<uint32_t, 5> statuses = {};
        float maximumError = 0;
        for (size_t index = 0; index < batch.rays.size(); ++index)
        {
            const auto& ray = batch.rays[index];
            const auto& actual = results[index];
            const auto expected = Threshold::TraceLocal(ray.grid, batch.nodes, ray.origin, ray.direction,
                ray.minimum, ray.maximum, ray.parameters);
            const std::string label = "threshold ray " + std::to_string(index);
            if (actual.status != uint32_t(expected.status))
                std::cerr << label << " status actual=" << actual.status << " expected=" << uint32_t(expected.status) <<
                    " distance=" << actual.distance << ',' << expected.distance << " samples=" << actual.sampleCount << ',' << expected.sampleCount << '\n';
            Require(actual.status == uint32_t(expected.status), label + " CPU/GPU status mismatch");
            Require(actual.status < statuses.size(), label + " invalid status");
            ++statuses[actual.status];
            Require(actual.sampleCount <= ray.parameters.maxSteps, label + " exceeded sample budget");
            if (expected.sampleCount)
            {
                const float error = (std::max)({ std::abs(actual.distance - expected.distance),
                    std::abs(actual.fieldDistance - expected.fieldDistance), std::abs(actual.position.x - expected.position.x),
                    std::abs(actual.position.y - expected.position.y), std::abs(actual.position.z - expected.position.z) });
                Require(std::isfinite(error) && error <= 6e-4f, label + " CPU/GPU distance/position error=" + std::to_string(error));
                maximumError = (std::max)(maximumError, error);
            }
            if (actual.status == uint32_t(Threshold::Status::Hit))
            {
                Require(actual.distance >= ray.minimum && actual.distance <= ray.maximum, label + " left original finite interval");
                Require(actual.fieldDistance <= ray.parameters.hitThresholdCells * ray.grid.cellSize, label + " ignored distance threshold");
            }
        }
        const auto warnings = gpu.Warnings();
        Require(warnings == 0, "D3D12 debug / GPU validation warnings");
        std::cout << "VXGISDFThreshold GPU: " << batch.rays.size() << " CPU/GPU queries passed; " <<
            "miss=" << statuses[0] << " hit=" << statuses[1] << " invalid=" << statuses[2] <<
            " exhausted=" << statuses[3] << " stalled=" << statuses[4] <<
            "; maximum error=" << std::setprecision(8) << maximumError << "; warnings=" << warnings << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VXGISDFThreshold GPU: " << error.what() << '\n';
        return 1;
    }
}
