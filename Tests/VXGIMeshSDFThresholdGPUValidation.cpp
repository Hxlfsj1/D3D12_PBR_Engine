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
#include "../Resources/VXGIMeshSDFThreshold.h"
#include "../Resources/VXGIMeshSDFGPUData.h"
#include "../Resources/VXGIProbeState.h"
#include "../Resources/VXGIProbeTopology.h"
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
    using Scene = VXGIMeshSDFScene;
    using Query = VXGIMeshSDFThreshold;
    using Pack = VXGIMeshSDFGPUData;
    using Probe = VXGIProbeState::GPURecord;
    using Lookup = VXGIProbeTopology::LookupRecord;
    struct HybridParameters
    {
        Float3 gridOrigin; float cellSize;
        std::array<uint32_t,3> cellDimensions; uint32_t triangleCount;
        Float3 boundsMaximum; uint32_t materialCount;
        Float3 reservedLighting0; float materialMipLevel;
        Float3 reservedLighting1; float reservedLighting2;
        float reservedWorldRange, voxelSize; uint32_t probeCount, worldMode;
        uint32_t nodeCount, assetCount, instanceCount, bvhNodeCount;
    };
    struct Ray
    {
        Float3 origin = {}; float minimum = 0;
        Float3 direction = {0,0,1}; float maximum = 20;
        Threshold::Parameters parameters;
        uint32_t operation = 0, padding[3] = {};
    };
    struct HybridResult
    {
        Float3 radiance; uint32_t hit;
        float distance; uint32_t status,backend,worldTraceStatus;
        float confidence; uint32_t anchorStatus,triangleIndex,probeHasSupport;
        float probeSupportWeight; uint32_t probeAcceptedCount,probeTraceFailures,sourceValid;
    };
    struct Hit
    {
        Float3 position; float distance;
        float fieldDistance; uint32_t status, instanceIndex, instanceId;
        uint32_t sampleCount, candidateCount, padding[2];
        HybridResult hybrid;
    };
    struct Constants { HybridParameters hybrid; uint32_t rayCount, padding[3]; };
    static_assert(sizeof(HybridParameters)==112 && sizeof(Ray)==64 && sizeof(Hit)==112 && sizeof(Constants)==128);

    ComPtr<IDxcBlob> Compile(bool production = false)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create DXC utilities");
        Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create DXC compiler");
        const wchar_t* path = production ? L"Shaders/Shaders_For_SSGI.hlsl" : L"Shaders/VXGIMeshSDFThresholdValidate.hlsl";
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(path, nullptr, &source), "load threshold shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes), "create shader include handler");
        std::vector<const wchar_t*> arguments = { path, L"-E", production ? L"CSMain_Trace" : L"CSMain", L"-T", L"cs_6_6",
            L"-HV", L"2021", L"-I", L"Shaders", L"-WX", L"-D", L"VXGI_SSGI_THRESHOLD=1" };
        if (production) arguments.insert(arguments.end(), {L"-D", L"SSGI_PASS_TRACE=1"});
#if defined(_DEBUG)
        arguments.insert(arguments.end(), { L"-Od", L"-Zi", L"-Qembed_debug" });
        const char* suffix = "Debug";
#else
        arguments.push_back(L"-O3");
        const char* suffix = "O2";
