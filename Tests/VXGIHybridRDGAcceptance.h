#pragma once

// Included by VXGIHybridGPUTests.cpp after its shared Scene/SourceScene and
// numeric fixture helpers. This wrapper creates a separate production device,
// resource manager and complete SSGI RDG; it never displays or presents a frame.
#include <DirectXPackedVector.h>
#include "../Resources/VXGIMeshSDFScene.h"

struct VXGIHybridRDGReport
{
    uint32_t submissions=0,slots=3,resizes=0,warnings=0;
    float maxError=0;
    bool hidden=true,present=false;
};

namespace VXGIHybridRDGDetail
{
    using Record=VXGIProbeState::GPURecord;
    class HiddenWindow
    {
    public:
        HiddenWindow()
        {
            WNDCLASSW description{};
            description.lpfnWndProc=DefWindowProcW;
            description.hInstance=GetModuleHandleW(nullptr);
            description.lpszClassName=L"VXGIHybridRDGBackgroundAcceptance";
            Require(RegisterClassW(&description)!=0,"register hybrid background window");
            handle=CreateWindowExW(0,description.lpszClassName,L"SSGI background fixture",WS_POPUP,
                0,0,32,32,nullptr,nullptr,description.hInstance,nullptr);
            Require(handle && !IsWindowVisible(handle),"hybrid fixture must remain hidden");
        }
        ~HiddenWindow()
        {
            if(handle) DestroyWindow(handle);
            UnregisterClassW(L"VXGIHybridRDGBackgroundAcceptance",GetModuleHandleW(nullptr));
        }
        HWND handle=nullptr;
    };

