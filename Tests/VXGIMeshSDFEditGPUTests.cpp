// Small production-wrapper acceptance for moving immutable Mesh SDF instances.
// The synthetic floor/cube scene runs without visible UI, raster or Present.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dx12.h>
#include "../Passes/VXGIProbePass.h"
#include "../Passes/VXGIProbeFilterPass.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
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
    using Record=VXGIProbeState::GPURecord;
    constexpr UINT Slots=3;
    constexpr uint32_t Known=0x80000000u;
    void Require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
    void Check(HRESULT value,const char* operation)
    {if(FAILED(value)){std::ostringstream message;message<<operation<<": HRESULT 0x"<<std::hex<<uint32_t(value);throw std::runtime_error(message.str());}}
    std::string UTF8(const wchar_t* input)
    {
        const int bytes=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);
        std::string result(size_t(bytes),'\0');WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),bytes,nullptr,nullptr);result.pop_back();return result;
    }
    struct Vertex{XMFLOAT3 Position={},Normal={0,1,0};XMFLOAT2 TexCoords={};XMFLOAT3 Tangent={1,0,0},Bitangent={0,0,1};};
    struct Resource{void* Get() const{return nullptr;}};
    struct Texture{uint32_t type=0;Resource Resource;std::string path;};
    struct Mesh
    {
        std::vector<Vertex> vertices;std::vector<std::vector<uint32_t>> lodIndices={{}};uint32_t materialID=0;
        bool isUnlit=false;XMFLOAT4 baseColorFactor={1,1,1,1};XMFLOAT3 emissiveFactor={};std::vector<Texture> textures;
    };
    struct Model{std::vector<Mesh> meshes;};
    struct Instance
    {
        Model* pModel=nullptr;uint32_t editorId=0,customMaterialID=UINT32_MAX;
        XMFLOAT3 translation={},rotation={},scale={1,1,1};bool isTransparent=false,isCutout=false;
    };
    void Quad(Mesh& mesh,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d)
    {
        const uint32_t first=uint32_t(mesh.vertices.size());
        const auto n=DirectX::XMVector3Normalize(DirectX::XMVector3Cross(
            DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&b),DirectX::XMLoadFloat3(&a)),
            DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&c),DirectX::XMLoadFloat3(&a))));
        XMFLOAT3 normal;DirectX::XMStoreFloat3(&normal,n);
        mesh.vertices.insert(mesh.vertices.end(),{{a,normal},{b,normal},{c,normal},{d,normal}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    Model Floor()
    {Mesh mesh;Quad(mesh,{-2.1f,0,-2.1f},{-2.1f,0,2.1f},{2.1f,0,2.1f},{2.1f,0,-2.1f});return {{std::move(mesh)}};}
    Model Cube()
    {
        Mesh mesh;mesh.materialID=1;constexpr float a=.7f;
        Quad(mesh,{-a,-a,a},{a,-a,a},{a,a,a},{-a,a,a});
        Quad(mesh,{-a,-a,-a},{-a,a,-a},{a,a,-a},{a,-a,-a});
        Quad(mesh,{a,-a,-a},{a,a,-a},{a,a,a},{a,-a,a});
        Quad(mesh,{-a,-a,-a},{-a,-a,a},{-a,a,a},{-a,a,-a});
        Quad(mesh,{-a,a,-a},{-a,a,a},{a,a,a},{a,a,-a});
        Quad(mesh,{-a,-a,-a},{a,-a,-a},{a,-a,a},{-a,-a,a});
        return {{std::move(mesh)}};
    }
    struct Snapshot
    {
        std::vector<Record> records,filtered;
        std::vector<uint32_t> edges;
        std::vector<VXGIWorld::Coordinate> coordinates;
        VXGIProbeState::PendingPlan plan;
    };
    class Fixture
    {
    public:
        Fixture()
        {
            WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"VXGIMeshSDFEditAcceptance";
            Require(RegisterClassW(&type)!=0,"register hidden fixture window");
            window=CreateWindowExW(0,type.lpszClassName,L"Mesh edit background fixture",WS_POPUP,0,0,32,32,nullptr,nullptr,type.hInstance,nullptr);
            Require(window&&!IsWindowVisible(window),"fixture window must remain hidden");
            Require(device.Initialize(window,32,32,int(Slots)),"initialize production RenderDevice");
            Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"get production InfoQueue");
            ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"create adapter lookup");
            ComPtr<IDXGIAdapter1> adapter;Check(factory->EnumAdapterByLuid(device.GetDevice()->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"lookup production adapter");
            DXGI_ADAPTER_DESC1 description{};Check(adapter->GetDesc1(&description),"read adapter");adapterName=UTF8(description.Description);
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};Check(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)),"check double precision");
            Require(options.DoublePrecisionFloatShaderOps,"Mesh SDF requires double precision shader operations");
            shared.Initialize(Slots);Require(probes.Initialize(device,Slots,true)&&filter.Initialize(device,Slots,true),"initialize production Mesh Probe/Filter passes");
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heapDesc.NumDescriptors=4;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device.GetDevice()->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"create material texture heap");
            parameters.probesPerFrame=4096;parameters.classifyRays=64;parameters.raysPerUpdate=128;parameters.maxHistoryUpdates=4;parameters.rayMaxDistance=32;
            sourceParameters.lightColor={0,0,0};sourceParameters.maxRayDistance=parameters.rayMaxDistance;
            floor=Floor();cube=Cube();instances={{&floor,1},{&cube,2}};instances[1].translation={0,.8f,0};
            materials.resize(2);for(auto& material:materials){material.albedoIdx=0;material.normalIdx=1;material.ormIdx=2;material.emissiveIdx=3;}
            materials[0].baseColorFactor={0,0,0,1};materials[1].emissiveFactor={8,2,1};
            Check(device.GetCommandList()->Close(),"close empty device initialization");
            Reset(0);InitializeMaterialResources();Submit(0);Wait(0);staging.clear();UpdateSources();
        }
        ~Fixture()
        {for(UINT slot=0;slot<Slots;++slot)if(device.GetFence(int(slot)))device.WaitForPreviousFrame(int(slot));if(window)DestroyWindow(window);UnregisterClassW(L"VXGIMeshSDFEditAcceptance",GetModuleHandleW(nullptr));}
        void Run()
        {
            auto initial=Frame(0);const auto oldSnapshot=initial;
            Frame(1);initial=Frame(2);
            const size_t target=Find(initial,{0,0,0}); // Fixed center (.25,.25,.25), inside the cube.
            Require(initial.records[target].state==VXGIProbeState::Disabled&&initial.records[target].reason==VXGIProbeState::Backface,"initial target probe must be disabled inside the closed cube");
            Require((initial.edges[target]&Known)!=0&&(initial.edges[target]&(1u<<1))==0,"cube wall must block target's +X neighbor");
            VerifyComplete(initial);Pass("initial full classification, source capture and blocked neighbor visibility");
            const auto assetBuilds=cache.GetStats().builds,allocation=world.GetAllocationRevision(),topologyRevision=topology.GetRevision(),meshRevision=meshScene.GetRevision();
            const auto surfaceRevision=surface.GetRevision(),epoch=state.GetRevision();
            std::array<VXGIGPUScene::MeshUploadStats,Slots> uploads;std::array<std::array<ID3D12Resource*,5>,Slots> assetBuffers={};
            constexpr std::array<UINT,5> assetSlots={0,1,2,3,7};
            for(UINT slot=0;slot<Slots;++slot){uploads[slot]=shared.GetMeshUploadStats(int(slot));for(size_t i=0;i<assetSlots.size();++i)assetBuffers[slot][i]=shared.Get(int(slot),assetSlots[i]);}
            Require(state.Prepare(Sources(),parameters),state.GetLastError());const auto stalePlan=state.GetPendingPlan();
            instances[1].translation.x=2;UpdateSources();
            Require(world.GetAllocationRevision()>allocation&&topology.GetRevision()>topologyRevision&&meshScene.GetRevision()>meshRevision&&surface.GetRevision()>surfaceRevision,"TRS did not propagate World -> Mesh -> Topology -> Surface revisions");
            parameters.probesPerFrame=1;Require(state.Prepare(Sources(),parameters),state.GetLastError());
            Require(!state.CommitSubmitted(stalePlan),"pre-movement pending plan was accepted after sources changed");
            auto partial=Frame(1);Require(partial.plan.resetMode==VXGIProbeState::ResetClassification&&partial.plan.epoch==epoch+1,"movement did not create classification reset epoch");
            for(size_t i=1;i<partial.records.size();++i)
            {Require(partial.records[i].state==VXGIProbeState::Unclassified&&partial.records[i].updateCount==0&&partial.edges[i]==0,"partial reset retained old classification/history/known neighbor edges");Require(SHError(partial.filtered[i],Record{})==0,"partial reset retained old filtered light");}
            Pass("model movement invalidates all probe history and neighbor edges before a partial batch");
            const auto accepted=state.GetStats().acceptedReadbacks;
            Require(probes.Poll(device,0,state),"old submitted GPU snapshot must be discarded without failing Poll");
            Require(state.GetStats().acceptedReadbacks==accepted&&!state.GetStats().hasCompletedReadback,"obsolete GPU snapshot was accepted in the new epoch");
            Require(!state.AcceptReadback(oldSnapshot.records,oldSnapshot.plan.frameIndex,oldSnapshot.plan.epoch),"explicit stale-epoch readback was accepted");
            Require(probes.Poll(device,1,state),"accept the current epoch's partial snapshot");
            Require(state.GetStats().acceptedReadbacks==accepted+1&&state.GetStats().epoch==epoch+1,"current epoch snapshot was not accepted");
            Pass("stale pending plans and old submitted GPU/explicit readbacks are rejected");
            parameters.probesPerFrame=4096;Frame(2);auto moved=Frame(0);VerifyComplete(moved);
            const size_t movedTarget=Find(moved,{0,0,0});
            Require(moved.records[movedTarget].state==VXGIProbeState::Valid&&moved.records[movedTarget].updateCount>0,"fixed target probe was not reclassified valid after the cube moved");
            Require((moved.edges[movedTarget]&(Known|(1u<<1)))==(Known|(1u<<1)),"old blocked +X neighbor visibility was not refreshed to visible");
            uint32_t changedStates=0,changedSource=0;
            for(size_t i=0;i<initial.coordinates.size();++i){const auto found=std::find(moved.coordinates.begin(),moved.coordinates.end(),initial.coordinates[i]);if(found==moved.coordinates.end())continue;const size_t j=size_t(found-moved.coordinates.begin());changedStates+=initial.records[i].state!=moved.records[j].state;changedSource+=SHError(initial.records[i],moved.records[j])>1e-5f;}
            Require(changedStates>0&&changedSource>0,"movement did not update classification and cached source radiance at common fixed probes");
            changedStateCount=changedStates;changedSourceCount=changedSource;Pass("reclassification, reopened neighbor edge and cached source light follow the moved model");
            for(UINT slot=0;slot<Slots;++slot){const auto& after=shared.GetMeshUploadStats(int(slot));Require(after.assetUploads==uploads[slot].assetUploads&&after.instanceUploads==uploads[slot].instanceUploads+1&&after.materialMapUploads==uploads[slot].materialMapUploads,"TRS update did not reuse shared GPU assets/material maps on every slot");for(size_t i=0;i<assetSlots.size();++i)Require(assetBuffers[slot][i]==shared.Get(int(slot),assetSlots[i]),"TRS update replaced an immutable asset resource");}
            Require(cache.GetStats().builds==assetBuilds,"movement rebaked immutable Mesh SDF assets");Pass("three frame slots reuse immutable asset resources while only instance metadata changes");
            instances[1].translation.x=0;UpdateSources();Frame(1);Frame(2);auto restored=Frame(0);VerifyComplete(restored);
            Require(initial.coordinates==restored.coordinates,"restore did not restore the original sparse probe allocation");
            float restoreError=0;
            for(size_t i=0;i<initial.records.size();++i){Require(initial.records[i].state==restored.records[i].state&&initial.records[i].reason==restored.records[i].reason&&initial.edges[i]==restored.edges[i],"restored scene retained moved classification or neighbor visibility");restoreError=std::max(restoreError,SHError(initial.records[i],restored.records[i]));}
            Require(restoreError<=1e-5f&&cache.GetStats().builds==assetBuilds,"restored source history differs or asset was rebaked");
            maximumRestoreError=restoreError;Pass("TRS restore regenerates original classification, visibility and source SH");
            VerifyInstanceBVHLifecycle();
            Finish();
        }
    private:
        static float SHError(const Record& a,const Record& b)
        {float result=0;for(size_t i=0;i<4;++i){result=std::max(result,std::abs(a.sh[i].x-b.sh[i].x));result=std::max(result,std::abs(a.sh[i].y-b.sh[i].y));result=std::max(result,std::abs(a.sh[i].z-b.sh[i].z));}return result;}
        static size_t Find(const Snapshot& snapshot,VXGIWorld::Coordinate coordinate)
        {const auto found=std::find(snapshot.coordinates.begin(),snapshot.coordinates.end(),coordinate);Require(found!=snapshot.coordinates.end(),"target fixed probe is absent from sparse allocation");return size_t(found-snapshot.coordinates.begin());}
        void Pass(const char* name){++groups;std::cout<<"PASS "<<name<<'\n';}
        VXGIProbeState::Sources Sources() const
        {return {world.GetGeometryRevision(),world.GetAllocationRevision(),meshScene.GetRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};}
        void UpdateSources()
        {Require(world.Update(instances,.5f),world.GetLastError());Require(meshScene.UpdateInstances(instances,cache,{.25f,64}),meshScene.GetLastError());Require(topology.Update(world),topology.GetLastError());Require(surface.Update(materials,world.GetGeometryRevision(),meshScene.GetRevision(),world.GetMaterialRevision(),sourceParameters,world.GetCoverageRevision()),surface.GetLastError());}
        void PrepareOnly(UINT slot)
        {
            Reset(slot);Require(shared.Prepare(device,int(slot),world,meshScene,&topology),"prepare BVH lifecycle GPU scene");
            const auto& expected=meshScene.GetInstanceBVH().GetNodes();
            Require(shared.Get(int(slot),10)!=nullptr,"empty or populated BVH has no safe root address");
            Require(shared.GetInstanceBVHNodeCount(int(slot))==expected.size(),"BVH root count differs from CPU node count");
            ComPtr<ID3D12Resource> copy;
            if(!expected.empty())
            {
                Require(shared.GetInstanceBVHRevision(int(slot))==meshScene.GetInstanceBVH().GetRevision(),"frame slot retained an obsolete BVH revision");
                copy=CopyForReadback<VXGIInstanceBVH::Node>(shared.Get(int(slot),10),expected.size());
            }
            Submit(slot);Wait(slot);
            if(copy)
            {
                const auto actual=Read<VXGIInstanceBVH::Node>(copy.Get(),expected.size());
                Require(std::memcmp(actual.data(),expected.data(),expected.size()*sizeof(VXGIInstanceBVH::Node))==0,
                    "GPU BVH bounds, instance indices or escape indices differ from the committed CPU tree");
            }
        }
        void VerifyInstanceBVHLifecycle()
        {
            struct SlotState
            {
                VXGIGPUScene::MeshUploadStats uploads;
                ComPtr<ID3D12Resource> bvh;
                uint32_t distanceNodes=0;
            };
            auto snapshot=[&]()
            {
                std::array<SlotState,Slots> result;
                for(UINT slot=0;slot<Slots;++slot)
                {
                    result[slot].uploads=shared.GetMeshUploadStats(int(slot));result[slot].bvh=shared.Get(int(slot),10);
                    result[slot].distanceNodes=shared.GetDistanceNodeCount(int(slot));
                }
                return result;
            };
            const auto initial=snapshot();const auto bakes=cache.GetStats().builds;
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);const auto& uploads=shared.GetMeshUploadStats(int(slot));
                Require(uploads.assetUploads==initial[slot].uploads.assetUploads && uploads.instanceUploads==initial[slot].uploads.instanceUploads &&
                    uploads.materialMapUploads==initial[slot].uploads.materialMapUploads && uploads.instanceBVHUploads==initial[slot].uploads.instanceBVHUploads &&
                    shared.Get(int(slot),10)==initial[slot].bvh.Get() && shared.GetDistanceNodeCount(int(slot))==initial[slot].distanceNodes,
                    "static frame uploaded or replaced unchanged BVH, geometry, instance or material data");
            }
            Pass("static frame slots preserve BVH resources/revisions and explicit distance-node counts");
            instances[0].customMaterialID=1;const auto materialBVHRevision=meshScene.GetInstanceBVH().GetRevision();UpdateSources();
            Require(meshScene.GetInstanceBVH().GetRevision()==materialBVHRevision,"material-only change rebuilt/refit instance BVH");
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);const auto& uploads=shared.GetMeshUploadStats(int(slot));
                Require(uploads.assetUploads==initial[slot].uploads.assetUploads && uploads.instanceUploads==initial[slot].uploads.instanceUploads &&
                    uploads.materialMapUploads==initial[slot].uploads.materialMapUploads+1 && uploads.instanceBVHUploads==initial[slot].uploads.instanceBVHUploads &&
                    shared.Get(int(slot),10)==initial[slot].bvh.Get(),"material-only edit replaced BVH/asset/instance data instead of just material IDs");
            }
            Pass("material-only edits upload material IDs while all three BVH resources remain unchanged");
            const auto beforeMove=snapshot();instances[1].translation.x=1.5f;UpdateSources();
            Require(meshScene.GetInstanceBVH().GetStats().lastUpdateRefit,"TRS did not refit the instance BVH");
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);const auto& uploads=shared.GetMeshUploadStats(int(slot));
                Require(uploads.assetUploads==beforeMove[slot].uploads.assetUploads && uploads.instanceUploads==beforeMove[slot].uploads.instanceUploads+1 &&
                    uploads.materialMapUploads==beforeMove[slot].uploads.materialMapUploads && uploads.instanceBVHUploads==beforeMove[slot].uploads.instanceBVHUploads+1 &&
                    uploads.instanceBVHBytes==beforeMove[slot].uploads.instanceBVHBytes+3*sizeof(VXGIInstanceBVH::Node) &&
                    shared.Get(int(slot),10)!=beforeMove[slot].bvh.Get() && shared.GetInstanceCount(int(slot))==2 && shared.GetAssetCount(int(slot))==2 &&
                    shared.GetDistanceNodeCount(int(slot))==beforeMove[slot].distanceNodes,
                    "TRS did not upload exactly the instance metadata and refitted three-node BVH");
            }
            Pass("TRS updates upload per-slot refitted BVHs with byte-exact GPU readback and reuse distance fields");
            const auto beforeRemove=snapshot();instances.pop_back();UpdateSources();
            Require(meshScene.GetInstanceBVH().GetStats().lastUpdateRebuilt,"removing an instance did not rebuild BVH membership");
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);const auto& uploads=shared.GetMeshUploadStats(int(slot));
                Require(uploads.instanceBVHUploads==beforeRemove[slot].uploads.instanceBVHUploads+1 &&
                    uploads.instanceBVHBytes==beforeRemove[slot].uploads.instanceBVHBytes+sizeof(VXGIInstanceBVH::Node) &&
                    shared.Get(int(slot),10)!=beforeRemove[slot].bvh.Get() && shared.GetInstanceBVHNodeCount(int(slot))==1 &&
                    shared.GetInstanceCount(int(slot))==1 && shared.GetAssetCount(int(slot))==1,
                    "membership rebuild retained old node counts, indices or GPU resources");
            }
            Pass("membership rebuild replaces each GPU tree and publishes the reduced instance/asset/node counts");
            instances.clear();UpdateSources();
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);Require(shared.GetInstanceCount(int(slot))==0 && shared.GetAssetCount(int(slot))==0 &&
                    shared.GetDistanceNodeCount(int(slot))==0 && shared.GetInstanceBVHNodeCount(int(slot))==0,
                    "empty scene retained nonzero root counts");
            }
            const auto empty=snapshot();
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);Require(shared.Get(int(slot),10)==empty[slot].bvh.Get(),"empty static frame replaced its safe dummy BVH buffer");
            }
            instances={{&floor,1},{&cube,2}};instances[1].translation={0,.8f,0};UpdateSources();
            for(UINT slot=0;slot<Slots;++slot)
            {
                PrepareOnly(slot);Require(shared.GetInstanceCount(int(slot))==2 && shared.GetAssetCount(int(slot))==2 &&
                    shared.GetDistanceNodeCount(int(slot))==initial[slot].distanceNodes && shared.GetInstanceBVHNodeCount(int(slot))==3,
                    "restoring empty scene failed to republish complete root counts");
            }
            Require(cache.GetStats().builds==bakes,"BVH lifecycle edits rebaked immutable mesh distance fields");
            Pass("empty count-gated dummy BVH and restored scene are safe across all three frame slots");
        }
        void Reset(UINT slot)
        {Wait(slot);Check(device.GetCommandAllocator(int(slot))->Reset(),"reset edit allocator");Check(device.GetCommandList()->Reset(device.GetCommandAllocator(int(slot)),nullptr),"reset edit command list");Require(!IsWindowVisible(window),"fixture window became visible");}
        uint64_t Submit(UINT slot)
        {Check(device.GetCommandList()->Close(),"close edit commands");ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);const uint64_t value=++device.GetFenceValue(int(slot));Check(device.GetCommandQueue()->Signal(device.GetFence(int(slot)),value),"signal edit commands");++submissions;return value;}
        void Wait(UINT slot){device.WaitForPreviousFrame(int(slot));Check(device.GetDevice()->GetDeviceRemovedReason(),"edit device status");}
        ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initial)
        {const auto properties=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes);ComPtr<ID3D12Resource> resource;Check(device.GetDevice()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,initial,nullptr,IID_PPV_ARGS(&resource)),"create edit buffer");return resource;}
        void InitializeMaterialResources()
        {
            const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,1,1,1,1);
            constexpr std::array<XMFLOAT4,4> pixels={XMFLOAT4{1,1,1,1},XMFLOAT4{.5f,.5f,1,1},XMFLOAT4{1,1,0,1},XMFLOAT4{1,1,1,1}};
            for(UINT i=0;i<4;++i)
            {
                Check(device.GetDevice()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[i])),"create edit texture");
                auto upload=Buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map edit texture");std::memcpy(mapped,&pixels[i],sizeof(XMFLOAT4));upload->Unmap(0,nullptr);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};footprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,1,1,1,256};
                const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprint),destination(textures[i].Get(),0);device.GetCommandList()->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);device.GetCommandList()->ResourceBarrier(1,&barrier);
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
                auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(i)*device.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);device.GetDevice()->CreateShaderResourceView(textures[i].Get(),&view,cpu);staging.push_back(upload);
            }
            const size_t materialBytes=materials.size()*sizeof(VXGISurface::MaterialRecord);
            auto upload=Buffer(materialBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map edit materials");std::memcpy(mapped,materials.data(),materialBytes);upload->Unmap(0,nullptr);
            materialBuffer=Buffer(materialBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
            const auto before=CD3DX12_RESOURCE_BARRIER::Transition(materialBuffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);device.GetCommandList()->ResourceBarrier(1,&before);device.GetCommandList()->CopyBufferRegion(materialBuffer.Get(),0,upload.Get(),0,materialBytes);
            const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(materialBuffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);device.GetCommandList()->ResourceBarrier(1,&barrier);staging.push_back(upload);
        }
        template<class T> ComPtr<ID3D12Resource> CopyForReadback(ID3D12Resource* source,size_t count)
        {
            auto copy=Buffer(count*sizeof(T),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto before=CD3DX12_RESOURCE_BARRIER::Transition(source,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);device.GetCommandList()->ResourceBarrier(1,&before);
            device.GetCommandList()->CopyBufferRegion(copy.Get(),0,source,0,count*sizeof(T));const auto after=CD3DX12_RESOURCE_BARRIER::Transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);device.GetCommandList()->ResourceBarrier(1,&after);return copy;
        }
        template<class T> static std::vector<T> Read(ID3D12Resource* resource,size_t count)
        {void* mapped=nullptr;const D3D12_RANGE reads{0,count*sizeof(T)};Check(resource->Map(0,&reads,&mapped),"map edit snapshot");const auto* first=static_cast<const T*>(mapped);std::vector<T> result(first,first+count);const D3D12_RANGE writes{0,0};resource->Unmap(0,&writes);return result;}
        Snapshot Frame(UINT slot)
        {
            Reset(slot);Require(state.Prepare(Sources(),parameters),state.GetLastError());Snapshot result;result.plan=state.GetPendingPlan();
            Require(shared.Prepare(device,int(slot),world,meshScene,&topology),"prepare moved Mesh GPU scene");Require(probes.Record(device,heap.Get(),int(slot),shared,unusedLegacy,surface,materialBuffer->GetGPUVirtualAddress(),state),"record Mesh production probes");
            auto* raw=probes.GetDisplayHistory();Require(raw,"production RAW missing");Require(filter.Record(device,heap.Get(),int(slot),shared,unusedLegacy,raw,state,.5f,.25f,surface,materialBuffer.Get()),"record Mesh production visibility/filter");
            const size_t count=world.GetOccupiedCenters().size();auto rawCopy=CopyForReadback<Record>(raw,count),filteredCopy=CopyForReadback<Record>(filter.GetFilteredRecords(),count);auto edgeCopy=CopyForReadback<uint32_t>(filter.GetVisibilityMasks(),count);
            const auto fence=Submit(slot);Require(probes.OnSubmitted(int(slot),fence,state)&&filter.OnSubmitted(int(slot),fence),"commit Mesh edit update");Wait(slot);
            result.records=Read<Record>(rawCopy.Get(),count);result.filtered=Read<Record>(filteredCopy.Get(),count);result.edges=Read<uint32_t>(edgeCopy.Get(),count);for(const auto& cell:world.GetCells())result.coordinates.push_back(cell.coordinate);return result;
        }
        static void VerifyComplete(const Snapshot& snapshot)
        {
            for(size_t i=0;i<snapshot.records.size();++i)
            {
                const auto& record=snapshot.records[i];Require(record.state!=VXGIProbeState::Unclassified&&record.reason<VXGIProbeState::TraceFailure,"full scene contains unclassified or failed probe");
                Require((snapshot.edges[i]&Known)!=0&&(snapshot.edges[i]&0x3f00u)==0,"full scene contains unknown/failed neighbor visibility");
                for(const auto& coefficient:record.sh)Require(std::isfinite(coefficient.x)&&std::isfinite(coefficient.y)&&std::isfinite(coefficient.z)&&coefficient.w==0,"nonfinite or invalid SH layout");
                if(record.state==VXGIProbeState::Valid)Require(record.updateCount>0&&record.lastUpdateFrame==snapshot.plan.frameIndex,"valid probe has stale source capture");
            }
        }
        void Finish()
        {
            uint32_t warnings=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;Check(info->GetMessage(i,nullptr,&bytes),"measure edit validation message");std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(i,message,&bytes),"read edit validation message");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){++warnings;std::cerr<<message->pDescription<<'\n';}}
            Require(warnings==0&&!IsWindowVisible(window),"Mesh edit fixture reported validation warnings or visible UI");
            std::ofstream file("tmp/VXGIMeshSDFEditGPU-report.json");
            file<<"{\n  \"adapter\":\""<<adapterName<<"\",\n  \"passed\":true,\n  \"groups\":"<<groups<<",\n  \"submissions\":"<<submissions<<",\n  \"frame_slots\":3,\n  \"asset_builds\":"<<cache.GetStats().builds<<",\n  \"changed_common_probe_states\":"<<changedStateCount<<",\n  \"changed_common_probe_source_sh\":"<<changedSourceCount<<",\n  \"maximum_restore_sh_error\":"<<maximumRestoreError<<",\n  \"debug_layer\":true,\n  \"gpu_based_validation\":true,\n  \"d3d12_warnings_errors\":0,\n  \"window_visible\":false,\n  \"present_calls\":0,\n  \"scope\":\"Synthetic floor and movable emissive cube; actual World/MeshScene/Topology/Surface/ProbeState plus three-slot Mesh GPU Prepare/ProbePass/FilterPass. Tests partial reset, reclassification, cached visibility/source refresh, stale pending plans/old GPU snapshots, immutable asset upload reuse and restore. No main editor, smooth-motion image, or full-frame timing acceptance.\"\n}\n";
            Require(bool(file),"write Mesh edit report");std::cout<<"PASS Mesh edit GPU acceptance: "<<groups<<" groups, 0 validation warnings/errors\n";
        }
        HWND window=nullptr;RenderDevice device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12DescriptorHeap> heap;std::array<ComPtr<ID3D12Resource>,4> textures;ComPtr<ID3D12Resource> materialBuffer;std::vector<ComPtr<ID3D12Resource>> staging;
        Model floor,cube;std::vector<Instance> instances;std::vector<VXGISurface::MaterialRecord> materials;VXGIWorld world;VXGISDF unusedLegacy;VXGIMeshSDFCache cache;VXGIMeshSDFScene meshScene;VXGIProbeTopology topology;VXGISurface surface;VXGIGPUScene shared;VXGIProbeState state;VXGIProbeState::Parameters parameters;VXGISurface::Parameters sourceParameters;VXGIProbePass probes;VXGIProbeFilterPass filter;
        std::string adapterName;uint32_t groups=0,submissions=0,changedStateCount=0,changedSourceCount=0;float maximumRestoreError=0;
    };
}
int main()
{
    try{ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get edit debug layer");debug->EnableDebugLayer();ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get edit GPU validation");validation->SetEnableGPUBasedValidation(TRUE);Fixture fixture;fixture.Run();return 0;}
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}
}