#endif
        const std::string dumpPath = std::string("tmp/") + (production ? "SSGIThreshold-production-" : "VXGIMeshSDFThresholdValidate-") + suffix + ".ll";
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
        for (const std::string& name : names)
            Require(name.find("Triangle") == std::string::npos && name.find("triangle") == std::string::npos &&
                name.find("Offset") == std::string::npos && name.find("offset") == std::string::npos &&
                name.find("reference") == std::string::npos && name.find("Material") == std::string::npos && name.find("material") == std::string::npos,
                "threshold shader binds an exact geometry/material resource: " + name);
        Require(names.contains("VXGIMeshSDFAssets") && names.contains("VXGIMeshSDFInstances") && names.contains("VXGIMeshSDFBVH"),
            "threshold shader did not consume instance BVH and Mesh SDF metadata");
        if (!production) Require(names == std::set<std::string>{"TestConstants","nodes","rays","lookup","raw","results",
            "VXGIMeshSDFAssets","VXGIMeshSDFInstances","VXGIMeshSDFBVH"}, "unexpected threshold validation resources");
        if (production)
        {
            Require(names == std::set<std::string>{"SSGITraceConstants","hybridNodes","hybridLookup","hybridRawRecords",
                "VXGIMeshSDFAssets","VXGIMeshSDFInstances","VXGIMeshSDFBVH"}, "unexpected production SSGI fixed resources");
            D3D12_SHADER_BUFFER_DESC buffer = {};
            Check(reflection->GetConstantBufferByName("SSGITraceConstants")->GetDesc(&buffer),"describe production trace constants");
            Require(buffer.Size==624,"production trace constant buffer is not 624 bytes");
            for (UINT index=0;index<description.BoundResources;++index)
            {
                D3D12_SHADER_INPUT_BIND_DESC binding = {};
                Check(reflection->GetResourceBindingDesc(index,&binding),"describe production binding");
                const std::string name=binding.Name;
                const UINT expectedSpace=name.starts_with("VXGIMeshSDF")?1:0;
                const UINT expectedRegister=name=="hybridLookup"?6:name=="hybridRawRecords"?7:
                    name=="VXGIMeshSDFInstances"?1:name=="VXGIMeshSDFBVH"?3:0;
                Require(binding.Space==expectedSpace && binding.BindPoint==expectedRegister,
                    "production fixed register differs: "+name);
            }
        }
        std::cout << (production ? "Production SSGI" : "Integrated threshold") << " DXIL verified FP32, no triangles/CSR/materials; resources=" << names.size() << "; " << dumpPath << '\n';
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
            CD3DX12_ROOT_PARAMETER roots[9];
            roots[0].InitAsConstants(sizeof(Constants) / 4, 0);
            roots[1].InitAsShaderResourceView(0);
            roots[2].InitAsShaderResourceView(1);
            roots[3].InitAsShaderResourceView(2);
            roots[4].InitAsShaderResourceView(3);
            roots[5].InitAsShaderResourceView(0,1);
            roots[6].InitAsShaderResourceView(1,1);
            roots[7].InitAsShaderResourceView(3,1);
            roots[8].InitAsUnorderedAccessView(0);
            CD3DX12_ROOT_SIGNATURE_DESC signature(9, roots);
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
        std::vector<Hit> Dispatch(const Scene& scene, const std::vector<Ray>& rays,
            const std::vector<Lookup>& lookup = {}, const std::vector<Probe>& raw = {}, uint32_t probeCount = UINT32_MAX)
        {
            const auto owners = Pack::CollectAssets(scene);
            const auto geometry = Pack::BuildAssets(owners);
            const auto instances = Pack::BuildInstances(scene,owners);
            const auto& bvh = scene.GetInstanceBVH().GetNodes();
            const std::array<ComPtr<ID3D12Resource>,7> input = {Upload(geometry.nodes),Upload(rays),Upload(lookup),Upload(raw),
                Upload(geometry.assets),Upload(instances.instances),Upload(bvh)};
            const uint64_t bytes = rays.size() * sizeof(Hit);
            auto output = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Constants constants = {};
            auto& h = constants.hybrid;
            h.gridOrigin={-32,-32,-32};h.cellSize=.25f;h.cellDimensions={256,256,256};h.boundsMaximum={32,32,32};
            h.voxelSize=1;h.probeCount=probeCount==UINT32_MAX ? uint32_t(raw.size()) : probeCount;h.worldMode=2;
            h.nodeCount=uint32_t(geometry.nodes.size());h.assetCount=uint32_t(geometry.assets.size());
            h.instanceCount=uint32_t(instances.instances.size());h.bvhNodeCount=uint32_t(bvh.size());
            constants.rayCount=uint32_t(rays.size());
            commands->SetComputeRootSignature(root.Get());
            commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            for (UINT index=0;index<input.size();++index) commands->SetComputeRootShaderResourceView(index+1,input[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(8, output->GetGPUVirtualAddress());
            Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->Dispatch((UINT(rays.size()) + 63) / 64, 1, 1);
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyResource(readback.Get(), output.Get());
            Check(commands->Close(), "close threshold dispatch");
            ID3D12CommandList* lists[] = { commands.Get() };
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++fenceValue), "signal threshold fence");
            Check(fence->SetEventOnCompletion(fenceValue, event), "arm threshold fence");
            Require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "threshold GPU timeout");
            Check(device->GetDeviceRemovedReason(), "threshold device removed reason");
            std::vector<Hit> results(rays.size());
            void* mapped = nullptr;
            const D3D12_RANGE range = { 0, SIZE_T(bytes) };
            Check(readback->Map(0, &range, &mapped), "map threshold readback");
            std::memcpy(results.data(), mapped, size_t(bytes));
            const D3D12_RANGE writes = { 0, 0 };
            readback->Unmap(0, &writes);
            Check(allocator->Reset(),"reset threshold allocator");
            Check(commands->Reset(allocator.Get(),nullptr),"reset threshold command list");
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
            const auto description = CD3DX12_RESOURCE_DESC::Buffer((std::max)(uint64_t(16),bytes), flags);
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                state, nullptr, IID_PPV_ARGS(&result)), "create threshold buffer");
            return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            auto resource = Buffer(values.size() * sizeof(T), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            const D3D12_RANGE range = { 0, 0 };
            Check(resource->Map(0, &range, &mapped), "map threshold upload");
            if (!values.empty()) std::memcpy(mapped, values.data(), values.size() * sizeof(T));
            else std::memset(mapped,0,16);
            resource->Unmap(0, nullptr);
            return resource;
        }
        void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            commands->ResourceBarrier(1, &barrier);
        }
        UINT64 fenceValue = 0;
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
    struct Vertex { Float3 Position; };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<uint32_t>> lodIndices = {{}};
        uint32_t materialID = 0;
    };
    struct Model { std::vector<Mesh> meshes; };
    struct Instance
    {
        uint32_t editorId = 0; Model* pModel = nullptr;
        Float3 translation = {}, rotation = {}, scale = {1,1,1};
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false;
    };
    void Quad(Mesh& mesh,Float3 a,Float3 b,Float3 c,Float3 d)
    {
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{a},{b},{c},{d}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    Model Plane(float z=0)
    {
        Mesh mesh; Quad(mesh,{-1,-1,z},{1,-1,z},{1,1,z},{-1,1,z});
        return {{std::move(mesh)}};
    }
    Model Hollow()
    {
        Mesh mesh;
        Quad(mesh,{-6,-6,0},{-4,-6,0},{-4,6,0},{-6,6,0});
        Quad(mesh,{4,-6,0},{6,-6,0},{6,6,0},{4,6,0});
        Quad(mesh,{-4,-6,0},{4,-6,0},{4,-4,0},{-4,-4,0});
        Quad(mesh,{-4,4,0},{4,4,0},{4,6,0},{-4,6,0});
        return {{std::move(mesh)}};
    }
    Instance Make(Model& model,uint32_t id,Float3 translation={},Float3 scale={1,1,1},Float3 rotation={})
    {
        Instance result;result.pModel=&model;result.editorId=id;result.translation=translation;
        result.scale=scale;result.rotation=rotation;return result;
    }
    void Update(Scene& scene,VXGIMeshSDFCache& cache,const std::vector<Instance>& instances)
    {
        Require(scene.UpdateInstances(instances,cache,{.25f,128}),scene.GetLastError());
    }
    Float3 Transform(Float3 value,const DirectX::XMFLOAT4X4& matrix,bool normal=false)
    {
        Float3 result;const auto v=DirectX::XMLoadFloat3(&value);const auto m=DirectX::XMLoadFloat4x4(&matrix);
        DirectX::XMStoreFloat3(&result,normal?DirectX::XMVector3TransformNormal(v,m):DirectX::XMVector3TransformCoord(v,m));return result;
    }
    float Difference(Float3 a,Float3 b)
    {
        return (std::max)({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.z-b.z)});
    }
    uint32_t compared = 0, fallbackCompared = 0;
    float maximumError = 0;
    std::array<uint32_t,5> statuses = {};
    std::vector<Hit> Compare(GPU& gpu,const Scene& scene,const std::vector<Ray>& rays)
    {
        const auto results=gpu.Dispatch(scene,rays);
        for (size_t index=0;index<rays.size();++index)
        {
            const auto& ray=rays[index];const auto& actual=results[index];Query::Stats stats;
            const auto expected=Query::Trace(scene,ray.origin,ray.direction,ray.minimum,ray.maximum,ray.parameters,&stats);
            const std::string label="integrated ray "+std::to_string(compared);
            if (actual.status!=uint32_t(expected.status)) std::cerr<<label<<" status actual="<<actual.status<<" expected="<<uint32_t(expected.status)<<" instance="<<actual.instanceId<<','<<expected.instanceId<<" distance="<<actual.distance<<','<<expected.distance<<'\n';
            Require(actual.status==uint32_t(expected.status),label+" status differs from CPU reference");
            Require(actual.status<statuses.size(),label+" unknown status");++statuses[actual.status];++compared;
            Require(actual.candidateCount==stats.candidates && actual.candidateCount<=2,label+" violated top-two AABB budget");
            Require(actual.sampleCount<=2*ray.parameters.maxSteps,label+" exceeded two local march budgets");
            if (expected.status!=Query::Status::Hit) continue;
            Require(actual.instanceId==expected.instanceId && actual.instanceIndex==expected.instanceIndex,label+" selected wrong world surface");
            const float error=(std::max)({std::abs(actual.distance-expected.distance),std::abs(actual.fieldDistance-expected.fieldDistance),Difference(actual.position,expected.position)});
            Require(std::isfinite(error) && error<=1e-3f,label+" position/distance error="+std::to_string(error));
            maximumError=(std::max)(maximumError,error);
            Require(actual.distance>=ray.minimum && actual.distance<=ray.maximum,label+" extended original finite ray");
        }
        return results;
    }
    void QueryTests(GPU& gpu)
    {
        auto frame=Hollow(),plane=Plane(),offsetPlane=Plane(.1375f);
        VXGIMeshSDFCache cache;Scene scene;
        std::vector<Instance> instances={Make(frame,10,{0,0,2}),Make(frame,20,{0,0,4}),Make(plane,30,{0,0,6})};
        Update(scene,cache,instances);
        auto results=Compare(gpu,scene,{{{},0,{0,0,7},10}});
        Require(results[0].status==0 && results[0].candidateCount==2,"top-two holes must omit third solid model");
        instances.erase(instances.begin()+1);Update(scene,cache,instances);
        results=Compare(gpu,scene,{{{},0,{0,0,7},10},{ {},0,{0,0,1},5.99f},{ {},6,{0,0,1},6},
            {{},6.001f,{0,0,1},10},{{20,0,0},0,{0,0,1},10},{{},0,{},10},{{},2,{0,0,1},1}});
        Require(results[0].status==1 && results[0].instanceId==30,"second model was not queried after first field miss");
        Require(results[1].status==0 && results[2].status==1 && results[3].status==0,"finite endpoint/AABB clipping changed");
        Require(results[4].candidateCount==0 && results[4].sampleCount==0,"AABB miss sampled local field");
        std::mt19937 random(0x544f5032u);std::uniform_real_distribution<float> xy(-.6f,.6f),slope(-.1f,.1f);
        const std::array<Float3,3> scales={{{1,1,1},{2,.5f,3},{-2,.5f,-3}}};
        for (const auto scale:scales)
        {
            Update(scene,cache,{Make(offsetPlane,51,{2,-3,4},scale,{.3f,-.6f,.2f})});
            const auto& instance=scene.GetInstances()[0];std::vector<Ray> rays;
            for (uint32_t index=0;index<512;++index)
            {
                Ray ray;ray.origin=Transform({xy(random),xy(random),-2},instance.worldFromLocal);
                ray.direction=Transform({slope(random),slope(random),index%2?7.f:1.f},instance.worldFromLocal,true);
                ray.maximum=index%5?20.f:.25f;rays.push_back(ray);
            }
            Compare(gpu,scene,rays);
        }
        Update(scene,cache,{Make(plane,1,{0,0,6},{1,1,.2f}),Make(plane,2,{0,0,4},{1,1,4})});
        results=Compare(gpu,scene,{{{},0,{0,0,1},10}});
        Require(results[0].instanceId==2,"local distances were compared across nonuniform scales");
        Mesh delayedMesh;Quad(delayedMesh,{5,-1,1},{6,-1,1},{6,1,1},{5,1,1});
        Quad(delayedMesh,{-1,-1,8},{1,-1,8},{1,1,8},{-1,1,8});Model delayed={{std::move(delayedMesh)}};
        Update(scene,cache,{Make(delayed,10),Make(plane,20,{0,0,4})});
        Ray cap;cap.maximum=10;cap.parameters.maxSteps=1;
        results=Compare(gpu,scene,{{{},0,{0,0,1},10},cap});
        Require(results[0].instanceId==20 && results[1].status==3 && results[1].instanceIndex==UINT32_MAX,
            "nearest-world / unfinished-nearer-candidate status changed");
        Update(scene,cache,{});
        results=Compare(gpu,scene,{{{},0,{0,0,1},10},{{},0,{},10}});
        Require(results[0].status==0 && results[1].status==2,"empty valid/invalid ray statuses changed");
        std::cout<<"Integrated query: top-two omission, finite endpoints, 1536 transformed rays, nearest-world and incomplete/empty status passed\n";
    }
    Probe Constant(Float3 value)
    {
        Probe result;result.sh[0]={value.x/.28209479177387814f,value.y/.28209479177387814f,value.z/.28209479177387814f,0};
        result.state=VXGIProbeState::Valid;result.updateCount=3;result.sampleCount=24;return result;
    }
    void FallbackTests(GPU& gpu)
    {
        auto plane=Plane();VXGIMeshSDFCache cache;Scene scene;Update(scene,cache,{Make(plane,7)});
        std::vector<Lookup> lookup;std::vector<Probe> raw;
        for (int x=-1;x<=0;++x) for (int y=-1;y<=0;++y) for (int z=-1;z<=0;++z)
        {lookup.push_back({{x,y,z},uint32_t(raw.size())});raw.push_back(Constant({.2f,.4f,.6f}));}
        Ray ray;ray.origin={0,0,-1};ray.direction={0,0,9};ray.maximum=2;ray.operation=1;
        const auto CheckFallback=[&](const HybridResult& hit,bool support,Float3 expected,const char* label)
        {
            Require(hit.hit==1 && hit.backend==2 && hit.worldTraceStatus==1 && hit.confidence==1 && hit.triangleIndex==UINT32_MAX,
                std::string(label)+" lost approximate geometric termination");
            Require(hit.probeHasSupport==uint32_t(support) && hit.sourceValid==uint32_t(support) && hit.probeTraceFailures==0,
                std::string(label)+" probe source validity changed");
            Require(Difference(hit.radiance,expected)<4e-5f,std::string(label)+" cached radiance differs");++fallbackCompared;
        };
        auto hit=gpu.Dispatch(scene,{ray},lookup,raw)[0].hybrid;
        CheckFallback(hit,true,{.2f,.4f,.6f},"normal cache");
        Require(hit.probeAcceptedCount==8 && std::abs(hit.distance-1)<1e-3f,"fallback did not sample existing eight probes at world hit");
        for (auto& record:raw) record=Constant({});
        CheckFallback(gpu.Dispatch(scene,{ray},lookup,raw)[0].hybrid,true,{},"black cache");
        for (auto& record:raw) record.state=VXGIProbeState::Disabled;
        CheckFallback(gpu.Dispatch(scene,{ray},lookup,raw)[0].hybrid,false,{},"disabled cache");
        CheckFallback(gpu.Dispatch(scene,{ray},{},{},0)[0].hybrid,false,{},"unavailable cache");
        ray.maximum=.99f;hit=gpu.Dispatch(scene,{ray},lookup,raw)[0].hybrid;
        Require(hit.hit==0 && hit.worldTraceStatus==0 && hit.backend==2 && Difference(hit.radiance,{})==0,"fallback extended original finite SSGI ray");++fallbackCompared;
        ray.maximum=2;ray.direction={};hit=gpu.Dispatch(scene,{ray},lookup,raw)[0].hybrid;
        Require(hit.hit==0 && hit.worldTraceStatus==2,"fallback invalid direction was accepted");++fallbackCompared;
        std::cout<<"Production SSGIWorldRaySource: cached RGB, valid black, disabled/missing support termination, finite ray and invalid direction passed\n";
    }
}
int main(int argc,char** argv)
{
    try
    {
        if (argc>1 && std::string(argv[1])=="--audit")
        {Compile();Compile(true);std::cout<<"Exact production defines audited without creating a GPU device\n";return 0;}
        GPU gpu;QueryTests(gpu);FallbackTests(gpu);Compile(true);
        const auto warnings=gpu.Warnings();Require(warnings==0,"D3D12 debug / GPU validation warnings");
        std::cout<<"VXGIMeshSDFThreshold GPU: "<<compared<<" CPU/GPU scene queries; "<<fallbackCompared
            <<" production fallback cases; miss="<<statuses[0]<<" hit="<<statuses[1]<<" invalid="<<statuses[2]<<" exhausted="<<statuses[3]
            <<" stalled="<<statuses[4]<<"; maximum error="<<std::setprecision(8)<<maximumError<<"; warnings="<<warnings<<'\n';
        return 0;
    }
    catch (const std::exception& error)
    {std::cerr<<"VXGIMeshSDFThreshold GPU: "<<error.what()<<'\n';return 1;}
}
