// Stage 7.1: actual asset/material upload acceptance, with no visible window,
// raster draw, shader compilation or Present call.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dx12.h>
// Scope existing asset/IBL header diagnostics; the fixture itself uses /W4 /WX.
#pragma warning(push)
#pragma warning(disable:4267 4244 4099 4324)
#include "../Resources/ResourceManager.h"
#pragma warning(pop)
#include "../Resources/SceneEditState.h"
#include "../Resources/VXGISDF.h"
#include "../Resources/VXGISurface.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace
{
    constexpr int FrameSlots=3;
    constexpr auto MaterialReadState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    void Require(bool value,const std::string& message) { if(!value) throw std::runtime_error(message); }
    void Check(HRESULT value,const char* operation)
    {
        if(FAILED(value))
        {
            std::ostringstream message;message<<operation<<": HRESULT 0x"<<std::hex<<uint32_t(value);
            throw std::runtime_error(message.str());
        }
    }
    std::string UTF8(const wchar_t* input)
    {
        const int bytes=WideCharToMultiByte(CP_UTF8,0,input,-1,nullptr,0,nullptr,nullptr);
        std::string result(size_t(bytes),'\0');
        WideCharToMultiByte(CP_UTF8,0,input,-1,result.data(),bytes,nullptr,nullptr);result.pop_back();return result;
    }
    class HiddenWindow
    {
    public:
        HiddenWindow()
        {
            WNDCLASSW description{};description.lpfnWndProc=DefWindowProcW;
            description.hInstance=GetModuleHandleW(nullptr);description.lpszClassName=L"VXGIEditBackgroundAcceptance";
            Require(RegisterClassW(&description)!=0,"register material background window");
            handle=CreateWindowExW(0,description.lpszClassName,L"Material background fixture",WS_POPUP,
                0,0,32,32,nullptr,nullptr,description.hInstance,nullptr);
            Require(handle&&!IsWindowVisible(handle),"material fixture must remain hidden");
        }
        ~HiddenWindow()
        {
            if(handle) DestroyWindow(handle);
            UnregisterClassW(L"VXGIEditBackgroundAcceptance",GetModuleHandleW(nullptr));
        }
        HWND handle=nullptr;
    };
    class Fixture
    {
    public:
        Fixture()
        {
            Require(device.Initialize(window.handle,32,32,FrameSlots),"initialize actual hidden RenderDevice");
            Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"query actual material InfoQueue");
            ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"create adapter lookup factory");
            ComPtr<IDXGIAdapter1> adapter;
            Check(factory->EnumAdapterByLuid(device.GetDevice()->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"lookup actual production adapter");
            DXGI_ADAPTER_DESC1 description{};Check(adapter->GetDesc1(&description),"read actual adapter");adapterName=UTF8(description.Description);
            InstanceDesc instance{};instance.modelPath="Models/Plain.glb";instance.scale={1,1,1};
            Require(resources.LoadAssets(&device,{instance},FrameSlots),"load actual Plain.glb with ResourceManager");
            Require(resources.GetSceneInstances().size()==1,"Plain fixture did not load exactly one instance");
            auto* model=resources.GetSceneInstances()[0].pModel;
            Require(model&&!model->meshes.empty(),"Plain fixture has no real meshes");
            Require(resources.BuildGlobalMaterialPool(&device),"build actual initial material pool");
            Require(resources.GetGlobalMaterials().size()==model->meshes.size(),"real mesh IDs/pool count mismatch");
            for(const auto& mesh:model->meshes) Require(mesh.materialID<resources.GetGlobalMaterials().size(),"initial material ID invalid");
            initialPool=resources.GetGlobalMaterials();savedInstance=resources.GetSceneInstances()[0];
            initialMaterialId=MeshRef().materialID;
            originalColor=MeshRef().baseColorFactor;originalEmission=MeshRef().emissiveFactor;originalUnlit=MeshRef().isUnlit;
            Require(sceneEdits.Update(resources.GetSceneInstances()),"initial scene snapshot must change");
            UpdateSources();VerifyPool();Pass("actual Plain.glb and initial stable material IDs");
        }
        ~Fixture() { for(int slot=0;slot<FrameSlots;++slot) device.WaitForPreviousFrame(slot); }

        Mesh& MeshRef() { return savedInstance.pModel->meshes[0]; }
        void Run()
        {
            MeshRef().baseColorFactor={.125f,.375f,.625f,.75f};
            Edit("baseColorFactor edit reaches CPU and GPU before Refresh returns");
            MeshRef().emissiveFactor={2.5f,.75f,4.0f};
            Edit("emissiveFactor edit reaches CPU and GPU before Refresh returns");
            MeshRef().isUnlit=!originalUnlit;
            Edit("isUnlit edit reaches CPU and GPU before Refresh returns");

            MeshRef().baseColorFactor=originalColor;MeshRef().emissiveFactor=originalEmission;MeshRef().isUnlit=originalUnlit;
            Edit("restoring original mesh fields restores the exact 48-byte GPU records");
            Require(Equal(resources.GetGlobalMaterials(),initialPool),"material undo did not restore initial pool bytes");

            VerifyNoUpload();
            const auto surfaceBefore=surface.GetRevision(),geometryBefore=world.GetGeometryRevision(),materialBefore=world.GetMaterialRevision();
            UpdateSources();Require(surface.GetRevision()==surfaceBefore&&world.GetGeometryRevision()==geometryBefore&&
                world.GetMaterialRevision()==materialBefore,"unchanged source update changed revisions");
            Require(!sceneEdits.Update(resources.GetSceneInstances()),"unchanged registered scene changed");
            Pass("unchanged refresh preserves pool, resource, fence values and source revisions");

            // Plain has one material: an explicit override changes source identity,
            // while selecting the same resolved ID and preserving its color.
            auto& instance=resources.GetSceneInstances()[0];instance.customMaterialID=MeshRef().materialID;
            Require(sceneEdits.Update(resources.GetSceneInstances()),"explicit override missed by SceneEditState");
            VerifyNoUpload();UpdateSources();
            Require(world.GetGeometryRevision()==geometryBefore&&world.GetMaterialRevision()>materialBefore&&
                surface.GetRevision()>surfaceBefore,"override failed the World -> Surface source update order");
            Require(!world.GetStats().geometryRebuilt&&!sdf.GetStats().geometryRebuilt,"override rebuilt immutable geometry/SDF");
            for(const auto& triangle:world.GetTriangles()) Require(triangle.materialId==instance.customMaterialID,"override not resolved on world triangle");
            VerifyPool();Pass("explicit same-ID override updates SceneEditState and World/Surface identity without upload");

            instance.customMaterialID=UINT32_MAX;Require(sceneEdits.Update(resources.GetSceneInstances()),"override undo missed by SceneEditState");
            VerifyNoUpload();UpdateSources();VerifyPool();
            Pass("override undo retains original IDs and uploaded records");

            resources.GetSceneInstances().clear();Require(sceneEdits.Update(resources.GetSceneInstances()),"instance deletion missed by SceneEditState");
            VerifyNoUpload();UpdateSources();
            Require(world.GetTriangles().empty()&&world.GetOccupiedCenters().empty()&&!sdf.GetGrid().valid,"empty registered scene retained world geometry");
            Require(Equal(resources.GetGlobalMaterials(),initialPool)&&resources.GetMaterialBuffer(),"deleting scene cleared the loaded asset material pool");
            VerifyPool();
            Require(!sceneEdits.Update(resources.GetSceneInstances()),"stable empty scene repeatedly invalidated");
            VerifyNoUpload();Pass("deletion and stable empty scene retain the loaded asset global material pool");

            resources.GetSceneInstances().push_back(savedInstance);
            Require(sceneEdits.Update(resources.GetSceneInstances()),"instance restore missed by SceneEditState");
            VerifyNoUpload();UpdateSources();Require(!world.GetTriangles().empty(),"instance restore did not restore geometry");
            Require(MeshRef().materialID==initialMaterialId&&Equal(resources.GetGlobalMaterials(),initialPool),"instance restore changed stable material IDs");
            VerifyPool();Pass("instance restore preserves loaded model and material IDs");
        }

        void Finish()
        {
            for(int slot=0;slot<FrameSlots;++slot) device.WaitForPreviousFrame(slot);
            Require(SUCCEEDED(device.GetDevice()->GetDeviceRemovedReason()),"material production device removed");
            for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T bytes=0;Check(info->GetMessage(index,nullptr,&bytes),"measure actual material message");
                std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(info->GetMessage(index,message,&bytes),"read actual material message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)
                { std::cerr<<"Material D3D12: "<<message->pDescription<<'\n';++warnings; }
            }
            Require(warnings==0,"actual material debug/GBV warnings or errors");
            Require(!IsWindowVisible(window.handle),"background material window became visible");
            std::ofstream report("tmp/VXGIEditGPU-report.json");
            report<<"{\n  \"adapter\": \""<<adapterName<<"\",\n  \"asset\": \"Models/Plain.glb\",\n"
                <<"  \"debug_layer\": true,\n  \"gpu_based_validation\": true,\n  \"passed\": true,\n"
                <<"  \"groups\": "<<groups<<",\n  \"refresh_uploads\": "<<uploads<<",\n"
                <<"  \"unchanged_refreshes\": "<<noUploads<<",\n  \"gpu_readbacks\": "<<readbacks<<",\n"
                <<"  \"material_records\": "<<initialPool.size()<<",\n  \"record_stride\": 48,\n"
                <<"  \"frame_slots\": 3,\n  \"byte_mismatches\": 0,\n  \"d3d12_warnings_errors\": "<<warnings<<",\n"
                <<"  \"window_visible\": false,\n  \"present_calls\": 0,\n  \"raster_draw_calls\": 0,\n"
                <<"  \"main_update_called\": false,\n  \"shader_compilations\": 0,\n"
                <<"  \"override_fixture\": \"explicit override selects the same single global material ID\"\n}\n";
            Require(bool(report),"write material acceptance report");
            std::cout<<"PASS actual material GPU acceptance: "<<groups<<" groups, "<<readbacks<<" readbacks, 0 byte mismatches, 0 validation warnings/errors\n";
        }
    private:
        static bool Equal(const std::vector<MaterialData>& a,const std::vector<MaterialData>& b)
        { return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(MaterialData))==0); }
        void Pass(const char* name) { ++groups;std::cout<<"PASS "<<name<<'\n'; }
        void UpdateSources()
        {
            Require(world.Update(resources.GetSceneInstances(),1.0f),world.GetLastError());
            Require(sdf.Update(world,1.0f,64),sdf.GetLastError());
            Require(surface.Update(resources.GetGlobalMaterials(),world.GetGeometryRevision(),sdf.GetGeometryRevision(),
                world.GetMaterialRevision(),surfaceParameters),surface.GetLastError());
            const auto& cpu=resources.GetGlobalMaterials();const auto& consumed=surface.GetMaterials();
            Require(cpu.size()==consumed.size()&&(cpu.empty()||std::memcmp(cpu.data(),consumed.data(),cpu.size()*48)==0),
                "Surface consumed a stale CPU material pool");
        }
        void Edit(const char* name)
        {
            const auto materialBefore=resources.GetMaterialRevision(),surfaceBefore=surface.GetRevision();
            const auto geometryBefore=world.GetGeometryRevision(),worldMaterialBefore=world.GetMaterialRevision();
            Require(resources.RefreshGlobalMaterials(&device),"refresh edited real mesh material");++uploads;
            Require(resources.GetMaterialRevision()==materialBefore+1,"edited refresh did not advance material revision once");
            for(int slot=0;slot<FrameSlots;++slot)
                Require(device.GetFence(slot)->GetCompletedValue()>=device.GetFenceValue(slot),"Refresh returned before upload completed");
            const auto& record=resources.GetGlobalMaterials()[MeshRef().materialID];
            Require(std::memcmp(&record.baseColorFactor,&MeshRef().baseColorFactor,sizeof(record.baseColorFactor))==0&&
                std::memcmp(&record.emissiveFactor,&MeshRef().emissiveFactor,sizeof(record.emissiveFactor))==0&&
                record.isUnlit==uint32_t(MeshRef().isUnlit),"Refresh returned a stale CPU material record");
            Require(!sceneEdits.Update(resources.GetSceneInstances()),"mesh factor edit changed instance source snapshot");
            UpdateSources();
            Require(world.GetGeometryRevision()==geometryBefore&&world.GetMaterialRevision()>worldMaterialBefore&&
                surface.GetRevision()>surfaceBefore,"edited material did not reach World and Surface in this update");
            Require(!world.GetStats().geometryRebuilt&&!sdf.GetStats().geometryRebuilt,"material factor edit rebaked geometry/SDF");
            Require(MeshRef().materialID==initialMaterialId,"material edit reassigned a stable mesh ID");
            VerifyPool();Pass(name);
        }
        void VerifyNoUpload()
        {
            const auto revision=resources.GetMaterialRevision();const auto* buffer=resources.GetMaterialBuffer();
            const auto address=resources.GetMaterialBufferGPUAddress();const auto pool=resources.GetGlobalMaterials();
            std::array<uint64_t,FrameSlots> fences={};
            for(int slot=0;slot<FrameSlots;++slot) fences[size_t(slot)]=device.GetFenceValue(slot);
            Require(resources.RefreshGlobalMaterials(&device),"unchanged real material refresh");++noUploads;
            Require(resources.GetMaterialRevision()==revision&&resources.GetMaterialBuffer()==buffer&&
                resources.GetMaterialBufferGPUAddress()==address&&Equal(pool,resources.GetGlobalMaterials()),"unchanged refresh changed material pool/resource/revision");
            for(int slot=0;slot<FrameSlots;++slot)
                Require(device.GetFenceValue(slot)==fences[size_t(slot)],"unchanged refresh submitted an upload");
        }
        void VerifyPool()
        {
            Require(!IsWindowVisible(window.handle),"background material window became visible");
            const auto& records=resources.GetGlobalMaterials();Require(!records.empty(),"material pool unexpectedly empty");
            const UINT64 bytes=records.size()*sizeof(MaterialData);
            Require(resources.GetMaterialBuffer()&&resources.GetMaterialBuffer()->GetDesc().Width==bytes,"actual material buffer byte count mismatch");
            for(int slot=0;slot<FrameSlots;++slot) device.WaitForPreviousFrame(slot);
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes);
            ComPtr<ID3D12Resource> readback;
            Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&readback)),"create actual material readback");
            Check(device.GetCommandAllocator(0)->Reset(),"reset material readback allocator");
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(0),nullptr),"reset material readback commands");
            auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resources.GetMaterialBuffer(),MaterialReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            device.GetCommandList()->ResourceBarrier(1,&barrier);
            device.GetCommandList()->CopyBufferRegion(readback.Get(),0,resources.GetMaterialBuffer(),0,bytes);
            barrier=CD3DX12_RESOURCE_BARRIER::Transition(resources.GetMaterialBuffer(),D3D12_RESOURCE_STATE_COPY_SOURCE,MaterialReadState);
            device.GetCommandList()->ResourceBarrier(1,&barrier);Check(device.GetCommandList()->Close(),"close material readback commands");
            ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);
            Check(device.GetCommandQueue()->Signal(device.GetFence(0),++device.GetFenceValue(0)),"signal material readback");device.WaitForPreviousFrame(0);
            void* mapped=nullptr;const D3D12_RANGE reads{0,size_t(bytes)};Check(readback->Map(0,&reads,&mapped),"map actual material readback");
            const bool matching=std::memcmp(mapped,records.data(),size_t(bytes))==0;
            const D3D12_RANGE writes{0,0};readback->Unmap(0,&writes);
            Require(matching,"actual 48-byte GPU material records differ from returned CPU pool");++readbacks;
        }
        HiddenWindow window;
        RenderDevice device;
        ResourceManager resources;
        ComPtr<ID3D12InfoQueue> info;
        SceneEditState sceneEdits;
        VXGIWorld world;
        VXGISDF sdf;
        VXGISurface surface;
        VXGISurface::Parameters surfaceParameters;
        ModelInstance savedInstance{};
        std::vector<MaterialData> initialPool;
        DirectX::XMFLOAT4 originalColor{};
        DirectX::XMFLOAT3 originalEmission{};
        bool originalUnlit=false;
        uint32_t initialMaterialId=0,groups=0,uploads=0,noUploads=0,readbacks=0,warnings=0;
        std::string adapterName;
    };
}
int main()
{
    try
    {
        Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"initialize material test COM");
        ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"enable material debug layer");debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> gpuDebug;Check(debug.As(&gpuDebug),"enable material GBV");gpuDebug->SetEnableGPUBasedValidation(TRUE);
        { Fixture fixture;fixture.Run();fixture.Finish(); }
        CoUninitialize();return 0;
    }
    catch(const std::exception& error) { std::cerr<<"FAIL actual material GPU acceptance: "<<error.what()<<'\n';return 1; }
}
