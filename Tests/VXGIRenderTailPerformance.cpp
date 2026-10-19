// Production DLSS and tone-map passes on a frozen actual raster capture.
// The HWND stays hidden; this diagnostic never calls ShowWindow or Present.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#include <windows.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#pragma warning(push)
#pragma warning(disable:4267 4244 4099 4324 4100)
#include "../Resources/ResourceManager.h"
#include "../Passes/DLSSPass.h"
#include "../Passes/PostProcessPass.h"
#pragma warning(pop)
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
extern "C" {
    __declspec(dllexport) extern const UINT D3D12SDKVersion=619;
    __declspec(dllexport) const char* D3D12SDKPath=".\\D3D12\\";
}
namespace {
constexpr UINT Width=1707,Height=1067,OutputWidth=2560,OutputHeight=1600;
UINT Warmup=24,Samples=72;
std::string InputStem="tmp/vxgi-stage75-performance/threshold-realtime-live-probes-1707x1067-camera0_mode2";
std::string ReportPath="tmp/VXGIRenderTail-performance-report.json";
using Clock=std::chrono::steady_clock;
double Elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void Require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
void Check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(std::string(operation)+": HRESULT "+std::to_string(uint32_t(hr)));}
template<typename T> std::vector<T> Load(const std::string& path,size_t count){Require(std::filesystem::file_size(path)==count*sizeof(T),"input byte size: "+path);std::vector<T> result(count);std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(result.data()),std::streamsize(result.size()*sizeof(T)));Require(bool(file),"read "+path);return result;}
struct HiddenWindow {
    HWND handle=nullptr;
    HiddenWindow(){WNDCLASSW desc{};desc.lpfnWndProc=DefWindowProcW;desc.hInstance=GetModuleHandleW(nullptr);desc.lpszClassName=L"VXGIRenderTailBackground";Require(RegisterClassW(&desc)!=0,"register hidden HWND");handle=CreateWindowExW(0,desc.lpszClassName,L"Background renderer tail",WS_POPUP,0,0,OutputWidth,OutputHeight,nullptr,nullptr,desc.hInstance,nullptr);Require(handle&&!IsWindowVisible(handle),"create hidden HWND");}
    ~HiddenWindow(){if(handle)DestroyWindow(handle);UnregisterClassW(L"VXGIRenderTailBackground",GetModuleHandleW(nullptr));}
};
class Fixture {
public:
    Fixture(){
        Require(device.Initialize(window.handle,OutputWidth,OutputHeight,3),"initialize hidden RenderDevice");
        Check(device.GetDevice()->QueryInterface(IID_PPV_ARGS(&info)),"query D3D12 InfoQueue");
        Require(resources.LoadAssets(&device,{},3),"initialize production resource manager without unused model assets");
        Require(resources.InitDLSSResources(&device,OutputWidth,OutputHeight),"initialize production DLSS output");
        Require(pipelines.Initialize(&device),"initialize production pipelines");
        resources.SealPersistentSrvUavDescriptors();
        Require(dlss.Initialize(device.GetDevice())&&dlss.QueryDLSSCapability(),"initialize available DLSS");
        DLSSOptimalSettings optimal{};Require(dlss.QueryOptimalSettings(OutputWidth,OutputHeight,DLSSQualityMode::Quality,&optimal),"query DLSS Quality optimal settings");
        Require(optimal.renderWidth==Width&&optimal.renderHeight==Height,"DLSS recommended input differs from actual capture");
        Require(dlss.ConfigureFeature(optimal),"configure production DLSS");
        const int slot=Reset();Require(dlss.CreateFeature(device.GetCommandList()),"create production DLSS feature");
        const auto hdr=Load<DirectX::XMFLOAT4>(InputStem+".float4",size_t(Width)*Height);
        std::vector<uint16_t> half(hdr.size()*4);for(size_t i=0;i<hdr.size();++i){const float* values=&hdr[i].x;for(size_t c=0;c<4;++c){Require(std::isfinite(values[c]),"nonfinite actual HDR");half[4*i+c]=DirectX::PackedVector::XMConvertFloatToHalf(values[c]);}}
        const auto depthValues=Load<float>(InputStem+"-depth.float32",size_t(Width)*Height);for(float value:depthValues)Require(std::isfinite(value)&&value>=0&&value<=1,"invalid actual raster depth");
        std::vector<uint16_t> motion(size_t(Width)*Height*2,0);
        color=Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,half.data(),Width*8);
        depth=Texture(DXGI_FORMAT_R32_TYPELESS,depthValues.data(),Width*4);
        vectors=Texture(DXGI_FORMAT_R16G16_FLOAT,motion.data(),Width*4);
        Submit(slot);Wait(slot);uploads.clear();
        D3D12_QUERY_HEAP_DESC desc{};desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;desc.Count=6;Check(device.GetDevice()->CreateQueryHeap(&desc,IID_PPV_ARGS(&timestamps)),"create tail timestamps");timing=Buffer(48,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);Check(device.GetCommandQueue()->GetTimestampFrequency(&frequency),"query tail timestamp frequency");
        report["scope"]="Production DLSSPass + PostProcessPass on frozen actual renderer HDR/depth, static zero motion vectors and zero jitter. Not an end-to-end renderer or Present/FPS measurement.";
        report["input_hdr"]=InputStem+".float4";report["input_depth"]=InputStem+"-depth.float32";
        report["input_size"]={Width,Height};report["output_size"]={OutputWidth,OutputHeight};report["quality"]="Quality";report["warmup_frames"]=Warmup;report["retained_frames"]=Samples;
        report["pipeline"]="Production DLSSManager, DLSSPass::AddToGraph, PostProcessPass::AddToTextureGraph; production Debug shader compilation policy.";
        report["native_configuration"]="Od_Zi_MDd";report["native_debug"]=true;report["debug_layer"]=true;report["gpu_based_validation"]=false;
    }
    void Run(){for(UINT frame=0;frame<Warmup+Samples;++frame){Frame(frame);if((frame+1)%24==0)std::cout<<"Render tail frame "<<frame+1<<'/'<<Warmup+Samples<<'\n';}Warnings();Require(!IsWindowVisible(window.handle),"tail HWND became visible");report["present_calls"]=0;report["window_visible"]=false;report["completed"]=true;std::ofstream file(ReportPath);file<<std::setw(2)<<report<<'\n';Require(bool(file),"write tail report");std::cout<<"Report: "<<ReportPath<<'\n';}
