// Actual configured assets, production raster depth/materials/shadows/deferred
// lighting/probe capture/SSGI. Never shown HWND; never calls Present.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#pragma warning(push)
#pragma warning(disable:4267 4244 4099 4324)
#include "../Resources/ResourceManager.h"
#include "../Passes/GBufferPass.h"
#include "../Passes/ShadowPass.h"
#include "../Passes/HBAOPass.h"
#include "../Passes/DeferredLightingPass.h"
#include "../Passes/SSGIPass.h"
#include "../Passes/SkyboxPass.h"
#pragma warning(pop)
#include "../Passes/VXGIProbePass.h"
#include "../Passes/VXGIProbeFilterPass.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace DirectX;
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
extern "C" {
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace {
constexpr UINT Width=640,Height=400;
void Require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void Check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(std::string(operation)+": HRESULT "+std::to_string(uint32_t(hr)));}
XMFLOAT4X4 Stored(FXMMATRIX matrix){XMFLOAT4X4 result;XMStoreFloat4x4(&result,XMMatrixTranspose(matrix));return result;}
class HiddenWindow {
public:
    HiddenWindow(){WNDCLASSW desc{};desc.lpfnWndProc=DefWindowProcW;desc.hInstance=GetModuleHandleW(nullptr);desc.lpszClassName=L"VXGIStage74BackgroundRaster";
        Require(RegisterClassW(&desc)!=0,"register hidden raster HWND");handle=CreateWindowExW(0,desc.lpszClassName,L"Background raster acceptance",WS_POPUP,0,0,Width,Height,nullptr,nullptr,desc.hInstance,nullptr);
        Require(handle&&!IsWindowVisible(handle),"raster HWND must remain hidden");}
    ~HiddenWindow(){if(handle)DestroyWindow(handle);UnregisterClassW(L"VXGIStage74BackgroundRaster",GetModuleHandleW(nullptr));}
    HWND handle=nullptr;
};
void PNG(const std::string& path,const std::vector<uint8_t>& pixels,UINT width=Width,UINT height=Height){
    ComPtr<IWICImagingFactory> factory;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"create PNG factory");
    ComPtr<IWICStream> stream;Check(factory->CreateStream(&stream),"create PNG stream");const std::wstring wide(path.begin(),path.end());Check(stream->InitializeFromFilename(wide.c_str(),GENERIC_WRITE),"open PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;Check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"create PNG encoder");Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;Check(encoder->CreateNewFrame(&frame,nullptr),"create PNG frame");Check(frame->Initialize(nullptr),"initialize PNG frame");Check(frame->SetSize(width,height),"set PNG size");
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;Check(frame->SetPixelFormat(&format),"set PNG format");Require(format==GUID_WICPixelFormat24bppBGR,"PNG changed RGB format");
    auto bgr=pixels;for(size_t i=0;i<bgr.size();i+=3)std::swap(bgr[i],bgr[i+2]);
    Check(frame->WritePixels(height,width*3,UINT(bgr.size()),bgr.data()),"write PNG pixels");Check(frame->Commit(),"commit PNG frame");Check(encoder->Commit(),"commit PNG encoder");
}
struct Readback {ComPtr<ID3D12Resource> resource;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};uint64_t bytes=0;};
struct RasterResult {XMFLOAT3 irradiance;uint32_t anchorStatus;float supportWeight;uint32_t acceptedCount,traceFailures,triangleIndex;uint32_t eligible,depthULPs;XMFLOAT2 uvDelta;uint32_t instanceId,firstHitStatus;float deviceDepth,projectedDepth;std::array<uint32_t,8> failureReasons;};
struct RasterConstants {XMFLOAT4X4 view,projection,inverseViewProjection;SSGIHybridParameters hybrid;uint32_t width=Width,height=Height,depthIndex=0,normalIndex=0,ormIndex=0,padding0=0,padding1=0,padding2=0;};
static_assert(sizeof(RasterResult)==96&&sizeof(RasterConstants)==320);
class Capture {
public:
    Capture(){settings.LoadAllSettingsFromJson();Require(device.Initialize(window.handle,Width,Height,3),"initialize hidden raster device");
        Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"query raster InfoQueue");
        const auto scene=SettingsManager::LoadSceneFromJson("Settings/Scene.json");Require(resources.LoadAssets(&device,scene,3),"load configured real raster assets");
        Require(resources.BuildGlobalMaterialPool(&device),"build raster material pool");Require(resources.InitIBL(&device,SettingsManager::GetSkyboxPathFromJson()),"bake configured real IBL");
        Require(resources.InitShadowResources(&device),"initialize production shadows");Require(resources.InitPostProcess(&device,Width,Height),"initialize actual HDR output");
        Require(resources.InitializeBlueNoiseTexture(&device),"initialize actual STBN");Require(resources.InitSSGIHistoryResources(&device,Width,Height),"initialize production GI history");
        Require(pipelines.Initialize(&device)&&pipelines.InitializeSSGI(&device),"initialize production raster/SSGI pipelines");resources.SealPersistentSrvUavDescriptors();
        Require(world.Update(resources.GetSceneInstances(),settings.vxgi.voxelSize),world.GetLastError());Require(sdf.Update(world,settings.vxgi.sdfCellSize,0),sdf.GetLastError());Require(topology.Update(world),topology.GetLastError());
        VXGISurface::Parameters source;source.lightDirection=settings.lighting.lightDir;source.lightColor=settings.lighting.lightColor;source.materialMipLevel=settings.vxgi.surfaceMaterialMipLevel;source.rayMinDistance=settings.vxgi.surfaceRayMinDistance;source.maxRayDistance=settings.vxgi.surfaceMaxRayDistance;
        Require(surface.Update(resources.GetGlobalMaterials(),world.GetGeometryRevision(),sdf.GetGeometryRevision(),world.GetMaterialRevision(),source,world.GetCoverageRevision()),surface.GetLastError());
        probeParameters.probesPerFrame=uint32_t(settings.vxgi.probeProbesPerFrame);probeParameters.classifyRays=uint32_t(settings.vxgi.probeClassifyRays);probeParameters.raysPerUpdate=uint32_t(settings.vxgi.probeRaysPerUpdate);probeParameters.maxHistoryUpdates=uint32_t(settings.vxgi.probeMaxHistoryUpdates);probeParameters.backfaceThreshold=settings.vxgi.probeBackfaceThreshold;probeParameters.rayMinDistance=source.rayMinDistance;probeParameters.rayMaxDistance=source.maxRayDistance;
        shared.Initialize(3);Require(probes.Initialize(device,3)&&filter.Initialize(device,3),"initialize actual probe pipelines");
        for(auto& instance:resources.GetSceneInstances()){instance.UpdateTransform();instance.currentLodLevel=0;visible.push_back(&instance);}
        std::sort(visible.begin(),visible.end(),[](auto* a,auto* b){if(a->isCutout!=b->isCutout)return a->isCutout<b->isCutout;return a->pModel<b->pModel;});
        for(auto& list:shadowVisible)list=visible;
        for(UINT i=0;i<NUM_CASCADES;++i)shadowOffsets[i]=i*visible.size();
        CD3DX12_ROOT_PARAMETER roots[10];roots[0].InitAsConstantBufferView(0);for(UINT i=0;i<8;++i)roots[i+1].InitAsShaderResourceView(i);roots[9].InitAsUnorderedAccessView(0);
        const CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);const CD3DX12_ROOT_SIGNATURE_DESC rootDesc(10,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);ComPtr<ID3DBlob> blob;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize raster query root");Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&queryRoot)),"create raster query root");
        auto shader=ShaderCompiler::CompileFromFile(L"Shaders/VXGIStage74RasterValidate.hlsl",L"CSMain",L"cs_6_6");Require(shader!=nullptr,"compile real raster query diagnostic");D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=queryRoot.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&queryPSO)),"create real raster query diagnostic");
        queryBuffer=Buffer(Width*Height*sizeof(RasterResult),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);queryReadback=Buffer(Width*Height*sizeof(RasterResult),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC timestampDesc{};timestampDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;timestampDesc.Count=2;Check(device.GetDevice()->CreateQueryHeap(&timestampDesc,IID_PPV_ARGS(&timestamps)),"create raster timestamps");timestampReadback=Buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Check(device.GetCommandQueue()->GetTimestampFrequency(&timestampFrequency),"read raster timestamp frequency");
        report["scene"]={{"triangles",world.GetStats().triangles},{"probes",world.GetOccupiedCenters().size()},{"source","Settings/Scene.json"}};
        for(const auto& instance:resources.GetSceneInstances()){XMFLOAT3 low={FLT_MAX,FLT_MAX,FLT_MAX},high={-FLT_MAX,-FLT_MAX,-FLT_MAX};for(const auto& triangle:world.GetTriangles())if(triangle.instanceId==instance.editorId)for(const auto& p:triangle.positions){low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);low.z=std::min(low.z,p.z);high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);high.z=std::max(high.z,p.z);}report["scene"]["bounds"].push_back({{"instance_id",instance.editorId},{"asset",instance.name},{"minimum",{low.x,low.y,low.z}},{"maximum",{high.x,high.y,high.z}}});}
        report["scope"]="Actual LOD0 assets and production ShadowPass/GBufferPass/HBAOPass/DeferredLightingPass/SSGIPass, with actual two-round RAW capture and configured IBL. Production skybox included. No transparent/postprocess/DLSS/editor/present. GPU timings exclude readbacks and acceptance diagnostics; individually fence-waited frames are not main-program throughput.";
        report["preview_mapping"]="Linear HDR saved as binary float4. PNG uses fixed exposure 1, Reinhard x/(1+x), then linear-to-sRGB; GI preview uses exposure 4. This is a diagnostic presentation, not production postprocessing.";
        std::filesystem::create_directories("tmp/vxgi-stage74-render");
    }
    ~Capture(){for(int i=0;i<3;++i)device.WaitForPreviousFrame(i);}
    void Run(){const uint32_t rounds=uint32_t((world.GetOccupiedCenters().size()+probeParameters.probesPerFrame-1)/probeParameters.probesPerFrame);for(uint32_t i=0;i<rounds*2;++i){Warm();if(i%64==0)std::cout<<"Probe warm batch "<<i+1<<'/'<<rounds*2<<'\n';}
        report["probe_warmup_batches"]=rounds*2;report["raw_contract"]="Two allocation sweeps, at most two capture updates per valid probe; maximum_history_updates=32 is a cap, not evidence of 32-update convergence. Identical immutable RAW snapshot for all modes/cameras.";report["internal_resolution"]={Width,Height};report["ssgi_history_resolution"]={(Width+1)/2,(Height+1)/2};
        for(int camera=0;camera<4;++camera)for(int mode=-1;mode<=2;++mode){history={};std::vector<XMFLOAT4> first,last;for(uint32_t sample=0;sample<8;++sample){auto image=Frame(camera,mode,sample,sample==7);if(sample==0)first=image;if(sample==7)last=image;}
            double temporal=0;for(size_t i=0;i<last.size();++i)temporal+=(std::abs(last[i].x-first[i].x)+std::abs(last[i].y-first[i].y)+std::abs(last[i].z-first[i].z))/3;
            report["captures"].back()["mean_absolute_first_to_eighth_frame_rgb"]=temporal/last.size();std::cout<<"Captured camera "<<camera<<" mode "<<mode<<'\n';}
        history={};for(int camera=0;camera<4;++camera){for(uint32_t sample=0;sample<4;++sample){Frame(camera,2,uint32_t(camera)*4+sample,sample==0||sample==3,sample==0?"motion_first":"motion_fourth");}std::cout<<"Continuous-history camera "<<camera<<" completed"<<'\n';}
        report["continuous_history_contract"]="Mode2 four submitted frames at each of the same four camera poses, retains actual production SSGI history across 0.6m move, turn, and source leaving the screen, captures first/fourth pose frame. The fourth pose changes visible world regions while the car is out of view; this is not a same-world-ROI energy-continuity test. No claim of motion-continuous ground truth; same fixed immutable RAW, production reprojection/disocclusion.";
        Warnings();uint64_t failures=0;for(const auto& item:report["captures"])failures+=item["probe_visibility_trace_failures"].get<uint64_t>();report["probe_visibility_trace_failures_total"]=failures;report["numerical_validation_passed"]=failures==0;report["all_images_and_gi_finite"]=true;report["strict_endpoint_visibility_acceptance_passed"]=failures==0;report["present_calls"]=0;report["window_visible"]=IsWindowVisible(window.handle)!=FALSE;report["debug_layer"]=true;report["gpu_based_validation"]=false;report["completed"]=true;
        std::ofstream file("tmp/VXGIStage74RenderAcceptance-report.json");file<<std::setw(2)<<report<<'\n';
    }
