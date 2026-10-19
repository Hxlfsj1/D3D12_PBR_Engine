// Production Mesh SDF tracing against an independent full-triangle CPU query.
// This fixture creates no window, swap chain, or Present call.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <d3dx12.h>
#include "../Resources/VXGIMeshSDFGPUData.h"
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
    struct Vertex
    {
        XMFLOAT3 Position, Normal = {0, 0, 1};
        XMFLOAT2 TexCoords = {};
        XMFLOAT3 Tangent = {1, 0, 0}, Bitangent = {0, 1, 0};
    };
    struct TextureResource { void* Get() const { return nullptr; } };
    struct Texture { uint32_t type = 0; TextureResource Resource; std::string path; };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<uint32_t>> lodIndices = {{}};
        uint32_t materialID = 7;
        bool isUnlit = false;
        XMFLOAT4 baseColorFactor = {1, 1, 1, 1};
        XMFLOAT3 emissiveFactor = {};
        std::vector<Texture> textures;
    };
    struct Model { std::vector<Mesh> meshes; };
    struct SourceInstance
    {
        uint32_t editorId = 0;
        Model* pModel = nullptr;
        XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false;
    };
    using Scene = VXGIMeshSDFScene;
    using Pack = VXGIMeshSDFGPUData;
    using Material = VXGISurface::MaterialRecord;
    constexpr VXGIMeshSDFCache::BuildSettings Settings = {.5f, 128};

    void AddPlane(Mesh& mesh, float z)
    {
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), {
            {{-.4f, -.4f, z}, {0, 0, 1}, {0, 0}},
            {{ .4f, -.4f, z}, {0, 0, 1}, {1, 0}},
            {{ .4f,  .4f, z}, {0, 0, 1}, {1, 1}},
            {{-.4f,  .4f, z}, {0, 0, 1}, {0, 1}}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),
            {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    Model Planes(std::initializer_list<float> layers)
    {
        Mesh mesh;
        for (const float z : layers) AddPlane(mesh, z);
        return {{std::move(mesh)}};
    }
    SourceInstance Instance(Model& model, uint32_t id, XMFLOAT3 translation = {},
        XMFLOAT3 scale = {1, 1, 1}, XMFLOAT3 rotation = {})
    {
        SourceInstance result;
        result.editorId = id; result.pModel = &model; result.translation = translation;
        result.scale = scale; result.rotation = rotation;
        return result;
    }
    void Update(Scene& scene, VXGIMeshSDFCache& cache, const std::vector<SourceInstance>& sources)
    {
        Require(scene.UpdateInstances(sources, cache, Settings), scene.GetLastError());
    }
    XMFLOAT3 Transform(XMFLOAT3 point, const DirectX::XMFLOAT4X4& matrix, bool direction = false)
    {
        XMFLOAT3 result;
        const auto value = DirectX::XMLoadFloat3(&point);
        const auto transform = DirectX::XMLoadFloat4x4(&matrix);
        DirectX::XMStoreFloat3(&result, direction ? DirectX::XMVector3TransformNormal(value, transform) :
            DirectX::XMVector3TransformCoord(value, transform));
        return result;
    }
    XMFLOAT3 Normalize(XMFLOAT3 value)
    {
        XMFLOAT3 result;
        DirectX::XMStoreFloat3(&result, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&value)));
        return result;
    }
    struct Parameters { uint32_t rayCount, materialCount, geometryOnly = 0, padding = 0; };
    struct Ray
    {
        XMFLOAT3 origin = {.123f, -.187f, -1}; float minimum = 0;
        XMFLOAT3 direction = {0, 0, 1}; float maximum = 20;
    };
    struct Hit
    {
        XMFLOAT3 position; float distance;
        XMFLOAT3 barycentric; uint32_t triangleIndex;
        XMFLOAT3 geometricNormal; uint32_t triangleId;
        XMFLOAT3 shadingNormal; uint32_t materialId;
        uint32_t instanceId, instanceIndex, backFace, valid;
        uint32_t status, isCutout, reserved[2];
    };
    static_assert(sizeof(Parameters) == 16 && sizeof(Ray) == 32 && sizeof(Hit) == 96);

    ComPtr<IDxcBlob> Compile()
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create DXC utilities");
        Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create DXC compiler");
        const wchar_t* path = L"Shaders/VXGIMeshSDFValidate.hlsl";
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(path, nullptr, &source), "load Mesh SDF validation shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes), "create shader includes");
        std::vector<const wchar_t*> arguments = {path, L"-E", L"CSMain", L"-T", L"cs_6_6",
            L"-HV", L"2021", L"-I", L"Shaders", L"-WX"};