private:
    ComPtr<ID3D12Resource> Buffer(uint64_t bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES initial){auto heap=CD3DX12_HEAP_PROPERTIES(type);auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes);ComPtr<ID3D12Resource> result;Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&result)),"create tail buffer");return result;}
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format,const void* source,UINT pitch){auto desc=CD3DX12_RESOURCE_DESC::Tex2D(format,Width,Height,1,1);auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> result;Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"create captured input texture");D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"map captured input upload");for(UINT y=0;y<Height;++y)std::memcpy(static_cast<uint8_t*>(mapped)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,static_cast<const uint8_t*>(source)+size_t(y)*pitch,pitch);upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=result.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;device.GetCommandList()->CopyTextureRegion(&target,0,0,0,&from,nullptr);auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);device.GetCommandList()->ResourceBarrier(1,&barrier);uploads.push_back(upload);return result;}
    int Reset(){const int slot=int(submissions%3);device.WaitForPreviousFrame(slot);Check(device.GetCommandAllocator(slot)->Reset(),"reset tail allocator");Check(device.GetCommandList()->Reset(device.GetCommandAllocator(slot),nullptr),"reset tail command list");resources.BeginPassConstantsFrame(slot);resources.ResetTransientSrvUavDescriptors(slot);resources.BeginRDGFrame(&device,slot);return slot;}
    void Submit(int slot){Check(device.GetCommandList()->Close(),"close tail commands");ID3D12CommandList* lists[]={device.GetCommandList()};device.GetCommandQueue()->ExecuteCommandLists(1,lists);Check(device.GetCommandQueue()->Signal(device.GetFence(slot),++device.GetFenceValue(slot)),"signal tail commands");++submissions;}
    void Wait(int slot){device.WaitForPreviousFrame(slot);Check(device.GetDevice()->GetDeviceRemovedReason(),"tail device status");}
    void Frame(UINT frame){
        const auto frameStart=Clock::now();const int slot=Reset();const auto buildStart=Clock::now();
        RDGBuilder graph(&device,"ProductionRenderTailPerformance");
        graph.SetTransientResourceAllocator([&](const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_STATES final,const D3D12_CLEAR_VALUE* clear,RDGTransientResourceLease* lease){return resources.AllocateRDGTransientResource(&device,slot,desc,initial,final,clear,lease);});
        graph.SetTransientSrvUavDescriptorAllocator([&](UINT* index,D3D12_CPU_DESCRIPTOR_HANDLE* handle){return resources.AllocateTransientSrvUavDescriptor(index,handle);});
        DLSSPass::Input input{graph.RegisterExternalTexture(color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,"CapturedHDR"),graph.RegisterExternalTexture(depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,"CapturedDepth"),graph.RegisterExternalTexture(vectors.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,"StaticZeroMotion")};
        auto upscaled=DLSSPass::AddToGraph(graph,&dlss,&resources,0,0,1000.f/160.f,frame==0,input,[&](ID3D12GraphicsCommandList* cmd){ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);});Require(upscaled.pass.IsValid(),"add production DLSS pass");
        const D3D12_VIEWPORT viewport{0,0,float(OutputWidth),float(OutputHeight),0,1};const D3D12_RECT rect{0,0,LONG(OutputWidth),LONG(OutputHeight)};
        auto tone=PostProcessPass::AddToTextureGraph(graph,&resources,&pipelines,slot,viewport,rect,OutputWidth,OutputHeight,upscaled.outputTexture,false);Require(tone.pass.IsValid(),"add production tone-map pass");graph.MarkTextureAsOutput(tone.texture);
        const double buildMs=Elapsed(buildStart);std::vector<std::string> names;std::vector<double> cpu;Clock::time_point passStart;
        graph.SetPassExecutionObserver([&](ID3D12GraphicsCommandList* cmd,const char* name,bool begin){if(begin){names.emplace_back(name);Require(names.size()<=2,"unexpected tail pass count");passStart=Clock::now();}else{Require(!names.empty()&&names.back()==name,"tail observer mismatch");cpu.push_back(Elapsed(passStart));}cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2+2*UINT(names.size()-1)+(begin?0:1));});
        auto* cmd=device.GetCommandList();cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);const auto recordStart=Clock::now();graph.Execute(cmd);const double recordMs=Elapsed(recordStart);cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);if(!graph.GetCompileStats().compileSucceeded||names.size()!=2||!dlss.WasLastEvaluationSuccessful()){for(const auto& entry:ErrorLog::RecentEntries())std::cerr<<entry<<'\n';std::cerr<<"compiled="<<graph.GetCompileStats().compileSucceeded<<" passes="<<names.size()<<" evaluated="<<dlss.WasLastEvaluationSuccessful()<<'\n';Warnings();throw std::runtime_error("production tail graph/evaluation failed");}cmd->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,6,timing.Get(),0);
        auto start=Clock::now();Submit(slot);const double submitMs=Elapsed(start);start=Clock::now();Wait(slot);const double waitMs=Elapsed(start);void* mapped=nullptr;Check(timing->Map(0,nullptr,&mapped),"map tail timestamps");const auto* ticks=static_cast<const uint64_t*>(mapped);auto duration=[&](UINT i){Require(ticks[i+1]>=ticks[i],"negative tail timestamp");return double(ticks[i+1]-ticks[i])*1000/double(frequency);};Json passes=Json::array();for(size_t i=0;i<names.size();++i)passes.push_back({{"name",names[i]},{"gpu_ms",duration(2+2*UINT(i))},{"cpu_record_ms",cpu[i]}});const double gpuMs=duration(0);timing->Unmap(0,nullptr);
        if(frame>=Warmup)report["frames"].push_back({{"frame",frame-Warmup},{"graph_build_cpu_ms",buildMs},{"graph_compile_and_record_cpu_ms",recordMs},{"submit_cpu_ms",submitMs},{"fence_wait_wall_ms",waitMs},{"frame_cpu_wall_ms",Elapsed(frameStart)},{"tail_gpu_ms",gpuMs},{"passes",passes}});
    }
    void Warnings(){uint64_t count=0;for(uint64_t i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;Check(info->GetMessage(i,nullptr,&size),"query tail warning size");std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Check(info->GetMessage(i,message,&size),"read tail warning");if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<"Tail D3D12: "<<message->pDescription<<'\n';++count;}}report["d3d12_warnings_errors"]=count;Require(count==0,"tail D3D12 warnings/errors");}
    HiddenWindow window;RenderDevice device;ResourceManager resources;PipelineManager pipelines;DLSSManager dlss;ComPtr<ID3D12Resource> color,depth,vectors,timing;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12QueryHeap> timestamps;std::vector<ComPtr<ID3D12Resource>> uploads;uint64_t frequency=0,submissions=0;Json report=Json::object();
};
}
int main(int argc,char** argv){try{for(int i=1;i<argc;++i){Require(i+1<argc,"missing option value");const std::string key=argv[i],value=argv[++i];if(key=="--input-stem")InputStem=value;else if(key=="--report")ReportPath=value;else if(key=="--warmup")Warmup=UINT(std::stoul(value));else if(key=="--samples")Samples=UINT(std::stoul(value));else throw std::runtime_error("unknown option: "+key);}Require(Warmup>0&&Samples>0&&Warmup<=1024&&Samples<=2048,"invalid sample counts");std::cout.setf(std::ios::unitbuf);Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"initialize COM");Fixture fixture;fixture.Run();return 0;}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<'\n';return 1;}}
