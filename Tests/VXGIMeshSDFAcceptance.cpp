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
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "../ThirdParty/json.hpp"
#include <numeric>
#include <cfloat>

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
    XMFLOAT3 Normalize(XMFLOAT3 value)
    {
        XMFLOAT3 result;
        DirectX::XMStoreFloat3(&result, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&value)));
        return result;
    }
    struct Parameters { uint32_t rayCount, materialCount, geometryOnly = 1, padding = 0; XMFLOAT3 origin; float cellSize; std::array<uint32_t,3> dimensions; uint32_t pad1=0; XMFLOAT3 maximum; uint32_t pad2=0; };
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
    static_assert(sizeof(Parameters) == 64 && sizeof(Ray) == 32 && sizeof(Hit) == 96);

    ComPtr<IDxcBlob> Compile(bool global)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        Check(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create DXC utilities");
        Check(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create DXC compiler");
        const wchar_t* path = L"Shaders/VXGIMeshSDFAcceptance.hlsl";
        ComPtr<IDxcBlobEncoding> source;
        Check(utils->LoadFile(path, nullptr, &source), "load Mesh SDF validation shader");
        ComPtr<IDxcIncludeHandler> includes;
        Check(utils->CreateDefaultIncludeHandler(&includes), "create shader includes");
        std::vector<const wchar_t*> arguments = {path, L"-E", L"CSMain", L"-T", L"cs_6_6",
            L"-HV", L"2021", L"-I", L"Shaders", L"-WX"};
if(global) arguments.insert(arguments.end(), {L"-D", L"VXGI_ACCEPTANCE_GLOBAL=1"});
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
        GPU(bool global = false, bool benchmark = false) : globalBackend(global), benchmarkMode(benchmark)
        {
            ComPtr<ID3D12Debug> debug;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "get D3D12 debug interface");
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;
            Check(debug.As(&validation), "get GPU validation interface");
            validation->SetEnableGPUBasedValidation(benchmark ? FALSE : TRUE);
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
            if (benchmarkMode)
            {
                D3D12_QUERY_HEAP_DESC queries = {};
                queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = 2;
                Check(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&timestamps)), "create trace timestamps");
                timestampReadback = Buffer(sizeof(uint64_t) * 2, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                Check(queue->GetTimestampFrequency(&timestampFrequency), "read trace timestamp frequency");
            }
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
            const auto shader = Compile(global);
            D3D12_COMPUTE_PIPELINE_STATE_DESC state = {};
            state.pRootSignature = root.Get();
            state.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)), "create Mesh SDF pipeline");
            std::cout << "Mesh SDF GPU " << (benchmarkMode ? "timestamps without GBV" : "validation + GBV") << " on " << adapterName << "; no HWND / Present\n";
        }
        ~GPU() { if (event) CloseHandle(event); }
        std::vector<Hit> Dispatch(const Scene& scene, const std::vector<Ray>& rays,
            const std::vector<Material>& materials, bool accelerated = true, bool geometryOnly = true, const VXGISDF* globalSDF = nullptr)
        {
            Require(!rays.empty(), "empty GPU ray batch");
            std::array<ComPtr<ID3D12Resource>, 9> inputs;
            if (benchmarkMode && cachedInputs[0]) inputs = cachedInputs;
            else
            {
                const auto assets = Pack::CollectAssets(scene);
                auto geometry = Pack::BuildAssets(assets, accelerated);
                if(globalBackend)
                {
                    Require(globalSDF != nullptr, "Global reference buffers required");
                    geometry.nodes=globalSDF->GetNodes(); geometry.cellOffsets=globalSDF->GetGPUCellOffsets();
                    geometry.references=globalSDF->GetGPUTraceReferences(); geometry.triangles.clear();
                    for(size_t i=0;i<globalSDF->GetTriangles().size();++i){const auto& t=globalSDF->GetTriangles()[i]; Pack::Triangle q{}; q.p0=t.positions[0];q.p1=t.positions[1];q.p2=t.positions[2];q.triangleId=uint32_t(i);q.flags=t.isCutout?1u:0u;q.geometricNormal=t.geometricNormal;q.uv=t.uv;q.shadingNormals=t.shadingNormals;q.tangents=t.tangents;q.bitangents=t.bitangents;q.materialId=t.materialId;q.instanceId=t.instanceId;geometry.triangles.push_back(q);}
                }
                const auto instances = Pack::BuildInstances(scene, assets);
                UploadTextures();
                inputs = {Upload(geometry.nodes), Upload(geometry.cellOffsets), Upload(geometry.references),
                    Upload(geometry.triangles), Upload(materials), Upload(rays),
                    Upload(geometry.assets), Upload(instances.instances), Upload(instances.materialIds)};
                if (benchmarkMode) cachedInputs = inputs;
            }
            const uint64_t bytes = rays.size() * sizeof(Hit);
            auto output = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Parameters parameters = {uint32_t(rays.size()), uint32_t(materials.size())};
            parameters.geometryOnly = uint32_t(geometryOnly);
            if(globalSDF){const auto& grid=globalSDF->GetGrid();parameters.origin=grid.origin;parameters.cellSize=grid.cellSize;parameters.dimensions=grid.cellDimensions;parameters.maximum=grid.boundsMaximum;}
            ID3D12DescriptorHeap* heaps[] = {textureHeap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(root.Get());
            commands->SetPipelineState(pipeline.Get());
            commands->SetComputeRoot32BitConstants(0, sizeof(parameters) / 4, &parameters, 0);
            for (UINT index = 0; index < inputs.size(); ++index)
                commands->SetComputeRootShaderResourceView(index + 1, inputs[index]->GetGPUVirtualAddress());
            commands->SetComputeRootUnorderedAccessView(10, output->GetGPUVirtualAddress());
            Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (benchmarkMode) commands->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            commands->Dispatch((UINT(rays.size()) + 63) / 64, 1, 1);
            if (benchmarkMode)
            {
                commands->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                commands->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestampReadback.Get(), 0);
            }
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
            if (benchmarkMode)
            {
                const D3D12_RANGE timestampRange = {0, sizeof(uint64_t) * 2};
                Check(timestampReadback->Map(0, &timestampRange, &mapped), "map trace timestamps");
                const auto* values = static_cast<const uint64_t*>(mapped);
                lastKernelMicroseconds = double(values[1] - values[0]) * 1e6 / double(timestampFrequency);
                timestampReadback->Unmap(0, &writes);
            }
            Check(allocator->Reset(), "reset Mesh SDF allocator");
            Check(commands->Reset(allocator.Get(), nullptr), "reset Mesh SDF commands");
            if (!benchmarkMode) textures.clear();
            textureUploads.clear(); bufferUploads.clear();
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
        double lastKernelMicroseconds = 0;
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
            if (benchmarkMode)
            {
                // Static timing inputs live in GPU-local DEFAULT memory, like
                // production; PCIe reads from UPLOAD heaps would distort A/B.
                auto local = Buffer(values.size() * sizeof(T), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
                Transition(local.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                commands->CopyResource(local.Get(), resource.Get());
                Transition(local.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bufferUploads.push_back(resource);
                return local;
            }
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
        bool globalBackend=false, benchmarkMode=false;
        std::array<ComPtr<ID3D12Resource>, 9> cachedInputs;
        std::vector<ComPtr<ID3D12Resource>> bufferUploads;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timestampReadback;
        uint64_t timestampFrequency = 0;
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

}

namespace Acceptance
{
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
double Elapsed(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
XMFLOAT3 V3(const Json& item){return {item[0].get<float>(),item[1].get<float>(),item[2].get<float>()};}
DirectX::XMMATRIX Matrix(const aiMatrix4x4& m){return DirectX::XMMATRIX(m.a1,m.b1,m.c1,m.d1,m.a2,m.b2,m.c2,m.d2,m.a3,m.b3,m.c3,m.d3,m.a4,m.b4,m.c4,m.d4);}
void ImportNode(Model& output,const aiScene& scene,const aiNode& node,DirectX::FXMMATRIX parent,uint32_t& material)
{
    using namespace DirectX;
    const auto transform=Matrix(node.mTransformation)*parent;
    const auto normal=XMMatrixTranspose(XMMatrixInverse(nullptr,transform));
    for(uint32_t i=0;i<node.mNumMeshes;++i)
    {
        const auto& source=*scene.mMeshes[node.mMeshes[i]];
        Mesh mesh; mesh.materialID=material++;
        for(uint32_t j=0;j<source.mNumVertices;++j)
        {
            Vertex v{};
            XMStoreFloat3(&v.Position,XMVector4Transform(XMVectorSet(source.mVertices[j].x,source.mVertices[j].y,source.mVertices[j].z,1),transform));
            if(source.HasNormals())XMStoreFloat3(&v.Normal,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mNormals[j])),normal)));
            if(source.mTextureCoords[0])v.TexCoords={source.mTextureCoords[0][j].x,source.mTextureCoords[0][j].y};
            if(source.HasTangentsAndBitangents())
            {
                XMStoreFloat3(&v.Tangent,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mTangents[j])),normal)));
                XMStoreFloat3(&v.Bitangent,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mBitangents[j])),normal)));
            }
            mesh.vertices.push_back(v);
        }
        for(uint32_t j=0;j<source.mNumFaces;++j)
        {
            Require(source.mFaces[j].mNumIndices==3,"Assimp triangulation failed");
            mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),source.mFaces[j].mIndices,source.mFaces[j].mIndices+3);
        }
        output.meshes.push_back(std::move(mesh));
    }
    for(uint32_t i=0;i<node.mNumChildren;++i)ImportNode(output,scene,*node.mChildren[i],transform,material);
}
Model Load(const std::string& path,uint32_t& material)
{
    Assimp::Importer importer;
    const unsigned flags=aiProcess_Triangulate|aiProcess_GenSmoothNormals|aiProcess_CalcTangentSpace|aiProcess_JoinIdenticalVertices|aiProcess_MakeLeftHanded|aiProcess_FlipWindingOrder|aiProcess_FlipUVs;
    const auto* scene=importer.ReadFile(path,flags);
    Require(scene&&scene->mRootNode,"Cannot load configured model: "+path+": "+importer.GetErrorString());
    Model model;ImportNode(model,*scene,*scene->mRootNode,DirectX::XMMatrixIdentity(),material);return model;
}
struct D
{
    double x=0,y=0,z=0;
    D operator+(D q)const{return {x+q.x,y+q.y,z+q.z};}
    D operator-(D q)const{return {x-q.x,y-q.y,z-q.z};}
    D operator*(double q)const{return {x*q,y*q,z*q};}
    double operator[](int i)const{return i==0?x:i==1?y:z;}
};
D Double(XMFLOAT3 p){return {p.x,p.y,p.z};}
D Cross(D p,D q){return {p.y*q.z-p.z*q.y,p.z*q.x-p.x*q.z,p.x*q.y-p.y*q.x};}
double Dot(D p,D q){return p.x*q.x+p.y*q.y+p.z*q.z;}
struct Expected{bool hit=false;double distance=0;uint32_t triangle=UINT32_MAX;};
// Independent world-space median BVH. No asset-local bounds, inverse transforms,
// distance nodes, cell references, or production trace functions are consulted.
class Reference
{
    struct Bounds{D low={DBL_MAX,DBL_MAX,DBL_MAX},high={-DBL_MAX,-DBL_MAX,-DBL_MAX};};
    struct Node{Bounds bounds;uint32_t first=0,count=0,left=0,right=0;};
    const std::vector<VXGIWorld::Triangle>& triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    static void Expand(Bounds& b,D p){b.low={(std::min)(b.low.x,p.x),(std::min)(b.low.y,p.y),(std::min)(b.low.z,p.z)};b.high={(std::max)(b.high.x,p.x),(std::max)(b.high.y,p.y),(std::max)(b.high.z,p.z)};}
    D Center(uint32_t i)const{const auto& p=triangles[i].positions;return (Double(p[0])+Double(p[1])+Double(p[2]))*(1.0/3);}
    uint32_t Build(uint32_t first,uint32_t count)
    {
        const uint32_t index=uint32_t(nodes.size());nodes.push_back({});Bounds b,c;
        for(uint32_t i=first;i<first+count;++i){for(const auto& p:triangles[order[i]].positions)Expand(b,Double(p));Expand(c,Center(order[i]));}
        nodes[index].bounds=b;
        if(count<=8){nodes[index].first=first;nodes[index].count=count;return index;}
        const D e=c.high-c.low;const int axis=e.x>e.y?(e.x>e.z?0:2):(e.y>e.z?1:2);const uint32_t mid=first+count/2;
        std::nth_element(order.begin()+first,order.begin()+mid,order.begin()+first+count,[&](uint32_t a,uint32_t q){return Center(a)[axis]<Center(q)[axis];});
        const uint32_t left=Build(first,count/2),right=Build(mid,count-count/2);nodes[index].left=left;nodes[index].right=right;return index;
    }
    static bool Box(const Bounds& b,D o,D d,double low,double high)
    {
        for(int axis=0;axis<3;++axis){if(std::abs(d[axis])<1e-30){if(o[axis]<b.low[axis]||o[axis]>b.high[axis])return false;continue;}double a=(b.low[axis]-o[axis])/d[axis],z=(b.high[axis]-o[axis])/d[axis];if(a>z)std::swap(a,z);low=(std::max)(low,a);high=(std::min)(high,z);if(low>high)return false;}return true;
    }
public:
    explicit Reference(const VXGIWorld& world):triangles(world.GetTriangles()){order.resize(triangles.size());std::iota(order.begin(),order.end(),0u);Build(0,uint32_t(order.size()));}
    Expected Trace(const Ray& ray,bool alreadyNormalized=false)const
    {
        const D origin=Double(ray.origin),direction=alreadyNormalized?Double(ray.direction):Double(ray.direction)*(1/std::sqrt(Dot(Double(ray.direction),Double(ray.direction))));
        Expected best;double maximum=ray.maximum;std::vector<uint32_t> stack={0};
        while(!stack.empty())
        {
            const auto& node=nodes[stack.back()];stack.pop_back();if(!Box(node.bounds,origin,direction,ray.minimum,maximum))continue;
            if(!node.count){stack.push_back(node.left);stack.push_back(node.right);continue;}
            for(uint32_t j=0;j<node.count;++j)
            {
                const uint32_t index=order[node.first+j];const auto& q=triangles[index].positions;
                const D e1=Double(q[1])-Double(q[0]),e2=Double(q[2])-Double(q[0]),p=Cross(direction,e2);const double determinant=Dot(e1,p);
                if(std::abs(determinant)<1e-12)continue;
                const D s=origin-Double(q[0]),v=Cross(s,e1);const double u=Dot(s,p)/determinant,w=Dot(direction,v)/determinant,distance=Dot(e2,v)/determinant;
                constexpr double tolerance=2e-5;
                if(u < -tolerance||w < -tolerance||u+w>1+tolerance||distance<ray.minimum||distance>maximum)continue;
                best={true,distance,index};maximum=distance;
            }
        }
        return best;
    }
};
std::vector<Ray> Rays(const VXGIWorld& world,uint32_t count)
{
    std::mt19937 random(0x5DF0403);std::uniform_real_distribution<float> unit(0,1),signedUnit(-1,1);std::vector<Ray> result;
    const auto& triangles=world.GetTriangles();
    for(uint32_t i=0;i<count;++i)
    {
        const auto& triangle=triangles[random()%triangles.size()];const float u=.15f+.3f*unit(random),v=.15f+.3f*unit(random);
        const D center=Double(triangle.positions[0])*u+Double(triangle.positions[1])*v+Double(triangle.positions[2])*(1-u-v),normal=Double(triangle.geometricNormal);
        Ray ray;ray.minimum=.0001f;
        if(i%4==0||i%4==1)
        {
            const double distance=.02+.6*unit(random);const D origin=center+normal*(i%4==0?distance:-distance);
            ray.origin={float(origin.x),float(origin.y),float(origin.z)};ray.direction=Normalize({float(center.x-origin.x),float(center.y-origin.y),float(center.z-origin.z)});
            ray.maximum=i%3==0?.05f:i%3==1?.5f:4.f;
        }
        else if(i%4==2)
        {
            const D origin=center+normal*.003;ray.origin={float(origin.x),float(origin.y),float(origin.z)};
            ray.direction=Normalize({signedUnit(random),signedUnit(random),signedUnit(random)});ray.maximum=.5f;
        }
        else
        {
            ray.origin={signedUnit(random)*8,unit(random)*6-.5f,signedUnit(random)*8};
            ray.direction=Normalize({float(center.x)-ray.origin.x,float(center.y)-ray.origin.y,float(center.z)-ray.origin.z});ray.maximum=i%3==0?.5f:i%3==1?4.f:1000.f;
        }
        result.push_back(ray);
    }
    return result;
}
std::vector<Ray> FarRays(const VXGIWorld& world,uint32_t count)
{
    std::mt19937 random(0xFA20403);std::uniform_real_distribution<float> signedUnit(-1,1);std::vector<Ray> rays;
    for(uint32_t i=0;i<count;++i)
    {
        const auto& triangle=world.GetTriangles()[random()%world.GetTriangles().size()];
        const D center=(Double(triangle.positions[0])+Double(triangle.positions[1])+Double(triangle.positions[2]))*(1.0/3);
        const auto outward=Normalize({signedUnit(random),signedUnit(random),signedUnit(random)});const double range=i%2?10000:100000;
        Ray ray;ray.origin={float(center.x+outward.x*range),float(center.y+outward.y*range),float(center.z+outward.z*range)};
        ray.direction=Normalize({float(center.x-ray.origin.x),float(center.y-ray.origin.y),float(center.z-ray.origin.z)});ray.minimum=.0001f;ray.maximum=float(range+16);
        rays.push_back(ray);
    }
    return rays;
}
uint64_t MeshPayload(const Pack::Geometry& g,const Pack::Instances& i){return g.nodes.size()*4+g.cellOffsets.size()*4+g.references.size()*4+g.triangles.size()*sizeof(Pack::Triangle)+g.assets.size()*sizeof(Pack::Asset)+i.instances.size()*sizeof(Pack::Instance)+i.materialIds.size()*4;}
Json Compare(GPU& gpu,const Scene& scene,const VXGIWorld& world,const VXGISDF& global,const std::vector<Ray>& rays,uint32_t materialCount)
{
    Reference reference(world);std::vector<Material> materials(materialCount);for(auto& m:materials)m.albedoIdx=1;
    const auto results=gpu.Dispatch(scene,rays,materials,true,true,&global);
    uint32_t hits=0,misses=0,mismatches=0,identity=0,invalid=0;double maximumError=0;Json examples=Json::array();
    const auto start=Clock::now();
    for(size_t i=0;i<rays.size();++i)
    {
        const auto& actual=results[i];Ray normalizedRay=rays[i];normalizedRay.direction=actual.barycentric;
        const auto expected=reference.Trace(normalizedRay,true);
        if(actual.status>1)++invalid;
        bool different=bool(actual.valid)!=expected.hit;
        if(expected.hit)++hits;else ++misses;
        if(expected.hit&&actual.valid)
        {
            // Output distance is float; far rays require its representable ULP
            // tolerance, while hit identity and the finite interval stay strict.
            const double error=std::abs(double(actual.distance)-expected.distance);maximumError=(std::max)(maximumError,error);different=different||error>(std::max)(2e-4,std::abs(expected.distance)*1.3e-7);
            const auto& t=world.GetTriangles()[expected.triangle];
            if(actual.instanceId!=t.instanceId||actual.materialId!=t.materialId)++identity;
        }
        if(different){++mismatches;if(examples.size()<16)examples.push_back({{"ray",i},{"expected_hit",expected.hit},{"actual_hit",actual.valid},{"expected_distance",expected.distance},{"actual_distance",actual.distance},{"status",actual.status},{"origin",{rays[i].origin.x,rays[i].origin.y,rays[i].origin.z}},{"direction",{rays[i].direction.x,rays[i].direction.y,rays[i].direction.z}},{"interval",{rays[i].minimum,rays[i].maximum}}});}
    }
    return {{"rays",rays.size()},{"reference_hits",hits},{"reference_misses",misses},{"hit_or_distance_mismatches",mismatches},{"surface_identity_mismatches",identity},{"invalid_or_exhausted",invalid},{"maximum_distance_error",maximumError},{"cpu_reference_ms",Elapsed(start)},{"examples",examples}};
}
Json TimingSummary(const std::vector<double>& samples,uint32_t rays)
{
    auto sorted=samples;std::sort(sorted.begin(),sorted.end());
    const double median=(sorted[(sorted.size()-1)/2]+sorted[sorted.size()/2])*.5;
    const double p95=sorted[size_t(std::ceil(.95*double(sorted.size())))-1];
    return {{"rays_per_dispatch",rays},{"measured_dispatches",samples.size()},{"microseconds",samples},
        {"median_us",median},{"p95_us",p95},{"median_ms",median*.001},{"p95_ms",p95*.001},
        {"million_traces_per_second_at_median",double(rays)/median}};
}
void Benchmark(const Scene& scene,const VXGIWorld& world,const VXGISDF& global,const std::vector<Ray>& rays,uint32_t materialCount,Json report)
{
    GPU mesh(false,true),worldGPU(true,true);
    report["configured_scene_mesh"]=Compare(mesh,scene,world,global,rays,materialCount);
    report["configured_scene_global"]=Compare(worldGPU,scene,world,global,rays,materialCount);
    bool passed=true;
    for(const auto* key:{"configured_scene_mesh","configured_scene_global"})
    {
        const auto& group=report[key];passed=passed&&group["hit_or_distance_mismatches"]==0&&
            group["surface_identity_mismatches"]==0&&group["invalid_or_exhausted"]==0;
    }
    std::vector<Material> materials(materialCount);for(auto& m:materials)m.albedoIdx=1;
    constexpr uint32_t warmups=2,samples=12;
    std::vector<double> meshTimes,globalTimes;Json order=Json::array();
    for(uint32_t i=0;i<warmups+samples;++i)
    {
        const auto dispatch=[&](GPU& gpu,std::vector<double>& values)
        {
            gpu.Dispatch(scene,rays,materials,true,true,&global);
            if(i>=warmups)values.push_back(gpu.lastKernelMicroseconds);
        };
        if(i%2==0){dispatch(mesh,meshTimes);dispatch(worldGPU,globalTimes);order.push_back("Mesh then Global");}
        else{dispatch(worldGPU,globalTimes);dispatch(mesh,meshTimes);order.push_back("Global then Mesh");}
    }
    report["mesh_gpu_timing"]=TimingSummary(meshTimes,uint32_t(rays.size()));
    report["global_gpu_timing"]=TimingSummary(globalTimes,uint32_t(rays.size()));
    const double ratio=report["mesh_gpu_timing"]["median_us"].get<double>()/report["global_gpu_timing"]["median_us"].get<double>();
    report["mesh_over_global_median_ratio"]=ratio;report["mesh_median_time_change_percent"]=(ratio-1)*100;
    report["warmup_dispatches_per_backend"]=warmups;report["alternating_dispatch_order_including_warmups"]=order;
    report["mesh_warnings"]=mesh.Warnings();report["global_warnings"]=worldGPU.Warnings();
    passed=passed&&report["mesh_warnings"]==0&&report["global_warnings"]==0;
    report["passed"]=passed;report["adapter"]=mesh.adapterName;report["gpu_based_validation"]=false;
    report["debug_layer"]=true;report["no_hwnd_no_present"]=true;
    report["scope"]="Static configured actual LOD0 scene; identical finite geometry-only rays and settings, two shader variants including their production Global/Mesh trace functions. Actual Alpha textures and full SSGI/probe/raster passes are excluded. The initial GPU outputs from both backends independently match a world-space CPU BVH using the exact shader-normalized direction.";
    report["timing_contract"]="Release O3 shaders; GBV disabled. Inputs copied once to GPU-local DEFAULT buffers and reused. GPU timestamp pairs bracket only Dispatch; packing, asset bake/upload, texture transfers, output/readback allocations, result copies, fence waits, shader/device initialization, and CPU reference are excluded. Each submission uses a fresh UAV output, so no output is reused without a barrier. Two warmups per backend follow one untimed setup/correctness dispatch; twelve measured dispatches alternate backend order. Individually fence-waited static kernels are not whole-application throughput.";
    report["cpu_initialization_measurements_are_uncontended_benchmark"]=false;
    std::ofstream("tmp/VXGIMeshSDFAcceptance-GPUBenchmark-report.json")<<std::setw(2)<<report;
    std::cout<<std::setw(2)<<report<<'\n';Require(passed,"Static Global/Mesh GPU benchmark output validation failed");
}
}