    class Fixture
    {
    public:
        Fixture()
        {
            Require(device.Initialize(window.handle,32,32,3),"initialize hidden hybrid RenderDevice");
            Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"query hybrid production InfoQueue");
            Require(resources.LoadAssets(&device,{},3),"initialize actual empty ResourceManager");
            Require(resources.InitializeBlueNoiseTexture(&device),"initialize actual STBN");
            Require(resources.InitSSGIHistoryResources(&device,16,16),"initialize actual SSGI histories");
            Require(pipelines.InitializeSSGI(&device),"initialize actual SSGI pipelines");
            resources.SealPersistentSrvUavDescriptors();
            sceneGPU.Initialize(3);
            // Every synthetic receiver gets a surface within its original .5 m
            // segment. This lattice avoids the old fixture's 3 m ray extension.
            scene=SourceScene();
            scene.model.meshes[0].vertices.clear();scene.model.meshes[0].lodIndices[0].clear();
            for(int plane=-8;plane<=9;++plane)
            {
                const float x=float(plane)*.25f;
                Quad(scene.model.meshes[0],{x,-3,1.5f},{x,-3,2.5f},{x,3,2.5f},{x,3,1.5f});
            }
            scene.world.Invalidate();scene.Build(.125f);
            Require(scene.topology.Update(scene.world),scene.topology.GetLastError());
            // Production world fallback consumes immutable local Mesh SDF
            // assets plus instance/BVH tables, rather than the legacy global
            // SDF retained by the independent per-ray reference fixture.
            std::vector<TestInstance> instances(1);
            instances[0].pModel=&scene.model;
            Require(meshScene.UpdateInstances(instances,meshCache,{.25f,128}),meshScene.GetLastError());
            Require(meshScene.GetInstances().size()==1&&meshCache.GetStats().builds==1,
                "hybrid fixture must build one immutable Mesh SDF asset and instance");
            Require(!meshScene.GetInstanceBVH().GetNodes().empty(),"hybrid fixture instance BVH is empty");
            for(auto& material:scene.materials)
            {
                material.albedoIdx=resources.GetDummyAlbedoIdx();
                material.normalIdx=resources.GetDummyNormalIdx();
                material.ormIdx=resources.GetDummyORMIdx();
                material.emissiveIdx=resources.GetDummyAlbedoIdx();
            }
            surfaceParameters.lightDirection={1,0,0};
            surfaceParameters.lightColor={};
            surfaceParameters.maxRayDistance=20;
            projection=Stored(DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2,1,.1f,20));
            invProjection=Stored(DirectX::XMMatrixInverse(nullptr,
                DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2,1,.1f,20)));
            view=Stored(DirectX::XMMatrixIdentity());
            FillNoise();
        }
        ~Fixture() { for(int slot=0;slot<3;++slot) device.WaitForPreviousFrame(slot); }

        void Resize(UINT width,UINT height)
        {
            for(int slot=0;slot<3;++slot) device.WaitForPreviousFrame(slot);
            Require(resources.InitSSGIHistoryResources(&device,width,height),"resize SSGI histories");
            history.valid=false;sourceState.Invalidate();
            ++report.resizes;
        }

        void Submit(UINT width,UINT height,uint32_t mode,uint64_t epoch,uint64_t screenStamp,
            XMFLOAT3 expected,bool expectedReset,float expectedHistoryLength,
            bool missingRaw=false,bool missingWorld=false)
        {
            Require(!IsWindowVisible(window.handle),"background SSGI window became visible");
            const int slot=int(report.submissions%3);
            device.WaitForPreviousFrame(slot);
            Require(device.GetFence(slot)->GetCompletedValue()>=device.GetFenceValue(slot),"SSGI slot fence incomplete");
            Check(device.GetCommandAllocator(slot)->Reset(),"reset hybrid production allocator");
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset hybrid production commands");
            resources.BeginPassConstantsFrame(slot);
            resources.ResetTransientSrvUavDescriptors(slot);
            resources.BeginRDGFrame(&device,slot);
            std::vector<ComPtr<ID3D12Resource>> staging;
            Require(surface.Update(scene.materials,scene.world.GetGeometryRevision(),meshScene.GetRevision(),
                scene.world.GetMaterialRevision(),surfaceParameters),surface.GetLastError());
            Require(sceneGPU.Prepare(device,slot,scene.world,meshScene,&scene.topology),"upload actual hybrid Mesh SDF world scene");
            Require(sceneGPU.IsMeshBackend(slot)&&sceneGPU.GetAssetCount(slot)==1&&sceneGPU.GetInstanceCount(slot)==1&&
                sceneGPU.GetDistanceNodeCount(slot)>0&&
                sceneGPU.GetInstanceBVHNodeCount(slot)==meshScene.GetInstanceBVH().GetNodes().size(),
                "hybrid fixture must upload the current Mesh SDF asset, instance and BVH buffers");
            auto materialBuffer=Upload(scene.materials,staging);
            auto raw=Upload(scene.raw,staging);
            auto depth=Texture(width,height,DXGI_FORMAT_R32_FLOAT,4,staging,
                [&](uint8_t* pixel){const float z=DeviceDepth(2);std::memcpy(pixel,&z,sizeof(z));});
            auto normal=Texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,staging,
                [](uint8_t* pixel){const XMFLOAT4 value{1,.5f,.5f,1};std::memcpy(pixel,&value,sizeof(value));});
            auto orm=Texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,staging,
                [](uint8_t* pixel){const XMFLOAT4 value{1,.5f,0,1};std::memcpy(pixel,&value,sizeof(value));});
            auto albedo=Texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,staging,
                [](uint8_t* pixel){const XMFLOAT4 value{1,1,1,1};std::memcpy(pixel,&value,sizeof(value));});
            auto bounce=Texture(width,height,RenderFormats::SceneColor,8,staging,
                [](uint8_t* pixel){const std::array<uint16_t,4> value={0,0,0,DirectX::PackedVector::XMConvertFloatToHalf(1)};
                    std::memcpy(pixel,value.data(),sizeof(value));});
            auto color=Texture(width,height,RenderFormats::SceneColor,8,staging,
                [](uint8_t* pixel){const std::array<uint16_t,4> value={0,0,0,DirectX::PackedVector::XMConvertFloatToHalf(1)};
                    std::memcpy(pixel,value.data(),sizeof(value));},D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            SSGIPass::WorldInput world;
            world.scene=missingWorld?nullptr:&sceneGPU;world.surface=&surface;
            world.materialBuffer=materialBuffer.Get();world.rawRecords=missingRaw?nullptr:raw.Get();
            world.probeCount=uint32_t(scene.raw.size());world.voxelSize=scene.world.GetVoxelSize();world.mode=mode;
            const auto effective=SSGIPass::EffectiveWorldMode(&world,slot);
            Require(effective==(missingWorld||missingRaw?0u:mode),"missing cache must select screen-only");
            SSGIHybridState::Key key;
            key.effectiveWorldMode=effective;key.screenRadius=.5f;key.quality=1;
            key.worldGeometryRevision=effective?scene.world.GetGeometryRevision():0;
            key.sdfGeometryRevision=meshScene.GetRevision();
            key.surfaceRevision=effective?surface.GetRevision():0;
            key.probeResetEpoch=epoch;key.screenSourceStamp=screenStamp;
            const bool reset=sourceState.NeedsReset(key);
            Require(reset==expectedReset,"real SSGI source reset policy differs from plan");
            if(reset) history.valid=false;
            // A recording attempt never commits the pending source key.
            Require(sourceState.NeedsReset(key)==reset,"SSGI source key consumed before submission");
            RDGBuilder graph(&device,"SSGIHybridBackgroundGraph");
            graph.SetTransientResourceAllocator([&](const D3D12_RESOURCE_DESC& description,D3D12_RESOURCE_STATES initial,
                D3D12_RESOURCE_STATES final,const D3D12_CLEAR_VALUE* clear,RDGTransientResourceLease* lease)
                {return resources.AllocateRDGTransientResource(&device,slot,description,initial,final,clear,lease);});
            graph.SetTransientSrvUavDescriptorAllocator([&](UINT* index,D3D12_CPU_DESCRIPTOR_HANDLE* handle)
                {return resources.AllocateTransientSrvUavDescriptor(index,handle);});
            auto import=[&](ID3D12Resource* resource,const char* name)
                {return graph.RegisterExternalTexture(resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,name);};
            SSGIPass::Input input{import(color.Get(),"Fixture.Scene"),import(depth.Get(),"Fixture.Depth"),
                import(normal.Get(),"Fixture.Normal"),import(orm.Get(),"Fixture.ORM"),
                import(albedo.Get(),"Fixture.Albedo"),import(bounce.Get(),"Fixture.ScreenSource")};
            SSGIConfig config;config.enabled=true;config.radius=.5f;config.quality=1;config.intensity=1;
            const auto output=SSGIPass::AddToGraph(graph,&resources,&pipelines,history,view,projection,invProjection,
                invProjection,width,height,slot,report.submissions,config,input,&world);
            Require(output.sceneColor.IsValid()&&output.pass.IsValid(),"real full SSGIPass construction failed");
            graph.MarkTextureAsOutput(output.sceneColor);
            graph.Execute(device.GetCommandList());
            Require(graph.GetCompileStats().compileSucceeded,"real complete SSGI RDG compilation failed");
            const int writtenIndex=resources.GetSSGICurrentHistoryIdx();
            auto gi=CopyTexture(resources.GetSSGIHistoryTextures(writtenIndex).gi.Get());
            auto composed=CopyTexture(color.Get());
            Check(device.GetCommandList()->Close(),"close actual SSGI graph");
            ID3D12CommandList* lists[]={device.GetCommandList()};
            device.GetCommandQueue()->ExecuteCommandLists(1,lists);
            Check(device.GetCommandQueue()->Signal(device.GetFence(slot),++device.GetFenceValue(slot)),"signal actual SSGI graph");
            sourceState.Commit(key);resources.FlipSSGIHistoryIndex();
            history.previousViewProj=projection;history.previousInvViewProj=invProjection;history.valid=true;
            ++report.submissions;
            Require(!sourceState.NeedsReset(key),"submitted SSGI key did not stabilize");
            device.WaitForPreviousFrame(slot);
            Require(SUCCEEDED(device.GetDevice()->GetDeviceRemovedReason()),"SSGI production device removed");
            const auto actual=ReadHalf(gi,width/2,height/2);
            const float error=Error(Color(actual),expected);
            report.maxError=(std::max)(report.maxError,error);
            if(error>=.004f)
            {
                std::ostringstream message;message<<"SSGI RDG color mismatch mode "<<effective<<" submission "<<report.submissions
                    <<" actual "<<actual.x<<','<<actual.y<<','<<actual.z<<" expected "<<expected.x<<','<<expected.y<<','<<expected.z;
                throw std::runtime_error(message.str());
            }
            Require(std::abs(actual.w-expectedHistoryLength)<.01f,"actual temporal alpha did not reflect reset/stable history");
            const auto composite=ReadHalf(composed,width,height);
            Require(std::isfinite(composite.x)&&std::isfinite(composite.y)&&std::isfinite(composite.z)&&composite.w==1,
                "actual composite is nonfinite or changed scene alpha");
            // White metallic-zero receiver, roughness .5 and NdotV=0 produce
            // a diffuse energy factor .5. E/pi is already represented by GI.
            const XMFLOAT3 expectedComposite{expected.x*.5f,expected.y*.5f,expected.z*.5f};
            const float compositeError=Error(Color(composite),expectedComposite);
            report.maxError=(std::max)(report.maxError,compositeError);
            Require(compositeError<.004f,"actual SSGI composite applied the receiver response or pi incorrectly");
        }

        VXGIHybridRDGReport Finish()
        {
            for(int slot=0;slot<3;++slot) device.WaitForPreviousFrame(slot);
            for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
            {
                SIZE_T bytes=0;Check(info->GetMessage(index,nullptr,&bytes),"measure actual SSGI message");
                std::vector<char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(info->GetMessage(index,message,&bytes),"read actual SSGI message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)
                {std::cerr<<"SSGI production D3D12: "<<message->pDescription<<'\n';++report.warnings;}
            }
            Require(report.warnings==0,"actual SSGI debug/GBV warnings or errors");
            report.hidden=!IsWindowVisible(window.handle);Require(report.hidden,"background fixture became visible");
            return report;
        }
        Scene scene;
    private:
        struct Readback { ComPtr<ID3D12Resource> resource;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0; };
        ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(type);const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes);
            ComPtr<ID3D12Resource> result;
            Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,state,nullptr,IID_PPV_ARGS(&result)),"create hybrid fixture buffer");
            return result;
        }
        void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);device.GetCommandList()->ResourceBarrier(1,&barrier);}
        template<class T> ComPtr<ID3D12Resource> Upload(const std::vector<T>& values,std::vector<ComPtr<ID3D12Resource>>& uploads)
        {
            const UINT64 bytes=values.size()*sizeof(T);Require(bytes>0,"empty hybrid fixture upload");
            auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;const D3D12_RANGE noReads{0,0};Check(upload->Map(0,&noReads,&mapped),"map hybrid fixture buffer");
            std::memcpy(mapped,values.data(),size_t(bytes));upload->Unmap(0,nullptr);
            auto result=Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
            Transition(result.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            device.GetCommandList()->CopyResource(result.Get(),upload.Get());Transition(result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);
            uploads.push_back(upload);return result;
        }
        template<class Fill> ComPtr<ID3D12Resource> Texture(UINT width,UINT height,DXGI_FORMAT format,UINT pixelBytes,
            std::vector<ComPtr<ID3D12Resource>>& uploads,Fill fill,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            const auto description=CD3DX12_RESOURCE_DESC::Tex2D(format,width,height,1,1,1,0,flags);
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> result;
            Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"create synthetic GBuffer texture");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
            device.GetDevice()->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&bytes);
            auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;
            const D3D12_RANGE noReads{0,0};Check(upload->Map(0,&noReads,&mapped),"map synthetic GBuffer");
            for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x)
                fill(static_cast<uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+size_t(x)*pixelBytes);
            upload->Unmap(0,nullptr);const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprint),destination(result.Get(),0);
            device.GetCommandList()->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
            Transition(result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);uploads.push_back(upload);return result;
        }
        void FillNoise()
        {
            Check(device.GetCommandAllocator(0)->Reset(),"reset STBN allocator");
            Check(device.GetCommandList()->Reset(device.GetCommandAllocator(0),nullptr),"reset STBN commands");
            auto* noise=resources.GetBlueNoiseTexture();const auto description=noise->GetDesc();
            const UINT count=description.DepthOrArraySize*description.MipLevels;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);UINT64 bytes=0;
            device.GetDevice()->GetCopyableFootprints(&description,0,count,0,footprints.data(),nullptr,nullptr,&bytes);
            auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;
            const D3D12_RANGE noReads{0,0};Check(upload->Map(0,&noReads,&mapped),"map deterministic STBN");
            for(const auto& footprint:footprints) for(UINT y=0;y<footprint.Footprint.Height;++y)
                for(UINT x=0;x<footprint.Footprint.Width;++x)
                {auto* pixel=static_cast<uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+x*2;pixel[0]=0;pixel[1]=128;}
            upload->Unmap(0,nullptr);Transition(noise,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            for(UINT index=0;index<count;++index)
            {const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(),footprints[index]),destination(noise,index);device.GetCommandList()->CopyTextureRegion(&destination,0,0,0,&source,nullptr);}
            Transition(noise,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Check(device.GetCommandList()->Close(),"close STBN commands");ID3D12CommandList* lists[]={device.GetCommandList()};
            device.GetCommandQueue()->ExecuteCommandLists(1,lists);Check(device.GetCommandQueue()->Signal(device.GetFence(0),++device.GetFenceValue(0)),"signal deterministic STBN");
            device.WaitForPreviousFrame(0);
        }
        Readback CopyTexture(ID3D12Resource* source)
        {
            Readback result;const auto description=source->GetDesc();
            device.GetDevice()->GetCopyableFootprints(&description,0,1,0,&result.footprint,nullptr,nullptr,&result.bytes);
            result.resource=Buffer(result.bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            const CD3DX12_TEXTURE_COPY_LOCATION src(source,0),dst(result.resource.Get(),result.footprint);
            device.GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);return result;
        }
        static XMFLOAT4 ReadHalf(const Readback& readback,UINT width,UINT height)
        {
            void* mapped=nullptr;const D3D12_RANGE reads{0,SIZE_T(readback.bytes)};
            Check(readback.resource->Map(0,&reads,&mapped),"read actual SSGI history/composite");
            const auto* half=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped)+readback.footprint.Offset+
                size_t(height/2)*readback.footprint.Footprint.RowPitch+size_t(width/2)*8);
            const XMFLOAT4 result{DirectX::PackedVector::XMConvertHalfToFloat(half[0]),DirectX::PackedVector::XMConvertHalfToFloat(half[1]),
                DirectX::PackedVector::XMConvertHalfToFloat(half[2]),DirectX::PackedVector::XMConvertHalfToFloat(half[3])};
            const D3D12_RANGE noWrites{0,0};readback.resource->Unmap(0,&noWrites);return result;
        }
        HiddenWindow window;RenderDevice device;ResourceManager resources;PipelineManager pipelines;
        ComPtr<ID3D12InfoQueue> info;VXGIMeshSDFCache meshCache;VXGIMeshSDFScene meshScene;VXGIGPUScene sceneGPU;VXGISurface surface;
        VXGISurface::Parameters surfaceParameters;SSGIHistoryState history;SSGIHybridState sourceState;
        XMFLOAT4X4 projection={},invProjection={},view={};VXGIHybridRDGReport report;
    };
}

