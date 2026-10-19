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
#include "../Resources/VXGIProbeCache.h"
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
        VXGIProbeState::PendingPlan plan;std::vector<VXGIProbeState::WorkItem> work;
    };
    class Fixture
    {
    public:
        Fixture()
        {
            WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"VXGILazyProbeAcceptance";
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
            parameters.probesPerFrame=4096;parameters.classifyRays=64;parameters.raysPerUpdate=128;parameters.maxHistoryUpdates=2;parameters.rayMaxDistance=4;
            sourceParameters.lightColor={0,0,0};sourceParameters.maxRayDistance=parameters.rayMaxDistance;
            floor=Floor();cube=Cube();instances={{&floor,1},{&cube,2},{&floor,3},{&cube,4}};instances[1].translation={0,.8f,0};instances[2].translation={20,0,0};instances[3].translation={20,.8f,0};
            materials.resize(2);for(auto& material:materials){material.albedoIdx=0;material.normalIdx=1;material.ormIdx=2;material.emissiveIdx=3;}
            materials[0].baseColorFactor={0,0,0,1};materials[1].emissiveFactor={8,2,1};
            Check(device.GetCommandList()->Close(),"close empty device initialization");
            Reset(0);InitializeMaterialResources();Submit(0);Wait(0);staging.clear();UpdateSources();
        }
        ~Fixture()
        {for(UINT slot=0;slot<Slots;++slot)if(device.GetFence(int(slot)))device.WaitForPreviousFrame(int(slot));if(window)DestroyWindow(window);UnregisterClassW(L"VXGILazyProbeAcceptance",GetModuleHandleW(nullptr));}
        void Run()
        {
            auto initial=Converge();
            Require(!initial.records.empty(),"initial history missing");
            uint32_t valid=0,disabled=0;
            for(const auto& record:initial.records)
            {
                Require(record.state!=VXGIProbeState::Unclassified,"probe never classified");
                if(record.state==VXGIProbeState::Valid){++valid;Require(record.updateCount==2,"valid probe failed finite target");}
                else{++disabled;Require(record.updateCount==0,"disabled probe was repeatedly captured");}
            }
            Require(valid&&disabled,"fixture requires valid and disabled probes");
            initialValid=valid;initialDisabled=disabled;Pass("finite initial target and disabled probes stop updating");
            const auto idleBefore=probes.GetActivityStats();const auto filterBefore=filter.GetActivityStats();const auto history=probes.GetDisplayHistory();
            for(uint32_t frame=0;frame<64;++frame)
            {
                const auto idle=Frame(frame%Slots);
                Require(!idle.plan.HasWork(),"static cache scheduled more work");
                Require(SameRecords(initial.records,idle.records),"idle raw SH/classification/counters changed");
                Require(SameRecords(initial.filtered,idle.filtered)&&initial.edges==idle.edges,"idle derived SH or neighbor visibility changed");
                Require(probes.GetDisplayHistory()==history,"idle cache changed persistent history buffer");
            }
            const auto idleAfter=probes.GetActivityStats();
            Require(idleAfter.historyCopies==idleBefore.historyCopies&&idleAfter.remapDispatches==idleBefore.remapDispatches&&
                idleAfter.invalidationDispatches==idleBefore.invalidationDispatches&&idleAfter.updateDispatches==idleBefore.updateDispatches&&
                idleAfter.feedbackCopies==idleBefore.feedbackCopies&&idleAfter.legacySnapshotCopies==idleBefore.legacySnapshotCopies,
                "idle production pass copied, dispatched or read back");
            const auto filterAfter=filter.GetActivityStats();
            Require(filterAfter.activeBatches==filterBefore.activeBatches&&filterAfter.remapDispatches==filterBefore.remapDispatches&&
                filterAfter.invalidationDispatches==filterBefore.invalidationDispatches&&filterAfter.resetDispatches==filterBefore.resetDispatches&&
                filterAfter.visibilityDispatches==filterBefore.visibilityDispatches&&filterAfter.filterDispatches==filterBefore.filterDispatches,
                "idle filter pass dispatched GPU work");
            Pass("64 idle frames retain bit-exact raw SH with zero probe copies/dispatches/readbacks");
            const auto oldDomain=probeCache.GetDomain().generation;const auto bakes=cache.GetStats().builds;
            instances[1].translation.x=.5f;UpdateSources();
            Require(probeCache.GetDomain().generation==oldDomain,"small movement expanded dependency domain");
            Require(!lastDirtyClass.empty()&&lastDirtyClass.size()<world.GetCells().size(),"known dependencies failed local invalidation");
            const auto moved=Converge();
            farRetained=VerifyFarRetained(initial,moved);
            Require(farRetained>0&&cache.GetStats().builds==bakes,"local move failed far cache preservation or rebaked assets");
            Pass("small model move invalidates old/new dependency paths and preserves distant cached SH");

            // Explicit same-count reorder checks remapping independently of any
            // allocation-size change or geometry event.
            auto permutation=moved.coordinates;std::reverse(permutation.begin(),permutation.end());
            std::vector<uint32_t> remap(permutation.size());
            for(uint32_t i=0;i<remap.size();++i)remap[i]=uint32_t(remap.size()-1-i);
            Require(state.ConfigureCache(layoutRevision+1,remap,{}),state.GetLastError());
            const auto reordered=Frame(0,true,false);
            Require(reordered.plan.layoutChanged&&reordered.records.size()==moved.records.size(),"same-count layout did not dispatch remap");
            for(size_t i=0;i<remap.size();++i)Require(std::memcmp(&reordered.records[i],&moved.records[remap[i]],sizeof(Record))==0,"same-count remap did not preserve exact records");
            Require(state.ConfigureCache(layoutRevision,remap,{}),state.GetLastError());
            const auto restored=Frame(1,true,false);Require(SameRecords(restored.records,moved.records),"reverse remap did not restore exact history");
            Pass("same-count stable history remap preserves complete GPU records");

            materials[1].emissiveFactor={2,8,1};UploadMaterials();UpdateSources();
            Require(lastDirtyClass.empty()&&!lastDirtySH.empty(),"opaque material edit reset classification or scheduled no SH");
            const auto material=Converge();VerifyClassification(restored,material);
            Pass("material payload invalidates dependent SH while preserving probe classification");
            sourceParameters.lightColor={1,1,1};sourceParameters.lightDirection={0,-1,0};parameters.probesPerFrame=17;UpdateSources();
            Require(lastDirtyClass.empty()&&lastDirtySH.size()==world.GetCells().size(),"solar edit must invalidate all SH only");
            const auto partial=Frame(0);
            Require(partial.work.size()==17&&partial.work.size()<partial.records.size(),"low-budget batch did not honor the selected probe budget");
            std::vector<bool> selected(partial.records.size());for(const auto& work:partial.work)selected[work.probeIndex]=true;
            uint32_t resetOutsideBatch=0;
            for(size_t i=0;i<partial.records.size();++i)if(!selected[i])
            {
                const auto& record=partial.records[i];
                Require(record.updateCount==0&&record.sampleCount==0,"dirty SH outside current budget retained old accumulation");
                const std::array<XMFLOAT4,4> zero={};
                Require(std::memcmp(record.sh,zero.data(),sizeof(record.sh))==0,"dirty SH outside current budget retained stale coefficients");
                ++resetOutsideBatch;
            }
            Require(resetOutsideBatch>0,"partial invalidation did not exercise unscheduled dirty probes");
            VerifyClassification(material,partial);Pass("17-probe budget resets all stale SH immediately while retaining classification");
            const auto lit=Converge();VerifyClassification(material,lit);
            parameters.probesPerFrame=4096;
            Pass("global solar change refreshes SH and retains classification");

            // Submit a batch but deliberately withhold polling. A new event
            // invalidates the generation before its dependency feedback arrives.
            sourceParameters.lightColor={2,1,1};UpdateSources();const auto stale=Frame(0,false);
            Require(!stale.work.empty(),"stale feedback fixture submitted no work");
            sourceParameters.lightColor={3,1,1};UpdateSources();
            for(const auto& work:stale.work)if(state.GetGeneration(work.probeIndex)!=work.generation)++staleRejected;
            Require(staleRejected>0,"event did not supersede in-flight work generations");
            Require(probes.Poll(device,0,state),"poll obsolete sparse batch");
            const auto dependencies=probes.TakeCompletedDependencies();
            for(const auto& result:dependencies)Require(state.GetGeneration(result.probeIndex)==result.generation,"obsolete dependency feedback escaped production filtering");
            Converge();Pass("in-flight generation changes reject obsolete sparse dependency feedback");
            Finish();
        }
    private:
        static bool SameRecords(const std::vector<Record>& a,const std::vector<Record>& b)
        {return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(Record))==0);}
        static size_t Find(const Snapshot& snapshot,VXGIWorld::Coordinate coordinate)
        {const auto found=std::find(snapshot.coordinates.begin(),snapshot.coordinates.end(),coordinate);Require(found!=snapshot.coordinates.end(),"retained world cell missing");return size_t(found-snapshot.coordinates.begin());}
        static uint32_t VerifyFarRetained(const Snapshot& before,const Snapshot& after)
        {uint32_t retained=0;for(size_t i=0;i<before.coordinates.size();++i)if(before.coordinates[i].x>25){const auto j=Find(after,before.coordinates[i]);Require(std::memcmp(&before.records[i],&after.records[j],sizeof(Record))==0,"distant retained cache changed");++retained;}return retained;}
        static void VerifyClassification(const Snapshot& before,const Snapshot& after)
        {Require(before.coordinates==after.coordinates,"SH event changed allocation");for(size_t i=0;i<before.records.size();++i){const auto& a=before.records[i];const auto& b=after.records[i];Require(a.state==b.state&&a.reason==b.reason&&a.classificationRays==b.classificationRays&&a.backfaceRatio==b.backfaceRatio&&a.minHitDistance==b.minHitDistance,"SH event changed classification");}}
        void Pass(const char* name){++groups;std::cout<<"PASS "<<name<<'\n';}
        VXGIProbeState::Sources Sources() const
        {return {world.GetGeometryRevision(),world.GetAllocationRevision(),meshScene.GetRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};}
        static uint64_t HashMaterial(const VXGISurface::MaterialRecord& material)
        {uint64_t value=1469598103934665603ull;const auto* bytes=reinterpret_cast<const unsigned char*>(&material);for(size_t i=0;i<sizeof(material);++i){value^=bytes[i];value*=1099511628211ull;}return value;}
        void UpdateSources()
        {
            Require(world.Update(instances,.5f),world.GetLastError());Require(meshScene.UpdateInstances(instances,cache,{.25f,64}),meshScene.GetLastError());
            Require(topology.Update(world),topology.GetLastError());Require(surface.Update(materials,world.GetGeometryRevision(),meshScene.GetRevision(),world.GetMaterialRevision(),sourceParameters,world.GetCoverageRevision()),surface.GetLastError());
            Require(probeCache.Prepare(world,meshScene,surface,state,parameters),probeCache.GetLastError());
            const auto& change=probeCache.GetChange();lastDirtyClass=change.dirtyClassification;lastDirtySH=change.dirtySH;
            if(change.topologyChanged)++layoutRevision;
            probes.SetDependencyDomain(probeCache.GetDomain());
            if(filterTopologyRevision!=topology.GetRevision()){filter.SetTopology(topology.GetNeighbors());filterTopologyRevision=topology.GetRevision();}
        }
        void PollAndDrain(UINT slot)
        {
            Require(probes.Poll(device,int(slot),state),"poll sparse probe feedback");
            probeCache.AcceptDependencies(probes.TakeCompletedDependencies(),state);
        }
        void UploadMaterials()
        {
            Reset(2);const auto bytes=materials.size()*sizeof(VXGISurface::MaterialRecord);
            auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map changed material payload");std::memcpy(mapped,materials.data(),bytes);upload->Unmap(0,nullptr);
            const auto before=CD3DX12_RESOURCE_BARRIER::Transition(materialBuffer.Get(),VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_DEST);device.GetCommandList()->ResourceBarrier(1,&before);device.GetCommandList()->CopyBufferRegion(materialBuffer.Get(),0,upload.Get(),0,bytes);
            const auto after=CD3DX12_RESOURCE_BARRIER::Transition(materialBuffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);device.GetCommandList()->ResourceBarrier(1,&after);Submit(2);Wait(2);
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
        Snapshot Frame(UINT slot,bool accept=true,bool prepareCache=true)
        {
            Reset(slot);PollAndDrain(slot);if(prepareCache)Require(probeCache.Prepare(world,meshScene,surface,state,parameters),probeCache.GetLastError());else Require(state.Prepare(Sources(),parameters),state.GetLastError());Snapshot result;result.plan=state.GetPendingPlan();result.work.assign(state.GetPendingWork().begin(),state.GetPendingWork().end());
            Require(shared.Prepare(device,int(slot),world,meshScene,&topology),"prepare lazy GPU scene");Require(probes.Record(device,heap.Get(),int(slot),shared,unusedLegacy,surface,materialBuffer->GetGPUVirtualAddress(),state),"record lazy production probes");
            auto* raw=probes.GetDisplayHistory();Require(raw,"production RAW missing");Require(filter.Record(device,heap.Get(),int(slot),shared,unusedLegacy,raw,state,.5f,.25f,surface,materialBuffer.Get()),"record lazy filter");
            const size_t count=world.GetCells().size();auto rawCopy=CopyForReadback<Record>(raw,count);auto filteredCopy=CopyForReadback<Record>(filter.GetFilteredRecords(),count);auto edgeCopy=CopyForReadback<uint32_t>(filter.GetVisibilityMasks(),count);
            const auto fence=Submit(slot);Require(probes.OnSubmitted(int(slot),fence,state)&&filter.OnSubmitted(int(slot),fence),"commit lazy production update");
            probeCache.OnSubmitted(result.work);Wait(slot);
            result.records=Read<Record>(rawCopy.Get(),count);result.filtered=Read<Record>(filteredCopy.Get(),count);result.edges=Read<uint32_t>(edgeCopy.Get(),count);for(const auto& cell:world.GetCells())result.coordinates.push_back(cell.coordinate);
            if(accept)PollAndDrain(slot);return result;
        }
        Snapshot Converge()
        {
            for(uint32_t frame=0;frame<512;++frame)
            {auto snapshot=Frame(frame%Slots);if(!snapshot.plan.HasWork())return snapshot;}
            throw std::runtime_error("lazy scene did not converge to idle");
        }
        void Finish()
        {
            uint32_t warnings=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;Check(info->GetMessage(i,nullptr,&bytes),"measure lazy validation message");std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(i,message,&bytes),"read lazy validation message");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){++warnings;std::cerr<<message->pDescription<<'\n';}}
            Require(warnings==0&&!IsWindowVisible(window),"lazy fixture reported validation warnings or visible UI");
            const auto stats=probes.GetActivityStats();std::ofstream file("tmp/VXGILazyProbeGPU-report.json");
            file<<"{\n  \"passed\":true,\n  \"groups\":"<<groups<<",\n  \"adapter\":\""<<adapterName<<"\",\n  \"submissions\":"<<submissions<<",\n  \"initial_valid\":"<<initialValid<<",\n  \"initial_disabled\":"<<initialDisabled<<",\n  \"idle_frames\":64,\n  \"far_records_bitexact\":"<<farRetained<<",\n  \"stale_work_rejected\":"<<staleRejected<<",\n  \"probe_active_batches\":"<<stats.activeBatches<<",\n  \"probe_update_dispatches\":"<<stats.updateDispatches<<",\n  \"gpu_validation_warnings_errors\":0,\n  \"window_visible\":false,\n  \"present_calls\":0\n}\n";
            Require(bool(file),"write lazy report");std::cout<<"PASS lazy probe GPU acceptance: "<<groups<<" groups, 0 validation warnings/errors\n";
        }
        HWND window=nullptr;RenderDevice device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12DescriptorHeap> heap;std::array<ComPtr<ID3D12Resource>,4> textures;ComPtr<ID3D12Resource> materialBuffer;std::vector<ComPtr<ID3D12Resource>> staging;
        Model floor,cube;std::vector<Instance> instances;std::vector<VXGISurface::MaterialRecord> materials;VXGIWorld world;VXGISDF unusedLegacy;VXGIMeshSDFCache cache;VXGIMeshSDFScene meshScene;VXGIProbeTopology topology;VXGISurface surface;VXGIGPUScene shared;VXGIProbeState state;VXGIProbeState::Parameters parameters;VXGISurface::Parameters sourceParameters;VXGIProbePass probes;VXGIProbeFilterPass filter;VXGIProbeCache probeCache;
        uint64_t layoutRevision=0,filterTopologyRevision=0;std::vector<uint32_t> lastDirtyClass,lastDirtySH;
        std::string adapterName;uint32_t groups=0,submissions=0,initialValid=0,initialDisabled=0,farRetained=0,staleRejected=0;
    };
}
int main()
{
    try{ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"get lazy debug layer");debug->EnableDebugLayer();ComPtr<ID3D12Debug1> validation;Check(debug.As(&validation),"get lazy GPU validation");validation->SetEnableGPUBasedValidation(TRUE);Fixture fixture;fixture.Run();return 0;}
    catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}
}