int main(int argc,char** argv)
{
    try
    {
        using namespace Acceptance;
        uint32_t count=16384;bool transforms=false,benchmark=false;for(int i=1;i<argc;++i){const std::string option=argv[i];if(option=="--rays"&&i+1<argc)count=uint32_t(std::stoul(argv[++i]));else if(option=="--transforms")transforms=true;else if(option=="--benchmark")benchmark=true;else throw std::runtime_error("unknown acceptance argument "+option);}
        Require(!benchmark||!transforms,"Static GPU benchmark cannot change immutable test inputs with --transforms");
#if defined(_DEBUG)
        Require(!benchmark,"GPU benchmark uses the O2 fixture and Release O3 shader policy");
#endif
        Json config,pipeline;std::ifstream("Settings/Scene.json")>>config;std::ifstream("Settings/Pipeline.json")>>pipeline;
        std::vector<Model> models;std::vector<SourceInstance> sources;models.reserve(config["instances"].size());uint32_t material=0;
        auto start=Clock::now();
        for(const auto& item:config["instances"]){models.push_back(Load(item["model_path"],material));SourceInstance instance;instance.pModel=&models.back();instance.editorId=uint32_t(sources.size()+1);instance.translation=V3(item["pos"]);instance.rotation=V3(item["rot"]);instance.scale=V3(item["scale"]);instance.isCutout=item.value("is_cutout",false);instance.isTransparent=item.value("is_transparent",false);sources.push_back(instance);}
        Json report;report["import_ms"]=Elapsed(start);const VXGIMeshSDFCache::BuildSettings settings={pipeline["vxgi"]["sdf"]["cell_size"].get<float>()};
        VXGIMeshSDFCache cache;Scene scene;start=Clock::now();Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());report["mesh_initialization_ms"]=Elapsed(start);
        VXGIWorld world;start=Clock::now();Require(world.Update(sources,pipeline["vxgi"]["voxel_size"].get<float>()),world.GetLastError());report["world_allocation_ms"]=Elapsed(start);
        VXGISDF global;start=Clock::now();Require(global.Update(world,settings.cellSize,settings.maxAxisResolution),global.GetLastError());report["global_initialization_ms"]=Elapsed(start);
        const auto assets=Pack::CollectAssets(scene);const auto packed=Pack::BuildAssets(assets);const auto instances=Pack::BuildInstances(scene,assets);
        report["scene"]={{"instances",sources.size()},{"assets",assets.size()},{"triangles",world.GetTriangles().size()},{"local_cpu_bytes",cache.GetStats().memoryBytes},{"global_cpu_bytes",global.GetStats().memoryBytes},{"mesh_static_gpu_payload_bytes",MeshPayload(packed,instances)},{"global_static_gpu_payload_bytes",global.GetNodes().size()*4+global.GetGPUCellOffsets().size()*4+global.GetGPUTraceReferences().size()*4+global.GetTriangles().size()*sizeof(Pack::Triangle)},{"cutout_geometry_present",true}};
        report["settings"]={{"cell_size",settings.cellSize},{"node_sizing","automatic"}};
        start=Clock::now();for(int i=0;i<1000;++i)Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());report["unchanged_1000_updates_ms"]=Elapsed(start);
        const auto initialBuilds=cache.GetStats().builds;const auto initialBytes=cache.GetStats().memoryBytes;
        start=Clock::now();for(int i=0;i<1000;++i){sources[0].translation.x+=.001f;Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());}report["transform_1000_updates_ms"]=Elapsed(start);
        Require(cache.GetStats().builds==initialBuilds&&cache.GetStats().memoryBytes==initialBytes,"transform updates rebaked or resized static assets");report["transform_updates_asset_builds_added"]=cache.GetStats().builds-initialBuilds;
        sources[0].translation=V3(config["instances"][0]["pos"]);Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());
        if(benchmark){Benchmark(scene,world,global,Rays(world,count),material,std::move(report));return 0;}
        GPU meshGPU,globalGPU(true);const auto rays=Rays(world,count);
        report["configured_scene_mesh"]=Compare(meshGPU,scene,world,global,rays,material);
        report["configured_scene_global"]=Compare(globalGPU,scene,world,global,rays,material);
        if(transforms)
        {
            sources[0].translation={1.3f,.17f,-.8f};sources[0].rotation={.13f,.37f,-.09f};sources[0].scale={-1.3f,.7f,1.8f};
            Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());Require(world.Update(sources,pipeline["vxgi"]["voxel_size"].get<float>()),world.GetLastError());
            report["transformed_scene_mesh"]=Compare(meshGPU,scene,world,global,Rays(world,count/2),material);
            const auto farRays=FarRays(world,count/4);
            report["far_transformed_scene_mesh"]=Compare(meshGPU,scene,world,global,farRays,material);
            Require(global.Update(world,settings.cellSize,settings.maxAxisResolution),global.GetLastError());
            report["far_transformed_scene_global"]=Compare(globalGPU,scene,world,global,farRays,material);
            report["transformed_asset_builds_added"]=cache.GetStats().builds-initialBuilds;
        }
        report["mesh_warnings"]=meshGPU.Warnings();report["global_warnings"]=globalGPU.Warnings();
        report["scope"]="Configured actual LOD0 GLBs imported with production Assimp flags and node transforms. Finite surface, outward, and world rays compared to independent world-space double triangle/BVH reference. Geometry-only tracing deliberately retains cutout triangles; actual alpha textures are excluded here, with synthetic cutoff/holes covered by VXGIMeshSDFGPUValidation and actual-material rendering covered by Stage75.";
        report["no_hwnd_no_present"]=true;report["gpu_based_validation"]=true;report["adapter"]=meshGPU.adapterName;
        bool passed=report["mesh_warnings"]==0&&report["global_warnings"]==0;
        for(const auto* key:{"configured_scene_mesh","transformed_scene_mesh","far_transformed_scene_mesh"})if(report.contains(key)){const auto& group=report[key];passed=passed&&group["hit_or_distance_mismatches"]==0&&group["surface_identity_mismatches"]==0&&group["invalid_or_exhausted"]==0;}
        report["passed"]=passed;std::ofstream("tmp/VXGIMeshSDFAcceptance-report.json")<<std::setw(2)<<report;
        std::cout<<std::setw(2)<<report<<'\n';Require(passed,"Actual-scene Mesh SDF acceptance failed; see JSON examples");return 0;
    }
    catch(const std::exception& error){std::cerr<<"Mesh SDF actual-scene acceptance FAILED: "<<error.what()<<'\n';return 1;}
}