inline VXGIHybridRDGReport RunVXGIHybridRDGAcceptance()
{
    VXGIHybridRDGDetail::Fixture fixture;
    fixture.Submit(16,16,2,1,1,{.2f,.4f,.6f},true,1);
    fixture.Submit(16,16,2,1,1,{.2f,.4f,.6f},false,2);
    for(auto& record:fixture.scene.raw) record={};
    fixture.Submit(16,16,2,2,1,{},true,1);
    fixture.Submit(16,16,1,2,1,{},true,1);
    fixture.Submit(16,16,0,2,1,{},true,1);
    for(auto& material:fixture.scene.materials) material.emissiveFactor={.12f,.24f,.36f};
    fixture.Submit(16,16,1,2,2,{},true,1);
    fixture.Resize(20,12);
    fixture.Submit(20,12,1,2,2,{},true,1);
    fixture.Submit(20,12,1,2,2,{},false,2);
    fixture.Resize(16,16);
    fixture.scene.raw.assign(fixture.scene.raw.size(),Scene::Constant({.2f,.4f,.6f}));
    fixture.Submit(16,16,2,3,2,{.2f,.4f,.6f},true,1);
    fixture.Submit(16,16,2,3,2,{},true,1,true);
    fixture.Submit(16,16,2,3,2,{},false,2,false,true);
    fixture.Submit(16,16,0,3,3,{},true,1);
    return fixture.Finish();
}