#if defined(_DEBUG)
        arguments.insert(arguments.end(), {L"-Od", L"-Zi", L"-Qembed_debug"});
#else
        arguments.push_back(L"-O3");
#endif
        DxcBuffer input = {source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        Check(compiler->Compile(&input, arguments.data(), UINT(arguments.size()), includes.Get(), IID_PPV_ARGS(&result)), "compile Mesh SDF shader");
        ComPtr<IDxcBlobUtf8> diagnostics;
        Check(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr), "get shader diagnostics");
        if (diagnostics && diagnostics->GetStringLength()) std::cerr << diagnostics->GetStringPointer();
        HRESULT status;
        Check(result->GetStatus(&status), "get shader status");
        Check(status, "Mesh SDF shader status");
        ComPtr<IDxcBlob> bytecode;
        Check(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&bytecode), nullptr), "get shader object");
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
            D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
            Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "check FP64 support");
            Require(options.DoublePrecisionFloatShaderOps, "Mesh SDF exact world candidate refinement requires FP64 shader operations");
            DXGI_ADAPTER_DESC1 description = {};
            Check(adapter->GetDesc1(&description), "describe selected adapter");
            adapterName = UTF8(description.Description);
            Check(device.As(&info), "get D3D12 info queue");
            D3D12_COMMAND_QUEUE_DESC queueDescription = {};
            Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "create command queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create command allocator");
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "create command list");
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Require(event != nullptr, "create fence event");
            CD3DX12_ROOT_PARAMETER roots[11];
            roots[0].InitAsConstants(sizeof(Parameters) / 4, 0);
            for (UINT index = 0; index < 6; ++index) roots[index + 1].InitAsShaderResourceView(index);
            for (UINT index = 0; index < 3; ++index) roots[index + 7].InitAsShaderResourceView(index, 1);
            roots[10].InitAsUnorderedAccessView(0);
            CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
            CD3DX12_ROOT_SIGNATURE_DESC signature(11, roots, 1, &sampler,
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
            ComPtr<ID3DBlob> serialized;
            Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "serialize Mesh SDF root signature");
            Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "create Mesh SDF root signature");
            const auto shader = Compile();
            D3D12_COMPUTE_PIPELINE_STATE_DESC state = {};
            state.pRootSignature = root.Get();
            state.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)), "create Mesh SDF pipeline");
            std::cout << "Mesh SDF GPU validation on " << adapterName << "; debug + GBV; no HWND / Present\n";
        }
        ~GPU() { if (event) CloseHandle(event); }
        std::vector<Hit> Dispatch(const Scene& scene, const std::vector<Ray>& rays,
            const std::vector<Material>& materials, bool accelerated = true, bool geometryOnly = false)
        {
            const auto assets = Pack::CollectAssets(scene);
            const auto geometry = Pack::BuildAssets(assets, accelerated);
            const auto instances = Pack::BuildInstances(scene, assets);
            Require(!rays.empty(), "empty GPU ray batch");
            UploadTextures();
            const std::array<ComPtr<ID3D12Resource>, 9> inputs = {
                Upload(geometry.nodes), Upload(geometry.cellOffsets), Upload(geometry.references),
                Upload(geometry.triangles), Upload(materials), Upload(rays),
                Upload(geometry.assets), Upload(instances.instances), Upload(instances.materialIds)};
            const uint64_t bytes = rays.size() * sizeof(Hit);
            auto output = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Parameters parameters = {uint32_t(rays.size()), uint32_t(materials.size())};
            parameters.geometryOnly = uint32_t(geometryOnly);
            ID3D12DescriptorHeap* heaps[] = {textureHeap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(root.Get());
            commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0, sizeof(parameters) / 4, &parameters, 0);
            for (UINT index = 0; index < inputs.size(); ++index)
                commands->SetComputeRootShaderResourceView(index + 1, inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(10, output->GetGPUVirtualAddress());
            Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->Dispatch((UINT(rays.size()) + 63) / 64, 1, 1);
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyResource(readback.Get(), output.Get());
            Check(commands->Close(), "close Mesh SDF dispatch");
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++fenceValue), "signal Mesh SDF fence");
            Check(fence->SetEventOnCompletion(fenceValue, event), "arm Mesh SDF fence");
            Require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "Mesh SDF GPU timeout");
            Check(device->GetDeviceRemovedReason(), "Mesh SDF device removed reason");
            std::vector<Hit> results(rays.size());
            void* mapped = nullptr;
            const D3D12_RANGE range = {0, SIZE_T(bytes)};
            Check(readback->Map(0, &range, &mapped), "map Mesh SDF readback");
            std::memcpy(results.data(), mapped, size_t(bytes));
            const D3D12_RANGE writes = {0, 0};
            readback->Unmap(0, &writes);
            Check(allocator->Reset(), "reset Mesh SDF allocator");
            Check(commands->Reset(allocator.Get(), nullptr), "reset Mesh SDF commands");
            textures.clear(); textureUploads.clear();
            ++submissions;
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
        uint32_t submissions = 0;
    private:
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes, D3D12_HEAP_TYPE type,
            D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(type);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                state, nullptr, IID_PPV_ARGS(&result)), "create Mesh SDF buffer");
            return result;
        }
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values)
        {
            Require(!values.empty(), "GPU input buffer cannot be empty");
            auto resource = Buffer(values.size() * sizeof(T), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            const D3D12_RANGE range = {0, 0};
            Check(resource->Map(0, &range, &mapped), "map Mesh SDF upload");
            std::memcpy(mapped, values.data(), values.size() * sizeof(T));
            resource->Unmap(0, nullptr);
            return resource;
        }
        void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            commands->ResourceBarrier(1, &barrier);
        }
        void UploadTextures()
        {
            D3D12_DESCRIPTOR_HEAP_DESC description = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            Check(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&textureHeap)), "create alpha heap");
            const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const std::array<XMFLOAT4, 2> pixels = {XMFLOAT4{1, 1, 1, 0}, XMFLOAT4{1, 1, 1, .5f}};
            for (UINT index = 0; index < pixels.size(); ++index)
            {
                const auto textureDescription = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                ComPtr<ID3D12Resource> texture;
                Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &textureDescription,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "create alpha texture");
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
                UINT64 bytes = 0;
                device->GetCopyableFootprints(&textureDescription, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
                auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                void* mapped = nullptr;
                Check(upload->Map(0, nullptr, &mapped), "map alpha texture");
                std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset, &pixels[index], sizeof(XMFLOAT4));
                upload->Unmap(0, nullptr);
                Transition(texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint), destination(texture.Get(), 0);
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
                view.Format = textureDescription.Format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
                const CD3DX12_CPU_DESCRIPTOR_HANDLE descriptor(textureHeap->GetCPUDescriptorHandleForHeapStart(), INT(index), increment);
                device->CreateShaderResourceView(texture.Get(), &view, descriptor);
                textures.push_back(texture); textureUploads.push_back(upload);
            }
        }
        HANDLE event = nullptr;
        uint64_t fenceValue = 0;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> info;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12DescriptorHeap> textureHeap;
        std::vector<ComPtr<ID3D12Resource>> textures, textureUploads;
    };

    std::vector<Material> Materials()
    {
        std::vector<Material> result(16);
        for (auto& material : result) material.albedoIdx = 1;
        result[3].albedoIdx = 0;
        result[5].albedoIdx = 0; // Opaque instances must ignore this hole texture.
        return result;
    }
    float ComponentError(XMFLOAT3 a, XMFLOAT3 b)
    {
        return (std::max)({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)});
    }
    struct Group
    {
        std::string name;
        uint32_t rays = 0, hits = 0, misses = 0;
        float maximumError = 0;
    };
    Group Compare(GPU& gpu, const std::string& name, const Scene& scene, const std::vector<Ray>& rays,
        const Scene* coverageReference = nullptr, bool accelerated = true, bool geometryOnly = false)
    {
        const auto results = gpu.Dispatch(scene, rays, Materials(), accelerated, geometryOnly);
        const Scene& reference = coverageReference ? *coverageReference : scene;
        Group group; group.name = name; group.rays = uint32_t(rays.size());
        for (size_t index = 0; index < rays.size(); ++index)
        {
            const auto& ray = rays[index]; const auto& result = results[index];
            const auto expected = reference.TraceGeometry(ray.origin, ray.direction, ray.minimum, ray.maximum);
            const std::string label = name + " ray " + std::to_string(index);
            if ((result.valid != 0) != expected.hit)
                std::cerr << std::setprecision(10) << label << " origin=" << ray.origin.x << ',' << ray.origin.y << ',' << ray.origin.z
                    << " direction=" << ray.direction.x << ',' << ray.direction.y << ',' << ray.direction.z
                    << " interval=" << ray.minimum << ',' << ray.maximum << " expected=" << expected.hit << ',' << expected.instanceId
                    << ',' << expected.distance << " actual=" << result.valid << ',' << result.status << ',' << result.instanceIndex << '\n';
            Require(result.status <= 1, label + " invalid or exhausted production trace, status=" + std::to_string(result.status));
            Require((result.valid != 0) == expected.hit, label + " CPU/GPU hit mismatch");
            if (!expected.hit) { ++group.misses; continue; }
            ++group.hits;
            Require(result.instanceId == expected.instanceId && result.materialId == expected.materialId &&
                result.triangleId == expected.triangleIndex && result.backFace == uint32_t(expected.backFace) &&
                result.isCutout == uint32_t(expected.isCutout), label + " surface identity mismatch");
            Require(result.instanceIndex < scene.GetInstances().size() &&
                scene.GetInstances()[result.instanceIndex].instanceId == expected.instanceId, label + " invalid compact instance index");
            const auto& instance = scene.GetInstances()[result.instanceIndex];
            const auto& triangle = instance.asset->GetSDF().GetTriangles()[expected.triangleIndex];
            XMFLOAT3 shadingNormal = {
                triangle.shadingNormals[0].x * expected.barycentrics.x + triangle.shadingNormals[1].x * expected.barycentrics.y + triangle.shadingNormals[2].x * expected.barycentrics.z,
                triangle.shadingNormals[0].y * expected.barycentrics.x + triangle.shadingNormals[1].y * expected.barycentrics.y + triangle.shadingNormals[2].y * expected.barycentrics.z,
                triangle.shadingNormals[0].z * expected.barycentrics.x + triangle.shadingNormals[1].z * expected.barycentrics.y + triangle.shadingNormals[2].z * expected.barycentrics.z};
            shadingNormal = Normalize(Transform(shadingNormal, instance.normalFromLocal, true));
            const float error = (std::max)({std::abs(result.distance - expected.distance),
                ComponentError(result.position, expected.position), ComponentError(result.barycentric, expected.barycentrics),
                ComponentError(result.geometricNormal, expected.geometricNormal), ComponentError(result.shadingNormal, shadingNormal)});
            group.maximumError = (std::max)(group.maximumError, error);
            Require(std::isfinite(error) && error <= 4e-4f, label + " CPU/GPU position/distance/normal/barycentric error=" + std::to_string(error));
            Require(result.distance >= ray.minimum && result.distance <= ray.maximum, label + " output outside original finite interval");
        }
        std::cout << name << ": " << group.rays << " rays; hits=" << group.hits << ", miss=" << group.misses
            << ", maximum error=" << group.maximumError << '\n';
        return group;
    }
    std::vector<Ray> LocalRays(const Scene& scene, uint32_t countPerInstance)
    {
        std::vector<Ray> rays;
        std::mt19937 random(9174);
        std::uniform_real_distribution<float> xy(-.33f, .33f), slope(-.01f, .01f);
        for (const auto& instance : scene.GetInstances())
            for (uint32_t index = 0; index < countPerInstance; ++index)
            {
                Ray ray;
                const bool reverse = index % 3 == 0;
                ray.origin = Transform({xy(random), xy(random), reverse ? 3.f : -3.f}, instance.worldFromLocal);
                ray.direction = Transform({slope(random), slope(random), reverse ? -1.f : 1.f}, instance.worldFromLocal, true);
                rays.push_back(ray);
            }
        for (uint32_t index = 0; index < 24; ++index)
        {
            Ray ray; ray.origin = {100 + xy(random), 100 + xy(random), -10}; rays.push_back(ray);
        }
        return rays;
    }
}