private:
    ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES initialState,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){const auto heap=CD3DX12_HEAP_PROPERTIES(heapType);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);ComPtr<ID3D12Resource> result;Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initialState,nullptr,IID_PPV_ARGS(&result)),"create raster acceptance buffer");return result;}
    void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);device.GetCommandList()->ResourceBarrier(1,&barrier);}
    int Reset(){const int slot=int(submissions%3);device.WaitForPreviousFrame(slot);Check(device.GetCommandAllocator(slot)->Reset(),"reset raster allocator");Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset raster command list");resources.BeginPassConstantsFrame(slot);resources.ResetTransientSrvUavDescriptors(slot);resources.BeginRDGFrame(&device,slot);Require(!IsWindowVisible(window.handle),"raster acceptance HWND became visible");return slot;}
    uint64_t Submit(int slot){Check(device.GetCommandList()->Close(),"close raster commands");ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);const auto fence=++device.GetFenceValue(slot);Check(device.GetCommandQueue()->Signal(device.GetFence(slot),fence),"signal raster commands");++submissions;return fence;}
    void Wait(int slot){device.WaitForPreviousFrame(slot);Check(device.GetDevice()->GetDeviceRemovedReason(),"raster acceptance device status");}
    void Warm(){const int slot=Reset();VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),sdf.GetGeometryRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};Require(state.Prepare(sources,probeParameters),state.GetLastError());Require(shared.Prepare(device,slot,world,sdf,&topology),"prepare actual world");Require(probes.Record(device,resources.GetMainDescriptorHeap(),slot,shared,sdf,surface,resources.GetMaterialBufferGPUAddress(),state),"record probe warmup");raw=probes.GetDisplayHistory();Require(filter.Record(device,resources.GetMainDescriptorHeap(),slot,shared,sdf,raw.Get(),state,world.GetVoxelSize(),settings.vxgi.probeFilterNeighborWeight,surface,resources.GetMaterialBuffer()),"record filter warmup");const auto fence=Submit(slot);Require(probes.OnSubmitted(slot,fence,state)&&filter.OnSubmitted(slot,fence),"commit probe warmup");Wait(slot);Require(probes.Poll(device,slot,state),"poll probe warmup");}
    Readback Copy(ID3D12Resource* source,D3D12_RESOURCE_STATES before){Readback result;const auto desc=source->GetDesc();device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&result.footprint,nullptr,nullptr,&result.bytes);result.resource=Buffer(result.bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(source,before,D3D12_RESOURCE_STATE_COPY_SOURCE);const CD3DX12_TEXTURE_COPY_LOCATION src(source,0),dst(result.resource.Get(),result.footprint);device.GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,before);return result;}
    std::vector<XMFLOAT4> Half(const Readback& copy){const UINT width=copy.footprint.Footprint.Width,height=copy.footprint.Footprint.Height;std::vector<XMFLOAT4> result(width*height);void* mapped=nullptr;Check(copy.resource->Map(0,nullptr,&mapped),"map actual HDR readback");for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto* half=reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(mapped)+copy.footprint.Offset+y*copy.footprint.Footprint.RowPitch+x*8);result[y*width+x]={PackedVector::XMConvertHalfToFloat(half[0]),PackedVector::XMConvertHalfToFloat(half[1]),PackedVector::XMConvertHalfToFloat(half[2]),PackedVector::XMConvertHalfToFloat(half[3])};}copy.resource->Unmap(0,nullptr);return result;}
    static std::vector<uint8_t> Preview(const std::vector<XMFLOAT4>& values,float exposure=1){std::vector<uint8_t> result(values.size()*3);for(size_t i=0;i<values.size();++i){const float rgb[3]={values[i].x,values[i].y,values[i].z};for(UINT c=0;c<3;++c){const float linear=std::max(0.f,rgb[c])*exposure;const float tone=linear/(1+linear);const float srgb=tone<=.0031308f?12.92f*tone:1.055f*std::pow(tone,1/2.4f)-.055f;result[i*3+c]=uint8_t(std::clamp(srgb*255.f+.5f,0.f,255.f));}}return result;}
    std::vector<XMFLOAT4> Frame(int cameraIndex,int mode,uint32_t sample,bool capture,const std::string& sequence="static"){const int slot=Reset();Require(shared.Prepare(device,slot,world,sdf,&topology),"prepare render slot geometry");
        const XMFLOAT3 positions[4]={{8,4,-10},{8.6f,4,-10},{8,4,-10},{8,4,-10}};const XMFLOAT3 targets[4]={{0,1,0},{0,1,0},{-8,0,2},{-30,0,-14}};
        const auto eye=XMLoadFloat3(&positions[cameraIndex]);const auto target=XMLoadFloat3(&targets[cameraIndex]);const auto forward=XMVector3Normalize(target-eye);Camera camera;camera.Position=positions[cameraIndex];XMStoreFloat3(&camera.Front,forward);camera.Up={0,1,0};camera.Zoom=45;
        const auto viewCPU=camera.GetViewMatrix(),projectionCPU=XMMatrixPerspectiveFovLH(XMConvertToRadians(45),float(Width)/Height,.1f,1000),vpCPU=viewCPU*projectionCPU;
        const auto view=Stored(viewCPU),projection=Stored(projectionCPU),invProjection=Stored(XMMatrixInverse(nullptr,projectionCPU)),invVP=Stored(XMMatrixInverse(nullptr,vpCPU));
        const auto shadowFrame=ShadowPass::PrepareFrame({&camera,settings.lighting.lightDir,float(Width)/Height,float(resources.GetShadowMapSize())});PassConstants constants{};constants.camPos=camera.Position;constants.cameraForward=camera.Front;constants.lightDir=shadowFrame.lightDir;constants.lightColor=settings.lighting.lightColor;constants.environmentIntensity=settings.lighting.environmentIntensity;constants.tanSunAngularRadius=std::tan(XMConvertToRadians(settings.lighting.sunAngularRadiusDegrees));constants.cascadeSplits=shadowFrame.cascadeSplits;constants.cascadeOrthoWidths=shadowFrame.cascadeOrthoWidths;constants.cascadeDepthRanges=shadowFrame.cascadeDepthRanges;std::copy(shadowFrame.lightViewProj.begin(),shadowFrame.lightViewProj.end(),constants.lightViewProj);constants.iblPrefilterIdx=resources.GetIblPrefilterIdx();constants.iblBRDFIdx=resources.GetIblBRDFIdx();auto* cb=resources.GetCBVAddress(slot);std::memcpy(cb,&constants,sizeof(constants));auto* instanceData=reinterpret_cast<InstanceData*>(cb+kPassConstantsAlignedSize);
        for(size_t i=0;i<visible.size();++i){const auto* instance=visible[i];instanceData[i].wvpMat=Stored(instance->cachedWorldMat*vpCPU);instanceData[i].worldMat=Stored(instance->cachedWorldMat);instanceData[i].normalMat=Stored(instance->cachedNormalMat);instanceData[i].customMaterialID=instance->customMaterialID;for(UINT cascade=0;cascade<NUM_CASCADES;++cascade)instanceData[visible.size()+cascade*visible.size()+i]=instanceData[i];}
        RDGBuilder graph(&device,"ActualRasterAcceptance");graph.SetTransientResourceAllocator([&](const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_STATES final,const D3D12_CLEAR_VALUE* clear,RDGTransientResourceLease* lease){return resources.AllocateRDGTransientResource(&device,slot,desc,initial,final,clear,lease);});graph.SetTransientSrvUavDescriptorAllocator([&](UINT* index,D3D12_CPU_DESCRIPTOR_HANDLE* handle){return resources.AllocateTransientSrvUavDescriptor(index,handle);});
        auto hdrTarget=graph.RegisterExternalTexture(resources.GetPostProcessRT(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,"Acceptance.HDR");auto hdrRtv=graph.CreateTextureRTVView(hdrTarget);RDGPassParameters clearParameters;clearParameters.WriteRTV(hdrRtv);graph.AddPass("Acceptance.ClearBackground",ERDGPassFlags::Graphics,clearParameters,[hdrRtv](ID3D12GraphicsCommandList* cmd){const float black[4]={.2f,.3f,.4f,1};cmd->ClearRenderTargetView(hdrRtv.cpuHandle,black,0,nullptr);});
        auto shadows=ShadowPass::AddToGraph(graph,&resources,&pipelines,slot,shadowVisible,shadowOffsets,visible.size());const D3D12_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};const D3D12_RECT rect{0,0,Width,Height};size_t transparent=visible.size();auto gbuffer=GBufferPass::AddToGraph(graph,&device,&resources,&pipelines,slot,viewport,rect,visible,transparent);Require(gbuffer.depth.IsValid()&&shadows.pass.IsValid(),"build real GBuffer/shadows");
        auto ao=HBAOPass::AddToGraph(graph,&resources,&pipelines,view,projection,invProjection,Width,Height,slot,sample,settings.hbao,{gbuffer.depth,gbuffer.normal});Require(ao.blurPass.IsValid(),"build actual HBAO");
        DeferredLightingPass::Input light{gbuffer.albedo,gbuffer.normal,gbuffer.orm,gbuffer.emissive,gbuffer.depth,ao.blurredTexture,shadows.shadowMapSrv};auto deferred=DeferredLightingPass::AddToGraph(graph,&resources,&pipelines,invVP,Width,Height,slot,light,mode>=0);Require(deferred.pass.IsValid(),"build real DeferredLightingPass");graph.AddPassDependencies(deferred.pass,{shadows.pass,ao.blurPass});
        RDGPassHandle colorProducer=deferred.pass;
        if(mode>=0){SSGIPass::WorldInput worldInput{&shared,&sdf,&surface,raw.Get(),resources.GetMaterialBuffer(),uint32_t(world.GetOccupiedCenters().size()),world.GetVoxelSize(),uint32_t(mode)};Require(SSGIPass::EffectiveWorldMode(&worldInput,slot)==uint32_t(mode),"actual SSGI mode unexpectedly downgraded");auto gi=SSGIPass::AddToGraph(graph,&resources,&pipelines,history,view,projection,invProjection,invVP,Width,Height,slot,sample,settings.ssgi,{deferred.sceneColor,gbuffer.depth,gbuffer.normal,gbuffer.orm,gbuffer.albedo,deferred.bounceSource},&worldInput);Require(gi.pass.IsValid(),"build actual SSGI");colorProducer=gi.pass;}
        auto skybox=SkyboxPass::AddToGraph(graph,&device,&resources,&pipelines,camera,viewport,rect,Width,Height,{deferred.sceneColor,gbuffer.depth});Require(skybox.IsValid(),"build production skybox");graph.AddPassDependencies(skybox,{colorProducer});
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc{};depthDesc.Format=DXGI_FORMAT_R32_FLOAT;depthDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;depthDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;depthDesc.Texture2D.MipLevels=1;auto depthSrv=graph.CreateTextureSRVView(gbuffer.depth,&depthDesc);auto normalSrv=graph.CreateTextureSRVView(gbuffer.normal);auto ormSrv=graph.CreateTextureSRVView(gbuffer.orm);graph.MarkTextureAsOutput(gbuffer.depth);graph.MarkTextureAsOutput(gbuffer.normal);
        device.GetCommandList()->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);graph.Execute(device.GetCommandList());device.GetCommandList()->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);device.GetCommandList()->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestampReadback.Get(),0);Require(graph.GetCompileStats().compileSucceeded,"compile real raster graph");Require(!resources.PassConstantsAllocationFailed(slot),"real raster constants overflow");
        const auto imageCopy=Copy(resources.GetPostProcessRT(),D3D12_RESOURCE_STATE_RENDER_TARGET);Readback depthCopy,giCopy;
        if(capture){depthCopy=Copy(graph.GetTextureResource(gbuffer.depth),D3D12_RESOURCE_STATE_DEPTH_WRITE);if(mode>=0)giCopy=Copy(resources.GetSSGIHistoryTextures(resources.GetSSGICurrentHistoryIdx()).gi.Get(),D3D12_RESOURCE_STATE_COMMON);
            auto* depth=graph.GetTextureResource(gbuffer.depth);auto* normal=graph.GetTextureResource(gbuffer.normal);auto* orm=graph.GetTextureResource(gbuffer.orm);if(!queryInitialized){Transition(queryBuffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);queryInitialized=true;}Transition(orm,D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);Transition(depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,VXGIGPUScene::ReadState);Transition(normal,D3D12_RESOURCE_STATE_COMMON,VXGIGPUScene::ReadState);
            RasterConstants query;query.view=view;query.projection=projection;query.inverseViewProjection=invVP;query.hybrid=Hybrid();query.depthIndex=depthSrv.descriptorIndex;query.normalIndex=normalSrv.descriptorIndex;query.ormIndex=ormSrv.descriptorIndex;auto allocation=resources.AllocatePassConstants(slot,sizeof(query));Require(bool(allocation),"allocate raster query constants");std::memcpy(allocation.cpuAddress,&query,sizeof(query));auto* cmd=device.GetCommandList();ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(queryRoot.Get());cmd->SetPipelineState(queryPSO.Get());cmd->SetComputeRootConstantBufferView(0,allocation.gpuAddress);const std::array<D3D12_GPU_VIRTUAL_ADDRESS,8> addresses={shared.Get(slot,0)->GetGPUVirtualAddress(),shared.Get(slot,1)->GetGPUVirtualAddress(),shared.Get(slot,2)->GetGPUVirtualAddress(),shared.Get(slot,3)->GetGPUVirtualAddress(),resources.GetMaterialBufferGPUAddress(),shared.Get(slot,4)->GetGPUVirtualAddress(),shared.Get(slot,5)->GetGPUVirtualAddress(),raw->GetGPUVirtualAddress()};for(UINT i=0;i<8;++i)cmd->SetComputeRootShaderResourceView(i+1,addresses[i]);cmd->SetComputeRootUnorderedAccessView(9,queryBuffer->GetGPUVirtualAddress());cmd->Dispatch((Width+7)/8,(Height+7)/8,1);Transition(queryBuffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyResource(queryReadback.Get(),queryBuffer.Get());Transition(queryBuffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Transition(depth,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);Transition(normal,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COMMON);Transition(orm,VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COMMON);}
        Submit(slot);Wait(slot);void* timingData=nullptr;Check(timestampReadback->Map(0,nullptr,&timingData),"read actual renderer timestamp");const auto* ticks=static_cast<const uint64_t*>(timingData);const double elapsed=double(ticks[1]-ticks[0])*1000/double(timestampFrequency);timestampReadback->Unmap(0,nullptr);Require(std::isfinite(elapsed)&&elapsed>=0,"invalid raster timestamp");report["frame_gpu_samples"].push_back({{"camera",cameraIndex},{"mode",mode},{"sample",sample},{"renderer_gpu_ms",elapsed}});auto image=Half(imageCopy);uint64_t nonfinite=0;for(const auto& p:image)nonfinite+=!(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.w));Require(nonfinite==0,"actual HDR image has nonfinite pixels");
        if(mode>=0){resources.FlipSSGIHistoryIndex();history.valid=true;history.previousViewProj=Stored(vpCPU);history.previousInvViewProj=invVP;history.previousPosition=camera.Position;history.previousForward=camera.Front;history.previousFov=camera.Zoom;}
        if(capture){const std::string name=(sequence=="static"?"":sequence+"_")+"camera"+std::to_string(cameraIndex)+(mode<0?"_off":"_mode"+std::to_string(mode));const std::string stem="tmp/vxgi-stage74-render/"+name;PNG(stem+".png",Preview(image));std::ofstream hdr(stem+".float4",std::ios::binary);hdr.write(reinterpret_cast<const char*>(image.data()),std::streamsize(image.size()*sizeof(XMFLOAT4)));
            auto stats=Statistics(image,depthCopy);stats["name"]=name;stats["camera_position"]={camera.Position.x,camera.Position.y,camera.Position.z};stats["camera_target"]={targets[cameraIndex].x,targets[cameraIndex].y,targets[cameraIndex].z};stats["mode"]=mode;stats["frames"]=sample+1;stats["sequence"]=sequence;stats["sample_frame"]=sample;stats["image"]=stem+".png";stats["linear_hdr"]=stem+".float4";
            if(mode>=0){auto gi=Half(giCopy);for(const auto& p:gi)Require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.w),"actual GI history has nonfinite pixels");PNG(stem+"_gi.png",Preview(gi,4),(Width+1)/2,(Height+1)/2);double mean=0;for(const auto& p:gi)mean+=(p.x+p.y+p.z)/3;stats["mean_history_irradiance_over_pi_rgb"]=mean/gi.size();}
            report["captures"].push_back(stats);RasterStatistics(stem);}
        return image;
    }
    SSGIHybridParameters Hybrid(){SSGIHybridParameters p{};const auto& grid=sdf.GetGrid();p.gridOrigin=grid.origin;p.cellSize=grid.cellSize;p.cellDimensions={grid.cellDimensions[0],grid.cellDimensions[1],grid.cellDimensions[2]};p.triangleCount=uint32_t(sdf.GetTriangles().size());p.boundsMaximum=grid.boundsMaximum;p.materialCount=uint32_t(surface.GetMaterials().size());p.materialMipLevel=surface.GetParameters().materialMipLevel;p.voxelSize=world.GetVoxelSize();p.probeCount=uint32_t(world.GetOccupiedCenters().size());p.worldMode=2;return p;}
    Json Statistics(const std::vector<XMFLOAT4>& image,const Readback& depth){void* mapped=nullptr;Check(depth.resource->Map(0,nullptr,&mapped),"map actual raster depth");uint64_t foreground=0;double luminance=0;float maximum=0;std::vector<uint8_t> pixels(Width*Height*3);for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){const float value=*reinterpret_cast<const float*>(static_cast<uint8_t*>(mapped)+depth.footprint.Offset+y*depth.footprint.Footprint.RowPitch+x*4);Require(std::isfinite(value)&&value>=0&&value<=1,"actual raster depth invalid");const auto& p=image[y*Width+x];if(value<1){++foreground;luminance+=.2126*p.x+.7152*p.y+.0722*p.z;}maximum=std::max(maximum,std::max(p.x,std::max(p.y,p.z)));const auto intensity=uint8_t(std::clamp((value<1?1-value:0)*2000,0.f,255.f));for(int c=0;c<3;++c)pixels[(y*Width+x)*3+c]=intensity;}depth.resource->Unmap(0,nullptr);return {{"foreground_pixels",foreground},{"foreground_mean_linear_luminance",foreground?luminance/foreground:0},{"maximum_hdr_channel",maximum},{"nonfinite_pixels",0}};}
    void RasterStatistics(const std::string& stem)
    {
        void* mapped=nullptr;Check(queryReadback->Map(0,nullptr,&mapped),"map production raster anchor results");
        const auto* query=static_cast<RasterResult*>(mapped);std::array<uint64_t,6> statuses{};
        uint64_t supported=0,failures=0,accepted=0,eligible=0,eligibleVerified=0,eligibleSupported=0;
        uint64_t depthMismatch=0,uvMismatch=0,otherMismatch=0;
        std::map<uint32_t,std::array<uint64_t,6>> groups;
        std::array<uint64_t,7> failureReasons{};uint64_t replayMismatch=0;std::map<uint32_t,std::array<uint64_t,7>> instanceFailureReasons;
        std::map<uint32_t,uint64_t> ulpHistogram;
        std::vector<XMFLOAT4> epi(Width*Height);std::vector<uint8_t> colors(Width*Height*3);
        for(size_t i=0;i<Width*Height;++i)
        {
            const auto& q=query[i];
            Require(q.anchorStatus<statuses.size()&&q.eligible<=1&&std::isfinite(q.supportWeight)&&std::isfinite(q.irradiance.x)&&std::isfinite(q.irradiance.y)&&std::isfinite(q.irradiance.z),"invalid production raster support query");
            ++statuses[q.anchorStatus];supported+=q.acceptedCount>0;accepted+=q.acceptedCount;failures+=q.traceFailures;
            eligible+=q.eligible;eligibleVerified+=q.eligible&&q.anchorStatus==1;eligibleSupported+=q.eligible&&q.acceptedCount>0;
            if(q.anchorStatus)
            {
                auto& group=groups[q.instanceId];++group[0];group[1]+=q.anchorStatus==1;group[2]+=q.acceptedCount>0;group[3]+=q.eligible;group[4]+=q.eligible&&q.acceptedCount>0;group[5]+=q.traceFailures;
            }
            uint64_t replayCount=0;for(size_t reason=0;reason<7;++reason){failureReasons[reason]+=q.failureReasons[reason];replayCount+=q.failureReasons[reason];instanceFailureReasons[q.instanceId][reason]+=q.failureReasons[reason];}replayMismatch+=replayCount!=q.traceFailures;
            if(q.firstHitStatus==1)
            {
                Require(std::isfinite(q.uvDelta.x)&&std::isfinite(q.uvDelta.y)&&std::isfinite(q.projectedDepth),"invalid first-hit anchor diagnostic");
                ++ulpHistogram[std::min(q.depthULPs,16u)];
            }
            if(q.anchorStatus==5)
            {
                const bool depthBad=q.depthULPs>2;
                const bool uvBad=q.uvDelta.x>std::max(8*FLT_EPSILON,.01f/Width)||q.uvDelta.y>std::max(8*FLT_EPSILON,.01f/Height);
                depthMismatch+=depthBad;uvMismatch+=uvBad;otherMismatch+=!depthBad&&!uvBad;
            }
            epi[i]={q.irradiance.x,q.irradiance.y,q.irradiance.z,1};
            if(q.anchorStatus==1){colors[i*3+1]=q.acceptedCount?255:80;colors[i*3+2]=q.acceptedCount?0:255;}
            else if(q.anchorStatus>1)colors[i*3]=255;
        }
        std::ofstream rawOutput(stem+"_raster_queries.bin",std::ios::binary);
        rawOutput.write(static_cast<const char*>(mapped),std::streamsize(Width*Height*sizeof(RasterResult)));
        queryReadback->Unmap(0,nullptr);PNG(stem+"_support.png",colors);PNG(stem+"_probe_epi.png",Preview(epi,4));
        auto& capture=report["captures"].back();capture["camera_anchor_status_counts"]=statuses;
        capture["camera_anchor_status_legend"]={"background","verified","miss","invalid","iteration_limit","mismatch"};
        const auto foreground=capture["foreground_pixels"].get<uint64_t>();
        capture["camera_anchor_verified_fraction_of_foreground"]=foreground?double(statuses[1])/foreground:0;
        capture["supported_raster_pixels"]=supported;capture["support_fraction_of_verified_anchors"]=statuses[1]?double(supported)/statuses[1]:0;
        capture["gi_eligible_raster_pixels"]=eligible;capture["gi_eligible_verified_anchors"]=eligibleVerified;capture["gi_eligible_supported_raster_pixels"]=eligibleSupported;
        capture["gi_eligible_support_fraction_of_foreground"]=eligible?double(eligibleSupported)/eligible:0;
        capture["probe_visibility_trace_failures"]=failures;capture["accepted_contributors"]=accepted;capture["visibility_failure_reason_counts"]=failureReasons;capture["visibility_failure_reason_legend"]={"invalid_input_or_zero_distance","miss_cannot_prove_endpoint","invalid_trace","iteration_limit","same_triangle_endpoint_distance","same_triangle_endpoint_position","other"};capture["visibility_replay_mismatch_pixels"]=replayMismatch;Require(replayMismatch==0,"visibility diagnostic replay differs from production failure count");
        capture["anchor_mismatch_reasons"]={{"depth_over_2_ulp",depthMismatch},{"uv_over_tolerance",uvMismatch},{"neither",otherMismatch},{"contract","Reasons can overlap. Depth ULP 16 bin includes 16 and larger. First-hit instance attribution can differ from raster primitive at failed anchors."}};
        for(const auto& [ulp,count]:ulpHistogram)capture["anchor_depth_ulp_histogram"].push_back({{"ulp",ulp},{"pixels",count}});
        for(const auto& [instance,count]:groups)capture["first_hit_instance_groups"].push_back({{"instance_id",instance},{"foreground_first_hits",count[0]},{"verified",count[1]},{"supported",count[2]},{"gi_eligible",count[3]},{"gi_eligible_supported",count[4]},{"visibility_failures",count[5]},{"visibility_failure_reason_counts",instanceFailureReasons[instance]}});
        capture["support_legend"]="black sky, red camera anchor failed, cyan verified anchor without local RAW support, green verified with RAW support. Includes unlit surfaces; separate gi_eligible metrics use actual raster ORM.a>=0.5.";
    }
    void Warnings(){uint64_t count=0;for(uint64_t i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;Check(info->GetMessage(i,nullptr,&bytes),"measure raster D3D message");std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());Check(info->GetMessage(i,message,&bytes),"read raster D3D message");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<"Raster D3D12: "<<message->pDescription<<'\n';++count;}}report["d3d12_warnings_errors"]=count;Require(count==0,"actual raster acceptance D3D warnings/errors");}
    HiddenWindow window;RenderDevice device;ResourceManager resources;PipelineManager pipelines;SettingsManager settings;VXGIWorld world;VXGISDF sdf;VXGIProbeTopology topology;VXGISurface surface;VXGIGPUScene shared;VXGIProbeState state;VXGIProbeState::Parameters probeParameters;VXGIProbePass probes;VXGIProbeFilterPass filter;SSGIHistoryState history;ComPtr<ID3D12Resource> raw,queryBuffer,queryReadback;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12RootSignature> queryRoot;ComPtr<ID3D12PipelineState> queryPSO;std::vector<ModelInstance*> visible;std::array<std::vector<ModelInstance*>,NUM_CASCADES> shadowVisible;std::array<size_t,NUM_CASCADES> shadowOffsets{};ComPtr<ID3D12QueryHeap> timestamps;ComPtr<ID3D12Resource> timestampReadback;uint64_t timestampFrequency=0;bool queryInitialized=false;uint64_t submissions=0;Json report=Json::object();
};
}
int main(){try{std::cout.setf(std::ios::unitbuf);Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"initialize acceptance COM");ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"enable actual raster debug layer");debug->EnableDebugLayer();Capture capture;capture.Run();std::cout<<"Actual raster acceptance completed without visible windows or Present.\n";return 0;}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}}







