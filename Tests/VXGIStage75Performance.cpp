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
#include "../Passes/MotionVectorPass.h"
#include "../Passes/ScalarTemporalFilterPass.h"
#pragma warning(pop)
#include "../Passes/VXGIProbePass.h"
#include "../Passes/VXGIProbeFilterPass.h"
#include "../Resources/VXGIProbeCache.h"
#include "../Resources/SSGIHybridState.h"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace DirectX;
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
extern "C" {
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace {
UINT Width=640,Height=400;
std::string Label="baseline",ReuseRaw;
std::string CameraEyeText,CameraTargetText,CavityRaysPath;
bool CustomCamera=false;
XMFLOAT3 CustomEye{},CustomTarget{};
uint32_t Samples=3;
uint32_t DownsampleFactor=2,Repeat=1;
uint32_t WarmupRounds=2,WarmupBatches=0;
uint32_t SteadySamples=72,SettleMaxBatches=16384;
uint32_t ABRounds=2,ABWarmup=24;
int ProbePaddingLayers=-1; // -1 uses the production Pipeline.json setting.
bool RayRoutingStats=false;
bool GpuValidation=false,GIOnly=false,ColdStart=false,VerifyMovement=false,LazyProbes=false,LazySettle=false,CompareWorldModes=false,StartupToggle=false,InitializeProbeCache=false;
bool RasterTemporal(){return LazyProbes||CompareWorldModes;}
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
#if defined(DEBUG) || defined(_DEBUG)
constexpr bool DebugShaderPolicy=true;
#else
constexpr bool DebugShaderPolicy=false;
#endif
#if defined(_DEBUG)
constexpr bool NativeDebug=true;
constexpr const char* NativeConfiguration="Od_Zi_MDd";
#else
constexpr bool NativeDebug=false;
constexpr const char* NativeConfiguration="O2_MD";
#endif
#if defined(VXGI_ACCEPTANCE_EXACT_FALLBACK)
constexpr bool ExactFallback=true;
#else
constexpr bool ExactFallback=false;
#endif
std::vector<int> Cameras={0,3};
std::vector<int> Modes={-1,0,1,2};
constexpr UINT TimestampCapacity=256;
constexpr UINT ColdQueryFirst=TimestampCapacity-4;
void Require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
XMFLOAT3 ParseVector(std::string value){std::replace(value.begin(),value.end(),',',' ');std::istringstream input(value);XMFLOAT3 result{};Require(bool(input>>result.x>>result.y>>result.z)&&std::isfinite(result.x)&&std::isfinite(result.y)&&std::isfinite(result.z),"camera vectors require three finite comma-separated values");input>>std::ws;Require(input.eof(),"unexpected camera vector data");return result;}
void Check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(std::string(operation)+": HRESULT "+std::to_string(uint32_t(hr)));}
XMFLOAT4X4 Stored(FXMMATRIX matrix){XMFLOAT4X4 result;XMStoreFloat4x4(&result,XMMatrixTranspose(matrix));return result;}
class HiddenWindow {
public:
    HiddenWindow(){WNDCLASSW desc{};desc.lpfnWndProc=DefWindowProcW;desc.hInstance=GetModuleHandleW(nullptr);desc.lpszClassName=L"VXGIStage75BackgroundPerformance";
        Require(RegisterClassW(&desc)!=0,"register hidden raster HWND");handle=CreateWindowExW(0,desc.lpszClassName,L"Background GI performance",WS_POPUP,0,0,Width,Height,nullptr,nullptr,desc.hInstance,nullptr);
        Require(handle&&!IsWindowVisible(handle),"raster HWND must remain hidden");}
    ~HiddenWindow(){if(handle)DestroyWindow(handle);UnregisterClassW(L"VXGIStage75BackgroundPerformance",GetModuleHandleW(nullptr));}
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
class Capture {
public:
    // The harness output contract follows -GIOnly, independently of the interactive viewport setting.
    Capture(){settings.LoadAllSettingsFromJson();if(ProbePaddingLayers>=0)settings.vxgi.probePaddingLayers=ProbePaddingLayers;settings.ssgi.giOnly=GIOnly;settings.ssgi.traceDownsampleFactor=int(DownsampleFactor);Require(device.Initialize(window.handle,Width,Height,3),"initialize hidden raster device");
        Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"query raster InfoQueue");
        if(CompareWorldModes){Check(info->SetMessageCountLimit(65536),"retain complete long world A/B diagnostics");report["d3d12_message_count_limit"]=65536;}
        const auto scene=SettingsManager::LoadSceneFromJson("Settings/Scene.json");Require(resources.LoadAssets(&device,scene,3),"load configured real raster assets");
        Require(resources.BuildGlobalMaterialPool(&device),"build raster material pool");Require(resources.InitIBL(&device,SettingsManager::GetSkyboxPathFromJson()),"bake configured real IBL");
        Require(resources.InitShadowResources(&device),"initialize production shadows");Require(resources.InitPostProcess(&device,Width,Height),"initialize actual HDR output");
        Require(resources.InitializeBlueNoiseTexture(&device),"initialize actual STBN");Require(resources.InitSSGIHistoryResources(&device,Width,Height,DownsampleFactor),"initialize production GI history");
        Require(pipelines.Initialize(&device)&&pipelines.InitializeSSGI(&device),"initialize production raster/SSGI pipelines");resources.SealPersistentSrvUavDescriptors();
        if(RayRoutingStats)InitializeRouting();
        if(InitializeProbeCache){
            const auto initializeStart=Clock::now();
            Require(settings.vxgi.enabled,"resource initialization acceptance requires configured VXGI enabled");
            Require(resources.InitVXGI(&device,settings.vxgi,settings.lighting,3),"initialize actual ResourceManager VXGI cache before raster");
            report["resource_initialization"]={{"cpu_wall_ms",Milliseconds(initializeStart)},{"production_initialization_ms",resources.GetVXGI().GetInitializationMilliseconds()},{"capture_batches",resources.GetVXGI().GetInitializationBatches()},{"before_first_raster",true},{"default_world_mode",2}};
            report["initial_scene_prepare_cpu"]={{"owner","ResourceManager::InitVXGI"}};
            raw=probes.GetDisplayHistory();Require(raw!=nullptr,"ResourceManager initialization returned no probe history");
            Require(state.GetStats().unclassified==0&&state.GetStats().valid>0,"ResourceManager initialization returned an incomplete probe classification");
        }else report["initial_scene_prepare_cpu"]=PrepareSceneSources();
        const auto& source=surface.GetParameters();
        probeParameters.probesPerFrame=uint32_t(settings.vxgi.probeProbesPerFrame);probeParameters.classifyRays=uint32_t(settings.vxgi.probeClassifyRays);probeParameters.raysPerUpdate=uint32_t(settings.vxgi.probeRaysPerUpdate);probeParameters.maxHistoryUpdates=uint32_t(settings.vxgi.probeMaxHistoryUpdates);probeParameters.backfaceThreshold=settings.vxgi.probeBackfaceThreshold;probeParameters.rayMinDistance=source.rayMinDistance;probeParameters.rayMaxDistance=source.maxRayDistance;
        if(!InitializeProbeCache){shared.Initialize(3);Require(probes.Initialize(device,3,true)&&filter.Initialize(device,3,true),"initialize actual Mesh SDF probe pipelines");}
        if(LazyProbes){if(!InitializeProbeCache)filter.SetTopology(topology.GetNeighbors());filterTopologyRevision=topology.GetRevision();}
        for(auto& instance:resources.GetSceneInstances()){instance.UpdateTransform();instance.currentLodLevel=0;visible.push_back(&instance);}
        std::sort(visible.begin(),visible.end(),[](auto* a,auto* b){if(a->isCutout!=b->isCutout)return a->isCutout<b->isCutout;return a->pModel<b->pModel;});
        for(auto& list:shadowVisible)list=visible;
        for(UINT i=0;i<NUM_CASCADES;++i)shadowOffsets[i]=i*visible.size();
        D3D12_QUERY_HEAP_DESC timestampDesc{};timestampDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;timestampDesc.Count=TimestampCapacity;Check(device.GetDevice()->CreateQueryHeap(&timestampDesc,IID_PPV_ARGS(&timestamps)),"create raster timestamps");timestampReadback=Buffer(uint64_t(TimestampCapacity)*sizeof(uint64_t),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Check(device.GetCommandQueue()->GetTimestampFrequency(&timestampFrequency),"read raster timestamp frequency");
        report["scene"]={{"triangles",world.GetStats().triangles},{"probes",world.GetOccupiedCenters().size()},{"source","Settings/Scene.json"}};
        report["camera_override"]={{"enabled",CustomCamera},{"eye",{CustomEye.x,CustomEye.y,CustomEye.z}},{"target",{CustomTarget.x,CustomTarget.y,CustomTarget.z}}};
        report["probe_allocation"]={{"padding_layers",settings.vxgi.probePaddingLayers},{"voxel_size",world.GetVoxelSize()},{"geometry_cells",world.GetCells().size()},{"probe_cells",world.GetProbeCells().size()},{"halo_probe_cells",world.GetStats().haloProbeCells},{"geometry_triangle_references",world.GetTriangleReferences().size()},{"geometry_fingerprint",world.GetStats().geometryFingerprint},{"query_neighbors",8},{"query_shader_changed",false}};
        report["distance_field_backend"]="Mesh SDF: shared immutable local assets and world instance AABBs; no Global SDF bake.";
        report["exact_fallback"]=ExactFallback;
        report["ssgi_threshold_fallback"]={{"enabled",!ExactFallback},{"candidate_budget",2},{"hit_threshold_cells",.9f},{"step_scale",.9f},{"maximum_samples_per_candidate",512},{"instance_bvh_nodes",meshScene.GetInstanceBVH().GetNodes().size()},{"bvh_node_bytes",sizeof(VXGIInstanceBVH::Node)}};
        report["ssgi_consumer_contract"]={{"backend",ExactFallback?"acceptance_only_legacy_exact_mesh":"production_threshold_mesh"},{"triangle_refinement",ExactFallback},{"fp64",ExactFallback}};
        report["trace_acceleration"]["scope"]="Exact triangle acceleration retained for independent probe cache update and legacy validation; SSGI fallback uses only the instance BVH and distance thresholds.";
        for(const auto& asset:VXGIMeshSDFGPUData::CollectAssets(meshScene)){
            const auto& local=asset->GetSDF();const auto& acceleration=local.GetTraceAccelerationStats();
            report["trace_acceleration"]["assets"].push_back({{"triangles",local.GetTriangles().size()},{"cell_size",local.GetGrid().cellSize},{"accelerated_cells",acceleration.acceleratedCells},{"node_count",acceleration.nodeCount},{"leaf_count",acceleration.leafCount},{"maximum_cell_candidates",acceleration.maximumCellCandidates},{"packed_words",acceleration.packedWords},{"cpu_build_ms",acceleration.buildMilliseconds},{"cpu_original_csr_references",local.GetTriangleReferences().size()},{"gpu_reference_payload_bytes",local.GetGPUTraceReferences().size()*sizeof(uint32_t)},{"gpu_offset_payload_bytes",local.GetGPUCellOffsets().size()*sizeof(uint32_t)}});
        }
        for(const auto& instance:resources.GetSceneInstances()){XMFLOAT3 low={FLT_MAX,FLT_MAX,FLT_MAX},high={-FLT_MAX,-FLT_MAX,-FLT_MAX};for(const auto& triangle:world.GetTriangles())if(triangle.instanceId==instance.editorId)for(const auto& p:triangle.positions){low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);low.z=std::min(low.z,p.z);high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);high.z=std::max(high.z,p.z);}report["scene"]["bounds"].push_back({{"instance_id",instance.editorId},{"asset",instance.name},{"minimum",{low.x,low.y,low.z}},{"maximum",{high.x,high.y,high.z}}});}
        report["scope"]=GIOnly ? "Actual LOD0 assets and production ShadowPass/GBufferPass/HBAOPass/DeferredLightingPass/SSGIPass. Deferred direct/emissive bounce source and real GBuffer remain unchanged; SSGI displays completed reconstructed/temporal/spatial GI (E/pi times intensity) before receiver albedo, Fresnel or metalness. Captured output contains GI contribution only, without deferred direct/environment base, skybox, overlays, transparent/postprocess/DLSS/editor/present. Probe RAW is frozen after explicit production warmup. GPU timings exclude readbacks and probe updates; fence-waited frames are not main-program throughput." : "Actual LOD0 assets and production ShadowPass/GBufferPass/HBAOPass/DeferredLightingPass/SSGIPass, with production RAW capture and configured IBL. Production skybox included. No transparent/postprocess/DLSS/editor/present. GPU timings exclude readbacks and acceptance diagnostics; individually fence-waited frames are not main-program throughput.";
        report["gi_only"]=GIOnly;
        report["world_fallback_contract"]="Only unresolved original SSGI segments are transformed to world coordinates, including their original bias, radius and near-plane limit. A stackless instance BVH selects at most the two nearest tight AABBs by entry distance; local unsigned SDF distance <= 0.9 cell accepts an approximate hit, without triangle refinement or FP64. The nearest world hit directly reads and interpolates the eight neighboring raw-probe SH records and evaluates directional radiance, then returns, including black or unsupported hits. No per-probe visibility traces, on-hit material lighting, sunlight ray, source albedo multiplication or independent world range.";
        if(ExactFallback)report["world_fallback_contract"]="Acceptance-only reference: original finite SSGI segment uses the previous exact Mesh SDF path, including local CSR/dense triangle BVH and FP64 world triangle refinement. It reads the identical frozen raw probe SH after exact hit and returns. No per-hit direct lighting, probe visibility or ray extension. Production remains the threshold backend.";
        report["preview_exposure"]=GIOnly?4:1;
        report["preview_mapping"]="Linear HDR saved as binary float4. PNG uses the reported fixed preview_exposure, Reinhard x/(1+x), then linear-to-sRGB. This is a diagnostic presentation, not production postprocessing.";
        report["debug_shader_policy"]=DebugShaderPolicy;
        report["native_debug"]=NativeDebug;report["native_configuration"]=NativeConfiguration;
        report["lazy_probes"]=LazyProbes;report["lazy_settle"]=LazySettle;report["compare_world_modes"]=CompareWorldModes;report["startup_toggle"]=StartupToggle;report["initialize_probe_cache"]=InitializeProbeCache;
        report["probe_cache_policy"]={{"scheduler",CompareWorldModes?"frozen_completed_production_cache":LazyProbes?"production_event_driven_finite":"legacy_cyclic_validation"},{"probes_per_frame",probeParameters.probesPerFrame},{"classify_rays",probeParameters.classifyRays},{"rays_per_update",probeParameters.raysPerUpdate},{"target_history_updates",probeParameters.maxHistoryUpdates}};
        report["shader_policy_contract"]=NativeDebug ? "Native harness /Od /Zi /MDd with _DEBUG and matching Debug native dependencies, using production ShaderCompiler Debug symbols and per-call optimization policy. Actual Debug CPU/CRT is exercised in the headless harness, which still excludes main-app UI/DLSS/Present and a Visual Studio attached debugger." : DebugShaderPolicy ? "Native harness O2 /MD with DEBUG defined, using production ShaderCompiler Debug symbols and per-call optimization policy. This validates shader policy, not Debug CPU throughput." : "Native harness O2 /MD with no DEBUG/_DEBUG, using production release shader compilation policy.";
        std::filesystem::create_directories("tmp/vxgi-stage75-performance");
    }
    ~Capture(){for(int i=0;i<3;++i)device.WaitForPreviousFrame(i);}
    void Run(){
        std::cout<<"Initial scene CPU preparation "<<report["initial_scene_prepare_cpu"].dump()<<'\n';
        std::cout<<"Scene probes "<<world.GetOccupiedCenters().size()<<"; batch size "<<probeParameters.probesPerFrame<<"; requested sweeps "<<WarmupRounds<<'\n';
        if(ColdStart){
            report["probe_warmup_batches"]=0;
            report["scope"]="Cold probe-cache startup using the actual static scene and production World/MeshSDF/Topology/Surface/ProbeState preparation, shared uploads, ProbePass/FilterPass and raster/SSGI passes each frame. Initial CPU scene/SDF preparation is timed separately before frame zero. No probe warmup or imported RAW. No visible window, Present, DLSS, postprocess, editor or transparency. Native CPU/CRT configuration is identified by native_debug/native_configuration and shader_policy_contract. Individually fence-waited frames are diagnostic throughput, not main-program FPS.";
        }else if(ReuseRaw.empty()){
            const uint32_t rounds=uint32_t((world.GetOccupiedCenters().size()+probeParameters.probesPerFrame-1)/probeParameters.probesPerFrame);
            const uint32_t batches=WarmupBatches ? std::min(rounds*WarmupRounds,WarmupBatches) : rounds*WarmupRounds;
            for(uint32_t i=0;i<batches;++i){Warm();if(i%64==0)std::cout<<"Probe warm batch "<<i+1<<'/'<<batches<<'\n';}
            report["probe_warmup_batches"]=batches;
            // Production filter timestamps are returned when their frame slot
            // is next recorded. These three real update batches retire the
            // last measured sweep's three timers, and remain separately tagged.
            for(uint32_t i=0;i<3;++i)Warm(true);
            report["probe_warmup_timing_drain_batches"]=3;
            report["stable_scene_prepare_cpu"]=PrepareSceneSources();
            if(VerifyMovement) VerifyMeshInstanceUploads();
            SaveRaw();
        }else{LoadRaw();report["probe_warmup_batches"]=0;}
        if(!CavityRaysPath.empty())InspectCavityRays();
        report["requested_probe_warmup_rounds"]=WarmupRounds;
        report["probe_warmup_batch_limit"]=WarmupBatches;
        report["raw_contract"]=ColdStart ? "Live production probe history is updated before every rendered frame from initially unclassified records; no warmup/import. raw_snapshot is exported only after all sampled frames and identifies the final history, not a frozen input." : "One immutable RAW snapshot, either actual production capture using requested_probe_warmup_rounds budget sweeps limited by optional probe_warmup_batch_limit, or explicitly imported saved snapshot. A nonzero batch limit may leave most probes unclassified and is intended for pipeline smoke checks, not image-quality acceptance. Actual executed count is probe_warmup_batches. Imported data is identified by raw_snapshot and its SHA256; requested warmup rounds are not executed for imports. Maximum history count 32 is a cap; probe work excluded from renderer pass timings.";
        report["label"]=Label;report["internal_resolution"]={Width,Height};const auto historyDesc=resources.GetSSGIHistoryTextures(0).gi->GetDesc();report["ssgi_history_resolution"]={historyDesc.Width,historyDesc.Height};report["ssgi_trace_resolution"]={historyDesc.Width,historyDesc.Height};report["downsample_factor"]=DownsampleFactor;report["samples_per_sequence"]=Samples;report["repeat_sequences"]=Repeat;Require(historyDesc.Width==(Width+DownsampleFactor-1)/DownsampleFactor&&historyDesc.Height==(Height+DownsampleFactor-1)/DownsampleFactor,"production GI history does not match requested trace factor");
        report["timing_contract"]="D3D12 GPU timestamps around each RDG live pass including its preceding transitions. Outer renderer timer includes final barriers. All frames fence-waited; no readbacks, Present, editor, postprocess, DLSS, transparency or probe updates inside timed region. Current quality/range and sample counts; exact_fallback explicitly identifies the acceptance-only legacy reference algorithm.";
        report["cold_start"]=ColdStart;
        if(ColdStart)report["cold_start_timing_contract"]="Per-frame CPU scene revision checks, shared upload preparation, probe/filter command recording, current-slot probe Poll and renderer recording are separate. Renderer GPU timestamps exclude probe/filter work. Additional probe/filter outer GPU spans bracket all commands emitted by their production Record calls, including optional reset/statistics copy. Probe production timestamps are polled after that same frame's fence wait and labeled with its plan frame; filter production timestamps become public only during the next Record on that slot and are written back to their original frame (normally three frames later). Final unreused filter slots remain explicitly pending; their outer GPU spans are still available. CPU frame wall includes serial fence wait and diagnostics; never main-program FPS. Only the final frame copies/maps/scans HDR/depth.";
        if(LazyProbes){report["raw_contract"]="Live production VXGIProbeCache event invalidation and finite per-probe scheduling. No imported/frozen warmup shortcut. Cold frames are sampled from unclassified startup; lazy_settle performs real configured capture work without raster timing until a zero-work plan; lazy_steady samples the unchanged clean cache. RAW export and bit comparison occur outside sampled frame timings.";report["scope"]="Hidden no-Present configured-asset production raster with current event-driven VXGIProbeCache/ProbePass/FilterPass, MotionUV, temporal HBAO and geometry-history copy, shadows, GBuffer, deferred lighting, SSGI and skybox. Current configured ray counts, range, per-frame budget and finite capture target. Initial scene/SDF builds are timed separately. Main-app editor/UI/DLSS/postprocess/transparency and attached Visual Studio debugger are excluded. Fence-waited diagnostic frames are not main-program FPS.";report["timing_contract"]="Production RDG pass GPU timestamps include their preceding transitions. Renderer GPU excludes probe/filter and diagnostic image copies; separate outer probe/filter spans include all their emitted commands. CPU stages separate completed-slot Poll, dependency-mask acceptance, scene checks, finite cache preparation, shared uploads, Probe/Filter Record and renderer Record. Cold/settle/clean phases are labeled separately. No Present or main-program FPS claim.";report["lazy_raster_extra_passes"]={"MotionUV","ScalarTemporalFilter.AmbientOcclusion","HBAOGeometryHistoryCopy"};report["hbao_temporal_delta_seconds"]=1.0/160.0;}
        report["pipeline"]={{"ssgi_quality",settings.ssgi.quality},{"ssgi_radius",settings.ssgi.radius},{"mesh_sdf_local_cell_size",settings.vxgi.sdfCellSize},{"probe_update_max_ray",surface.GetParameters().maxRayDistance},{"world_fallback_max_ray",settings.ssgi.radius},{"trace_downsample_factor",DownsampleFactor}};
        if(InitializeProbeCache){
            SaveRaw();
            Require(report["raw_validation"]["quality_ready"].get<bool>()&&report["raw_validation"]["valid_minimum_updates"].get<uint32_t>()>=probeParameters.maxHistoryUpdates,"ResourceManager returned before every valid probe reached the configured finite capture target");
            report["resource_initialization"]["initial_raw_validation"]=report["raw_validation"];
            report["resource_initialization"]["activity_before_first_raster"]=Activity();
            report["scope"]="Actual ResourceManager::InitVXGI completes the configured real-scene probe cache before any raster frame. Subsequent production frames use mode 2, then 0 for two frames, then 2, with normal lazy preparation. First and every sampled frame must perform zero probe/filter GPU work; initialized RAW is checked byte-for-byte after the complete sequence. No visible UI or Present.";
            report["raw_contract"]="Production ResourceManager initialization, without imported RAW or a test-owned convergence loop, prepares every valid probe to the configured finite update target before the reference is copied. The same immutable RAW is consumed by every sampled frame and verified after toggling world mode 2 -> 0 -> 2.";
        }
        if(LazyProbes)report["lazy_activity_before_cold"]=Activity();
        if(CompareWorldModes){
            Require(report["raw_validation"]["quality_ready"].get<bool>()&&report["raw_validation"]["valid_minimum_updates"].get<uint32_t>()>=probeParameters.maxHistoryUpdates,"world A/B requires a fully classified RAW cache at the configured finite capture target");
            report["world_ab"]={{"mode_a",0},{"mode_b",2},{"order",{0,2,2,0}},{"rounds",ABRounds},{"blocks",4*ABRounds},{"warmup_samples_per_block",ABWarmup},{"retained_samples_per_block",Samples},{"same_process_and_device",true},{"same_trace_pso",true},{"same_camera",Cameras.front()},{"history_reset_per_block",true},{"probe_updates_during_comparison",0},{"filter_updates_during_comparison",0},{"timed_blocks_image_readback",false},{"validation_frames_separate",true}};
            report["scope"]="Direct interleaved A/B on the same hidden device and production trace PSO: A=original SSGI screen tracing then zero on an unresolved ray (world mode 0); B=same screen tracing then production two-AABB Mesh SDF threshold fallback and frozen raw-probe SH (world mode 2). Same configured assets, camera, resolution, frame/sample sequence, MotionUV, temporal HBAO, geometry-history copy, shadows, GBuffer, deferred lighting and skybox. Each block resets SSGI and AO history validity, discards its labeled warmup frames and retains the same subsequent frame indices. Completed production RAW is imported unchanged for both; no Probe/Filter capture or initial scene/SDF construction in sampled GPU spans. No visible window, Present, editor, postprocess, DLSS or transparency. Fence-waited diagnostic frames are not main-program FPS.";
            report["raw_contract"]="The same imported, fully classified production lazy-cache RAW at the configured finite history target is immutable across all A/B blocks; byte-for-byte GPU verification runs after all blocks. Static probe/filter producer cost is excluded here and separately measured as zero when the production event-driven cache is clean. This comparison measures the consumer cost of enabling world fallback, not startup or moving-scene capture.";
            report["world_ab_history_contract"]="At each block SSGIHistoryState is reset and hbaoHistoryValid=false. Production temporal shaders initialize all output pixels and bypass previous history when invalid; no previous block history contributes. Samples 0..warmup+retained-1 and camera are identical in every block. Timed blocks do no HDR/depth copies, maps, scans or PNG encoding. After every timed block has completed, two separately labeled validation sequences repeat the same sample range and capture only their final HDR/depth outside renderer GPU timestamps.";
            report["raster_extra_passes"]={"MotionUV","ScalarTemporalFilter.AmbientOcclusion","HBAOGeometryHistoryCopy"};report["hbao_temporal_delta_seconds"]=1.0/160.0;
            const int order[4]={0,2,2,0};report["ab_rounds"]=ABRounds;report["ab_warmup_frames"]=ABWarmup;report["ab_retained_samples_per_block"]=Samples;report["ab_order"]=Json::array();for(uint32_t block=0;block<4*ABRounds;++block)report["ab_order"].push_back(order[block%4]);
            for(uint32_t block=0;block<4*ABRounds;++block){history={};hbaoHistoryValid=false;const int mode=order[block%4];for(uint32_t sample=0;sample<ABWarmup+Samples;++sample)Frame(Cameras.front(),mode,sample,false,"world_ab_block"+std::to_string(block),block);std::cout<<"Profiled world A/B block "<<block+1<<'/'<<4*ABRounds<<" mode "<<mode<<'\n';}
            report["ab_validation_frames_separate"]=true;report["ab_validation_sequence_count"]=2;report["ab_validation_frames"]=2*(ABWarmup+Samples);
            for(uint32_t variant=0;variant<2;++variant){history={};hbaoHistoryValid=false;const int mode=variant==0?0:2;for(uint32_t sample=0;sample<ABWarmup+Samples;++sample)Frame(Cameras.front(),mode,sample,sample+1==ABWarmup+Samples,"world_ab_validation",4*ABRounds+variant);std::cout<<"Validated world A/B mode "<<mode<<'\n';}
        }else if(StartupToggle){
            report["startup_toggle_schedule"]={{"initial_mode",2},{"off_samples",{8,9}},{"restore_sample",10},{"continuous_sample_frame",true},{"producer_continues_while_off",true},{"history_contract","Production SSGIHybridState source key invalidates SSGI history on the mode transitions. AO history and producer scheduling are not reset."}};
            history={};
            for(uint32_t sample=0;sample<Samples;++sample)Frame(Cameras.front(),sample==8||sample==9?0:2,sample,sample+1==Samples,"cold_start");
        }else for(int camera:Cameras)for(int mode:Modes)for(uint32_t repeat=0;repeat<Repeat;++repeat){history={};for(uint32_t sample=0;sample<Samples;++sample)Frame(camera,mode,sample,sample+1==Samples&&repeat+1==Repeat,LazyProbes?"cold_start":"static",repeat);std::cout<<"Profiled camera "<<camera<<" mode "<<mode<<" repeat "<<repeat<<'\n';}
        if(LazyProbes)report["lazy_activity_after_cold"]=Activity();
        if(InitializeProbeCache){
            const auto before=report["resource_initialization"]["activity_before_first_raster"],after=Activity();
            for(const auto* stage:{"probe","filter"})for(auto it=before[stage].begin();it!=before[stage].end();++it)
                if(it.key()!="idle_frames")Require(after[stage][it.key()]==it.value(),std::string("initialized first/toggled frames performed GPU cache work: ")+stage+"."+it.key());
            VerifyFrozenRaw();report["resource_initialization"]["first_and_toggled_frames_zero_cache_gpu_work"]=true;report["resource_initialization"]["sampled_frames"]=Samples;
        }
        if(LazySettle){
            const auto settleStart=Clock::now();uint32_t batches=0;bool clean=false;
            for(;batches<SettleMaxBatches;++batches){
                const bool worked=WarmLazy(batches);
                if(batches%128==0)std::cout<<"Lazy settle batch "<<batches+1<<"; valid "<<state.GetStats().valid<<"; disabled "<<state.GetStats().disabled<<"; average updates "<<state.GetStats().averageUpdates<<'\n';
                if(!worked){clean=true;++batches;break;}
            }
            Require(clean,"lazy cache did not reach a clean idle plan within settle batch limit");
            report["lazy_settle_batches"]=batches;report["lazy_settle_wall_ms"]=Milliseconds(settleStart);report["lazy_settle_batch_limit"]=SettleMaxBatches;
            // Establish one byte-for-byte reference only after the configured
            // finite capture target has completed. The copy is outside frames.
            SaveRaw();const auto before=Activity();report["lazy_activity_before_steady"]=before;
            for(uint32_t sample=0;sample<SteadySamples;++sample)Frame(Cameras.front(),Modes.front(),sample,sample+1==SteadySamples,"lazy_steady");
            const auto after=Activity();report["lazy_activity_after_steady"]=after;
            for(const auto* stage:{"probe","filter"})for(auto it=before[stage].begin();it!=before[stage].end();++it)
                if(it.key()!="idle_frames")Require(after[stage][it.key()]==it.value(),std::string("steady lazy phase performed GPU cache work: ")+stage+"."+it.key());
            report["lazy_steady_samples"]=SteadySamples;report["lazy_steady_zero_cache_gpu_work"]=true;VerifyFrozenRaw();
        }
        if(!ColdStart)VerifyFrozenRaw();
        if(ColdStart){if(!LazySettle&&!InitializeProbeCache)SaveRaw();report["cold_start_scene_totals"]={{"prepare_calls",scenePrepareCalls},{"world_geometry_rebuilds",worldGeometryRebuilds},{"mesh_asset_builds",meshCache.GetStats().builds},{"first_sweep_submission_frames",(world.GetOccupiedCenters().size()+probeParameters.probesPerFrame-1)/probeParameters.probesPerFrame}};}
        for(UINT slot=0;slot<3;++slot){const auto& uploads=shared.GetMeshUploadStats(slot);Require(shared.GetInstanceBVHNodeCount(int(slot))==meshScene.GetInstanceBVH().GetNodes().size() && shared.GetInstanceBVHRevision(int(slot))==meshScene.GetInstanceBVH().GetRevision(),"actual scene slot contains a stale instance BVH");report["mesh_gpu_uploads"].push_back({{"slot",slot},{"assets",shared.GetAssetCount(slot)},{"instances",shared.GetInstanceCount(slot)},{"asset_uploads",uploads.assetUploads},{"instance_uploads",uploads.instanceUploads},{"material_map_uploads",uploads.materialMapUploads},{"asset_bytes_uploaded",uploads.assetBytes},{"instance_bvh_uploads",uploads.instanceBVHUploads},{"instance_bvh_bytes_uploaded",uploads.instanceBVHBytes},{"instance_bvh_nodes",shared.GetInstanceBVHNodeCount(int(slot))},{"distance_nodes",shared.GetDistanceNodeCount(int(slot))}});}
        constexpr std::array<const char*,11> bufferNames={"distance_nodes","cell_offsets","triangle_refs_and_bounds","local_triangles","probe_centers","probe_lookup","probe_neighbors","mesh_assets","mesh_instances","mesh_material_ids","instance_bvh"};
        for(UINT slot=0;slot<3;++slot)for(UINT index=0;index<bufferNames.size();++index)if(auto* buffer=shared.Get(slot,index)){const auto desc=buffer->GetDesc();const auto allocation=device.GetDevice()->GetResourceAllocationInfo(0,1,&desc);report["shared_trace_buffer_allocations"].push_back({{"slot",slot},{"type",bufferNames[index]},{"payload_bytes",desc.Width},{"committed_allocation_bytes",allocation.SizeInBytes}});}
        report["shared_trace_buffer_allocation_contract"]="Actual public DEFAULT buffer descriptions and D3D12 GetResourceAllocationInfo for all eleven prepared shared-scene buffers per slot, including the instance BVH. Shared bytes may additionally include retained staging. Neither physical residency nor whole renderer VRAM.";
        const auto& cacheStats=meshCache.GetStats();
        report["memory_and_cache"]={{"mesh_assets",cacheStats.assets},{"mesh_asset_builds",cacheStats.builds},{"mesh_cache_hits",cacheStats.cacheHits},{"mesh_asset_cpu_retained_bytes",cacheStats.memoryBytes},{"world_cpu_retained_bytes",world.GetStats().memoryBytes},{"topology_cpu_retained_bytes",topology.GetStats().memoryBytes},{"surface_cpu_retained_bytes",surface.GetStats().memoryBytes},{"shared_gpu_bytes_including_retained_staging",shared.GetGPUMemoryBytes()},{"probe_gpu_bytes_including_readbacks",probes.GetGPUMemoryBytes()},{"filter_gpu_bytes",filter.GetGPUMemoryBytes()}};
        const bool diagnosticsValid=Warnings();report["present_calls"]=0;report["window_visible"]=IsWindowVisible(window.handle)!=FALSE;report["debug_layer"]=true;report["gpu_based_validation"]=GpuValidation;report["completed"]=diagnosticsValid;
        std::ofstream file("tmp/VXGIStage75-"+Label+"-"+std::to_string(Width)+"x"+std::to_string(Height)+"-report.json");file<<std::setw(2)<<report<<'\n';file.close();Require(diagnosticsValid,"actual raster performance run has unexpected D3D warnings/errors; saved report retains timings and diagnostics");
    }
private:
#include "VXGICavityCapture.inl"
#include "VXGIRayRoutingCapture.inl"
    Json Activity()const{
        const auto& p=probes.GetActivityStats();const auto& f=filter.GetActivityStats();
        return {{"probe",{{"history_copies",p.historyCopies},{"remap_dispatches",p.remapDispatches},{"invalidation_dispatches",p.invalidationDispatches},{"update_dispatches",p.updateDispatches},{"feedback_copies",p.feedbackCopies},{"legacy_snapshot_copies",p.legacySnapshotCopies},{"active_batches",p.activeBatches},{"idle_frames",p.idleFrames}}},
            {"filter",{{"remap_dispatches",f.remapDispatches},{"invalidation_dispatches",f.invalidationDispatches},{"reset_dispatches",f.resetDispatches},{"visibility_dispatches",f.visibilityDispatches},{"filter_dispatches",f.filterDispatches},{"active_batches",f.activeBatches},{"idle_frames",f.idleFrames}}}};
    }
    void PollCompletedSlots(){
        for(int slot=0;slot<3;++slot){const auto completed=device.GetFence(slot)->GetCompletedValue();Require(completed!=UINT64_MAX,"lazy probe device removed");if(completed>=device.GetFenceValue(slot))Require(probes.Poll(device,slot,state),"poll completed lazy probe slot");}
    }
    void VerifyMeshInstanceUploads(){
        Require(!resources.GetSceneInstances().empty(),"instance-upload verification requires a model");
        const auto builds=meshCache.GetStats().builds;
        std::array<VXGIGPUScene::MeshUploadStats,3> before;
        std::array<std::array<ID3D12Resource*,5>,3> assetBuffers{};
        constexpr std::array<UINT,5> assetSlots={0,1,2,3,7};
        for(int slot=0;slot<3;++slot){Require(shared.IsMeshBackend(slot),"instance-upload verification requires all three frame slots warmed");before[size_t(slot)]=shared.GetMeshUploadStats(slot);for(size_t i=0;i<assetSlots.size();++i)assetBuffers[size_t(slot)][i]=shared.Get(slot,assetSlots[i]);}
        auto& instance=resources.GetSceneInstances().front();const auto original=instance.translation;
        for(UINT phase=0;phase<2;++phase){
            instance.translation=original;if(phase==0)instance.translation.x+=0.125f;
            Require(meshScene.UpdateInstances(resources.GetSceneInstances(),meshCache,{settings.vxgi.sdfCellSize}),meshScene.GetLastError());
            for(UINT frame=0;frame<3;++frame){const int slot=Reset();Require(shared.Prepare(device,slot,world,meshScene,&topology),"prepare moved/restored Mesh SDF instance table");const auto& after=shared.GetMeshUploadStats(slot);
                Require(after.assetUploads==before[size_t(slot)].assetUploads&&after.instanceUploads==before[size_t(slot)].instanceUploads+phase+1&&after.materialMapUploads==before[size_t(slot)].materialMapUploads,"TRS update reuploaded immutable asset/material data or skipped instance data");
                for(size_t i=0;i<assetSlots.size();++i)Require(shared.Get(slot,assetSlots[i])==assetBuffers[size_t(slot)][i],"TRS update replaced an immutable GPU asset buffer");Submit(slot);Wait(slot);}
        }
        Require(meshCache.GetStats().builds==builds,"TRS upload verification rebaked a model asset");
        report["mesh_instance_upload_verification"]={{"completed",true},{"temporary_translation_x",0.125},{"restored_before_raster",true},{"frame_slots",3},{"instance_uploads_added_per_slot",2},{"asset_uploads_added",0},{"material_map_uploads_added",0},{"asset_builds_added",0}};
    }
    Json PrepareSceneSources(){
        const auto worldBefore=world.GetRevision(),worldGeometryBefore=world.GetGeometryRevision(),meshBefore=meshScene.GetRevision(),assetBuildsBefore=meshCache.GetStats().builds;
        auto start=Clock::now();Require(world.Update(resources.GetSceneInstances(),settings.vxgi.voxelSize,uint32_t(settings.vxgi.probePaddingLayers)),world.GetLastError());const double worldMs=Milliseconds(start);
        start=Clock::now();Require(meshScene.UpdateInstances(resources.GetSceneInstances(),meshCache,{settings.vxgi.sdfCellSize}),meshScene.GetLastError());const double meshMs=Milliseconds(start);
        start=Clock::now();Require(topology.Update(world),topology.GetLastError());const double topologyMs=Milliseconds(start);
        VXGISurface::Parameters source;source.lightDirection=settings.lighting.lightDir;source.lightColor=settings.lighting.lightColor;source.materialMipLevel=settings.vxgi.surfaceMaterialMipLevel;source.rayMinDistance=settings.vxgi.surfaceRayMinDistance;source.maxRayDistance=settings.vxgi.surfaceMaxRayDistance;
        start=Clock::now();Require(surface.Update(resources.GetGlobalMaterials(),world.GetGeometryRevision(),meshScene.GetRevision(),world.GetMaterialRevision(),source,world.GetCoverageRevision()),surface.GetLastError());const double surfaceMs=Milliseconds(start);
        ++scenePrepareCalls;const bool worldRebuilt=world.GetGeometryRevision()!=worldGeometryBefore;worldGeometryRebuilds+=worldRebuilt?1u:0u;
        return {{"world_cpu_ms",worldMs},{"mesh_sdf_cpu_ms",meshMs},{"topology_cpu_ms",topologyMs},{"surface_cpu_ms",surfaceMs},{"world_revision_before",worldBefore},{"world_revision_after",world.GetRevision()},{"world_geometry_revision",world.GetGeometryRevision()},{"mesh_instance_revision_before",meshBefore},{"mesh_instance_revision_after",meshScene.GetRevision()},{"mesh_asset_builds_before",assetBuildsBefore},{"mesh_asset_builds_after",meshCache.GetStats().builds},{"world_geometry_rebuilt",worldRebuilt},{"world_geometry_rebuilds_total",worldGeometryRebuilds}};
    }
    std::string Stem()const{return Label+"-"+std::to_string(Width)+"x"+std::to_string(Height);}
    uint64_t RawBytes()const{return uint64_t(world.GetOccupiedCenters().size())*sizeof(VXGIProbeState::GPURecord);}
    void InspectRaw(const void* data,bool imported){
        const auto records=std::span(static_cast<const VXGIProbeState::GPURecord*>(data),world.GetOccupiedCenters().size());
        uint64_t valid=0,disabled=0,unclassified=0,updatedValid=0,nonzeroSH=0,invalid=0;
        std::array<uint64_t,5> reasons={};std::map<uint32_t,uint64_t> validUpdates;
        uint32_t maximumFrame=0,minimumValidUpdates=UINT32_MAX,maximumValidUpdates=0;
        double maximumSH=0,totalValidUpdates=0;
        for(const auto& record:records){
            bool recordValid=record.state<=VXGIProbeState::Disabled&&record.reason<=VXGIProbeState::SampleFailure&&std::isfinite(record.backfaceRatio)&&record.backfaceRatio>=0&&record.backfaceRatio<=1&&std::isfinite(record.minHitDistance)&&record.minHitDistance>=0&&(record.classificationRays==0||(record.classificationRays>=8&&record.classificationRays<=256&&record.classificationRays%2==0));
            bool hasSH=false;for(const auto& coefficient:record.sh){recordValid&=std::isfinite(coefficient.x)&&std::isfinite(coefficient.y)&&std::isfinite(coefficient.z)&&coefficient.w==0;for(float value:{coefficient.x,coefficient.y,coefficient.z}){hasSH|=value!=0;if(std::isfinite(value))maximumSH=std::max(maximumSH,double(std::abs(value)));}}
            invalid+=!recordValid;nonzeroSH+=hasSH;maximumFrame=std::max(maximumFrame,record.lastUpdateFrame);
            if(record.reason<reasons.size())++reasons[record.reason];
            if(record.state==VXGIProbeState::Valid){++valid;updatedValid+=record.updateCount>0;++validUpdates[record.updateCount];minimumValidUpdates=std::min(minimumValidUpdates,record.updateCount);maximumValidUpdates=std::max(maximumValidUpdates,record.updateCount);totalValidUpdates+=record.updateCount;}
            else if(record.state==VXGIProbeState::Disabled)++disabled;else if(record.state==VXGIProbeState::Unclassified)++unclassified;
        }
        Require(invalid==0,"frozen RAW contains invalid SH/classification/layout records");
        if(!imported)Require(state.AcceptReadback(records,state.GetStats().submittedFrameIndex),state.GetLastError());
        Json distribution=Json::array();for(const auto& [updates,count]:validUpdates)distribution.push_back({{"updates",updates},{"probes",count}});
        const bool qualityReady=unclassified==0&&reasons[VXGIProbeState::TraceFailure]==0&&reasons[VXGIProbeState::SampleFailure]==0&&updatedValid==valid;
        report["raw_validation"]={{"records",records.size()},{"imported",imported},{"valid",valid},{"disabled",disabled},{"unclassified",unclassified},{"reason_none",reasons[0]},{"reason_backface",reasons[1]},{"reason_unsafe_origin",reasons[2]},{"reason_trace_failure",reasons[3]},{"reason_sample_failure",reasons[4]},{"invalid_records",invalid},{"nonzero_sh_records",nonzeroSH},{"maximum_absolute_sh_coefficient",maximumSH},{"valid_probes_with_updates",updatedValid},{"valid_minimum_updates",valid?minimumValidUpdates:0},{"valid_maximum_updates",maximumValidUpdates},{"valid_average_updates",valid?totalValidUpdates/double(valid):0},{"valid_update_distribution",distribution},{"maximum_record_update_frame",maximumFrame},{"cache_classification_complete",unclassified==0},{"quality_ready",qualityReady}};
        if(!ColdStart&&!imported&&WarmupBatches==0){Require(qualityReady,"full warmup left unclassified, failed or uncaptured valid probes");Require(valid==0||minimumValidUpdates>=WarmupRounds,"full warmup did not capture every valid probe on every requested sweep");}
    }
    void SaveRaw(){
        const int slot=Reset();auto copy=Buffer(RawBytes(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(raw.Get(),VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);device.GetCommandList()->CopyBufferRegion(copy.Get(),0,raw.Get(),0,RawBytes());Transition(raw.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);Submit(slot);Wait(slot);
        void* mapped=nullptr;Check(copy->Map(0,nullptr,&mapped),"map frozen RAW");InspectRaw(mapped,false);const std::string path="tmp/vxgi-stage75-performance/"+Stem()+"-raw.bin";std::ofstream file(path,std::ios::binary);file.write(static_cast<const char*>(mapped),std::streamsize(RawBytes()));Require(bool(file),"save frozen RAW");copy->Unmap(0,nullptr);report["raw_snapshot"]=path;
    }
    void LoadRaw(){
        Require(std::filesystem::file_size(ReuseRaw)==RawBytes(),"saved RAW size differs from actual world probe count");std::vector<uint8_t> bytes(size_t(RawBytes()),0);std::ifstream file(ReuseRaw,std::ios::binary);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));Require(bool(file),"load frozen RAW");InspectRaw(bytes.data(),true);
        const int slot=Reset();auto upload=Buffer(RawBytes(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map imported RAW upload");std::memcpy(mapped,bytes.data(),bytes.size());upload->Unmap(0,nullptr);raw=Buffer(RawBytes(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);Transition(raw.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);device.GetCommandList()->CopyBufferRegion(raw.Get(),0,upload.Get(),0,RawBytes());Transition(raw.Get(),D3D12_RESOURCE_STATE_COPY_DEST,VXGIGPUScene::ReadState);Submit(slot);Wait(slot);report["raw_snapshot"]=ReuseRaw;
    }
    void VerifyFrozenRaw(){
        const std::string path=report["raw_snapshot"].get<std::string>();std::vector<uint8_t> expected(size_t(RawBytes()),0);std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(expected.data()),std::streamsize(expected.size()));Require(bool(file),"read immutable RAW reference");
        const int slot=Reset();auto copy=Buffer(RawBytes(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(raw.Get(),VXGIGPUScene::ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);device.GetCommandList()->CopyBufferRegion(copy.Get(),0,raw.Get(),0,RawBytes());Transition(raw.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,VXGIGPUScene::ReadState);Submit(slot);Wait(slot);
        void* mapped=nullptr;Check(copy->Map(0,nullptr,&mapped),"map final frozen RAW");const bool identical=std::memcmp(mapped,expected.data(),expected.size())==0;copy->Unmap(0,nullptr);Require(identical,"RAW changed during frozen camera/mode capture sequences");report["raw_gpu_bytes_unchanged_after_all_captures"]=true;
    }
    ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES initialState,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){const auto heap=CD3DX12_HEAP_PROPERTIES(heapType);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,flags);ComPtr<ID3D12Resource> result;Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initialState,nullptr,IID_PPV_ARGS(&result)),"create raster acceptance buffer");return result;}
    void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);device.GetCommandList()->ResourceBarrier(1,&barrier);}
    int Reset(){const int slot=int(submissions%3);device.WaitForPreviousFrame(slot);Check(device.GetCommandAllocator(slot)->Reset(),"reset raster allocator");Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset raster command list");resources.BeginPassConstantsFrame(slot);resources.ResetTransientSrvUavDescriptors(slot);resources.BeginRDGFrame(&device,slot);Require(!IsWindowVisible(window.handle),"raster acceptance HWND became visible");return slot;}
    uint64_t Submit(int slot){Check(device.GetCommandList()->Close(),"close raster commands");ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);const auto fence=++device.GetFenceValue(slot);Check(device.GetCommandQueue()->Signal(device.GetFence(slot),fence),"signal raster commands");++submissions;return fence;}
    void Wait(int slot){device.WaitForPreviousFrame(slot);Check(device.GetDevice()->GetDeviceRemovedReason(),"raster acceptance device status");}
    void Warm(bool drain=false){
        const auto frameStart=Clock::now();const int slot=Reset();
        VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),meshScene.GetRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};
        auto start=Clock::now();Require(state.Prepare(sources,probeParameters),state.GetLastError());const double stateMs=Milliseconds(start);const auto plan=state.GetPendingPlan();
        start=Clock::now();Require(shared.Prepare(device,slot,world,meshScene,&topology),"prepare actual world");const double sharedMs=Milliseconds(start);
        auto* cmd=device.GetCommandList();cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst);
        start=Clock::now();Require(probes.Record(device,resources.GetMainDescriptorHeap(),slot,shared,legacySDF,surface,resources.GetMaterialBufferGPUAddress(),state),"record probe warmup");const double probeMs=Milliseconds(start);
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+1);raw=probes.GetDisplayHistory();
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+2);
        start=Clock::now();Require(filter.Record(device,resources.GetMainDescriptorHeap(),slot,shared,legacySDF,raw.Get(),state,world.GetVoxelSize(),settings.vxgi.probeFilterNeighborWeight,surface,resources.GetMaterialBuffer()),"record filter warmup");const double filterMs=Milliseconds(start);
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+3);
        const int64_t previous=warmFilterSamples[size_t(slot)];
        if(previous>=0){auto& old=report["probe_warmup_samples"][size_t(previous)];old["filter_production_gpu_ms"]=filter.GetGPUMilliseconds();old["filter_timestamp_pending"]=false;old["filter_timestamp_retrieved_at_plan_frame"]=plan.frameIndex;}
        warmFilterSamples[size_t(slot)]=int64_t(report["probe_warmup_samples"].size());
        cmd->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst,4,timestampReadback.Get(),uint64_t(ColdQueryFirst)*sizeof(uint64_t));
        const auto fence=Submit(slot);Require(probes.OnSubmitted(slot,fence,state)&&filter.OnSubmitted(slot,fence),"commit probe warmup");
        start=Clock::now();Wait(slot);const double waitMs=Milliseconds(start);Require(probes.Poll(device,slot,state),"poll probe warmup");
        void* timing=nullptr;Check(timestampReadback->Map(0,nullptr,&timing),"read warmup timestamps");const auto* ticks=static_cast<const uint64_t*>(timing);
        Require(ticks[ColdQueryFirst+1]>=ticks[ColdQueryFirst]&&ticks[ColdQueryFirst+3]>=ticks[ColdQueryFirst+2],"negative warmup GPU timestamp interval");
        const double probeOuter=double(ticks[ColdQueryFirst+1]-ticks[ColdQueryFirst])*1000/double(timestampFrequency),filterOuter=double(ticks[ColdQueryFirst+3]-ticks[ColdQueryFirst+2])*1000/double(timestampFrequency);timestampReadback->Unmap(0,nullptr);
        const uint32_t sweepBatches=uint32_t((world.GetOccupiedCenters().size()+probeParameters.probesPerFrame-1)/probeParameters.probesPerFrame);
        const auto& stats=state.GetStats();
        report["probe_warmup_samples"].push_back({{"plan_frame",plan.frameIndex},{"sweep",(plan.frameIndex-1)/sweepBatches+1},{"timing_drain",drain},{"slot",slot},{"cursor_start",plan.start},{"update_count",plan.count},{"reset_mode",plan.resetMode},{"state_prepare_cpu_ms",stateMs},{"shared_prepare_cpu_ms",sharedMs},{"probe_record_cpu_ms",probeMs},{"filter_record_cpu_ms",filterMs},{"fence_wait_wall_ms",waitMs},{"frame_wall_ms",Milliseconds(frameStart)},{"probe_record_outer_gpu_ms",probeOuter},{"filter_record_outer_gpu_ms",filterOuter},{"probe_production_gpu_ms",probes.GetGPUMilliseconds()},{"filter_production_gpu_ms",nullptr},{"filter_timestamp_pending",true},{"snapshot_completed_frame",stats.completedFrameIndex},{"snapshot_valid",stats.valid},{"snapshot_disabled",stats.disabled},{"snapshot_unclassified",stats.unclassified}});
    }
    Json RecordColdProbes(int slot,uint32_t sample,const std::string& phase="cold_start",bool sampledFrame=true){
        auto start=Clock::now();if(LazyProbes)PollCompletedSlots();const double priorPollMs=Milliseconds(start);
        start=Clock::now();if(LazyProbes)cache.AcceptDependencies(probes.TakeCompletedDependencies(),state);const double dependencyMs=Milliseconds(start);
        start=Clock::now();Require(resources.RefreshGlobalMaterials(&device),"refresh cold-start materials");const double materialMs=Milliseconds(start);
        Json cold=InitializeProbeCache?Json{{"owner","VXGIResources::Prepare"}}:PrepareSceneSources();cold["material_refresh_cpu_ms"]=materialMs;cold["prior_completed_slots_poll_cpu_ms"]=priorPollMs;cold["prior_dependency_accept_cpu_ms"]=dependencyMs;cold["sample"]=sample;cold["slot"]=slot;cold["phase"]=phase;
        VXGIProbeState::Sources sources{world.GetGeometryRevision(),world.GetAllocationRevision(),meshScene.GetRevision(),surface.GetRevision(),world.GetOccupiedCenters().size(),surface.GetCoverageRevision()};
        start=Clock::now();if(InitializeProbeCache)Require(resources.GetVXGI().Prepare(device,resources.GetSceneInstances(),resources.GetGlobalMaterials(),settings.vxgi,settings.lighting),resources.GetVXGI().GetLastError());else if(LazyProbes){Require(cache.Prepare(world,meshScene,surface,state,probeParameters),cache.GetLastError());probes.SetDependencyDomain(cache.GetDomain());if(filterTopologyRevision!=topology.GetRevision()){filter.SetTopology(topology.GetNeighbors());filterTopologyRevision=topology.GetRevision();}}else Require(state.Prepare(sources,probeParameters),state.GetLastError());
        cold["state_prepare_cpu_ms"]=Milliseconds(start);cold["cache_prepare_cpu_ms"]=LazyProbes?cold["state_prepare_cpu_ms"]:Json(0);
        const auto plan=state.GetPendingPlan();cold["plan_frame"]=plan.frameIndex;cold["epoch"]=plan.epoch;cold["reset_mode"]=plan.resetMode;cold["cursor_start"]=plan.start;cold["update_count"]=plan.count;cold["has_work"]=plan.HasWork();cold["layout_changed"]=plan.layoutChanged;cold["statistics_snapshot_requested"]=!LazyProbes&&(plan.resetMode!=0||plan.frameIndex%32==0);cold["compact_feedback_requested"]=LazyProbes&&plan.count>0;
        if(InitializeProbeCache)Require(!plan.HasWork(),"initialized first/toggled main frame scheduled probe work");
        start=Clock::now();Require(shared.Prepare(device,slot,world,meshScene,&topology),"prepare cold-start render slot geometry");cold["shared_prepare_cpu_ms"]=Milliseconds(start);
        start=Clock::now();Require(probes.Poll(device,slot,state),"poll prior cold-start slot");cold["prior_probe_poll_cpu_ms"]=Milliseconds(start);
        auto* cmd=device.GetCommandList();cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst);
        start=Clock::now();Require(probes.Record(device,resources.GetMainDescriptorHeap(),slot,shared,legacySDF,surface,resources.GetMaterialBufferGPUAddress(),state),"record cold-start probe update");cold["probe_record_cpu_ms"]=Milliseconds(start);
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+1);raw=probes.GetDisplayHistory();Require(raw!=nullptr,"cold-start production RAW unavailable");
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+2);
        start=Clock::now();Require(filter.Record(device,resources.GetMainDescriptorHeap(),slot,shared,legacySDF,raw.Get(),state,world.GetVoxelSize(),settings.vxgi.probeFilterNeighborWeight,surface,resources.GetMaterialBuffer()),"record cold-start filter");cold["filter_record_and_delayed_poll_cpu_ms"]=Milliseconds(start);
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst+3);
        // Filter exposes the previous submission's timer only during Record on
        // this same slot. Attribute it back, never to the frame being recorded.
        const int64_t previousSample=coldFilterSamples[size_t(slot)];
        if(previousSample>=0&&(!LazyProbes||plan.HasWork())){auto& previous=report["cold_start_frames"][size_t(previousSample)];previous["filter_production_gpu_ms"]=filter.GetGPUMilliseconds();previous["filter_timestamp_pending"]=false;previous["filter_timestamp_retrieved_at_sample"]=sample;previous["filter_timestamp_retrieved_at_slot"]=slot;}
        const int64_t previousWarm=warmFilterSamples[size_t(slot)];
        if(previousWarm>=0&&plan.HasWork()){auto& previous=report["probe_warmup_samples"][size_t(previousWarm)];previous["filter_production_gpu_ms"]=filter.GetGPUMilliseconds();previous["filter_timestamp_pending"]=false;previous["filter_timestamp_retrieved_at_sample"]=sample;}
        warmFilterSamples[size_t(slot)]=-1;
        coldFilterSamples[size_t(slot)]=sampledFrame&&(!LazyProbes||plan.HasWork())?int64_t(report["cold_start_frames"].size()):-1;cold["filter_timestamp_pending"]=!LazyProbes||plan.HasWork();cold["filter_production_gpu_ms"]=LazyProbes&&!plan.HasWork()?Json(0):Json(nullptr);
        return cold;
    }
    bool WarmLazy(uint32_t batch){
        const auto frameStart=Clock::now();auto start=Clock::now();const int slot=Reset();const double resetMs=Milliseconds(start);
        auto record=RecordColdProbes(slot,batch,"lazy_settle",false);record["allocator_reset_and_slot_wait_cpu_ms"]=resetMs;const bool worked=state.GetPendingPlan().HasWork();
        if(worked)warmFilterSamples[size_t(slot)]=int64_t(report["probe_warmup_samples"].size());
        device.GetCommandList()->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst,4,timestampReadback.Get(),uint64_t(ColdQueryFirst)*sizeof(uint64_t));
        start=Clock::now();const auto fence=Submit(slot);Require(probes.OnSubmitted(slot,fence,state)&&filter.OnSubmitted(slot,fence),"commit finite lazy settling batch");cache.OnSubmitted(state.GetPendingWork());record["submit_and_commit_cpu_ms"]=Milliseconds(start);
        start=Clock::now();Wait(slot);record["fence_wait_wall_ms"]=Milliseconds(start);start=Clock::now();Require(probes.Poll(device,slot,state),"poll finite lazy settling batch");record["completed_probe_poll_cpu_ms"]=Milliseconds(start);
        start=Clock::now();cache.AcceptDependencies(probes.TakeCompletedDependencies(),state);record["completed_dependency_accept_cpu_ms"]=Milliseconds(start);
        void* data=nullptr;Check(timestampReadback->Map(0,nullptr,&data),"read lazy settling timestamps");const auto* ticks=static_cast<const uint64_t*>(data);Require(ticks[ColdQueryFirst+1]>=ticks[ColdQueryFirst]&&ticks[ColdQueryFirst+3]>=ticks[ColdQueryFirst+2],"negative lazy settling timestamp");
        record["probe_record_outer_gpu_ms"]=double(ticks[ColdQueryFirst+1]-ticks[ColdQueryFirst])*1000/double(timestampFrequency);record["filter_record_outer_gpu_ms"]=double(ticks[ColdQueryFirst+3]-ticks[ColdQueryFirst+2])*1000/double(timestampFrequency);timestampReadback->Unmap(0,nullptr);
        const auto& stats=state.GetStats();record["snapshot_completed_frame"]=stats.completedFrameIndex;record["snapshot_valid"]=stats.valid;record["snapshot_disabled"]=stats.disabled;record["snapshot_unclassified"]=stats.unclassified;record["snapshot_average_updates"]=stats.averageUpdates;record["probe_production_gpu_ms"]=probes.GetGPUMilliseconds();record["frame_wall_ms"]=Milliseconds(frameStart);
        report["probe_warmup_samples"].push_back(std::move(record));return worked;
    }
    Readback Copy(ID3D12Resource* source,D3D12_RESOURCE_STATES before){Readback result;const auto desc=source->GetDesc();device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&result.footprint,nullptr,nullptr,&result.bytes);result.resource=Buffer(result.bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Transition(source,before,D3D12_RESOURCE_STATE_COPY_SOURCE);const CD3DX12_TEXTURE_COPY_LOCATION src(source,0),dst(result.resource.Get(),result.footprint);device.GetCommandList()->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,before);return result;}
    std::vector<XMFLOAT4> Half(const Readback& copy){const UINT width=copy.footprint.Footprint.Width,height=copy.footprint.Footprint.Height;std::vector<XMFLOAT4> result(width*height);void* mapped=nullptr;Check(copy.resource->Map(0,nullptr,&mapped),"map actual HDR readback");for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto* half=reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(mapped)+copy.footprint.Offset+y*copy.footprint.Footprint.RowPitch+x*8);result[y*width+x]={PackedVector::XMConvertHalfToFloat(half[0]),PackedVector::XMConvertHalfToFloat(half[1]),PackedVector::XMConvertHalfToFloat(half[2]),PackedVector::XMConvertHalfToFloat(half[3])};}copy.resource->Unmap(0,nullptr);return result;}
    static std::vector<uint8_t> Preview(const std::vector<XMFLOAT4>& values,float exposure=1){std::vector<uint8_t> result(values.size()*3);for(size_t i=0;i<values.size();++i){const float rgb[3]={values[i].x,values[i].y,values[i].z};for(UINT c=0;c<3;++c){const float linear=std::max(0.f,rgb[c])*exposure;const float tone=linear/(1+linear);const float srgb=tone<=.0031308f?12.92f*tone:1.055f*std::pow(tone,1/2.4f)-.055f;result[i*3+c]=uint8_t(std::clamp(srgb*255.f+.5f,0.f,255.f));}}return result;}
    ScalarTemporalFilterPass::Output AddTemporalAO(RDGBuilder& graph,int slot,const XMFLOAT4X4& invVP,const XMFLOAT4X4& vp,
        const GBufferPass::Output& gbuffer,const HBAOPass::Output& ao,const MotionVectorPass::Output& motion){
        const int index=resources.GetHBAOCurrentHistoryIdx();
        const auto current=graph.RegisterExternalTexture(resources.GetHBAOHistoryRT(index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOCurrentHistory");
        const auto previous=graph.RegisterExternalTexture(resources.GetHBAOHistoryRT(1-index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOPreviousHistory");
        const auto currentDepth=graph.RegisterExternalTexture(resources.GetHBAODepthHistoryRT(index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOCurrentDepthHistory");
        const auto previousDepth=graph.RegisterExternalTexture(resources.GetHBAODepthHistoryRT(1-index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOPreviousDepthHistory");
        const auto currentNormal=graph.RegisterExternalTexture(resources.GetHBAONormalHistoryRT(index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOCurrentNormalHistory");
        const auto previousNormal=graph.RegisterExternalTexture(resources.GetHBAONormalHistoryRT(1-index),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"HBAOPreviousNormalHistory");
        const ScalarTemporalFilterPass::Input input{ao.blurredTexture,previous,current,gbuffer.depth,motion.motionTexture,gbuffer.normal,previousDepth,previousNormal};
        const auto temporal=ScalarTemporalFilterPass::AddToGraph(graph,&device,&resources,&pipelines,invVP,vp,{0,0},slot,Width,Height,hbaoHistoryValid,ScalarTemporalFilterPass::GetAmbientOcclusionSettings(1.f/160),input);
        Require(temporal.pass.IsValid()&&temporal.historyTexture.IsValid(),"build production temporal HBAO");graph.AddPassDependencies(temporal.pass,{ao.blurPass,motion.pass});
        graph.MarkTextureAsOutput(currentDepth);graph.MarkTextureAsOutput(currentNormal);RDGPassParameters copy;copy.ReadCopySrc(gbuffer.depth);copy.ReadCopySrc(gbuffer.normal);copy.WriteCopyDst(currentDepth);copy.WriteCopyDst(currentNormal);
        auto* sourceDepth=graph.GetTextureResource(gbuffer.depth);auto* sourceNormal=graph.GetTextureResource(gbuffer.normal);auto* targetDepth=graph.GetTextureResource(currentDepth);auto* targetNormal=graph.GetTextureResource(currentNormal);
        Require(sourceDepth&&sourceNormal&&targetDepth&&targetNormal,"production AO geometry history textures");
        const auto pass=graph.AddPass("HBAOGeometryHistoryCopy",ERDGPassFlags::Copy,copy,[=](ID3D12GraphicsCommandList* cmd){const CD3DX12_TEXTURE_COPY_LOCATION srcDepth(sourceDepth,0),dstDepth(targetDepth,0),srcNormal(sourceNormal,0),dstNormal(targetNormal,0);cmd->CopyTextureRegion(&dstDepth,0,0,0,&srcDepth,nullptr);cmd->CopyTextureRegion(&dstNormal,0,0,0,&srcNormal,nullptr);});Require(pass.IsValid(),"build production AO geometry history copy");graph.AddPassDependencies(pass,{temporal.pass});return temporal;
    }
    std::vector<XMFLOAT4> Frame(int cameraIndex,int mode,uint32_t sample,bool capture,const std::string& sequence="static",uint32_t repeat=0){
        const auto frameStart=Clock::now();auto start=Clock::now();const int slot=Reset();const double resetMs=Milliseconds(start);Json cold;
        SSGIHybridState::Key startupSourceKey;bool startupSourceReset=false;
        if(ColdStart){cold=RecordColdProbes(slot,sample,sequence=="static"?"cold_start":sequence);cold["camera"]=cameraIndex;cold["mode"]=mode;cold["allocator_reset_and_slot_wait_cpu_ms"]=resetMs;}else Require(shared.Prepare(device,slot,world,meshScene,&topology),"prepare render slot geometry");
        const auto rendererRecordStart=Clock::now();
        const XMFLOAT3 positions[4]={{8,4,-10},{8.6f,4,-10},{8,4,-10},{8,4,-10}};const XMFLOAT3 targets[4]={{0,1,0},{0,1,0},{-8,0,2},{-30,0,-14}};
        const auto eyePosition=CustomCamera?CustomEye:positions[cameraIndex];const auto targetPosition=CustomCamera?CustomTarget:targets[cameraIndex];const auto eye=XMLoadFloat3(&eyePosition);const auto target=XMLoadFloat3(&targetPosition);const auto forward=XMVector3Normalize(target-eye);Camera camera;camera.Position=eyePosition;XMStoreFloat3(&camera.Front,forward);camera.Up={0,1,0};camera.Zoom=45;
        const auto viewCPU=camera.GetViewMatrix(),projectionCPU=XMMatrixPerspectiveFovLH(XMConvertToRadians(45),float(Width)/Height,.1f,1000),vpCPU=viewCPU*projectionCPU;
        const auto view=Stored(viewCPU),projection=Stored(projectionCPU),invProjection=Stored(XMMatrixInverse(nullptr,projectionCPU)),invVP=Stored(XMMatrixInverse(nullptr,vpCPU));
        const auto shadowFrame=ShadowPass::PrepareFrame({&camera,settings.lighting.lightDir,float(Width)/Height,float(resources.GetShadowMapSize())});PassConstants constants{};constants.camPos=camera.Position;constants.cameraForward=camera.Front;constants.lightDir=shadowFrame.lightDir;constants.lightColor=settings.lighting.lightColor;constants.environmentIntensity=settings.lighting.environmentIntensity;constants.tanSunAngularRadius=std::tan(XMConvertToRadians(settings.lighting.sunAngularRadiusDegrees));constants.cascadeSplits=shadowFrame.cascadeSplits;constants.cascadeOrthoWidths=shadowFrame.cascadeOrthoWidths;constants.cascadeDepthRanges=shadowFrame.cascadeDepthRanges;std::copy(shadowFrame.lightViewProj.begin(),shadowFrame.lightViewProj.end(),constants.lightViewProj);constants.iblPrefilterIdx=resources.GetIblPrefilterIdx();constants.iblBRDFIdx=resources.GetIblBRDFIdx();auto* cb=resources.GetCBVAddress(slot);std::memcpy(cb,&constants,sizeof(constants));auto* instanceData=reinterpret_cast<InstanceData*>(cb+kPassConstantsAlignedSize);
        for(size_t i=0;i<visible.size();++i){const auto* instance=visible[i];instanceData[i].wvpMat=Stored(instance->cachedWorldMat*vpCPU);instanceData[i].worldMat=Stored(instance->cachedWorldMat);instanceData[i].normalMat=Stored(instance->cachedNormalMat);instanceData[i].customMaterialID=instance->customMaterialID;for(UINT cascade=0;cascade<NUM_CASCADES;++cascade)instanceData[visible.size()+cascade*visible.size()+i]=instanceData[i];}
        RDGBuilder graph(&device,"ActualRasterAcceptance");graph.SetTransientResourceAllocator([&](const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_STATES final,const D3D12_CLEAR_VALUE* clear,RDGTransientResourceLease* lease){return resources.AllocateRDGTransientResource(&device,slot,desc,initial,final,clear,lease);});graph.SetTransientSrvUavDescriptorAllocator([&](UINT* index,D3D12_CPU_DESCRIPTOR_HANDLE* handle){return resources.AllocateTransientSrvUavDescriptor(index,handle);});
        auto hdrTarget=graph.RegisterExternalTexture(resources.GetPostProcessRT(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,"Acceptance.HDR");auto hdrRtv=graph.CreateTextureRTVView(hdrTarget);RDGPassParameters clearParameters;clearParameters.WriteRTV(hdrRtv);graph.AddPass("Acceptance.ClearBackground",ERDGPassFlags::Graphics,clearParameters,[hdrRtv](ID3D12GraphicsCommandList* cmd){const float black[4]={.2f,.3f,.4f,1};cmd->ClearRenderTargetView(hdrRtv.cpuHandle,black,0,nullptr);});
        auto shadows=ShadowPass::AddToGraph(graph,&resources,&pipelines,slot,shadowVisible,shadowOffsets,visible.size());const D3D12_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};const D3D12_RECT rect{0,0,LONG(Width),LONG(Height)};size_t transparent=visible.size();auto gbuffer=GBufferPass::AddToGraph(graph,&device,&resources,&pipelines,slot,viewport,rect,visible,transparent);Require(gbuffer.depth.IsValid()&&shadows.pass.IsValid(),"build real GBuffer/shadows");
        MotionVectorPass::Output motion;if(RasterTemporal()){motion=MotionVectorPass::AddToGraph(graph,&device,&resources,&pipelines,invVP,Stored(vpCPU),Stored(vpCPU),Width,Height,slot,{gbuffer.depth});Require(motion.pass.IsValid()&&motion.motionTexture.IsValid(),"build production MotionUV");graph.AddPassDependencies(motion.pass,{gbuffer.pass});}
        auto ao=HBAOPass::AddToGraph(graph,&resources,&pipelines,view,projection,invProjection,Width,Height,slot,sample,settings.hbao,{gbuffer.depth,gbuffer.normal});Require(ao.blurPass.IsValid(),"build actual HBAO");
        auto hbaoForLighting=ao.blurredTexture;auto hbaoProducer=ao.blurPass;if(RasterTemporal()){const auto temporal=AddTemporalAO(graph,slot,invVP,Stored(vpCPU),gbuffer,ao,motion);hbaoForLighting=temporal.historyTexture;hbaoProducer=temporal.pass;}
        DeferredLightingPass::Input light{gbuffer.albedo,gbuffer.normal,gbuffer.orm,gbuffer.emissive,gbuffer.depth,hbaoForLighting,shadows.shadowMapSrv};auto deferred=DeferredLightingPass::AddToGraph(graph,&resources,&pipelines,invVP,Width,Height,slot,light,mode>=0);Require(deferred.pass.IsValid(),"build real DeferredLightingPass");graph.AddPassDependencies(deferred.pass,{shadows.pass,hbaoProducer});
        auto outputColor=deferred.sceneColor;
        RDGPassHandle pureClear;
        if(GIOnly){RDGTextureDesc desc{};desc.width=Width;desc.height=Height;desc.format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;desc.hasClearValue=true;desc.clearValue.Format=desc.format;desc.clearValue.Color[3]=1;outputColor=graph.CreateTexture(desc,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET,"Acceptance.PureGI");Require(outputColor.IsValid(),"create pure GI HDR target");graph.MarkTextureAsOutput(outputColor);auto rtv=graph.CreateTextureRTVView(outputColor);Require(rtv.IsValid(),"create pure GI RTV");RDGPassParameters params;params.WriteRTV(rtv);pureClear=graph.AddPass("Acceptance.ClearPureGI",ERDGPassFlags::Graphics,params,[rtv](ID3D12GraphicsCommandList* cmd){const float black[4]={0,0,0,1};cmd->ClearRenderTargetView(rtv.cpuHandle,black,0,nullptr);});}
        RDGPassHandle colorProducer=deferred.pass;
        if(mode>=0){SSGIPass::WorldInput worldInput{&shared,nullptr,&surface,raw.Get(),resources.GetMaterialBuffer(),uint32_t(world.GetOccupiedCenters().size()),world.GetVoxelSize(),uint32_t(mode)};Require(SSGIPass::EffectiveWorldMode(&worldInput,slot)==uint32_t(mode),"actual SSGI mode unexpectedly downgraded");
            if(RayRoutingStats){
                worldInput.traceDiagnostics=[this](SSGITraceConstants& trace,D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
                    std::span<const D3D12_GPU_VIRTUAL_ADDRESS> addresses){
                    Require(addresses.size()==routingAddresses.size(),"routing world binding count mismatch");
                    routingTrace=trace;routingConstants=constantsAddress;
                    std::copy(addresses.begin(),addresses.end(),routingAddresses.begin());
                };
                worldInput.traceExecution=[this](ID3D12GraphicsCommandList* command,bool begin){if(!begin)RecordRouting(command);};
                worldInput.traceOutput=[this](ID3D12GraphicsCommandList*,ID3D12Resource* output,uint32_t,uint32_t){
                    routingRawCopy=Copy(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                };
            }
            if(StartupToggle){
                startupSourceKey.effectiveWorldMode=uint32_t(mode);startupSourceKey.screenRadius=settings.ssgi.radius;startupSourceKey.quality=uint32_t(settings.ssgi.quality);startupSourceKey.traceDownsampleFactor=DownsampleFactor;
                startupSourceKey.worldGeometryRevision=mode?world.GetGeometryRevision():0;startupSourceKey.sdfGeometryRevision=meshScene.GetRevision();startupSourceKey.surfaceRevision=mode?surface.GetRevision():0;startupSourceKey.probeResetEpoch=state.GetPendingPlan().epoch;
                startupSourceReset=startupSources.NeedsReset(startupSourceKey);if(startupSourceReset)history.valid=false;
                cold["sample_frame"]=sample;cold["startup_toggle_phase"]=sample<8?"before":sample<10?"off":"after";cold["ssgi_source_reset"]=startupSourceReset;cold["ssgi_history_valid_before_trace"]=history.valid;
            }
            auto gi=SSGIPass::AddToGraph(graph,&resources,&pipelines,history,view,projection,invProjection,invVP,Width,Height,slot,sample,settings.ssgi,{outputColor,gbuffer.depth,gbuffer.normal,gbuffer.orm,gbuffer.albedo,deferred.bounceSource},&worldInput);Require(gi.pass.IsValid(),"build actual SSGI");if(GIOnly)graph.AddPassDependencies(gi.pass,{pureClear});colorProducer=gi.pass;}
        if(!GIOnly){auto skybox=SkyboxPass::AddToGraph(graph,&device,&resources,&pipelines,camera,viewport,rect,Width,Height,{deferred.sceneColor,gbuffer.depth});Require(skybox.IsValid(),"build production skybox");graph.AddPassDependencies(skybox,{colorProducer});}
        std::vector<std::string> passNames;
        bool passActive=false;
        graph.SetPassExecutionObserver([&](ID3D12GraphicsCommandList* cmd,const char* name,bool begin){
            if(begin){Require(!passActive,"nested RDG timing callback");Require(2+2*(passNames.size()+1)<=(ColdStart?ColdQueryFirst:TimestampCapacity),"RDG timing query capacity");passNames.push_back(name);passActive=true;}
            else{Require(passActive&&!passNames.empty()&&passNames.back()==name,"unpaired RDG timing callback");passActive=false;}
            const UINT index=2+2*UINT(passNames.size()-1)+(begin?0:1);cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
        });
        device.GetCommandList()->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);graph.Execute(device.GetCommandList());device.GetCommandList()->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);Require(!passActive,"unterminated RDG timing callback");device.GetCommandList()->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2+2*UINT(passNames.size()),timestampReadback.Get(),0);Require(graph.GetCompileStats().compileSucceeded,"compile real raster graph");Require(!resources.PassConstantsAllocationFailed(slot),"real raster constants overflow");
        if(ColdStart){device.GetCommandList()->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,ColdQueryFirst,4,timestampReadback.Get(),uint64_t(ColdQueryFirst)*sizeof(uint64_t));cold["renderer_record_cpu_ms"]=Milliseconds(rendererRecordStart);}
        const double rendererRecordMs=Milliseconds(rendererRecordStart);
        const bool readImage=(!ColdStart&&!CompareWorldModes)||capture;Readback imageCopy;if(readImage)imageCopy=Copy(graph.GetTextureResource(outputColor),D3D12_RESOURCE_STATE_RENDER_TARGET);
        Readback depthCopy;if(capture)depthCopy=Copy(graph.GetTextureResource(gbuffer.depth),D3D12_RESOURCE_STATE_DEPTH_WRITE);Readback normalCopy;if(capture&&CustomCamera)normalCopy=Copy(graph.GetTextureResource(gbuffer.normal),D3D12_RESOURCE_STATE_COMMON);
        start=Clock::now();const auto fence=Submit(slot);if(RasterTemporal()){resources.FlipHBAOHistoryIndex();hbaoHistoryValid=true;}if(ColdStart){Require(probes.OnSubmitted(slot,fence,state)&&filter.OnSubmitted(slot,fence),"commit cold-start production probe update");if(LazyProbes)cache.OnSubmitted(state.GetPendingWork());cold["submit_and_commit_cpu_ms"]=Milliseconds(start);}
        start=Clock::now();Wait(slot);if(RayRoutingStats)CollectRouting(cameraIndex,mode,sample);if(ColdStart){cold["fence_wait_wall_ms"]=Milliseconds(start);start=Clock::now();Require(probes.Poll(device,slot,state),"poll current cold-start submission");cold["completed_probe_poll_cpu_ms"]=Milliseconds(start);start=Clock::now();if(LazyProbes)cache.AcceptDependencies(probes.TakeCompletedDependencies(),state);cold["completed_dependency_accept_cpu_ms"]=Milliseconds(start);cold["probe_production_gpu_ms"]=probes.GetGPUMilliseconds();cold["probe_timestamp_plan_frame"]=cold["plan_frame"];const auto& stats=state.GetStats();cold["completed_snapshot_frame"]=stats.completedFrameIndex;cold["has_completed_snapshot"]=stats.hasCompletedReadback;cold["snapshot_valid"]=stats.valid;cold["snapshot_disabled"]=stats.disabled;cold["snapshot_unclassified"]=stats.unclassified;cold["snapshot_probes_with_updates"]=stats.probesWithUpdates;cold["snapshot_average_updates"]=stats.averageUpdates;}
        void* timingData=nullptr;Check(timestampReadback->Map(0,nullptr,&timingData),"read actual renderer timestamp");const auto* ticks=static_cast<const uint64_t*>(timingData);const double elapsed=double(ticks[1]-ticks[0])*1000/double(timestampFrequency);Require(std::isfinite(elapsed)&&elapsed>=0,"invalid raster timestamp");Json passes=Json::array();double passSum=0,ssgiSum=0;for(size_t i=0;i<passNames.size();++i){Require(ticks[3+2*i]>=ticks[2+2*i],"negative RDG GPU timestamp interval");const double ms=double(ticks[3+2*i]-ticks[2+2*i])*1000/double(timestampFrequency);passSum+=ms;if(passNames[i].starts_with("SSGI."))ssgiSum+=ms;passes.push_back({{"name",passNames[i]},{"gpu_ms",ms}});}
        if(ColdStart){Require(ticks[ColdQueryFirst+1]>=ticks[ColdQueryFirst]&&ticks[ColdQueryFirst+3]>=ticks[ColdQueryFirst+2],"negative cold-start GPU timestamp interval");cold["probe_record_outer_gpu_ms"]=double(ticks[ColdQueryFirst+1]-ticks[ColdQueryFirst])*1000/double(timestampFrequency);cold["filter_record_outer_gpu_ms"]=double(ticks[ColdQueryFirst+3]-ticks[ColdQueryFirst+2])*1000/double(timestampFrequency);cold["renderer_gpu_ms"]=elapsed;cold["ssgi_gpu_ms"]=ssgiSum;}
        timestampReadback->Unmap(0,nullptr);report["frame_gpu_samples"].push_back({{"camera",cameraIndex},{"mode",mode},{"sample",sample},{"repeat",repeat},{"phase",CompareWorldModes?(sequence=="world_ab_validation"?"world_ab_validation":"world_ab"):ColdStart?(sequence=="static"?"cold_start":sequence):"frozen_raw"},{"sequence",sequence},{"renderer_gpu_ms",elapsed},{"sum_pass_gpu_ms",passSum},{"ssgi_gpu_ms",ssgiSum},{"passes",passes}});
        if(StartupToggle){auto& row=report["frame_gpu_samples"].back();row["sample_frame"]=sample;row["startup_toggle_phase"]=cold["startup_toggle_phase"];row["ssgi_source_reset"]=startupSourceReset;row["probe_plan_frame"]=cold["plan_frame"];row["probe_update_count"]=cold["update_count"];}
        if(CompareWorldModes){auto& row=report["frame_gpu_samples"].back();row["ab_block"]=repeat;row["ab_round"]=repeat/4;row["ab_order"]=repeat%4;row["ab_variant"]=mode==0?"A":"B";row["ab_validation"]=sequence=="world_ab_validation";row["ab_retained"]=sequence!="world_ab_validation"&&sample>=ABWarmup;row["ab_warmup"]=sample<ABWarmup;row["renderer_record_cpu_ms"]=rendererRecordMs;row["image_readback_and_scan"]=readImage;}
        std::vector<XMFLOAT4> image;if(readImage){image=Half(imageCopy);uint64_t nonfinite=0;for(const auto& p:image)nonfinite+=!(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.w));Require(nonfinite==0,"actual HDR image has nonfinite pixels");}
        if(mode>=0){resources.FlipSSGIHistoryIndex();history.valid=true;history.previousViewProj=Stored(vpCPU);history.previousInvViewProj=invVP;history.previousPosition=camera.Position;history.previousForward=camera.Front;history.previousFov=camera.Zoom;if(StartupToggle)startupSources.Commit(startupSourceKey);}
        if(capture){const std::string name=(sequence=="static"?"":sequence+"_")+"camera"+std::to_string(cameraIndex)+(mode<0?"_off":"_mode"+std::to_string(mode));const std::string stem="tmp/vxgi-stage75-performance/"+Stem()+"-"+name;PNG(stem+".png",Preview(image,GIOnly?4.f:1.f));std::ofstream hdr(stem+".float4",std::ios::binary);hdr.write(reinterpret_cast<const char*>(image.data()),std::streamsize(image.size()*sizeof(XMFLOAT4)));
            std::vector<float> depth(Width*Height);void* depthMapped=nullptr;Check(depthCopy.resource->Map(0,nullptr,&depthMapped),"read foreground D32");uint64_t foreground=0;for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){const float d=*reinterpret_cast<const float*>(static_cast<const uint8_t*>(depthMapped)+depthCopy.footprint.Offset+y*depthCopy.footprint.Footprint.RowPitch+x*sizeof(float));Require(std::isfinite(d)&&d>=0&&d<=1,"invalid foreground D32");depth[y*Width+x]=d;foreground+=d<1;}depthCopy.resource->Unmap(0,nullptr);std::ofstream depthFile(stem+"-depth.float32",std::ios::binary);depthFile.write(reinterpret_cast<const char*>(depth.data()),std::streamsize(depth.size()*sizeof(float)));
            report["captures"].push_back({{"camera",cameraIndex},{"mode",mode},{"sample",sample},{"repeat",repeat},{"image",stem+".png"},{"linear_hdr",stem+".float4"},{"raster_depth",stem+"-depth.float32"},{"foreground_pixels",foreground},{"finite",true}});
            if(CustomCamera){const auto encodedNormals=Half(normalCopy);const auto normalPath=stem+"-normals.float4";std::ofstream normalFile(normalPath,std::ios::binary);normalFile.write(reinterpret_cast<const char*>(encodedNormals.data()),std::streamsize(encodedNormals.size()*sizeof(XMFLOAT4)));XMFLOAT4X4 inverseCPU{};XMStoreFloat4x4(&inverseCPU,XMMatrixInverse(nullptr,vpCPU));const float* values=&inverseCPU._11;report["captures"].back()["encoded_world_normals"]=normalPath;report["captures"].back()["inverse_view_projection_row_major"]=std::vector<float>(values,values+16);report["captures"].back()["normal_bias"]=0.02f;}
        }
        if(ColdStart){cold["frame_cpu_wall_ms"]=Milliseconds(frameStart);cold["image_readback_and_scan"]=readImage;report["cold_start_frames"].push_back(std::move(cold));if(sample%128==0)std::cout<<sequence<<" frame "<<sample+1<<'/'<<(sequence=="lazy_steady"?SteadySamples:Samples)<<'\n';}
        return image;
    }
    bool Warnings(){
        uint64_t count=0,errors=0,unexpected=0,knownPerformance=0;std::map<std::pair<int,int>,Json> grouped;
        for(uint64_t i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T bytes=0;Check(info->GetMessage(i,nullptr,&bytes),"measure raster D3D message");std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());Check(info->GetMessage(i,message,&bytes),"read raster D3D message");
            if(message->Severity>D3D12_MESSAGE_SEVERITY_WARNING)continue;
            ++count;const bool error=message->Severity<D3D12_MESSAGE_SEVERITY_WARNING;
            const bool known=RasterTemporal()&&!error&&message->ID==D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE;
            errors+=error?1u:0u;knownPerformance+=known?1u:0u;unexpected+=!error&&!known?1u:0u;
            auto& group=grouped[{int(message->ID),int(message->Severity)}];
            if(group.is_null())group={{"id",int(message->ID)},{"severity",int(message->Severity)},{"count",0},{"known_production_performance_warning",known},{"first_description",message->pDescription}};
            group["count"]=group["count"].get<uint64_t>()+1;
        }
        report["d3d12_message_groups"]=Json::array();for(const auto& [key,group]:grouped){(void)key;report["d3d12_message_groups"].push_back(group);std::cerr<<"Raster D3D12 ID "<<group["id"]<<" severity "<<group["severity"]<<" count "<<group["count"]<<": "<<group["first_description"].get<std::string>()<<'\n';}
        report["d3d12_warnings_errors"]=count;report["d3d12_errors"]=errors;report["d3d12_unexpected_warnings"]=unexpected;report["d3d12_known_performance_warnings"]=knownPerformance;
        report["d3d12_warning_policy"]=RasterTemporal()?"Production lazy-raster profiling retains and reports WARNING ID 820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE as an observed performance warning. No InfoQueue filter, resource clear color, production setting or shader behavior is changed. Any other warning, error or corruption fails; the report is saved before failure.":"All warning, error and corruption messages fail; the report is saved before failure.";
        const auto discarded=info->GetNumMessagesDiscardedByMessageCountLimit();report["d3d12_messages_discarded_by_count_limit"]=discarded;
        return errors==0&&unexpected==0&&(!CompareWorldModes||discarded==0);
    }
    SSGIHybridState startupSources;
    HiddenWindow window;RenderDevice device;ResourceManager resources;PipelineManager pipelines;SettingsManager settings;
    VXGIWorld& world=resources.GetVXGI().world;VXGISDF& legacySDF=resources.GetVXGI().legacySDF;
    VXGIMeshSDFCache& meshCache=resources.GetVXGI().meshSDFCache;VXGIMeshSDFScene& meshScene=resources.GetVXGI().meshSDFScene;
    VXGIProbeTopology& topology=resources.GetVXGI().topology;VXGISurface& surface=resources.GetVXGI().surface;
    VXGIGPUScene& shared=resources.GetVXGI().gpuScene;VXGIProbeState& state=resources.GetVXGI().probes;VXGIProbeCache& cache=resources.GetVXGI().probeCache;
    VXGIProbePass& probes=resources.GetVXGI().probePass;VXGIProbeFilterPass& filter=resources.GetVXGI().filterPass;
    VXGIProbeState::Parameters probeParameters;SSGIHistoryState history;ComPtr<ID3D12Resource> raw;ComPtr<ID3D12InfoQueue> info;std::vector<ModelInstance*> visible;std::array<std::vector<ModelInstance*>,NUM_CASCADES> shadowVisible;std::array<size_t,NUM_CASCADES> shadowOffsets{};ComPtr<ID3D12QueryHeap> timestamps;ComPtr<ID3D12Resource> timestampReadback;uint64_t timestampFrequency=0;uint64_t submissions=0;uint64_t scenePrepareCalls=0,worldGeometryRebuilds=0,filterTopologyRevision=0;bool hbaoHistoryValid=false;std::array<int64_t,3> coldFilterSamples={-1,-1,-1},warmFilterSamples={-1,-1,-1};Json report=Json::object();
};
}
int main(int argc,char** argv){try{bool cameraRequested=false,modeRequested=false;for(int i=1;i<argc;++i){const std::string arg=argv[i];Require(i+1<argc,"missing CLI value");const std::string value=argv[++i];if(arg=="--width")Width=uint32_t(std::stoul(value));else if(arg=="--height")Height=uint32_t(std::stoul(value));else if(arg=="--label")Label=value;else if(arg=="--samples")Samples=uint32_t(std::stoul(value));else if(arg=="--downsample-factor")DownsampleFactor=uint32_t(std::stoul(value));else if(arg=="--repeat")Repeat=uint32_t(std::stoul(value));else if(arg=="--warmup-rounds")WarmupRounds=uint32_t(std::stoul(value));else if(arg=="--warmup-batches")WarmupBatches=uint32_t(std::stoul(value));else if(arg=="--verify-instance-updates")VerifyMovement=std::stoul(value)!=0;else if(arg=="--cold-start")ColdStart=std::stoul(value)!=0;else if(arg=="--lazy-probes")LazyProbes=std::stoul(value)!=0;else if(arg=="--lazy-settle")LazySettle=std::stoul(value)!=0;else if(arg=="--startup-toggle")StartupToggle=std::stoul(value)!=0;else if(arg=="--initialize-probe-cache")InitializeProbeCache=std::stoul(value)!=0;else if(arg=="--compare-world-modes")CompareWorldModes=std::stoul(value)!=0;else if(arg=="--ab-rounds")ABRounds=uint32_t(std::stoul(value));else if(arg=="--ab-warmup")ABWarmup=uint32_t(std::stoul(value));else if(arg=="--steady-samples")SteadySamples=uint32_t(std::stoul(value));else if(arg=="--settle-max-batches")SettleMaxBatches=uint32_t(std::stoul(value));else if(arg=="--camera-eye")CameraEyeText=value;else if(arg=="--camera-target")CameraTargetText=value;else if(arg=="--cavity-rays")CavityRaysPath=value;else if(arg=="--probe-padding-layers")ProbePaddingLayers=std::stoi(value);else if(arg=="--ray-routing-stats")RayRoutingStats=std::stoul(value)!=0;else if(arg=="--gi-only")GIOnly=std::stoul(value)!=0;else if(arg=="--gpu-validation")GpuValidation=std::stoul(value)!=0;else if(arg=="--reuse-raw")ReuseRaw=value;else if(arg=="--all-cameras"){if(std::stoul(value)!=0)Cameras={0,1,2,3};cameraRequested=true;}else if(arg=="--camera"){Cameras={std::stoi(value)};cameraRequested=true;}else if(arg=="--mode"){Modes={std::stoi(value)};modeRequested=true;}else throw std::runtime_error("unknown option "+arg);}Require(CameraEyeText.empty()==CameraTargetText.empty(),"custom camera requires both eye and target");if(!CameraEyeText.empty()){CustomCamera=true;CustomEye=ParseVector(CameraEyeText);CustomTarget=ParseVector(CameraTargetText);Require(XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&CustomEye)-XMLoadFloat3(&CustomTarget)))>1e-8f,"custom camera eye equals target");if(!cameraRequested)Cameras={0};Require(Cameras.size()==1,"custom camera requires one capture camera");}if(!CavityRaysPath.empty())Require(!ReuseRaw.empty()&&!ColdStart&&!CompareWorldModes&&!StartupToggle,"cavity replay requires frozen imported RAW without cold/startup/world-AB");Require(ProbePaddingLayers>=-1&&ProbePaddingLayers<=2,"invalid probe padding layers");Require(Width>0&&Width<=4096&&Height>0&&Height<=4096&&Samples>0&&Samples<=2048,"invalid dimensions/samples");Require((DownsampleFactor==2||DownsampleFactor==4||DownsampleFactor==8)&&Repeat>0&&Repeat<=12,"invalid trace factor or repeat count");Require(WarmupRounds>0&&WarmupRounds<=32&&WarmupBatches<=65535,"invalid probe warmup rounds/batch limit");Require(!LazyProbes||ColdStart,"lazy probes require cold-start production preparation");Require(!LazySettle||LazyProbes,"lazy settle requires lazy probes");Require(SteadySamples>0&&SteadySamples<=2048&&SettleMaxBatches>0&&SettleMaxBatches<=65535,"invalid lazy steady/settle limits");Require(!VerifyMovement||(!ColdStart&&ReuseRaw.empty()),"instance-upload verification requires actual warmup without cold-start/imported RAW");Require(Label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")==std::string::npos,"invalid report label");if(ColdStart){Require(ReuseRaw.empty()&&Repeat==1,"cold start cannot import RAW or repeat a warmed sequence");if(!cameraRequested)Cameras={0};if(!modeRequested)Modes={2};Require(Cameras.size()==1&&Modes.size()==1,"cold start requires one camera and mode");}if(CompareWorldModes){Require(!ColdStart&&!LazyProbes&&!LazySettle&&!GIOnly&&!ExactFallback&&!ReuseRaw.empty()&&Repeat==1&&!modeRequested,"world A/B requires imported RAW, production threshold fallback and full raster; cannot combine cold/lazy/GI-only/repeat/mode options");Require(ABRounds>0&&ABRounds<=8&&ABWarmup>0&&ABWarmup<=2048,"invalid world A/B block limits");if(!cameraRequested)Cameras={0};Require(Cameras.size()==1,"world A/B requires one fixed camera");Modes={0,2};}if(InitializeProbeCache)Require(StartupToggle,"ResourceManager initialization acceptance requires --startup-toggle 1 (and cold-start lazy probes)");if(StartupToggle){Require(ColdStart&&LazyProbes&&!LazySettle&&!CompareWorldModes&&!GIOnly&&!ExactFallback&&Samples>=16&&Modes.size()==1&&Modes.front()==2,"startup toggle requires cold-start lazy probes, mode 2 and at least 16 samples; no settle/world A-B/GI-only/exact options");}if(GIOnly&&!modeRequested&&!ColdStart)Modes={0,1,2};for(int camera:Cameras)Require(camera>=0&&camera<4,"invalid camera");for(int mode:Modes)Require(mode>=(GIOnly?0:-1)&&mode<=2,"invalid mode for capture output");if(RayRoutingStats)Require(!ExactFallback&&!ColdStart&&!CompareWorldModes&&!ReuseRaw.empty()&&Modes.size()==1&&Modes.front()>=0,"routing requires a frozen RAW and one production mode 0/1/2");std::cout.setf(std::ios::unitbuf);Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"initialize acceptance COM");ComPtr<ID3D12Debug> debug;Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"enable actual raster debug layer");debug->EnableDebugLayer();if(GpuValidation){ComPtr<ID3D12Debug1> gpuDebug;Check(debug.As(&gpuDebug),"enable raster GPU validation interface");gpuDebug->SetEnableGPUBasedValidation(TRUE);}Capture capture;capture.Run();std::cout<<"Actual raster acceptance completed without visible windows or Present.\n";return 0;}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}}