int main()
{
    try
    {
        GPU gpu;
        std::vector<Group> groups;
        uint64_t denseCells = 0;
        {
            auto model = Planes({0}), other = Planes({.37f});
            VXGIMeshSDFCache cache; Scene scene;
            std::vector<SourceInstance> sources = {
                Instance(model, 40, {-5, 0, 2}, {1, 1, 1}, {.3f, -.2f, .4f}),
                Instance(model, 10, {0, -3, 0}, {.3f, 3, 4}, {-.4f, .7f, -.2f}),
                Instance(model, 30, {6, 1, 2}, {-2, .5f, 1.5f}, {.4f, .2f, -.7f}),
                Instance(other, 20, {11, -2, -1}, {1.3f, -.6f, 2}, {.1f, -.5f, .2f})};
            sources[1].customMaterialID = 11; sources[2].customMaterialID = 12;
            Update(scene, cache, sources);
            Require(cache.GetStats().assets == 2 && scene.GetInstances().size() == 4, "shared asset fixture is not shared");
            groups.push_back(Compare(gpu, "shared_transformed_multiple_assets", scene, LocalRays(scene, 96)));
            const auto before = Pack::BuildAssets(Pack::CollectAssets(scene));
            const uint64_t builds = cache.GetStats().builds;
            sources[0].translation = {-7, 2, 3}; sources[0].rotation.z += .35f;
            sources[1].customMaterialID = 13;
            Update(scene, cache, sources);
            const auto after = Pack::BuildAssets(Pack::CollectAssets(scene));
            Require(builds == cache.GetStats().builds && before.nodes == after.nodes &&
                before.cellOffsets == after.cellOffsets && before.references == after.references &&
                before.triangles.size() == after.triangles.size() &&
                std::memcmp(before.triangles.data(), after.triangles.data(), before.triangles.size() * sizeof(Pack::Triangle)) == 0,
                "TRS/material change rebuilt or changed shared immutable geometry");
            groups.push_back(Compare(gpu, "instance_move_material_update_reuses_assets", scene, LocalRays(scene, 48)));
        }
        {
            auto model = Planes({0}); VXGIMeshSDFCache cache; Scene scene;
            auto farInstance = Instance(model, 1, {0, 0, 3}), nearInstance = Instance(model, 2, {0, 0, 1});
            nearInstance.customMaterialID = 9;
            Update(scene, cache, {farInstance, nearInstance});
            std::vector<Ray> rays(5);
            for (auto& ray : rays) ray.origin.z = -3;
            rays[1].maximum = 3.999f; rays[2].maximum = 4;
            rays[3].minimum = 4.001f; rays[4].minimum = 4; rays[4].maximum = 4;
            groups.push_back(Compare(gpu, "closest_instance_and_inclusive_finite_bounds", scene, rays));
        }
        {
            auto model = Planes({1}); VXGIMeshSDFCache cache; Scene scene;
            Update(scene, cache, {Instance(model, 1, {}, {1, 1, 1.3f})});
            Ray ray; ray.origin.z = .4f; ray.maximum = .8999999761581421f;
            groups.push_back(Compare(gpu, "scaled_upper_endpoint_rounding", scene, {ray}));
            Update(scene, cache, {Instance(model, 1, {0, 0, 1}, {1, 1, .7f})});
            ray.origin.z = 0; ray.minimum = ray.maximum = 1.7000000476837158f;
            groups.push_back(Compare(gpu, "scaled_zero_length_interval", scene, {ray}));
        }
        {
            auto model = Planes({1, 2}); VXGIMeshSDFCache cache; Scene scene;
            Update(scene, cache, {Instance(model, 1, {0, 0, .1f}, {1, 1, .3f})});
            Ray ray; ray.origin.z = .3f; ray.minimum = .1f; ray.maximum = 1;
            groups.push_back(Compare(gpu, "reject_near_candidate_continue_same_asset", scene, {ray}));
        }
        {
            auto farModel = Planes({1.0001f}), nearModel = Planes({1});
            VXGIMeshSDFCache cache; Scene scene;
            Update(scene, cache, {Instance(farModel, 1), Instance(nearModel, 2)});
            Ray ray; ray.origin.z = -10000; ray.maximum = 10002;
            groups.push_back(Compare(gpu, "same_float_distance_bin_selects_nearer", scene, {ray}));
        }
        {
            auto model = Planes({0}); VXGIMeshSDFCache cache; Scene scene, coverageReference;
            auto hole = Instance(model, 10, {0, 0, 1}); hole.isCutout = true; hole.customMaterialID = 3;
            auto cutoff = Instance(model, 20, {0, 0, 2}); cutoff.isCutout = true; cutoff.customMaterialID = 4;
            auto opaque = Instance(model, 30, {0, 0, 3}); opaque.customMaterialID = 5;
            Update(scene, cache, {hole, cutoff, opaque});
            Update(coverageReference, cache, {cutoff, opaque});
            std::vector<Ray> rays(4);
            for (auto& ray : rays) ray.origin.z = 0;
            rays[1].maximum = 1.5f;
            rays[2].origin.z = 4; rays[2].direction.z = -1;
            rays[3].minimum = 2.001f;
            groups.push_back(Compare(gpu, "per_instance_alpha_hole_cutoff_and_opaque", scene, rays, &coverageReference));
            groups.push_back(Compare(gpu, "geometry_only_keeps_alpha_geometry", scene, rays, nullptr, true, true));
        }
        {
            Model model; Mesh mesh;
            for (uint32_t layer = 0; layer < 48; ++layer) AddPlane(mesh, .1f + .003f * float(layer));
            model.meshes.push_back(std::move(mesh));
            VXGIMeshSDFCache cache; Scene scene;
            Update(scene, cache, {Instance(model, 1), Instance(model, 2, {3, 0, 0}, {-1, 1, 1})});
            denseCells = scene.GetInstances()[0].asset->GetSDF().GetTraceAccelerationStats().acceleratedCells;
            Require(denseCells > 0, "dense local-cell BVH fixture did not build acceleration");
            const auto rays = LocalRays(scene, 192);
            groups.push_back(Compare(gpu, "dense_local_DDA_packed_BVH", scene, rays));
            groups.push_back(Compare(gpu, "dense_local_DDA_original_CSR", scene, rays, nullptr, false));
        }
        const uint32_t warnings = gpu.Warnings();
        Require(warnings == 0, "D3D12 validation reported warning/error");
        uint32_t rayCount = 0; float maximumError = 0;
        for (const auto& group : groups)
        {
            rayCount += group.rays; maximumError = (std::max)(maximumError, group.maximumError);
        }
        std::ofstream report("tmp/VXGIMeshSDFGPUValidation-report.json");
        Require(bool(report), "cannot write Mesh SDF GPU report");
        report << std::setprecision(9) << "{\n  \"adapter\":\"" << gpu.adapterName
            << "\",\n  \"production_trace\":\"VXGI_MESH_SDF VXGISDFTrace\",\n  \"independent_reference\":\"VXGIMeshSDFScene::TraceGeometry\",\n"
            << "  \"debug_layer\":true,\n  \"gpu_based_validation\":true,\n  \"no_hwnd_no_present\":true,\n  \"groups\":[\n";
        for (size_t index = 0; index < groups.size(); ++index)
        {
            const auto& group = groups[index]; if (index) report << ",\n";
            report << "    {\"name\":\"" << group.name << "\",\"rays\":" << group.rays
                << ",\"hits\":" << group.hits << ",\"misses\":" << group.misses
                << ",\"maximum_error\":" << group.maximumError << '}';
        }
        report << "\n  ],\n  \"group_count\":" << groups.size() << ",\n  \"rays\":" << rayCount
            << ",\n  \"dense_cells\":" << denseCells << ",\n  \"submissions\":" << gpu.submissions
            << ",\n  \"maximum_error\":" << maximumError << ",\n  \"d3d12_warnings_errors\":" << warnings
            << ",\n  \"passed\":true\n}\n";
        std::cout << "Mesh SDF GPU validation: " << groups.size() << " groups / " << rayCount
            << " rays passed; 0 D3D12 warnings/errors.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Mesh SDF GPU validation FAILED: " << error.what() << '\n';
        return 1;
    }
}
