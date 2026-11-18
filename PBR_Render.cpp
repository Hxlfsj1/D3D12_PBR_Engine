#include "D3D12App.h"
#include "PBR_Shader.h"
#include "Window.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "InputManager.h"
#include "PipelineManager.h"

#include "Settings_Manager.h"

#include "RenderStructs.h"
#include "ShadowPass.h"
#include "SkyboxPass.h"
#include "PostProcessPass.h"
#include "PBRPass.h"
#include "GBufferPass.h"
#include "HBAOPass.h"
#include "DeferredLightingPass.h"
#include "SSGIPass.h"
#include "MotionVectorPass.h"
#include "TAAPass.h"
#include "TSRPass.h"
#include "SMAAPass.h"
#include "TemporalReconstructionShared.h"
#include "ScalarTemporalFilterPass.h"
#include "DLSSPass.h"
#include "EditorUI.h"
#include "RenderPassViewer.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

// The Win32 backend intentionally leaves this declaration inside a '#if 0' block in its header
// (to avoid dragging <windows.h> in); the header instructs callers to copy it into their .cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

using namespace DirectX;
// std::shared_ptr allocates an external reference counter, whereas ComPtr uses the internal counter of the COM object itself
using Microsoft::WRL::ComPtr;

static std::vector<ModelInstance*> g_visibleInstances;
static std::array<std::vector<ModelInstance*>, NUM_CASCADES> g_shadowVisibleInstancesByCascade;
static std::array<size_t, NUM_CASCADES> g_shadowInstanceOffsets;
struct InitializationStopped {};

// Global hook for Win32 message routing
D3D12App* g_App = nullptr;

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
    // WIC needs COM initialized before decoding model textures on this thread.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        ErrorLog::Write("Application: COM initialization failed.");
        return 1;
    }
    struct ComLifetime
    {
        ~ComLifetime() { CoUninitialize(); }
    } comLifetime;

    // Disable system DPI scaling to unlock raw GPU performance
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    D3D12App app(hInstance);
    g_App = &app;
    RenderPassRunOptions passOptions;
    std::string passOptionsError;
    if (!ParseRenderPassRunOptions(lpCmdLine, passOptions, passOptionsError))
    {
        ErrorLog::Write(passOptionsError);
        return 1;
    }
    app.SetPassRunOptions(passOptions);

    // App-only scale override for inspecting editor layouts without changing OS DPI.
    float uiScaleOverride = 0.0f;
    if (const char* option = std::strstr(lpCmdLine, "--ui-scale="))
    {
        uiScaleOverride = std::strtof(option + std::strlen("--ui-scale="), nullptr);
        if (!std::isfinite(uiScaleOverride) || uiScaleOverride < 0.75f || uiScaleOverride > 3.0f)
        {
            ErrorLog::Write("Application: --ui-scale requires a value from 0.75 to 3.");
            return 1;
        }
    }
    if (!app.Initialize(passOptions.hidden ? SW_HIDE : nShowCmd, uiScaleOverride))
    {
        ErrorLog::Write("Application: initialization failed; WinMain is returning failure.");
        return 1;
    }

    app.Run();

    return app.FinishPassRun();
}

// Explicit finite runs finish pending exports and leave a machine-readable
// outcome. Ordinary interactive runs retain the normal shutdown path.
RenderPassCapture::Json D3D12App::WorldGeometryReport() const
{
    const auto& config = m_settingsManager.vxgi;
    const auto& scene = m_worldGeometry.GetScene();
    const auto& cache = m_worldGeometry.GetCache().GetStats();
    const auto& global = m_worldGeometry.GetGlobalSDF();
    const auto& stats = global.GetStats();
    const auto& layout = global.GetPass().GetLayout();
    nlohmann::json clipmaps = nlohmann::json::array();
    for (uint32_t i = 0; i < layout.constants.clipmapCount; ++i)
    {
        const auto& layer = layout.constants.clipmaps[i];
        clipmaps.push_back({{"origin", {layer.origin.x, layer.origin.y, layer.origin.z}},
            {"cell_size", layer.cellSize}, {"scroll_offset", layer.scrollOffset}});
    }
    nlohmann::json result{{"enabled", config.enabled}, {"scene_revision", scene.GetRevision()},
        {"instances", scene.GetInstances().size()}, {"assets", cache.assets}, {"asset_builds", cache.builds},
        {"triangles", cache.triangles}, {"distance_nodes", cache.nodes},
        {"instance_bvh_nodes", scene.GetInstanceBVH().GetNodes().size()},
        {"mesh_cell_size", config.sdfCellSize}, {"mesh_trace_distance", config.meshTraceDistance},
        {"trace_distance", config.globalSDFTraceDistance},
        {"global_sdf", {{"enabled", config.enabled && config.globalSDFEnabled && (m_settingsManager.ssgi.lumen || m_passRunOptions.sdfView)},
            {"frames", stats.frames}, {"idle_frames", stats.idleFrames}, {"dispatches", stats.dispatches},
            {"updated_nodes", stats.updatedNodes}, {"full_layer_updates", stats.fullLayerUpdates},
            {"allocations", stats.allocations}, {"clipmaps", clipmaps}}}};
    if (config.enabled && !scene.GetInstances().empty())
    {
        const auto& grid = m_worldGeometry.GetGPUScene().GetGrid(m_geometryFrameSlot);
        result["mesh_envelope"] = {{"origin", {grid.origin.x, grid.origin.y, grid.origin.z}},
            {"bounds_maximum", {grid.boundsMaximum.x, grid.boundsMaximum.y, grid.boundsMaximum.z}}};
    }
    nlohmann::json cardAssets=nlohmann::json::array();
    const auto vec=[](const DirectX::XMFLOAT3& v){return nlohmann::json::array({v.x,v.y,v.z});};
    for(const auto& asset:m_worldGeometry.GetCardAssets())
    {
        const auto& data=asset.data;
        nlohmann::json cards=nlohmann::json::array();
        for(const auto& card:data.cards)
            cards.push_back({{"direction",card.direction},{"origin",vec(card.origin)},{"extent",vec(card.extent)},
                {"axis_x",vec(card.axisX)},{"axis_y",vec(card.axisY)},{"axis_z",vec(card.axisZ)},
                {"near_plane",card.nearPlane},{"surfels",card.surfelCount},
                {"coverage",card.coverage},{"weighted_coverage",card.weightedCoverage}});
        cardAssets.push_back({{"cell_size",data.cellSize},{"budget",data.settings.maxCards},
            {"candidate_cards",data.candidates},{"surfels",data.surfelCount},{"covered_surfels",data.coveredSurfels},
            {"surfel_weighted_coverage",data.totalWeight>0?data.coveredWeight/data.totalWeight:0},
            {"build_ms",data.buildMilliseconds},{"lookup",data.lookup},{"cards",cards}});
    }
    result["mesh_cards"]={{"builds",m_worldGeometry.GetCardBuildCount()},{"assets",cardAssets}};
    nlohmann::json tiles=nlohmann::json::array();
    for(const auto& tile:m_surfaceCacheCapture.GetTiles())
    {
        const auto& card=tile.card;const auto& instance=scene.GetInstances()[tile.instanceIndex];
        nlohmann::json matrix=nlohmann::json::array();
        for(uint32_t row=0;row<4;++row)
            matrix.push_back({instance.worldFromLocal.m[row][0],instance.worldFromLocal.m[row][1],
                instance.worldFromLocal.m[row][2],instance.worldFromLocal.m[row][3]});
        tiles.push_back({{"instance_index",tile.instanceIndex},{"instance_id",instance.instanceId},
            {"card_index",tile.cardIndex},{"direction",card.direction},
            {"rect",{tile.x,tile.y,tile.width,tile.height}},
            {"material_rect",{tile.materialX,tile.materialY,tile.width,tile.height}},
            {"material_instance_index",tile.materialInstanceIndex},
            {"origin",vec(card.origin)},{"extent",vec(card.extent)},{"axis_x",vec(card.axisX)},
            {"axis_y",vec(card.axisY)},{"axis_z",vec(card.axisZ)},{"world_from_local",matrix}});
    }
    const auto captureStats=m_surfaceCacheCapture.GetStats();
    result["card_capture"]={{"width",m_surfaceCacheCapture.GetWidth()},{"height",m_surfaceCacheCapture.GetHeight()},
        {"material_width",m_surfaceCacheCapture.GetMaterialWidth()},{"material_height",m_surfaceCacheCapture.GetMaterialHeight()},
        {"material_tiles",m_surfaceCacheCapture.GetMaterialTileCount()},
        {"keep_unused_frames",SurfaceCacheFeedback::KeepUnusedFrames},
        {"evicted_cards",m_surfaceCacheCapture.GetFeedback().GetEvictions()},
        {"reloaded_cards",m_surfaceCacheCapture.GetFeedback().GetReloads()},
        {"feedback_completed_frame",m_surfaceCacheCapture.GetFeedback().GetCompletedFrame()},
        {"budget_halvings",captureStats.budgetHalvings},{"budget_rejections",captureStats.budgetRejections},
        {"world_texel_size",m_settingsManager.surfaceCache.worldTexelSize},
        {"max_distance",m_settingsManager.surfaceCache.maxDistance},
        {"texel_distance_scale",0.02f},{"distance_metric","camera_to_card_world_obb"},
        {"min_card_resolution",m_settingsManager.surfaceCache.minCardResolution},
        {"max_card_resolution",m_settingsManager.surfaceCache.maxCardResolution},
        {"atlas_size",m_settingsManager.surfaceCache.atlasSize},
        {"capture_batches",captureStats.captureBatches},{"draws",captureStats.draws},{"tiles",tiles}};
    const auto& light=m_settingsManager.lighting;
    result["card_lighting"]={{"relight_batches",m_surfaceCacheLighting.GetRelightBatches()},
        {"migration_batches",m_surfaceCacheLighting.GetMigrationBatches()},
        {"update_block_size",SurfaceCacheLightingPass::UpdateBlockSize},
        {"update_blocks",m_surfaceCacheLighting.GetUpdateBlockCount()},
        {"remaining_blocks",m_surfaceCacheLighting.GetRemainingBlocks()},
        {"light_direction",vec(light.lightDir)},{"light_color",vec(light.lightColor)},
        {"shadow_backend","mesh_sdf_triangle_confirmation"},{"shadow_distance",config.globalSDFTraceDistance},
        {"ray_bias",config.surfaceRayMinDistance},{"source_contract","GIComputeSourceRadiance"},{"emissive_cap",2.0}};
    result["emissive_triangles"]=m_surfaceCacheGI.GetEmitterCount();
    return result;
}

int D3D12App::FinishPassRun()
{
    if (!m_passRunOptions.frameLimit && !m_passRunOptions.captureFrame) return 0;
    for (int slot = 0; slot < frameBufferCount; ++slot)
        m_deviceContext.WaitForPreviousFrame(slot);
    for(int slot=0;slot<frameBufferCount;++slot) m_finiteGPUFrameTimer.Collect(slot);
    const bool saved = m_passCapture.Poll();
    nlohmann::json messages = nlohmann::json::array();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation;
    const bool hasValidation = SUCCEEDED(m_deviceContext.GetDevice()->QueryInterface(IID_PPV_ARGS(&validation)));
    uint64_t warnings = 0, errors = 0;
    if (hasValidation)
    {
        const UINT64 count = validation->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T bytes = 0;
            validation->GetMessage(index, nullptr, &bytes);
            std::vector<uint8_t> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(validation->GetMessage(index, message, &bytes)))
            {
                if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++warnings;
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++errors;
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                    messages.push_back(message->pDescription);
            }
        }
    }
    const bool passed = saved && (!m_passRunOptions.captureFrame || !m_passCapture.GetLastSavedPath().empty()) &&
        (!m_passRunOptions.frameLimit || m_renderedFrames == m_passRunOptions.frameLimit) &&
        SUCCEEDED(m_deviceContext.GetDevice()->GetDeviceRemovedReason()) && errors == 0;
    nlohmann::json report{{"passed", passed}, {"frames", m_renderedFrames},
        {"capture", m_passCapture.GetLastSavedPath()}, {"capture_error", m_passCapture.GetLastError()},
        {"d3d12_validation_available", hasValidation}, {"d3d12_warnings", warnings}, {"d3d12_errors", errors},
        {"d3d12_messages", messages}, {"log", ErrorLog::RecentEntries()}};
    report["gpu_frame_ms"] = m_finiteGPUFrameTimer.Samples();
    report["gpu_pass_ms"] = m_finiteGPUFrameTimer.PassSamples();
    report["gpu_pass_timing_scope"] = "LEARNDX_PROFILE_PASSES=1 per-pass, =2 grouped; includes resource barriers; timestamp boundaries can perturb GPU overlap";
    report["frame_wall_ms"] = m_finiteGPUFrameTimer.WallSamples();
    report["gpu_timing_scope"] = "complete rendering command list including ImGui; excludes present, warmup first 32 and capture frame";
    report["gi_backend"] = m_settingsManager.ssgi.lumen?"Lumen":"SSGI";
    report["ssgi_trace"] = {{"method", m_settingsManager.ssgi.lumen?"hiz_then_mesh_sdf_then_global_sdf":"hiz"},
        {"global_hit_confirmation", m_settingsManager.ssgi.lumen?"mesh_sdf_triangle_confirmation":"none"},
        {"max_iterations", m_settingsManager.ssgi.maxTraceIterations},
        {"screen_distance", m_settingsManager.ssgi.screenTraceDistance},
        {"mesh_distance", m_settingsManager.vxgi.globalSDFEnabled ?
            m_settingsManager.vxgi.meshTraceDistance : m_settingsManager.vxgi.globalSDFTraceDistance},
        {"world_distance", m_settingsManager.vxgi.globalSDFTraceDistance}};
    report["world_geometry"] = WorldGeometryReport();
    report["mesh_sdf"] = {{"enabled", m_settingsManager.vxgi.enabled}, {"trace_method", "distance_field_with_triangle_confirmation"}};
    report["global_sdf"] = report["world_geometry"]["global_sdf"];
    std::ofstream output("PassViewRun.json", std::ios::binary | std::ios::trunc);
    output << report.dump(2) << '\n';
    return passed && output ? 0 : 1;
}

// Constructs the application instance and sets default parameters
D3D12App::D3D12App(HINSTANCE hInstance) : camera(XMFLOAT3(0.0f, 3.0f, -10.0f))
{
    mhAppInst = hInstance;
    hwnd = NULL;
    WindowName = L"3D12D_PBR_Render";
    Running = true;

    frameIndex = 0;
    deltaTime = 0.0f;

    m_temporalJitterFrameIndex = 0;
    m_dlssJitterFrameIndex = 0;
    m_hbaoTemporalFrameIndex = 0;
    m_temporalHistoryValid = false;
    m_hbaoHistoryValid = false;

    DirectX::XMStoreFloat4x4(&m_currUnjitteredViewProjGpu, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_currUnjitteredInvViewProjGpu, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_prevUnjitteredViewProjGpu, DirectX::XMMatrixIdentity());

    m_hasPrevUnjitteredViewProj = false;

    m_currJitterNdcX = 0.0f;
    m_currJitterNdcY = 0.0f;
    m_currJitterPixelX = 0.0f;
    m_currJitterPixelY = 0.0f;
}

D3D12App::~D3D12App()
{
    if (m_deviceContext.GetDevice() != nullptr)
    {
        // The scene fence is signalled before Present. A final queue marker also
        // covers presentation before shutdown releases the swap-chain buffers.
        if (m_loadingFence && m_deviceContext.GetCommandQueue())
            m_deviceContext.GetCommandQueue()->Signal(m_loadingFence.Get(), ++m_loadingFenceValue);
        WaitForLoadingFrame();
        // The swap-chain's current slot can precede other in-flight slots. Wait
        // every submission before releasing the SDF buffers and UI resources.
        for (int i = 0; i < frameBufferCount; ++i)
            if (m_deviceContext.GetFence(i)) m_deviceContext.WaitForPreviousFrame(i);
        m_passCapture.Poll();
        m_dlssManager.Shutdown();
        // The GPU is idle here; ImGui's device objects can be released safely
        ShutdownImGui();
    }
    // Retire the native window while its message target and input state still exist.
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    hwnd = nullptr;
    if (g_App == this) g_App = nullptr;
}

static std::wstring g_wWindowTitle;

bool D3D12App::Initialize(int nShowCmd, float uiScaleOverride)
{
    m_uiScaleOverride = uiScaleOverride;
    try
    {
        m_settingsManager.LoadAllSettingsFromJson();
    }
    catch (const std::exception& error)
    {
        ErrorLog::Write(std::string("Application: invalid startup settings.\nReason: ") + error.what());
        return false;
    }

    if (!m_passRunOptions.cameraPreset.empty())
    {
        try
        {
            std::ifstream input(m_passRunOptions.cameraPreset);
            if (!input) throw std::runtime_error("Cannot open the camera preset.");
            nlohmann::json preset;
            input >> preset;
            const auto eye = preset.at("eye").get<XMFLOAT3>();
            const auto target = preset.at("target").get<XMFLOAT3>();
            const auto up = preset.at("up").get<XMFLOAT3>();
            const float fov = preset.at("vertical_fov_degrees").get<float>();
            const XMFLOAT3 direction(target.x - eye.x, target.y - eye.y, target.z - eye.z);
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
            const auto finite = [](const XMFLOAT3& v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            };
            const float upLength = up.x * up.x + up.y * up.y + up.z * up.z;
            if (!finite(eye) || !finite(target) || !finite(up) || !std::isfinite(length) || length <= 0 ||
                !std::isfinite(upLength) || upLength <= 0 || !std::isfinite(fov) || fov <= 0 || fov >= 179)
                throw std::runtime_error("Invalid camera preset pose or field of view.");
            camera = Camera(eye, up, XMConvertToDegrees(std::atan2(direction.x, direction.z)),
                -XMConvertToDegrees(std::asin(std::clamp(direction.y / length, -1.0f, 1.0f))));
            camera.Zoom = fov;
        }
        catch (const std::exception& error)
        {
            ErrorLog::Write(std::string("Application: camera preset failed: ") + error.what());
            return false;
        }
    }

    Width = m_settingsManager.window.width;
    Height = m_settingsManager.window.height;
    FullScreen = m_settingsManager.window.fullScreen;
    m_antiAliasingMode = m_settingsManager.pipeline.antiAliasing;

    std::string titleStr = m_settingsManager.window.title;
    g_wWindowTitle = std::wstring(titleStr.begin(), titleStr.end());
    WindowTitle = g_wWindowTitle.c_str();

    m_inputManager.Init(Width, Height);

    // Ask the system for a window
    if (!InitializeWindow(nShowCmd))
    {
        ErrorLog::Write("Application: window initialization failed.");
        return false;
    }

    // "Fuel" the engine
    try
    {
        if (!InitD3D())
        {
            ErrorLog::Write("Application: Direct3D initialization failed.");
            return false;
        }
        m_resourceManager.FreeUploadHeaps();
        ShowLoadingProgress(0.97f, "Initializing editor renderers");
        if (!m_editorSelectionRenderer.Initialize(m_deviceContext, frameBufferCount))
        {
            ErrorLog::Write("Application: editor selection initialization failed.");
            return false;
        }



        ShowLoadingProgress(1.0f, "Ready");
        if (!WaitForLoadingFrame()) return false;
        return true;
    }
    catch (const InitializationStopped&) { return false; }
}

void D3D12App::Run()
{
    MSG msg;
    ZeroMemory(&msg, sizeof(MSG));

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    LARGE_INTEGER timeStart, timeCur;
    QueryPerformanceCounter(&timeStart);

    while (Running)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // High-resolution timer
            QueryPerformanceCounter(&timeCur);
            deltaTime = (float)(timeCur.QuadPart - timeStart.QuadPart) / frequency.QuadPart;
            timeStart = timeCur;

            // CPU-GPU synchronization
            WaitForPreviousFrame();
            // Logic update
            Update();
            // Graphics commands submission (generates drawcall)
            Render();
            if(m_passRunOptions.frameLimit && m_renderedFrames>32)
                m_finiteGPUFrameTimer.RecordWall(double(deltaTime)*1000.0);
        }
    }
}

// Handle user input
LRESULT D3D12App::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_RBUTTONUP || msg == WM_KILLFOCUS || msg == WM_CAPTURECHANGED)
        m_inputManager.EndMouseLook(camera);
    // Give Dear ImGui first refusal on every window message.
    // The handler silently returns 0 until the context exists, so this is safe before InitImGui.
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    // When the editor UI owns the mouse or keyboard (hovering a panel, typing into a
    // field), swallow the event here so the game input below never sees it. Without
    // this arbitration, dragging a slider would also rotate the camera.
    if (ImGui::GetCurrentContext() != nullptr)
    {
        const ImGuiIO& io = ImGui::GetIO();
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (io.WantCaptureMouse || m_editorGizmo.IsDragging())
            {
                return 0;
            }
            break;
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            if (io.WantCaptureKeyboard || (m_editorGizmo.IsDragging() && wParam == VK_ESCAPE))
            {
                return 0;
            }
            break;
        default:
            break;
        }
    }

    // Handle discrete input in MsgProc
    if (msg == WM_DESTROY)
    {
        Running = false;
        PostQuitMessage(0);
        return 0;
    }

    if (msg == WM_KEYDOWN || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL)
    {
        if (!m_inputManager.ProcessWindowMessage(hwnd, msg, wParam, lParam, camera))
        {
            Running = false;
            DestroyWindow(hwnd);
        }

        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Pre-allocation, pre-binding, pre-compilation, pre-loading, pre-computation/baking, and pre-defined states—it’s all about pre-processing
bool D3D12App::InitD3D()
{
    // Bootstrap Hardware : Initialize DXGI infrastructure, Device, Command Queue, and Swap Chain
    if (!m_deviceContext.Initialize(hwnd, Width, Height, frameBufferCount))
    {
        ErrorLog::Write("Application: RenderDevice initialization failed.");
        return false;
    }

    // The loading UI uses its own allocator/list so it can draw while asset
    // loaders are recording on the device's main command list.
    if (!InitImGui() || !InitLoadingRenderer()) return false;
    ShowLoadingProgress(0.02f, "Preparing graphics device and upscaling");

    // DLSS is an explicit mode. TSR and the other AA modes never initialize NGX.
    const bool dlssRequested = m_antiAliasingMode == AntiAliasingMode::DLSS;
    bool dlssConfigured = false;
    if (dlssRequested)
    {
        const bool ngxInitialized = m_dlssManager.Initialize(m_deviceContext.GetDevice());
        const bool dlssAvailable = ngxInitialized && m_dlssManager.QueryDLSSCapability();
        DLSSOptimalSettings selectedDLSSSettings = {};
        bool selectedDLSSModeAvailable = false;

        if (dlssAvailable)
        {
            for (DLSSQualityMode qualityMode : kDLSSQualityModesHighToLow)
            {
                DLSSOptimalSettings modeSettings = {};
                const bool modeAvailable = m_dlssManager.QueryOptimalSettings(
                    static_cast<unsigned int>(Width),
                    static_cast<unsigned int>(Height),
                    qualityMode,
                    &modeSettings);

                char message[256] = {};
                if (modeAvailable)
                {
                    sprintf_s(
                        message,
                        "DLSS: %s is available at %ux%u -> %ux%u (dynamic range %ux%u to %ux%u).\n",
                        GetDLSSQualityModeName(qualityMode),
                        modeSettings.renderWidth,
                        modeSettings.renderHeight,
                        modeSettings.outputWidth,
                        modeSettings.outputHeight,
                        modeSettings.minRenderWidth,
                        modeSettings.minRenderHeight,
                        modeSettings.maxRenderWidth,
                        modeSettings.maxRenderHeight);
                }
                else
                {
                    sprintf_s(
                        message,
                        "DLSS: %s is unavailable for the selected output resolution.\n",
                        GetDLSSQualityModeName(qualityMode));
                }
                OutputDebugStringA(message);

                if (qualityMode == m_settingsManager.pipeline.dlssQuality && modeAvailable)
                {
                    selectedDLSSSettings = modeSettings;
                    selectedDLSSModeAvailable = true;
                }
            }
        }

        if (dlssAvailable && !selectedDLSSModeAvailable)
        {
            char message[192] = {};
            sprintf_s(
                message,
                "Error: selected DLSS mode %s is unavailable; initialization aborted.\n",
                GetDLSSQualityModeName(m_settingsManager.pipeline.dlssQuality));
            OutputDebugStringA(message);
        }

        dlssConfigured =
            selectedDLSSModeAvailable &&
            m_dlssManager.ConfigureFeature(selectedDLSSSettings);

        if (!dlssConfigured)
        {
            ErrorLog::Write(
                "Application: DLSS was requested but NGX initialization, capability detection, or feature configuration failed.");
            OutputDebugStringA(
                "Error: DLSS was requested but NGX initialization, capability detection, or feature configuration failed; initialization aborted.\n");
            return false;
        }
    }

    const float tsrUpscaleFactor = m_settingsManager.window.tsrUpscaleFactor;
    if (m_antiAliasingMode == AntiAliasingMode::TSR &&
        (!std::isfinite(tsrUpscaleFactor) ||
         tsrUpscaleFactor < 1.0f ||
         tsrUpscaleFactor > 4.0f))
    {
        ErrorLog::Write("Application: TSR upscale factor is outside the supported [1.0, 4.0] range.");
        OutputDebugStringA(
            "Error: TSR upscale factor must be finite and within [1.0, 4.0]; initialization aborted.\n");
        return false;
    }

    SceneWidth = Width;
    SceneHeight = Height;
    if (dlssConfigured)
    {
        SceneWidth = static_cast<int>(m_dlssManager.GetRenderWidth());
        SceneHeight = static_cast<int>(m_dlssManager.GetRenderHeight());
    }
    else if (m_antiAliasingMode == AntiAliasingMode::TSR)
    {
        SceneWidth = (std::max)(1, static_cast<int>(std::ceil(Width / tsrUpscaleFactor)));
        SceneHeight = (std::max)(1, static_cast<int>(std::ceil(Height / tsrUpscaleFactor)));
    }

    frameIndex = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();

    ShowLoadingProgress(0.05f, "Compiling graphics shader pipelines");
    // Compile Pipeline States: Precompute Root Signatures and PSOs for both Graphics and Compute pipelines
    if (!m_pipelineManager.Initialize(&m_deviceContext))
    {
        ErrorLog::Write("Application: main pipeline initialization failed.");
        return false;
    }
    if (!m_passPreview.Initialize(m_deviceContext)) return false;
    if (m_settingsManager.pipeline.useDeferred && m_settingsManager.ssgi.enabled &&
        (!m_pipelineManager.InitializeSSGI(&m_deviceContext) ||
         !m_resourceManager.InitSSGIHistoryResources(&m_deviceContext, SceneWidth, SceneHeight,
             static_cast<UINT>(m_settingsManager.ssgi.traceDownsampleFactor), m_settingsManager.ssgi.screenProbes,
             static_cast<UINT>(m_settingsManager.ssgi.denoiseDownsampleFactor),
             m_settingsManager.ssgi.screenProbeAdaptiveEnabled, m_settingsManager.ssgi.screenProbeAdaptiveMaxFraction)))
    {
        ErrorLog::Write("Application: SSGI pipeline initialization failed.");
        return false;
    }
    DirectX::XMStoreFloat4x4(&m_ssgiHistory.previousViewProj, DirectX::XMMatrixIdentity());
    m_ssgiHistory.previousInvViewProj = m_ssgiHistory.previousViewProj;
    m_ssgiPreviewHistory = m_ssgiHistory;
    if (m_antiAliasingMode == AntiAliasingMode::TAA &&
        !m_pipelineManager.InitializeTAA(&m_deviceContext))
    {
        ErrorLog::Write("Application: TAA pipeline initialization failed.");
        return false;
    }
    if (m_antiAliasingMode == AntiAliasingMode::TSR &&
        !m_pipelineManager.InitializeTSR(&m_deviceContext))
    {
        ErrorLog::Write("Application: TSR pipeline initialization failed.");
        return false;
    }
    if (m_antiAliasingMode == AntiAliasingMode::SMAA &&
        !m_pipelineManager.InitializeSMAA(&m_deviceContext))
    {
        ErrorLog::Write("Application: SMAA pipeline initialization failed.");
        return false;
    }

    const auto stage = [this](float first, float last) -> InitializationProgress {
        return [this, first, last](float fraction, const std::string& status) {
            ShowLoadingProgress(first + (last - first) * fraction, status);
        };
    };
    // Overall ranges reserve most of the bar for the configured probe bake;
    // progress inside each builder comes from its own completed work.
    ShowLoadingProgress(0.15f, "Loading scene models and textures");
    if (!m_resourceManager.LoadAssets(&m_deviceContext, SettingsManager::LoadSceneFromJson("Settings/Scene.json"),
        frameBufferCount, stage(0.15f, 0.30f)))
    {
        ErrorLog::Write("Application: resource loading failed.");
        return false;
    }
    ShowLoadingProgress(0.30f, "Building the shared material pool");
    if (!m_resourceManager.BuildGlobalMaterialPool(&m_deviceContext)) return false;
    currentHDRPath = SettingsManager::GetSkyboxPathFromJson();
    if (!m_resourceManager.InitIBL(&m_deviceContext, currentHDRPath.c_str(), stage(0.32f, 0.45f)))
    {
        ErrorLog::Write("Application: image-based-lighting initialization failed.");
        return false;
    }



    if (m_settingsManager.vxgi.enabled)
    {
        ShowLoadingProgress(0.60f, "Building Mesh SDF geometry and instance BVH");
        if (!m_worldGeometry.Initialize(m_deviceContext, frameBufferCount) ||
            !m_worldGeometry.Update(m_resourceManager.GetSceneInstances(), m_settingsManager.vxgi, m_settingsManager.surfaceCache))
        {
            ErrorLog::Write(m_worldGeometry.GetLastError());
            return false;
        }
        if (!m_surfaceCacheCapture.Initialize(m_deviceContext, frameBufferCount)) return false;
        if (!m_surfaceCacheLighting.Initialize(m_deviceContext, frameBufferCount)) return false;
        m_surfaceCacheGI.Initialize(frameBufferCount);
    }
    if(m_settingsManager.pipeline.useDeferred && m_settingsManager.ssgi.enabled && !m_settingsManager.vxgi.enabled)
    {ErrorLog::Write("SSGI Surface Cache requires world_geometry.enabled.");return false;}
    if (m_passRunOptions.sdfView)
    {
        if (!m_settingsManager.vxgi.enabled ||
            (((m_passRunOptions.sdfView >= 13 && m_passRunOptions.sdfView <= 17) || m_passRunOptions.sdfView==28) &&
                !m_settingsManager.vxgi.globalSDFEnabled))
        {
            ErrorLog::Write("SDF diagnostics require the selected world_geometry backend to be enabled.");
            return false;
        }
        if (m_passRunOptions.sdfView < 20 && !m_sdfDebugRenderer.Initialize(m_deviceContext, frameBufferCount)) return false;
        if (m_passRunOptions.sdfView >= 26 && m_passRunOptions.sdfView <= 28 &&
            !m_surfaceCacheQueryDebug.Initialize(m_deviceContext, frameBufferCount)) return false;
    }

    ShowLoadingProgress(0.94f, "Creating shadows and render targets");
    if (!m_resourceManager.InitShadowResources(&m_deviceContext))
    {
        ErrorLog::Write("Application: shadow resource initialization failed.");
        return false;
    }

    if (!m_resourceManager.InitPostProcess(
        &m_deviceContext,
        SceneWidth,
        SceneHeight))
    {
        ErrorLog::Write("Application: post-process resource initialization failed.");
        return false;
    }

    ShowLoadingProgress(0.95f, "Preparing temporal history and sampling textures");
    const bool temporalReconstructionRequested =
        m_antiAliasingMode == AntiAliasingMode::TAA ||
        m_antiAliasingMode == AntiAliasingMode::TSR;
    if (temporalReconstructionRequested &&
        !m_resourceManager.InitTemporalHistoryResources(&m_deviceContext, Width, Height))
    {
        ErrorLog::Write("Application: temporal history resource initialization failed.");
        return false;
    }

    if (dlssConfigured &&
        !m_resourceManager.InitDLSSResources(&m_deviceContext, Width, Height))
    {
        ErrorLog::Write("Application: DLSS resource initialization failed.");
        return false;
    }

    if (m_antiAliasingMode == AntiAliasingMode::SMAA &&
        !m_resourceManager.InitializeSMAALookupTextures(&m_deviceContext))
    {
        ErrorLog::Write("Application: SMAA lookup texture initialization failed.");
        return false;
    }

    if (!m_resourceManager.InitializeBlueNoiseTexture(&m_deviceContext))
    {
        ErrorLog::Write("Application: blue-noise texture initialization failed.");
        return false;
    }
    m_passPreview.SetBlueNoiseTexture(m_resourceManager.GetBlueNoiseTexture());

    m_resourceManager.SealPersistentSrvUavDescriptors();

    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Width, (float)Height);
    scissorRect = CD3DX12_RECT(0, 0, Width, Height);
    sceneViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)SceneWidth, (float)SceneHeight);
    sceneScissorRect = CD3DX12_RECT(0, 0, SceneWidth, SceneHeight);

    return true;
}

void D3D12App::InvalidateTemporalHistories()
{
    m_temporalHistoryValid = false;
    m_hbaoHistoryValid = false;
    m_dlssHistoryValid = false;
    m_ssgiHistory.valid = false;
    m_ssgiPreviewHistory.valid = false;
    m_ssgiSources.Invalidate();
}

void D3D12App::UpdateTemporalHistoryValidity()
{
    // Preserve the SSGI camera-cut policy; other effects keep their own rejection rules.
    if (!m_ssgiHistory.valid)
    {
        return;
    }
    float distance =
        XMVectorGetX(XMVector3Length(XMLoadFloat3(&camera.Position) - XMLoadFloat3(&m_ssgiHistory.previousPosition)));
    float agreement =
        XMVectorGetX(XMVector3Dot(XMLoadFloat3(&camera.Front), XMLoadFloat3(&m_ssgiHistory.previousForward)));
    if (distance > 5.0f || agreement < 0.5f || std::abs(camera.Zoom - m_ssgiHistory.previousFov) > 0.001f ||
        deltaTime > 0.25f)
    {
        m_ssgiHistory.valid = false;
    }
}

void D3D12App::CommitTemporalHistories(bool temporalWritten, bool hbaoWritten, bool dlssWritten, bool ssgiWritten)
{
    // Advance only histories written by a command list that has actually been submitted.
    if (temporalWritten)
    {
        m_resourceManager.FlipTemporalHistoryIndex();
        m_temporalHistoryValid = true;
    }
    if (hbaoWritten)
    {
        m_resourceManager.FlipHBAOHistoryIndex();
        m_hbaoHistoryValid = true;
    }
    if (dlssWritten)
    {
        m_dlssHistoryValid = true;
    }
    if (ssgiWritten)
    {
        m_resourceManager.FlipSSGIHistoryIndex();
        m_ssgiHistory.previousViewProj = m_currJitteredViewProjGpu;
        m_ssgiHistory.previousInvViewProj = m_currJitteredInvViewProjGpu;
        m_ssgiHistory.previousPosition = camera.Position;
        m_ssgiHistory.previousForward = camera.Front;
        m_ssgiHistory.previousFov = camera.Zoom;
        m_ssgiHistory.valid = true;
    }
}

// Data is streamed directly from the Upload Heap to the GPU, utilizing a Ring Buffer mechanism (with a count of 3 to align with the Triple Buffering scheme)
void D3D12App::Update()
{
    auto& instances = m_resourceManager.GetSceneInstances();

    // Track source edits before culling clears isDirty, including registration,
    // removal and material overrides while VXGI is disabled. Direct TRS edits
    // also mark the raster matrix dirty; current motion vectors are camera-only.
    const bool sceneChanged = m_sceneEdits.Update(instances);
    if (sceneChanged || std::any_of(instances.begin(), instances.end(), [](const ModelInstance& instance) { return instance.isDirty; }))
    {
        InvalidateTemporalHistories();
    }
    // Screen and world shading share the same uploaded material pool. Refresh
    // it before computing this frame's source stamp, independently of VXGI mode.
    if (!m_resourceManager.RefreshGlobalMaterials(&m_deviceContext))
    {
        ErrorLog::Write("Application: material pool refresh failed.");
        Running = false;
        return;
    }

    // ====================================================================================================
    // Handle FPS
    // ====================================================================================================
    frameCount++;
    timeElapsed += deltaTime;

    if (timeElapsed >= 1.0f)
    {
        float fps = (float)frameCount / timeElapsed;
        float mspf = 1000.0f / fps;

        std::wstring fpsStr = std::to_wstring(fps);
        std::wstring mspfStr = std::to_wstring(mspf);

        fpsStr = fpsStr.substr(0, fpsStr.find(L'.') + 3);
        mspfStr = mspfStr.substr(0, mspfStr.find(L'.') + 3);

        std::wstring windowText = std::wstring(WindowTitle) +
            L"    |    FPS: " + fpsStr +
            L"    |    ms/frame: " + mspfStr +
            L"    |    Visible: " + std::to_wstring(m_visibleInstanceCount) + L"/" + std::to_wstring(instances.size()) +
            L"    |    Frustum: " + std::to_wstring(m_frustumInstanceCount);

        SetWindowText(hwnd, windowText.c_str());

        frameCount = 0;
        timeElapsed -= 1.0f;
    }

    // Continuous keyboard movement must update the camera before building frame matrices and culling volumes.
    const bool allowCameraNavigation = !m_editorGizmo.IsDragging() &&
        (ImGui::GetCurrentContext() == nullptr ||
            (!ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().AppFocusLost));
    m_inputManager.Update(deltaTime, camera, allowCameraNavigation);
    if (m_passRunOptions.frameLimit)
    {
        camera.Position.x += m_passRunOptions.cameraStep[0];
        camera.Position.y += m_passRunOptions.cameraStep[1];
        camera.Position.z += m_passRunOptions.cameraStep[2];
    }
    UpdateTemporalHistoryValidity();

    // ====================================================================================================
    // Calculate V * P matrix
    // ====================================================================================================
    XMMATRIX currViewCpu = camera.GetViewMatrix();
    XMMATRIX currUnjitteredProjCpu = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)Width / Height, 0.1f, 1000.0f);
    XMMATRIX currUnjitteredViewProjCpu = currViewCpu * currUnjitteredProjCpu;

    // Handle previous unjitter VP matrix
    if (m_hasPrevUnjitteredViewProj)
    {
        m_prevUnjitteredViewProjGpu = m_currUnjitteredViewProjGpu;
    }
    else
    {
        XMStoreFloat4x4(&m_prevUnjitteredViewProjGpu, XMMatrixTranspose(currUnjitteredViewProjCpu));
        m_hasPrevUnjitteredViewProj = true;
    }

    // Calculate current jitter value
    const bool dlssSamplingActive =
        m_antiAliasingMode == AntiAliasingMode::DLSS &&
        m_dlssManager.IsFeatureConfigured() &&
        (!m_dlssManager.WasFeatureCreationAttempted() ||
            m_dlssManager.CanEvaluate() ||
            m_dlssManager.WasLastEvaluationSuccessful());
    DirectX::XMFLOAT2 jitterPixels = {};
    bool jitterEnabled = false;
    if (m_antiAliasingMode == AntiAliasingMode::TAA ||
        m_antiAliasingMode == AntiAliasingMode::TSR)
    {
        jitterPixels = TemporalReconstruction::CalculateJitter(m_temporalJitterFrameIndex++);
        jitterEnabled = true;
    }
    else if (dlssSamplingActive)
    {
        jitterPixels = DLSSPass::CalculateJitter(
            m_dlssJitterFrameIndex++,
            SceneWidth,
            Width);
        jitterEnabled = true;
    }

    if (jitterEnabled)
    {
        m_currJitterNdcX = (jitterPixels.x * 2.0f) / SceneWidth;
        m_currJitterNdcY = (jitterPixels.y * 2.0f) / SceneHeight;
        m_currJitterPixelX = jitterPixels.x;
        m_currJitterPixelY = -jitterPixels.y;
    }
    else
    {
        m_currJitterNdcX = 0.0f;
        m_currJitterNdcY = 0.0f;
        m_currJitterPixelX = 0.0f;
        m_currJitterPixelY = 0.0f;
    }

    // Apply jitter value to matrix and set them to GPU
    DirectX::XMFLOAT4X4 currJitteredProjFloat;
    DirectX::XMStoreFloat4x4(&currJitteredProjFloat, currUnjitteredProjCpu);
    currJitteredProjFloat._31 += m_currJitterNdcX;
    currJitteredProjFloat._32 += m_currJitterNdcY;
    XMMATRIX currJitteredProjCpu = DirectX::XMLoadFloat4x4(&currJitteredProjFloat);

    XMMATRIX currJitteredViewProjCpu = currViewCpu * currJitteredProjCpu;

    XMVECTOR det;
    XMMATRIX currJitteredInvViewProjCpu = XMMatrixInverse(&det, currJitteredViewProjCpu);
    XMMATRIX currJitteredInvProjCpu = XMMatrixInverse(&det, currJitteredProjCpu);

    XMStoreFloat4x4(&m_currViewGpu, XMMatrixTranspose(currViewCpu));
    XMStoreFloat4x4(&m_currUnjitteredProjGpu, XMMatrixTranspose(currUnjitteredProjCpu));
    XMStoreFloat4x4(&m_currJitteredProjGpu, XMMatrixTranspose(currJitteredProjCpu));
    XMStoreFloat4x4(&m_currUnjitteredViewProjGpu, XMMatrixTranspose(currUnjitteredViewProjCpu));
    XMStoreFloat4x4(&m_currUnjitteredInvViewProjGpu, XMMatrixTranspose(XMMatrixInverse(nullptr, currUnjitteredViewProjCpu)));
    XMStoreFloat4x4(&m_currJitteredViewProjGpu, XMMatrixTranspose(currJitteredViewProjCpu));
    XMStoreFloat4x4(&m_currJitteredInvViewProjGpu, XMMatrixTranspose(currJitteredInvViewProjCpu));
    XMStoreFloat4x4(&m_currJitteredInvProjGpu, XMMatrixTranspose(currJitteredInvProjCpu));

    // ====================================================================================================
    // Environment setup
    // ====================================================================================================
    // Get the world-space view frustum of the current frame's camera
    BoundingFrustum frustum = camera.GetWorldSpaceFrustum((float)Width / Height, 0.1f, 1000.0f);
    // Bind CBVs to prepare for subsequent data updates to the GPU
    UINT8* cbvAddress = m_resourceManager.GetCBVAddress(frameIndex);

    // ====================================================================================================
    // Directional light matrix and shadow stability calculations
    // ====================================================================================================
    // Initialize passed data (camera position, light attributes, etc.)
    PassConstants passCb = {};
    passCb.camPos = camera.Position;
    if (dlssSamplingActive)
    {
        const float primaryResolutionFraction =
            static_cast<float>(SceneWidth) / static_cast<float>((std::max)(Width, 1));
        passCb.materialMipBias = std::log2(primaryResolutionFraction) - 1.0f;
    }
    else if (m_antiAliasingMode == AntiAliasingMode::TSR)
    {
        constexpr float minAutomaticViewMipBias = -2.0f;
        constexpr float automaticViewMipBiasOffset = -0.3f;
        const float primaryResolutionFraction =
            static_cast<float>(SceneWidth) / static_cast<float>(Width);
        const float resolutionMipBias =
            (std::max)(-std::log2(primaryResolutionFraction), 0.0f);
        passCb.materialMipBias = (std::max)(
            -(resolutionMipBias + automaticViewMipBiasOffset),
            minAutomaticViewMipBias);
    }
    passCb.cameraForward = camera.Front;
    passCb.lightColor = m_settingsManager.lighting.lightColor;
    passCb.environmentIntensity = m_settingsManager.lighting.environmentIntensity;

    ShadowPass::FramePreparationInput shadowInput = {};
    shadowInput.camera = &camera;
    shadowInput.lightDir = m_settingsManager.lighting.lightDir;
    shadowInput.aspectRatio = static_cast<float>(Width) / Height;
    shadowInput.shadowMapSize = static_cast<float>(m_resourceManager.GetShadowMapSize());
    shadowInput.transitionFraction = m_settingsManager.pipeline.shadowCascadeTransitionFraction;
    shadowInput.tanSunAngularRadius = std::tan(
        XMConvertToRadians(m_settingsManager.lighting.sunAngularRadiusDegrees));

    ShadowPass::FrameData shadowFrame = ShadowPass::PrepareFrame(shadowInput);
    passCb.lightDir = shadowFrame.lightDir;
    passCb.tanSunAngularRadius = shadowInput.tanSunAngularRadius;
    passCb.cascadeSplits = shadowFrame.cascadeSplits;
    passCb.cascadeOrthoWidths = shadowFrame.cascadeOrthoWidths;
    passCb.cascadeDepthRanges = shadowFrame.cascadeDepthRanges;
    passCb.cascadeBlendEnds = shadowFrame.cascadeBlendEnds;
    std::copy(
        shadowFrame.lightViewProj.begin(),
        shadowFrame.lightViewProj.end(),
        passCb.lightViewProj);

    passCb.iblPrefilterIdx = m_resourceManager.GetIblPrefilterIdx();
    passCb.iblBRDFIdx = m_resourceManager.GetIblBRDFIdx();
    memcpy(cbvAddress, &passCb, sizeof(PassConstants));
    // ====================================================================================================
    // Frustum culling and directional light shadow volume culling
    // ====================================================================================================
    g_visibleInstances.clear();
    m_visibleInstanceCount = 0;
    m_frustumInstanceCount = 0;
    for (auto& cascadeInstances : g_shadowVisibleInstancesByCascade)
    {
        cascadeInstances.clear();
    }
    g_shadowInstanceOffsets.fill(0);

    XMVECTOR camPosVec = XMLoadFloat3(&camera.Position);
    XMMATRIX lightView = XMLoadFloat4x4(&shadowFrame.lightView);
    for (size_t i = 0; i < instances.size(); ++i)
    {
        // Use the existing Intersects library function to determine if an object should be added to the render queue or the shadow queue
        if (instances[i].pModel == nullptr || instances[i].pModel->meshes.empty())
        {
            instances[i].isVisible = false;
            continue;
        }

        // LOD
        instances[i].UpdateTransform();

        BoundingBox worldBox;
        instances[i].pModel->boundingBox.Transform(worldBox, instances[i].cachedWorldMat);

        XMVECTOR boxCenter = XMLoadFloat3(&worldBox.Center);
        float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(camPosVec, boxCenter)));

        if (dist < instances[i].lod1Threshold)
        {
            instances[i].currentLodLevel = 0;
        }
        else if (dist < instances[i].lod2Threshold)
        {
            instances[i].currentLodLevel = 1;
        }
        else
        {
            instances[i].currentLodLevel = 2;
        }

        const bool inViewFrustum = frustum.Intersects(worldBox);
        if (inViewFrustum)
        {
            ++m_frustumInstanceCount;
        }

        if (inViewFrustum)
        {
            instances[i].isVisible = true;
            g_visibleInstances.push_back(&instances[i]);
        }
        else
        {
            instances[i].isVisible = false;
        }

        BoundingBox lightSpaceBox;
        worldBox.Transform(lightSpaceBox, lightView);

        for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
        {
            if (shadowFrame.cascadeShadowAreas[cascadeIdx].Intersects(lightSpaceBox))
            {
                g_shadowVisibleInstancesByCascade[cascadeIdx].push_back(&instances[i]);
            }
        }
    }

    m_visibleInstanceCount = static_cast<int>(g_visibleInstances.size());

    // ====================================================================================================
    // Render queue sorting and batching optimization
    // ====================================================================================================
    XMFLOAT3 camPos = camera.Position;
    std::sort(g_visibleInstances.begin(), g_visibleInstances.end(), [&camPos](ModelInstance* a, ModelInstance* b)
        {
            // It breaks the batching, but establishes a clear strict boundary between opaque and transparent objects
            if (a->isTransparent != b->isTransparent)
            {
                return !a->isTransparent;
            }

            if (!a->isTransparent)
            {
                if (a->pModel != b->pModel) return a->pModel < b->pModel;
                if (a->currentLodLevel != b->currentLodLevel) return a->currentLodLevel < b->currentLodLevel;

                return (int)a->isCutout < (int)b->isCutout;
            }

            // Rebuild the instancing batches for opaque objects
            if (!a->isTransparent)
            {
                return a->pModel < b->pModel;
            }

            // Sort transparent objects in a back-to-front order (batching is lost here, resulting in a performance hit)
            XMVECTOR posA = XMLoadFloat3(&a->translation);
            XMVECTOR posB = XMLoadFloat3(&b->translation);
            XMVECTOR cam = XMLoadFloat3(&camPos);

            float distA = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(posA, cam)));
            float distB = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(posB, cam)));
            return distA > distB;
        });

    auto shadowSort = [](ModelInstance* a, ModelInstance* b)
        {
            if (a->pModel != b->pModel)
            {
                return a->pModel < b->pModel;
            }
            return a->currentLodLevel < b->currentLodLevel;
        };

    for (auto& cascadeInstances : g_shadowVisibleInstancesByCascade)
    {
        std::sort(cascadeInstances.begin(), cascadeInstances.end(), shadowSort);
    }

    // ====================================================================================================
    // Instance-data submission and active camera-jitter matrix calculation
    // ====================================================================================================
    InstanceData* mappedInstanceData = reinterpret_cast<InstanceData*>(cbvAddress + kPassConstantsAlignedSize);

    for (size_t i = 0; i < g_visibleInstances.size(); ++i)
    {
        XMMATRIX world = g_visibleInstances[i]->cachedWorldMat;
        XMMATRIX normalMat = g_visibleInstances[i]->cachedNormalMat;

        XMStoreFloat4x4(&mappedInstanceData[i].wvpMat, XMMatrixTranspose(world * currJitteredViewProjCpu));
        XMStoreFloat4x4(&mappedInstanceData[i].worldMat, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mappedInstanceData[i].normalMat, XMMatrixTranspose(normalMat));

        mappedInstanceData[i].customMaterialID = g_visibleInstances[i]->customMaterialID;
    }

    size_t shadowWriteOffset = g_visibleInstances.size();
    for (UINT cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
    {
        g_shadowInstanceOffsets[cascadeIdx] = shadowWriteOffset - g_visibleInstances.size();

        const auto& cascadeInstances = g_shadowVisibleInstancesByCascade[cascadeIdx];
        for (size_t i = 0; i < cascadeInstances.size(); ++i)
        {
            XMMATRIX world = cascadeInstances[i]->cachedWorldMat;
            XMStoreFloat4x4(&mappedInstanceData[shadowWriteOffset].worldMat, XMMatrixTranspose(world));

            mappedInstanceData[shadowWriteOffset].customMaterialID = cascadeInstances[i]->customMaterialID;
            ++shadowWriteOffset;
        }
    }
}

// Log a per-frame HRESULT failure; if the D3D12 device has been removed,
// report the underlying removal reason and stop the main loop cleanly
void D3D12App::ReportFrameError(const char* operation, HRESULT hr)
{
    ErrorLog::HRESULT(operation, hr);

    ID3D12Device* device = m_deviceContext.GetDevice();
    if (device == nullptr)
    {
        return;
    }

    // GetDeviceRemovedReason returns S_OK while the device is still alive
    const HRESULT removedReason = device->GetDeviceRemovedReason();
    if (removedReason != S_OK)
    {
        ErrorLog::HRESULT(
            "Application: the D3D12 device has been removed; shutting down. GetDeviceRemovedReason:",
            removedReason);
        Running = false;
        PostQuitMessage(0);
    }
}

bool D3D12App::BeginFrame()
{
    m_resourceManager.BeginPassConstantsFrame(frameIndex);
    m_resourceManager.ResetTransientSrvUavDescriptors(frameIndex);
    m_resourceManager.BeginRDGFrame(&m_deviceContext, frameIndex);

    // Reset the command sequence from the previous frame
    HRESULT hr = m_deviceContext.GetCommandAllocator(frameIndex)->Reset();
    if (FAILED(hr))
    {
        ReportFrameError("Application: failed to reset the frame command allocator.", hr);
        return false;
    }
    hr = m_deviceContext.GetCommandList()->Reset(
        m_deviceContext.GetCommandAllocator(frameIndex),
        m_pipelineManager.GetPBR_PSO());
    if (FAILED(hr))
    {
        ReportFrameError("Application: failed to reset the frame command list.", hr);
        return false;
    }

    return true;
}

bool D3D12App::EndFrame()
{
    // Every exit path from Render() reaches this function exactly once, so the ImGui
    // frame started after BeginFrame() is guaranteed to be finalized here (paired Render)
    ImGui::Render();
    RecordImGuiDrawData();

    if(m_passRunOptions.frameLimit) m_finiteGPUFrameTimer.End(m_deviceContext.GetCommandList(),frameIndex);

    // Close the Command List to finalize recording, no further commands can be added until the next Reset
    // CPU recording is complete, but the GPU has yet to begin execution; therefore
    // A Fence must be signaled to track GPU progress, ensuring the CPU waits before reusing this memory in the NEXT frame
    HRESULT hr = m_deviceContext.GetCommandList()->Close();
    if (FAILED(hr))
    {
        ReportFrameError("Application: failed to close the frame command list.", hr);
        return false;
    }

    return true;
}

void D3D12App::Render()
{
    m_passCapture.Poll();
    if (m_passRunOptions.RequestsCapture(m_renderedFrames + 1))
        m_savePassViewRequested = true;
    bool ssgiHistoryWritten = false;
    bool ssgiPreviewHistoryWritten = false;
    bool ssgiSourceReset = false;
    SSGISourceState::Key ssgiSourceKey;
    bool temporalHistoryWritten = false;
    bool hbaoHistoryWritten = false;
    bool dlssHistoryWritten = false;
    bool dlssResetRequested = false;
    // Normal composition: add SSGI to scene lighting, then render sky/transparency.
    const bool useZPrepass =
        m_settingsManager.pipeline.useZPrepass &&
        m_antiAliasingMode != AntiAliasingMode::TSR;
    const bool enablePostProcessSharpen =
        m_antiAliasingMode == AntiAliasingMode::TAA ||
        m_antiAliasingMode == AntiAliasingMode::TSR;
    if (!BeginFrame())
    {
        // The command infrastructure is unusable this frame (e.g. the device was removed)
        return;
    }
    if(m_passRunOptions.frameLimit && !m_finiteGPUFrameTimer.Begin(m_deviceContext,frameIndex,frameBufferCount,
        m_renderedFrames,!m_passRunOptions.RequestsCapture(m_renderedFrames+1)))
    {
        ErrorLog::Write("Finite run GPU timestamps could not be initialized.");Running=false;return;
    }
    struct CaptureFrameGuard
    {
        RenderPassCapture& capture;
        WorldGeometryRuntime& geometry;
        SurfaceCacheCapturePass& atlas;
        SurfaceCacheLightingPass& lighting;
        UINT slot;
        bool executed = false;
        ~CaptureFrameGuard() { if (!executed) { capture.CancelRecorded(slot); geometry.CancelRecording(); atlas.CancelRecording(); lighting.CancelRecording(); } }
    } captureFrame{m_passCapture, m_worldGeometry, m_surfaceCacheCapture, m_surfaceCacheLighting, static_cast<UINT>(frameIndex)};

    // Start the ImGui frame before any graph work so panel state can affect this frame.
    // It is finalized inside EndFrame(), which every exit path below passes through exactly once.
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Win32 reports client-area coordinates, while our swap chain keeps its original
    // buffer size (even after a window resize). Scale UI rendering to that buffer so
    // presentation back to the client area preserves the mouse/layout coordinates.
    // Without this, the visual/input offset grows toward the right and bottom edges.
    ImGuiIO& io = ImGui::GetIO();
    const D3D12_RESOURCE_DESC uiTargetDesc = m_deviceContext.GetRenderTarget(frameIndex)->GetDesc();
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
    {
        io.DisplayFramebufferScale = ImVec2(
            static_cast<float>(uiTargetDesc.Width) / io.DisplaySize.x,
            static_cast<float>(uiTargetDesc.Height) / io.DisplaySize.y);
    }
    // Rebuild fonts and style only when the window's DPI changes, before NewFrame.
    // Resizing the client area must not change typography or control density.
    EditorUI::UpdateScale(m_uiScaleOverride > 0 ? m_uiScaleOverride :
        static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);

    ImGui::NewFrame();

    m_editorSelectionRenderer.Poll(m_deviceContext, m_resourceManager, m_editorSelection, m_editorHistory);
    const EditorGizmo::CameraFrame gizmoCamera{ m_currUnjitteredViewProjGpu, m_currViewGpu, m_currUnjitteredProjGpu };
    // Match the unjittered projection used above: live camera.Zoom, near .1, far 1000.
    const auto cameraSnapshot = EditorCameraSnapshot::Capture(camera,
        static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), 0.1f, 1000.0f);
    // Describe the configured pipeline without enabling features for a debug view.
    const RenderPassAvailability passAvailability{
        .deferred = m_settingsManager.pipeline.useDeferred,
        .zPrepass = useZPrepass,
        .hbao = m_settingsManager.hbao.enabled,
        .ssgi = m_settingsManager.ssgi.enabled,
        .antiAliasing = m_antiAliasingMode != AntiAliasingMode::None,
        .motionVectors = m_settingsManager.pipeline.useDeferred ||
            m_antiAliasingMode == AntiAliasingMode::TAA ||
            m_antiAliasingMode == AntiAliasingMode::TSR ||
            m_antiAliasingMode == AntiAliasingMode::DLSS,
        .transparent = true,
    };
    NormalizeRenderPassView(m_passView, passAvailability);
    NormalizeRenderDebugPass(m_debugPass, passAvailability);
    const bool quitRequested = EditorUI::Draw(m_resourceManager, m_editorSelection, m_editorGizmo,
        gizmoCamera, cameraSnapshot, m_editorHistory,
        m_debugPass, m_passView, passAvailability, true, &m_settingsManager.ssgi.lumen);
    const bool pureGIView = m_passView.pass == RenderPassId::GI &&
        m_passView.result == RenderPassResult::PassOutput;
    const bool screenOnlyGIView = pureGIView && m_passView.giSource == RenderPassGISource::SSGIOnly;

    if ((!screenOnlyGIView) || m_passView.giSource != m_lastRenderedPureGISource)
        m_ssgiPreviewHistory.valid = false;
    const bool saveThisFrame = m_savePassViewRequested && !m_passCapture.IsBusy(frameIndex);
    RenderPassViewer passViewer(m_passView, m_passPreview,
        m_resourceManager.GetMainDescriptorHeap(), static_cast<UINT>(Width), static_cast<UINT>(Height), saveThisFrame);
    std::vector<RenderPassCapture::RawOutput> giCaptureOutputs;
    const auto finishPassView = [&](RDGBuilder& graph) {
        if (!passViewer.NeedsResult()) return true;
        const auto target = graph.RegisterExternalTextureOutput(m_deviceContext.GetRenderTarget(frameIndex),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");
        if (!passViewer.Finish(graph, target)) return false;
        if (saveThisFrame)
        {
            const auto* info = GetRenderPassInfo(m_passView.pass);
            const auto* channel = &RenderPassChannels[static_cast<size_t>(m_passView.channel)];
            const char* aaNames[] = {"None", "TAA", "TSR", "SMAA", "DLSS"};
            const bool delta = m_passView.result == RenderPassResult::PassOutput &&
                (m_passView.pass == RenderPassId::Skybox || m_passView.pass == RenderPassId::Transparent);
            nlohmann::json metadata = {
                {"frame", m_renderedFrames + 1}, {"pass", info->id},
                {"result", m_passView.result == RenderPassResult::PassOutput ? "output" : "accumulated"},
                {"channel", m_passView.pass == RenderPassId::GBuffer ? channel->id : "none"},
                {"gi_source", m_passView.pass == RenderPassId::GI && m_passView.result == RenderPassResult::PassOutput ?
                    RenderPassGISourceId(m_passView.giSource) : "none"},
                {"gi_output_stage", pureGIView ? "after_configured_aa_and_tonemap" :
                    m_passView.pass == RenderPassId::GI ? "after_gi" : "none"},
                {"gi_preview_aa_enabled", pureGIView && passAvailability.antiAliasing},
                {"preview_mapping", static_cast<uint32_t>(passViewer.Source().mapping)},
                {"signed_after_minus_before", delta}, {"debug_gi_source", static_cast<int>(m_debugPass)},
                {"path", passAvailability.deferred ? "deferred" : "forward"},
                {"anti_aliasing", aaNames[static_cast<int>(m_antiAliasingMode)]},
                {"camera", nlohmann::json::parse(cameraSnapshot.ToJson())},
                {"render_size", {SceneWidth, SceneHeight}}, {"output_size", {Width, Height}},
                {"ssgi", {{"enabled", m_settingsManager.ssgi.enabled},
                    {"trace_method", m_settingsManager.ssgi.lumen?"hiz_then_mesh_sdf_then_global_sdf":"hiz"},
                    {"source", "surface_cache_lo0"},
                    {"mode",m_settingsManager.ssgi.lumen?"Lumen":"SSGI"},
                    {"global_hit_confirmation", m_settingsManager.ssgi.lumen?"mesh_sdf_triangle_confirmation":"none"},
                    {"source_stamp", ssgiSourceKey.sourceStamp},
                    {"source_history_reset", ssgiSourceReset},
                    {"world_emissive_triangles", m_surfaceCacheGI.GetEmitterCount()},
                    {"world_mesh_distance", m_settingsManager.vxgi.globalSDFEnabled ?
                        m_settingsManager.vxgi.meshTraceDistance : m_settingsManager.vxgi.globalSDFTraceDistance},
                    {"world_trace_distance", m_settingsManager.vxgi.globalSDFTraceDistance},
                    {"global_sdf_enabled", m_settingsManager.vxgi.globalSDFEnabled && m_settingsManager.ssgi.lumen},
                    {"max_trace_iterations", m_settingsManager.ssgi.maxTraceIterations},
                    {"screen_trace_distance", m_settingsManager.ssgi.screenTraceDistance},
                    {"camera_fade", {{"enabled", m_settingsManager.ssgi.cameraFadeEnabled},
                        {"start", m_settingsManager.ssgi.cameraFadeStart}, {"end", m_settingsManager.ssgi.cameraFadeEnd},
                        {"metric", "camera_to_receiver_distance"}, {"stage", "composite_after_denoising"}}},
                    {"intensity", m_settingsManager.ssgi.intensity},
                    {"trace_downsample_factor", m_settingsManager.ssgi.traceDownsampleFactor},
                    {"denoise_downsample_factor", m_settingsManager.ssgi.DenoiseFactor()},
                    {"checkerboard", true},
                    {"sampling", nlohmann::json::object()}}},
                {"hbao", {{"enabled", m_settingsManager.hbao.enabled}, {"radius", m_settingsManager.hbao.radius},
                    {"intensity", m_settingsManager.hbao.intensity}}},
                {"history_valid", {{"gi", m_ssgiHistory.valid}, {"color", m_temporalHistoryValid}, {"dlss", m_dlssHistoryValid}}}
            };
            if (m_settingsManager.ssgi.screenProbes)
            {
                const uint64_t factor = static_cast<uint64_t>(m_settingsManager.ssgi.traceDownsampleFactor);
                const uint64_t columns = (SceneWidth + factor - 1u) / factor;
                const uint64_t rows = (SceneHeight + factor - 1u) / factor;
                const uint64_t denoiseFactor = m_settingsManager.ssgi.DenoiseFactor();
                ScreenProbeLayout probeLayout;
                ScreenProbeLayout::Calculate(SceneWidth, SceneHeight, static_cast<uint32_t>(factor),
                    m_settingsManager.ssgi.screenProbeAdaptiveEnabled,
                    m_settingsManager.ssgi.screenProbeAdaptiveMaxFraction, probeLayout);
                const uint64_t atlasSlots = uint64_t(probeLayout.atlasWidth) * probeLayout.atlasHeight;
                metadata["ssgi"]["denoise_size"] = {
                    (SceneWidth + denoiseFactor - 1u) / denoiseFactor,
                    (SceneHeight + denoiseFactor - 1u) / denoiseFactor};
                metadata["ssgi"]["sampling_mode"] = "screen_probes";
                metadata["ssgi"]["sampling"] = {{"screen_probes", {
                    {"grid", {columns, rows}}, {"primary_receivers_per_cell", 1},
                    {"cell_width_internal_pixels", factor}, {"direction_cells", {8, 8}},
                    {"adaptive", {{"enabled", probeLayout.adaptiveCapacity > 0u},
                        {"max_fraction", m_settingsManager.ssgi.screenProbeAdaptiveMaxFraction},
                        {"candidate_attempts_per_tile", ScreenProbeLayout::AdaptiveSamplesPerTile},
                        {"capacity", probeLayout.adaptiveCapacity},
                        {"atlas", {probeLayout.atlasWidth, probeLayout.atlasHeight}},
                        {"placement", "UE 5.8.4 Mark/Spawn; lower-index candidate coverage rejection; tile lists"},
                        {"count_texture", "requested count; actual count is clamped to capacity"}}},
                    {"emissive_samples_per_probe", m_settingsManager.ssgi.emissiveSamplesPerProbe},
                    {"screen_rays_per_probe", m_settingsManager.ssgi.screenRaysPerProbe},
                    {"visibility", m_settingsManager.ssgi.lumen?"world_mesh_sdf_triangle_confirmation":"screen_hiz"},
                    {"screen_budget_policy", m_settingsManager.ssgi.lumen?"independent direction samples; screen misses trace world geometry":"independent direction samples; screen misses return zero"},
                    {"history_updates", m_settingsManager.ssgi.screenProbeHistoryUpdates},
                    {"jitter_enabled", m_settingsManager.ssgi.screenProbeJitterWidthFraction > 0.0f},
                    {"jitter_width_fraction", m_settingsManager.ssgi.screenProbeJitterWidthFraction},
                    {"configured_jitter_width_fraction", m_settingsManager.ssgi.screenProbeJitterWidthFraction},
                    {"direction_measure", "uniform-solid-angle-hemisphere plus separately weighted emitter samples; ordinary rays remove emission when explicit sampling is enabled"},
                    {"integration", "SH2 diffuse convolution (1,2/3,1/4), evaluated with each denoising receiver normal; geometry-guided final upsample"},
                    {"screen_trace_distance", m_settingsManager.ssgi.screenTraceDistance},
                    {"routing", m_settingsManager.ssgi.lumen?"Hi-Z, Mesh near, Global candidate plus Mesh confirmation far; Surface Cache Lo0":"Hi-Z only; Surface Cache Lo0; screen misses return zero"},
                    {"geometry", "stable nearest-center anchor; surface-compatible 8-phase receiver jitter within configured total width"},
                    {"filter", "same-direction 5x5 binomial, probe step 2; plane/normal/hit-distance guided"},
                    {"pixel_history_statistics_step", (factor + denoiseFactor - 1u) / denoiseFactor},
                    {"filtered_feedback", false},
                    {"update_policy", "full configured budget on fresh; checkerboard hold only after geometry/parallax acceptance"},
                    {"persistent_probe_bytes", atlasSlots * 2u * (64u * 8u + 16u + 16u + 8u + 4u + 8u) +
                        (probeLayout.adaptiveCapacity ? 2u * (4u + columns * rows * 4u * (1u + ScreenProbeLayout::AdaptiveSamplesPerTile)) : 0u)},
                    {"transient_filtered_direction_bytes", atlasSlots * 64u * 8u},
                    {"memory_metric", "logical texture payload, not committed/resident peak VRAM"}
                }}};
            }
            else metadata["ssgi"]["sampling_mode"] = "legacy";
            std::vector<RenderPassCapture::RawOutput> raw{{"result", passViewer.Source().texture,
                UINT_MAX, delta ? "signed linear HDR: after minus before" :
                m_passView.pass == RenderPassId::GI && m_passView.result == RenderPassResult::PassOutput ?
                "pure indirect-light display after configured anti-aliasing and tone mapping" :
                "native selected pass resource; DXGI format and preview mapping recorded"}};
            raw.insert(raw.end(), giCaptureOutputs.begin(), giCaptureOutputs.end());
            if (m_passRunOptions.captureCompact)
                std::erase_if(raw, [](const auto& output) {
                    return output.name != "gi-filtered" && output.name != "gi-full-reconstructed" &&
                        output.name != "screen-probe-diagnostics" && output.name != "screen-probe-adaptive-count";
                });
            if (m_passRunOptions.captureROI[2])
            {
                const auto* roi = m_passRunOptions.captureROI;
                if (uint64_t(roi[0]) + roi[2] > SceneWidth || uint64_t(roi[1]) + roi[3] > SceneHeight)
                {
                    ErrorLog::Write("Capture ROI exceeds the internal scene size.");
                    return false;
                }
                for (auto& output : raw)
                    if (output.name == "gi-filtered" || output.name == "gi-full-reconstructed" ||
                        output.name == "gi-full-raw" || output.name == "depth" || output.name == "normal")
                    {
                        // Signal i represents full pixel factor*i, including odd
                        // extents. A ratio of rounded texture sizes shifts the crop.
                        const uint64_t factor = output.name == "depth" || output.name == "normal"
                            ? 1u : m_settingsManager.ssgi.DenoiseFactor();
                        const auto left = roi[0] / factor, top = roi[1] / factor;
                        const auto right = (uint64_t(roi[0]) + roi[2] + factor - 1u) / factor;
                        const auto bottom = (uint64_t(roi[1]) + roi[3] + factor - 1u) / factor;
                        output.region = {UINT(left), UINT(top), 0u, UINT(right), UINT(bottom), 1u};
                    }
            }
            const std::string prefix = std::string("Captures/") + info->id + "-" +
                std::to_string(m_renderedFrames + 1) + "-" + std::to_string(GetTickCount64());
            if (!m_passCapture.AddToGraph(graph, m_deviceContext.GetDevice(), frameIndex,
                passViewer.PreviewTexture(), raw, prefix, metadata).IsValid())
                ErrorLog::Write("Pass capture: " + m_passCapture.GetLastError());
            m_savePassViewRequested = false;
        }
        return true;
    };
    const bool giAvailable = IsScreenGIAvailable(passAvailability);
    // Resolve after the menu so a click affects this same frame. The editor's
    // output selector never changes persistent quality settings.
    const bool showGIOnly = giAvailable && IsGIOnlyDebugPass(m_debugPass);
    SSGIConfig frameSSGI = m_settingsManager.ssgi;
    frameSSGI.giOnly = showGIOnly;
    if (m_debugPass != m_lastRenderedDebugPass || pureGIView != m_lastRenderedPureGI ||
        (pureGIView && m_passView.giSource != m_lastRenderedPureGISource))
    {
        // Color histories contain the composed viewport, unlike the GI signal's
        // own history, which already tracks screen source changes.
        m_temporalHistoryValid = false;
        m_dlssHistoryValid = false;
    }


    if (m_settingsManager.vxgi.enabled)
    {
        if (!m_worldGeometry.Update(m_resourceManager.GetSceneInstances(), m_settingsManager.vxgi, m_settingsManager.surfaceCache) ||
            !m_worldGeometry.Record(m_deviceContext, frameIndex, camera.Position,
                [&]{auto config=m_settingsManager.vxgi;if(!frameSSGI.lumen && !m_passRunOptions.sdfView)config.globalSDFEnabled=false;return config;}()) ||
            !m_surfaceCacheCapture.Record(m_deviceContext,m_resourceManager,m_worldGeometry,frameIndex,
                m_settingsManager.surfaceCache,camera.Position) ||
            !m_surfaceCacheLighting.Record(m_deviceContext,m_resourceManager,m_worldGeometry,m_surfaceCacheCapture,
                frameIndex,m_settingsManager.lighting,m_settingsManager.vxgi))
        {
            ErrorLog::Write(m_worldGeometry.GetLastError());
            ErrorLog::Write(m_surfaceCacheCapture.GetLastError());
            Running = false;
            EndFrame();
            return;
        }
        m_geometryFrameSlot = frameIndex;
    }
    const bool geometryDiagnostic = m_passRunOptions.sdfView != 0;

    if (!geometryDiagnostic && m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        if (!m_dlssManager.IsFeatureConfigured())
        {
            OutputDebugStringA(
                "Error: DLSS mode is active without a configured feature; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }

        if (!m_dlssManager.WasFeatureCreationAttempted() &&
            !m_dlssManager.CreateFeature(m_deviceContext.GetCommandList()))
        {
            OutputDebugStringA(
                "Error: DLSS feature creation failed; rendering stopped instead of degrading.\n");
            Running = false;
            EndFrame();
            return;
        }

        if (!m_dlssManager.CanEvaluate())
        {
            OutputDebugStringA(
                "Error: DLSS feature cannot be evaluated; rendering stopped instead of degrading.\n");
            Running = false;
            EndFrame();
            return;
        }
    }

    size_t transparentIdx = 0;
    bool temporalHistoryWrittenByDeferredGraph = false;
    bool hbaoTemporalHandledByDeferredGraph = false;
    bool dlssEvaluatedByDeferredGraph = false;

    if (geometryDiagnostic)
    {
        if(m_passRunOptions.sdfView>=26 && m_passRunOptions.sdfView<=28)
        {
            if(!m_surfaceCacheQueryDebug.Record(m_deviceContext,m_resourceManager,m_worldGeometry,m_surfaceCacheCapture,
                m_surfaceCacheLighting,frameIndex,m_currUnjitteredInvViewProjGpu,m_imguiRtvHandles[frameIndex],m_passRunOptions.sdfView,
                m_passRunOptions.cardQueryChannel,m_settingsManager.vxgi))
            {Running=false;EndFrame();return;}
        }
        else if(m_passRunOptions.sdfView>=20)
        {
            if(!m_surfaceCacheCapture.RecordPreview(m_deviceContext,m_resourceManager,frameIndex,
                m_imguiRtvHandles[frameIndex],m_passRunOptions.sdfView,m_surfaceCacheLighting.GetSRVCPUHandle()))
            {
                Running=false;EndFrame();return;
            }
        }
        else
        {
            const auto& global = m_worldGeometry.GetGlobalSDF().GetPass();
            UINT globalIndex = UINT_MAX;
            if (global.GetTexture())
            {
                D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
                if (!m_resourceManager.AllocateTransientSrvUavDescriptor(&globalIndex, &descriptor))
                {
                    Running = false; EndFrame(); return;
                }
                m_deviceContext.GetDevice()->CopyDescriptorsSimple(1, descriptor, global.GetSRVCPUHandle(),
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            auto config = m_settingsManager.vxgi;
            config.sdfSliceAxis = m_passRunOptions.sdfSliceAxis;
            config.sdfSlicePosition = m_passRunOptions.sdfSlicePosition;
            const auto& light = m_settingsManager.lighting.lightDir;
            const auto cards = m_passRunOptions.sdfView >= 18 ? m_worldGeometry.BuildDebugCards() :
                std::vector<SurfaceCacheCards::DebugCard>{};
            if (!m_sdfDebugRenderer.RecordGeometry(m_deviceContext, m_resourceManager.GetMainDescriptorHeap(), frameIndex,
                m_currUnjitteredInvViewProjGpu, m_imguiRtvHandles[frameIndex], m_worldGeometry.GetGPUScene(), global,
                globalIndex, config, m_passRunOptions.sdfView, {-light.x, -light.y, -light.z},
                m_resourceManager.GetMaterialBufferGPUAddress(), static_cast<uint32_t>(m_resourceManager.GetGlobalMaterials().size()),cards))
            {
                ErrorLog::Write("World geometry diagnostic recording failed.");
                Running = false; EndFrame(); return;
            }
        }
        if (saveThisFrame)
        {
            RDGBuilder graph(&m_deviceContext, "WorldGeometryCapture");
            const auto output = graph.RegisterExternalTexture(m_deviceContext.GetRenderTarget(frameIndex),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, "GeometryDiagnostic");
            const std::string prefix = "Captures/sdf-" + std::to_string(m_passRunOptions.sdfView) + "-" +
                std::to_string(m_renderedFrames + 1) + "-" + std::to_string(GetTickCount64());
            nlohmann::json metadata{{"frame", m_renderedFrames + 1}, {"pass", "world-geometry"},
                {"sdf_view", m_passRunOptions.sdfView},
                {"card_view", CardViewName(m_passRunOptions.sdfView)},
                {"camera", nlohmann::json::parse(cameraSnapshot.ToJson())},
                {"world_geometry", WorldGeometryReport()}};
            if(m_passRunOptions.sdfView>=26 && m_passRunOptions.sdfView<=28)
                metadata["card_query"]={{"mode",CardQueryModes[m_passRunOptions.sdfView-26]},
                    {"channel",CardQueryChannels[m_passRunOptions.cardQueryChannel]},
                    {"point_tolerance",0.001},{"global_tolerance","2 * (clipmap cell size + mesh cell size)"}};
            std::vector<RenderPassCapture::RawOutput> raw;
            if(m_passRunOptions.sdfView>=20)
            {
                const char* names[]={"albedo-metallic","normal-roughness","emissive-source","card-depth"};
                const char* descriptions[]={"linear base color RGB; metallic A","signed world shading normal XYZ; roughness A",
                    "uncapped linear emission RGB; GI source enabled A","orthographic depth: 0 at Card near plane, 1 at far plane; 1 is invalid"};
                for(UINT i=0;i<SurfaceCacheCapturePass::ChannelCount;++i)
                {
                    const auto texture=graph.RegisterExternalTexture(m_surfaceCacheCapture.GetTexture(i),
                        SurfaceCacheCapturePass::ReadState,SurfaceCacheCapturePass::ReadState,names[i]);
                    raw.push_back({names[i],texture,0,descriptions[i]});
                }
                const auto lighting=graph.RegisterExternalTexture(m_surfaceCacheLighting.GetTexture(),
                    SurfaceCacheCapturePass::ReadState,SurfaceCacheCapturePass::ReadState,"radiance-visibility");
                raw.push_back({"radiance-visibility",lighting,0,"linear outgoing diffuse source Lo0 RGB; directional visibility A; validity uses Card depth"});
            }
            if(m_passRunOptions.sdfView>=26 && m_passRunOptions.sdfView<=28)
            {
                const char* descriptions[]={"queried linear base color RGB; metallic A","queried signed world normal XYZ; roughness A",
                    "queried uncapped linear emission RGB; source enabled A","input world position XYZ; geometry hit flag W",
                    "instance index, local Card index, tile UV; instance and Card -1 for lookup miss",
                    "reconstructed atlas world position XYZ; lookup valid flag W",
                    "queried linear outgoing diffuse source Lo0 RGB; directional visibility A"};
                for(UINT i=0;i<SurfaceCacheQueryDebugRenderer::OutputCount;++i)
                {
                    const char* name=SurfaceCacheQueryDebugRenderer::OutputNames[i];
                    const auto texture=graph.RegisterExternalTexture(m_surfaceCacheQueryDebug.GetOutput(i),
                        SurfaceCacheCapturePass::ReadState,SurfaceCacheCapturePass::ReadState,name);
                    raw.push_back({name,texture,0,descriptions[i]});
                }
            }
            if (!m_passCapture.AddToGraph(graph, m_deviceContext.GetDevice(), frameIndex, output, raw, prefix, metadata).IsValid())
            {
                ErrorLog::Write(m_passCapture.GetLastError());
                Running = false; EndFrame(); return;
            }
            graph.Execute(m_deviceContext.GetCommandList());
            m_savePassViewRequested = false;
        }
    }
    else if (!m_settingsManager.pipeline.useDeferred)
    {
        const bool useInternalResolution =
            m_antiAliasingMode == AntiAliasingMode::TSR ||
            m_antiAliasingMode == AntiAliasingMode::DLSS;
        const D3D12_VIEWPORT& forwardViewport =
            useInternalResolution ? sceneViewport : viewport;
        const D3D12_RECT& forwardScissorRect =
            useInternalResolution ? sceneScissorRect : scissorRect;
        const int forwardRenderWidth = useInternalResolution ? SceneWidth : Width;
        const int forwardRenderHeight = useInternalResolution ? SceneHeight : Height;

        RDGBuilder forwardGraph(&m_deviceContext, "ForwardFrameGraph");
        forwardGraph.SetTransientResourceAllocator(
            [this](
                const D3D12_RESOURCE_DESC& resourceDesc,
                D3D12_RESOURCE_STATES initialState,
                D3D12_RESOURCE_STATES finalState,
                const D3D12_CLEAR_VALUE* clearValue,
                RDGTransientResourceLease* outResource)
            {
                return m_resourceManager.AllocateRDGTransientResource(
                    &m_deviceContext,
                    frameIndex,
                    resourceDesc,
                    initialState,
                    finalState,
                    clearValue,
                    outResource);
            });
        forwardGraph.SetTransientSrvUavDescriptorAllocator(
            [this](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return m_resourceManager.AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

        ShadowPass::Output shadowOutput = ShadowPass::AddToGraph(
            forwardGraph,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            g_shadowVisibleInstancesByCascade,
            g_shadowInstanceOffsets,
            g_visibleInstances.size());
        if (!shadowOutput.shadowMap.IsValid() ||
            !shadowOutput.shadowMapSrv.IsValid() ||
            !shadowOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Forward shadow RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }

        passViewer.Output(forwardGraph, RenderPassId::Shadow, shadowOutput.shadowMap, RenderPassViewer::Mapping::Shadow);

        PBRPass::ZPrepassOutput zPrepassOutput = {};
        if (useZPrepass)
        {
            zPrepassOutput = PBRPass::AddZPrepassToGraph(
                forwardGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                forwardViewport,
                forwardScissorRect,
                g_visibleInstances,
                transparentIdx);
            if (!zPrepassOutput.depth.IsValid() || !zPrepassOutput.pass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward Z-prepass RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
        }

        passViewer.Output(forwardGraph, RenderPassId::ZPrepass, zPrepassOutput.depth, RenderPassViewer::Mapping::Depth);

        PBRPass::OpaqueInput opaqueInput = {};
        opaqueInput.depth = zPrepassOutput.depth;
        opaqueInput.shadowMap = shadowOutput.shadowMapSrv;

        PBRPass::OpaqueOutput opaqueOutput = PBRPass::AddOpaqueToGraph(
            forwardGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            forwardViewport,
            forwardScissorRect,
            g_visibleInstances,
            useZPrepass && zPrepassOutput.depth.IsValid(),
            opaqueInput);
        if (!opaqueOutput.sceneColor.IsValid() ||
            !opaqueOutput.depth.IsValid() ||
            !opaqueOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Forward opaque RDG construction failed; rendering stopped instead of degrading.\n");
            Running = false;
            EndFrame();
            return;
        }
        transparentIdx = opaqueOutput.transparentStartIndex;
        passViewer.Color(forwardGraph, RenderPassId::PBROpaque, opaqueOutput.sceneColor);
        passViewer.Before(forwardGraph, RenderPassId::Skybox, opaqueOutput.sceneColor);

        SkyboxPass::Input skyboxInput = {};
        skyboxInput.sceneColor = opaqueOutput.sceneColor;
        skyboxInput.depth = opaqueOutput.depth;
        RDGPassHandle forwardSceneColorProducer = opaqueOutput.pass;
        RDGPassHandle skyboxPass = SkyboxPass::AddToGraph(
            forwardGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            camera,
            forwardViewport,
            forwardScissorRect,
            forwardRenderWidth,
            forwardRenderHeight,
            skyboxInput);
        if (!skyboxPass.IsValid())
        {
            OutputDebugStringA(
                "Error: Forward skybox RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }
        forwardGraph.AddPassDependencies(skyboxPass, { forwardSceneColorProducer });
        forwardSceneColorProducer = skyboxPass;
        passViewer.After(forwardGraph, RenderPassId::Skybox, opaqueOutput.sceneColor);
        passViewer.Before(forwardGraph, RenderPassId::Transparent, opaqueOutput.sceneColor);

        if (transparentIdx < g_visibleInstances.size())
        {
            PBRPass::TransparentInput transparentInput = {};
            transparentInput.sceneColor = opaqueOutput.sceneColor;
            transparentInput.depth = opaqueOutput.depth;
            transparentInput.shadowMap = shadowOutput.shadowMapSrv;
            RDGPassHandle transparentPass = PBRPass::AddTransparentToGraph(
                forwardGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                forwardViewport,
                forwardScissorRect,
                g_visibleInstances,
                transparentIdx,
                transparentInput);
            if (!transparentPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward transparent RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
            forwardSceneColorProducer = transparentPass;
        }

        passViewer.After(forwardGraph, RenderPassId::Transparent, opaqueOutput.sceneColor);

        RDGTextureHandle forwardFinalColor = opaqueOutput.sceneColor;
        RDGPassHandle forwardFinalColorProducer = forwardSceneColorProducer;
        bool temporalHistoryWrittenByForwardGraph = false;
        bool dlssEvaluatedByForwardGraph = false;
        if (m_antiAliasingMode == AntiAliasingMode::TAA ||
            m_antiAliasingMode == AntiAliasingMode::TSR ||
            m_antiAliasingMode == AntiAliasingMode::DLSS)
        {
            MotionVectorPass::Output motionOutput = MotionVectorPass::AddToGraph(
                forwardGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                m_currJitteredInvViewProjGpu,
                m_currUnjitteredViewProjGpu,
                m_prevUnjitteredViewProjGpu,
                forwardRenderWidth,
                forwardRenderHeight,
                frameIndex,
                { opaqueOutput.depth });

            if (!motionOutput.motionTexture.IsValid() || !motionOutput.pass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward motion-vector RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }

            passViewer.Output(forwardGraph, RenderPassId::MotionVector, motionOutput.motionTexture, RenderPassViewer::Mapping::Motion);
            if (m_antiAliasingMode == AntiAliasingMode::TAA)
            {
                TAAPass::Output taaOutput = TAAPass::AddToGraph(
                    forwardGraph,
                    &m_deviceContext,
                    &m_resourceManager,
                    &m_pipelineManager,
                    m_currJitteredInvViewProjGpu,
                    m_prevUnjitteredViewProjGpu,
                    m_currJitterNdcX,
                    m_currJitterNdcY,
                    frameIndex,
                    Width,
                    Height,
                    m_temporalHistoryValid,
                    { opaqueOutput.sceneColor, opaqueOutput.depth, motionOutput.motionTexture });

                if (!taaOutput.historyTexture.IsValid() || !taaOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Forward TAA RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                forwardGraph.AddPassDependencies(
                    taaOutput.pass,
                    { forwardSceneColorProducer, motionOutput.pass });
                forwardFinalColor = taaOutput.historyTexture;
                forwardFinalColorProducer = taaOutput.pass;
                temporalHistoryWrittenByForwardGraph = true;
            }
            else if (m_antiAliasingMode == AntiAliasingMode::TSR)
            {
                TSRPass::Output tsrOutput = TSRPass::AddToGraph(
                    forwardGraph,
                    &m_deviceContext,
                    &m_resourceManager,
                    &m_pipelineManager,
                    m_currJitteredInvViewProjGpu,
                    m_prevUnjitteredViewProjGpu,
                    m_currJitterNdcX,
                    m_currJitterNdcY,
                    frameIndex,
                    SceneWidth,
                    SceneHeight,
                    Width,
                    Height,
                    m_temporalHistoryValid,
                    { opaqueOutput.sceneColor, opaqueOutput.depth, motionOutput.motionTexture });

                if (!tsrOutput.historyTexture.IsValid() || !tsrOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Forward TSR RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                forwardGraph.AddPassDependencies(
                    tsrOutput.pass,
                    { forwardSceneColorProducer, motionOutput.pass });
                forwardFinalColor = tsrOutput.historyTexture;
                forwardFinalColorProducer = tsrOutput.pass;
                temporalHistoryWrittenByForwardGraph = true;
            }
            else
            {
                dlssResetRequested = !m_dlssHistoryValid;
                DLSSPass::Output dlssOutput = DLSSPass::AddToGraph(
                    forwardGraph,
                    &m_dlssManager,
                    &m_resourceManager,
                    m_currJitterPixelX,
                    m_currJitterPixelY,
                    (std::max)(deltaTime * 1000.0f, 0.0f),
                    dlssResetRequested,
                    { opaqueOutput.sceneColor, opaqueOutput.depth, motionOutput.motionTexture },
                    [this](ID3D12GraphicsCommandList* commandList)
                    {
                        // The post-process root signature declares
                        // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, so the heap must be bound first
                        ID3D12DescriptorHeap* heaps[] =
                        {
                            m_resourceManager.GetMainDescriptorHeap()
                        };
                        commandList->SetDescriptorHeaps(1, heaps);

                        commandList->SetGraphicsRootSignature(
                            m_pipelineManager.GetPostProcessRootSignature());
                        commandList->SetPipelineState(
                            m_pipelineManager.GetPostProcessPSO(false));
                        commandList->RSSetViewports(1, &viewport);
                        commandList->RSSetScissorRects(1, &scissorRect);
                        commandList->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    });

                if (!dlssOutput.outputTexture.IsValid() || !dlssOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Forward DLSS RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                forwardGraph.AddPassDependencies(
                    dlssOutput.pass,
                    { forwardSceneColorProducer, motionOutput.pass });
                forwardFinalColor = dlssOutput.outputTexture;
                forwardFinalColorProducer = dlssOutput.pass;
                dlssEvaluatedByForwardGraph = true;
            }
        }

        if (m_antiAliasingMode != AntiAliasingMode::None && m_antiAliasingMode != AntiAliasingMode::SMAA)
            passViewer.Accumulated(forwardGraph, RenderPassId::AntiAliasing, forwardFinalColor);

        if (m_antiAliasingMode == AntiAliasingMode::SMAA)
        {
            PostProcessPass::TextureOutput toneMapOutput =
                PostProcessPass::AddToTextureGraph(
                    forwardGraph,
                    &m_resourceManager,
                    &m_pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    Width,
                    Height,
                    forwardFinalColor,
                    false);
            if (!toneMapOutput.texture.IsValid() ||
                !toneMapOutput.pass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward SMAA tone-map RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            passViewer.Accumulated(forwardGraph, RenderPassId::PostProcess, toneMapOutput.texture, RenderPassViewer::Mapping::LDR);
            forwardGraph.AddPassDependency(
                toneMapOutput.pass,
                forwardFinalColorProducer);

            RDGTextureHandle backBuffer =
                forwardGraph.RegisterExternalTextureOutput(
                    m_deviceContext.GetRenderTarget(frameIndex),
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_PRESENT,
                    "BackBuffer");

            SMAAPass::Input smaaInput = {};
            smaaInput.color = toneMapOutput.texture;
            smaaInput.output = backBuffer;
            SMAAPass::Output smaaOutput = SMAAPass::AddToGraph(
                forwardGraph,
                &m_resourceManager,
                &m_pipelineManager,
                Width,
                Height,
                smaaInput);
            if (!smaaOutput.color.IsValid() ||
                !smaaOutput.neighborhoodPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward SMAA RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            passViewer.Accumulated(forwardGraph, RenderPassId::AntiAliasing, smaaOutput.color, RenderPassViewer::Mapping::LDR);
            forwardGraph.AddPassDependency(
                smaaOutput.edgePass,
                toneMapOutput.pass);
        }
        else
        {
            RDGPassHandle postProcessPass = PostProcessPass::AddFinalToGraph(
                forwardGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                viewport,
                scissorRect,
                forwardFinalColor,
                enablePostProcessSharpen);
            if (!postProcessPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Forward post-process RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            forwardGraph.AddPassDependencies(
                postProcessPass,
                { forwardFinalColorProducer });
        }

        if (m_antiAliasingMode != AntiAliasingMode::SMAA && passViewer.Wants(RenderPassId::PostProcess))
        {
            const auto finalColor = forwardGraph.RegisterExternalTexture(m_deviceContext.GetRenderTarget(frameIndex),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");
            passViewer.Accumulated(forwardGraph, RenderPassId::PostProcess, finalColor, RenderPassViewer::Mapping::LDR);
        }
        if (!finishPassView(forwardGraph))
        {
            Running = false;
            EndFrame();
            return;
        }
        forwardGraph.Execute(m_deviceContext.GetCommandList());
        if (m_resourceManager.PassConstantsAllocationFailed(frameIndex))
        {
            ErrorLog::Write("Application: pass constant allocation failed; rendering stopped.");
            Running = false;
            EndFrame();
            return;
        }

        if (dlssEvaluatedByForwardGraph)
        {
            if (!m_dlssManager.WasLastEvaluationSuccessful())
            {
                m_dlssHistoryValid = false;
                OutputDebugStringA(
                    "Error: Forward DLSS evaluation failed; the frame was not submitted.\n");
                Running = false;
                EndFrame();
                return;
            }

            dlssHistoryWritten = true;
        }

        if (temporalHistoryWrittenByForwardGraph)
        {
            temporalHistoryWritten = true;
        }
    }
    else
    {
        // Build the RDG for the deferred rendering pipeline
        RDGBuilder deferredGraph(&m_deviceContext, "DeferredFrameGraph");
        // Wire the transient resource allocator into the RDG
        deferredGraph.SetTransientResourceAllocator(
            [this](
                const D3D12_RESOURCE_DESC& resourceDesc,
                D3D12_RESOURCE_STATES initialState,
                D3D12_RESOURCE_STATES finalState,
                const D3D12_CLEAR_VALUE* clearValue,
                RDGTransientResourceLease* outResource)
            {
                return m_resourceManager.AllocateRDGTransientResource(
                    &m_deviceContext,
                    frameIndex,
                    resourceDesc,
                    initialState,
                    finalState,
                    clearValue,
                    outResource);
            });
        // Wire the descriptor allocator into the RDG
        deferredGraph.SetTransientSrvUavDescriptorAllocator(
            [this](UINT* descriptorIndex, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle)
            {
                return m_resourceManager.AllocateTransientSrvUavDescriptor(descriptorIndex, cpuHandle);
            });

        size_t transparentStartIndex = g_visibleInstances.size();

        ShadowPass::Output shadowOutput = ShadowPass::AddToGraph(
            deferredGraph,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            g_shadowVisibleInstancesByCascade,
            g_shadowInstanceOffsets,
            g_visibleInstances.size());
        if (!shadowOutput.shadowMap.IsValid() ||
            !shadowOutput.shadowMapSrv.IsValid() ||
            !shadowOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Deferred shadow RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }

        passViewer.Output(deferredGraph, RenderPassId::Shadow, shadowOutput.shadowMap, RenderPassViewer::Mapping::Shadow);

        PBRPass::ZPrepassOutput zPrepassOutput = {};
        if (useZPrepass)
        {
            zPrepassOutput = PBRPass::AddZPrepassToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                sceneViewport,
                sceneScissorRect,
                g_visibleInstances,
                transparentStartIndex);
            if (!zPrepassOutput.depth.IsValid() || !zPrepassOutput.pass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred Z-prepass RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
        }

        passViewer.Output(deferredGraph, RenderPassId::ZPrepass, zPrepassOutput.depth, RenderPassViewer::Mapping::Depth);
        GBufferPass::Output gbufferOutput = GBufferPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            sceneViewport,
            sceneScissorRect,
            g_visibleInstances,
            transparentStartIndex,
            useZPrepass,
            zPrepassOutput.depth);
        if (!gbufferOutput.albedo.IsValid() ||
            !gbufferOutput.normal.IsValid() ||
            !gbufferOutput.orm.IsValid() ||
            !gbufferOutput.emissive.IsValid() ||
            !gbufferOutput.depth.IsValid() ||
            !gbufferOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Deferred GBuffer RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }

        if (passViewer.Wants(RenderPassId::GBuffer))
        {
            switch (m_passView.channel)
            {
            case RenderPassChannel::Albedo: passViewer.Output(deferredGraph, RenderPassId::GBuffer, gbufferOutput.albedo, RenderPassViewer::Mapping::LinearColor); break;
            case RenderPassChannel::Normal: passViewer.Output(deferredGraph, RenderPassId::GBuffer, gbufferOutput.normal, RenderPassViewer::Mapping::Normal); break;
            case RenderPassChannel::ORM: passViewer.Output(deferredGraph, RenderPassId::GBuffer, gbufferOutput.orm, RenderPassViewer::Mapping::LDR); break;
            case RenderPassChannel::Emissive: passViewer.Output(deferredGraph, RenderPassId::GBuffer, gbufferOutput.emissive); break;
            case RenderPassChannel::Depth: passViewer.Output(deferredGraph, RenderPassId::GBuffer, gbufferOutput.depth, RenderPassViewer::Mapping::Depth); break;
            }
        }

        MotionVectorPass::Output motionOutput = MotionVectorPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            m_currJitteredInvViewProjGpu,
            m_currUnjitteredViewProjGpu,
            m_prevUnjitteredViewProjGpu,
            SceneWidth,
            SceneHeight,
            frameIndex,
            { gbufferOutput.depth });
        if (!motionOutput.motionTexture.IsValid() || !motionOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Deferred motion-vector RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }
        deferredGraph.AddPassDependencies(motionOutput.pass, { gbufferOutput.pass });
        passViewer.Output(deferredGraph, RenderPassId::MotionVector, motionOutput.motionTexture, RenderPassViewer::Mapping::Motion);

        RDGTextureHandle hbaoForLighting;
        RDGPassHandle hbaoProducer;
        if (m_settingsManager.hbao.enabled)
        {
            HBAOPass::Output hbaoOutput = HBAOPass::AddToGraph(
                deferredGraph,
                &m_resourceManager,
                &m_pipelineManager,
                m_currViewGpu,
                m_currJitteredProjGpu,
                m_currJitteredInvProjGpu,
                SceneWidth,
                SceneHeight,
                frameIndex,
                m_hbaoTemporalFrameIndex++,
                m_settingsManager.hbao,
                { gbufferOutput.depth, gbufferOutput.normal });
            if (!hbaoOutput.blurredTexture.IsValid() ||
                !hbaoOutput.rawPass.IsValid() ||
                !hbaoOutput.blurPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred HBAO RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
            deferredGraph.AddPassDependencies(hbaoOutput.rawPass, { gbufferOutput.pass });
            deferredGraph.AddPassDependencies(hbaoOutput.blurPass, { hbaoOutput.rawPass });

            hbaoForLighting = hbaoOutput.blurredTexture;
            hbaoProducer = hbaoOutput.blurPass;
            {
                const int hbaoHistoryIndex = m_resourceManager.GetHBAOCurrentHistoryIdx();
                RDGTextureHandle currentHBAOHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAOHistoryRT(hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOCurrentHistory");
                RDGTextureHandle previousHBAOHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAOHistoryRT(1 - hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOPreviousHistory");
                RDGTextureHandle currentHBAODepthHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAODepthHistoryRT(hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOCurrentDepthHistory");
                RDGTextureHandle previousHBAODepthHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAODepthHistoryRT(1 - hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOPreviousDepthHistory");
                RDGTextureHandle currentHBAONormalHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAONormalHistoryRT(hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOCurrentNormalHistory");
                RDGTextureHandle previousHBAONormalHistory = deferredGraph.RegisterExternalTexture(
                    m_resourceManager.GetHBAONormalHistoryRT(1 - hbaoHistoryIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    "HBAOPreviousNormalHistory");

                ScalarTemporalFilterPass::Input hbaoTemporalInput = {};
                hbaoTemporalInput.currentSignal = hbaoOutput.blurredTexture;
                hbaoTemporalInput.previousHistory = previousHBAOHistory;
                hbaoTemporalInput.historyOutput = currentHBAOHistory;
                hbaoTemporalInput.depth = gbufferOutput.depth;
                hbaoTemporalInput.motion = motionOutput.motionTexture;
                hbaoTemporalInput.normal = gbufferOutput.normal;
                hbaoTemporalInput.previousDepth = previousHBAODepthHistory;
                hbaoTemporalInput.previousNormal = previousHBAONormalHistory;

                ScalarTemporalFilterPass::Output hbaoTemporalOutput =
                    ScalarTemporalFilterPass::AddToGraph(
                        deferredGraph,
                        &m_deviceContext,
                        &m_resourceManager,
                        &m_pipelineManager,
                        m_currJitteredInvViewProjGpu,
                        m_prevUnjitteredViewProjGpu,
                        DirectX::XMFLOAT2(m_currJitterPixelX, m_currJitterPixelY),
                        frameIndex,
                        SceneWidth,
                        SceneHeight,
                        m_hbaoHistoryValid,
                        ScalarTemporalFilterPass::GetAmbientOcclusionSettings(deltaTime),
                        hbaoTemporalInput);

                if (!hbaoTemporalOutput.historyTexture.IsValid() ||
                    !hbaoTemporalOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Deferred temporal HBAO RDG construction failed; rendering stopped.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                {
                    deferredGraph.AddPassDependencies(
                        hbaoTemporalOutput.pass,
                        { hbaoOutput.blurPass, motionOutput.pass });

                    deferredGraph.MarkTextureAsOutput(currentHBAODepthHistory);
                    deferredGraph.MarkTextureAsOutput(currentHBAONormalHistory);

                    RDGPassParameters geometryHistoryCopyParameters;
                    geometryHistoryCopyParameters.ReadCopySrc(gbufferOutput.depth);
                    geometryHistoryCopyParameters.ReadCopySrc(gbufferOutput.normal);
                    geometryHistoryCopyParameters.WriteCopyDst(currentHBAODepthHistory);
                    geometryHistoryCopyParameters.WriteCopyDst(currentHBAONormalHistory);

                    ID3D12Resource* sceneDepthResource =
                        deferredGraph.GetTextureResource(gbufferOutput.depth);
                    ID3D12Resource* sceneNormalResource =
                        deferredGraph.GetTextureResource(gbufferOutput.normal);
                    ID3D12Resource* depthHistoryResource =
                        deferredGraph.GetTextureResource(currentHBAODepthHistory);
                    ID3D12Resource* normalHistoryResource =
                        deferredGraph.GetTextureResource(currentHBAONormalHistory);
                    if (sceneDepthResource == nullptr ||
                        sceneNormalResource == nullptr ||
                        depthHistoryResource == nullptr ||
                        normalHistoryResource == nullptr)
                    {
                        OutputDebugStringA(
                            "Error: Deferred HBAO geometry-history resources are invalid; rendering stopped.\n");
                        Running = false;
                        EndFrame();
                        return;
                    }

                    RDGPassHandle geometryHistoryCopyPass = deferredGraph.AddPass(
                        "HBAOGeometryHistoryCopy",
                        ERDGPassFlags::Copy,
                        geometryHistoryCopyParameters,
                        [=](ID3D12GraphicsCommandList* cmdList)
                        {
                            D3D12_TEXTURE_COPY_LOCATION sourceDepth = {};
                            sourceDepth.pResource = sceneDepthResource;
                            sourceDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            sourceDepth.SubresourceIndex = 0;

                            D3D12_TEXTURE_COPY_LOCATION destinationDepth = {};
                            destinationDepth.pResource = depthHistoryResource;
                            destinationDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            destinationDepth.SubresourceIndex = 0;

                            cmdList->CopyTextureRegion(
                                &destinationDepth,
                                0,
                                0,
                                0,
                                &sourceDepth,
                                nullptr);

                            D3D12_TEXTURE_COPY_LOCATION sourceNormal = {};
                            sourceNormal.pResource = sceneNormalResource;
                            sourceNormal.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            sourceNormal.SubresourceIndex = 0;

                            D3D12_TEXTURE_COPY_LOCATION destinationNormal = {};
                            destinationNormal.pResource = normalHistoryResource;
                            destinationNormal.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            destinationNormal.SubresourceIndex = 0;

                            cmdList->CopyTextureRegion(
                                &destinationNormal,
                                0,
                                0,
                                0,
                                &sourceNormal,
                            nullptr);
                        });
                    if (!geometryHistoryCopyPass.IsValid())
                    {
                        OutputDebugStringA(
                            "Error: Deferred HBAO geometry-history copy RDG construction failed; rendering stopped.\n");
                        Running = false;
                        EndFrame();
                        return;
                    }
                    deferredGraph.AddPassDependencies(
                        geometryHistoryCopyPass,
                        { hbaoTemporalOutput.pass });

                    hbaoForLighting = hbaoTemporalOutput.historyTexture;
                    hbaoProducer = hbaoTemporalOutput.pass;
                    hbaoTemporalHandledByDeferredGraph = true;
                }
            }
        }
        else
        {
            // White is the multiplicative identity for screen-space AO; material AO remains active.
            hbaoForLighting = deferredGraph.RegisterExternalTexture(
                m_resourceManager.GetWhiteTexture(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                "HBAO.DisabledWhite");
            m_hbaoHistoryValid = false;
        }

        passViewer.Output(deferredGraph, RenderPassId::HBAO, hbaoForLighting, RenderPassViewer::Mapping::Scalar);

        DeferredLightingPass::Input deferredInput = {};
        deferredInput.gbufferAlbedo = gbufferOutput.albedo;
        deferredInput.gbufferNormal = gbufferOutput.normal;
        deferredInput.gbufferORM = gbufferOutput.orm;
        deferredInput.gbufferEmissive = gbufferOutput.emissive;
        deferredInput.depth = gbufferOutput.depth;
        deferredInput.hbaoBlurred = hbaoForLighting;
        deferredInput.shadowMap = shadowOutput.shadowMapSrv;

        DeferredLightingPass::Output deferredOutput = DeferredLightingPass::AddToGraph(
            deferredGraph,
            &m_resourceManager,
            &m_pipelineManager,
            m_currJitteredInvViewProjGpu,
            SceneWidth,
            SceneHeight,
            frameIndex,
            deferredInput);
        if (!deferredOutput.sceneColor.IsValid() || !deferredOutput.pass.IsValid())
        {
            OutputDebugStringA(
                "Error: Deferred lighting RDG construction failed; rendering stopped.\n");
            Running = false;
            EndFrame();
            return;
        }
        deferredGraph.AddPassDependencies(deferredOutput.pass, { shadowOutput.pass });
        if (hbaoProducer.IsValid())
        {
            deferredGraph.AddPassDependencies(deferredOutput.pass, { hbaoProducer });
        }
        RDGPassHandle sceneColorProducer = deferredOutput.pass;
        passViewer.Color(deferredGraph, RenderPassId::DeferredLighting, deferredOutput.sceneColor);
        SSGIPass::IrradiancePreviewOutput giDisplay;

        if (m_settingsManager.ssgi.enabled)
        {
            SSGIPass::Input ssgiInput={deferredOutput.sceneColor,gbufferOutput.depth,gbufferOutput.normal,
                gbufferOutput.orm,gbufferOutput.albedo,gbufferOutput.emissive};
            if(!m_surfaceCacheGI.Prepare(m_deviceContext,m_resourceManager,m_worldGeometry,m_surfaceCacheCapture,
                m_surfaceCacheLighting,frameIndex,m_settingsManager.vxgi,frameSSGI,ssgiInput.world))
            {Running=false;EndFrame();return;}
            ssgiSourceKey.sourceStamp=ssgiInput.world.sourceStamp;
            ssgiSourceKey.lumen=frameSSGI.lumen;
            ssgiSourceKey.traceDownsampleFactor = static_cast<uint32_t>(m_settingsManager.ssgi.traceDownsampleFactor);
            ssgiSourceKey.denoiseDownsampleFactor = static_cast<uint32_t>(m_settingsManager.ssgi.denoiseDownsampleFactor);
            ssgiSourceKey.screenProbes = m_settingsManager.ssgi.screenProbes;
            ssgiSourceKey.maxTraceIterations = static_cast<uint32_t>(m_settingsManager.ssgi.maxTraceIterations);
            ssgiSourceKey.emissiveSamplesPerProbe = static_cast<uint32_t>(m_settingsManager.ssgi.emissiveSamplesPerProbe);
            ssgiSourceKey.screenRaysPerProbe = static_cast<uint32_t>(m_settingsManager.ssgi.screenRaysPerProbe);
            ssgiSourceKey.screenProbeHistoryUpdates = static_cast<uint32_t>(m_settingsManager.ssgi.screenProbeHistoryUpdates);
            ssgiSourceKey.screenProbeJitterWidthFraction = frameSSGI.screenProbeJitterWidthFraction;
            ssgiSourceKey.screenProbeAdaptiveEnabled = frameSSGI.screenProbes && frameSSGI.screenProbeAdaptiveEnabled &&
                frameSSGI.screenProbeAdaptiveMaxFraction > 0.0f;
            ssgiSourceKey.screenProbeAdaptiveMaxFraction = ssgiSourceKey.screenProbeAdaptiveEnabled
                ? frameSSGI.screenProbeAdaptiveMaxFraction : 0.0f;
            ssgiSourceKey.screenTraceDistance = m_settingsManager.ssgi.screenTraceDistance;
            ssgiSourceReset = m_ssgiSources.NeedsReset(ssgiSourceKey);
            if (ssgiSourceReset) m_ssgiHistory.valid = false;
            ssgiInput.diagnostics = saveThisFrame && m_passView.pass == RenderPassId::GI;
            auto ssgiOutput = SSGIPass::AddToGraph(deferredGraph, &m_resourceManager,
                &m_pipelineManager, m_ssgiHistory, m_currViewGpu, m_currJitteredProjGpu,
                m_currJitteredInvProjGpu, m_currJitteredInvViewProjGpu,
                SceneWidth, SceneHeight, frameIndex, m_ssgiFrameIndex, frameSSGI, ssgiInput);
            if (!ssgiOutput.sceneColor.IsValid() || !ssgiOutput.pass.IsValid())
            {
                ErrorLog::Write("SSGI: RDG construction failed; rendering stopped.");
                Running = false;
                EndFrame();
                return;
            }
            sceneColorProducer = ssgiOutput.pass;
            auto previewGI = ssgiOutput.indirectLighting;
            auto previewReceiverGuide = ssgiOutput.receiverGuide;

            m_ssgiPreviewHistory.valid = false;
            if (saveThisFrame && m_passView.pass == RenderPassId::GI)
            {
                giCaptureOutputs = {
                    {"gi-filtered", previewGI, UINT_MAX, "linear E/pi, selected GI tier, before camera fade/receiver response/intensity/AA/tone mapping"},
                    {"gi-full-raw", ssgiOutput.rawLighting, UINT_MAX, "Surface Cache GI raw observations; checker-skipped values may contain reused history"},
                    {"gi-full-reconstructed", ssgiOutput.reconstructedLighting, UINT_MAX, "GI at denoising resolution after reconstruction, before temporal and A-trous; dimensions in denoise_size"},
                    {"depth", gbufferOutput.depth, UINT_MAX, "device depth"},
                    {"normal", gbufferOutput.normal, UINT_MAX, "GBuffer normal"}
                };
                if (ssgiOutput.probeDirections.IsValid())
                    giCaptureOutputs.push_back({"screen-probe-directions", ssgiOutput.probeDirections, UINT_MAX,
                        "8x8 directional cells per receiver; RGB=temporally accumulated cached Li, alpha=min screen/world hit distance; no spatial feedback"});
                if (frameSSGI.screenProbes)
                {
                    giCaptureOutputs.push_back({"screen-probe-positions", ssgiOutput.probePosition, UINT_MAX,
                        "world xyz; alpha 0=invalid, 1=held, 2=fresh (configured ray budget per fresh receiver)"});
                    giCaptureOutputs.push_back({"screen-probe-normal-state", ssgiOutput.probeNormal, UINT_MAX,
                        "world geometric normal xyz; alpha=accepted fresh-batch blend age, not independent sample count"});
                    giCaptureOutputs.push_back({"screen-probe-receivers", ssgiOutput.receiverGuide, UINT_MAX,
                        "full-resolution pixel linear index + 1; zero invalid"});
                    giCaptureOutputs.push_back({"screen-probe-filtered-directions", ssgiOutput.probeFiltered, UINT_MAX,
                        "spatially filtered incident Li RGB, alpha=center hit distance; integrated only afterwards"});
                    giCaptureOutputs.push_back({"screen-probe-sh", ssgiOutput.probes.sh, UINT_MAX,
                        "3x3 SH2 coefficients per atlas probe, after directional filtering and diffuse convolution"});
                    giCaptureOutputs.push_back({"screen-probe-validity", ssgiOutput.probes.validity, UINT_MAX,
                        "RGB=slow GI history, alpha=accepted pixel history length; zero alpha is missing support"});
                    if (ssgiOutput.probes.adaptiveCount.IsValid())
                    {
                        giCaptureOutputs.push_back({"screen-probe-adaptive-count", ssgiOutput.probes.adaptiveCount, UINT_MAX,
                            "uint requested adaptive allocations; actual active count=min(requested, capacity)"});
                        giCaptureOutputs.push_back({"screen-probe-adaptive-header", ssgiOutput.probes.tileHeader, UINT_MAX,
                            "uint actual adaptive probe count per uniform tile, maximum eight"});
                        giCaptureOutputs.push_back({"screen-probe-adaptive-indices", ssgiOutput.probes.tileIndices, UINT_MAX,
                            "eight uint full atlas ProbeIDs per tile; read only header-count slots"});
                    }
                    if (ssgiOutput.probes.diagnostics.IsValid())
                        giCaptureOutputs.push_back({"screen-probe-diagnostics", ssgiOutput.probes.diagnostics, UINT_MAX,
                            "uint[32]: 0..13 placement/history/repair; 16 screen hits,17 near Mesh hits,18 Global candidates,19 cache misses,20 world emitter visibility queries,21 emitter samples accepted,22 emission replacements,23 screen cache reads,24 near Mesh cache reads,25 confirmed far cache reads; remaining reserved"});
                }
            }
            if (pureGIView)
            {
                giDisplay = SSGIPass::AddIrradiancePreviewToGraph(deferredGraph, &m_resourceManager,
                    &m_pipelineManager, m_currViewGpu, m_currJitteredInvProjGpu, m_currJitteredInvViewProjGpu, SceneWidth, SceneHeight,
                    frameIndex, frameSSGI.DenoiseFactor(), ssgiInput,
                    previewGI, frameSSGI, frameSSGI.screenProbes ? RDGTextureHandle{} : previewReceiverGuide, ssgiOutput.probes);
                if (!giDisplay.texture.IsValid() || !giDisplay.pass.IsValid())
                {
                    ErrorLog::Write("SSGI: irradiance display construction failed; rendering stopped.");
                    Running = false;
                    EndFrame();
                    return;
                }
            }
            passViewer.Accumulated(deferredGraph, RenderPassId::GI, ssgiOutput.sceneColor);
            ssgiHistoryWritten = true;
        }

        passViewer.Before(deferredGraph, RenderPassId::Skybox, deferredOutput.sceneColor);
        if (!showGIOnly)
        {
            RDGPassHandle skyboxPass = SkyboxPass::AddToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                camera,
                sceneViewport,
                sceneScissorRect,
                SceneWidth,
                SceneHeight,
                { deferredOutput.sceneColor, gbufferOutput.depth });
            if (!skyboxPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred skybox RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
            deferredGraph.AddPassDependencies(skyboxPass, { sceneColorProducer });
            sceneColorProducer = skyboxPass;
            passViewer.After(deferredGraph, RenderPassId::Skybox, deferredOutput.sceneColor);
            passViewer.Before(deferredGraph, RenderPassId::Transparent, deferredOutput.sceneColor);

            RDGPassHandle transparentPass = PBRPass::AddTransparentToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                sceneViewport,
                sceneScissorRect,
                g_visibleInstances,
                transparentStartIndex,
                { deferredOutput.sceneColor, gbufferOutput.depth, shadowOutput.shadowMapSrv });
            if (!transparentPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred transparent RDG construction failed; rendering stopped.\n");
                Running = false;
                EndFrame();
                return;
            }
            sceneColorProducer = transparentPass;
            passViewer.After(deferredGraph, RenderPassId::Transparent, deferredOutput.sceneColor);
        }

        // Pure GI uses the same reconstruction, configured AA and tone mapping
        // as the former GI-only view. Its lighting history remains independent
        // from color AA history, which is reset when the displayed source changes.
        const auto deferredAAColor = pureGIView ? giDisplay.texture : deferredOutput.sceneColor;
        const auto deferredAAProducer = pureGIView ? giDisplay.pass : sceneColorProducer;
        RDGTextureHandle deferredFinalColor = deferredAAColor;
        RDGPassHandle deferredFinalColorProducer = deferredAAProducer;
        if (m_antiAliasingMode == AntiAliasingMode::TAA ||
            m_antiAliasingMode == AntiAliasingMode::TSR ||
            m_antiAliasingMode == AntiAliasingMode::DLSS)
        {
            if (m_antiAliasingMode == AntiAliasingMode::TAA)
            {
                TAAPass::Output taaOutput = TAAPass::AddToGraph(
                    deferredGraph,
                    &m_deviceContext,
                    &m_resourceManager,
                    &m_pipelineManager,
                    m_currJitteredInvViewProjGpu,
                    m_prevUnjitteredViewProjGpu,
                    m_currJitterNdcX,
                    m_currJitterNdcY,
                    frameIndex,
                    Width,
                    Height,
                    m_temporalHistoryValid,
                    { deferredAAColor, gbufferOutput.depth, motionOutput.motionTexture });

                if (!taaOutput.historyTexture.IsValid() || !taaOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Deferred TAA RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                deferredGraph.AddPassDependencies(
                    taaOutput.pass,
                    { deferredAAProducer, motionOutput.pass });
                deferredFinalColor = taaOutput.historyTexture;
                deferredFinalColorProducer = taaOutput.pass;
                temporalHistoryWrittenByDeferredGraph = true;
            }
            else if (m_antiAliasingMode == AntiAliasingMode::TSR)
            {
                TSRPass::Output tsrOutput = TSRPass::AddToGraph(
                    deferredGraph,
                    &m_deviceContext,
                    &m_resourceManager,
                    &m_pipelineManager,
                    m_currJitteredInvViewProjGpu,
                    m_prevUnjitteredViewProjGpu,
                    m_currJitterNdcX,
                    m_currJitterNdcY,
                    frameIndex,
                    SceneWidth,
                    SceneHeight,
                    Width,
                    Height,
                    m_temporalHistoryValid,
                    { deferredAAColor, gbufferOutput.depth, motionOutput.motionTexture });

                if (!tsrOutput.historyTexture.IsValid() || !tsrOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Deferred TSR RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                deferredGraph.AddPassDependencies(
                    tsrOutput.pass,
                    { deferredAAProducer, motionOutput.pass });
                deferredFinalColor = tsrOutput.historyTexture;
                deferredFinalColorProducer = tsrOutput.pass;
                temporalHistoryWrittenByDeferredGraph = true;
            }
            else
            {
                dlssResetRequested = !m_dlssHistoryValid;
                DLSSPass::Output dlssOutput = DLSSPass::AddToGraph(
                    deferredGraph,
                    &m_dlssManager,
                    &m_resourceManager,
                    m_currJitterPixelX,
                    m_currJitterPixelY,
                    (std::max)(deltaTime * 1000.0f, 0.0f),
                    dlssResetRequested,
                    { deferredAAColor, gbufferOutput.depth, motionOutput.motionTexture },
                    [this](ID3D12GraphicsCommandList* commandList)
                    {
                        // The post-process root signature declares
                        // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, so the heap must be bound first
                        ID3D12DescriptorHeap* heaps[] =
                        {
                            m_resourceManager.GetMainDescriptorHeap()
                        };
                        commandList->SetDescriptorHeaps(1, heaps);

                        commandList->SetGraphicsRootSignature(
                            m_pipelineManager.GetPostProcessRootSignature());
                        commandList->SetPipelineState(
                            m_pipelineManager.GetPostProcessPSO(false));
                        commandList->RSSetViewports(1, &viewport);
                        commandList->RSSetScissorRects(1, &scissorRect);
                        commandList->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    });

                if (!dlssOutput.outputTexture.IsValid() || !dlssOutput.pass.IsValid())
                {
                    OutputDebugStringA(
                        "Error: Deferred DLSS RDG construction failed; rendering stopped instead of degrading.\n");
                    Running = false;
                    EndFrame();
                    return;
                }

                deferredGraph.AddPassDependencies(
                    dlssOutput.pass,
                    { deferredAAProducer, motionOutput.pass });
                deferredFinalColor = dlssOutput.outputTexture;
                deferredFinalColorProducer = dlssOutput.pass;
                dlssEvaluatedByDeferredGraph = true;
            }
        }

        if (m_antiAliasingMode != AntiAliasingMode::None && m_antiAliasingMode != AntiAliasingMode::SMAA)
            passViewer.Accumulated(deferredGraph, RenderPassId::AntiAliasing, deferredFinalColor);

        if (m_antiAliasingMode == AntiAliasingMode::SMAA)
        {
            PostProcessPass::TextureOutput toneMapOutput =
                PostProcessPass::AddToTextureGraph(
                    deferredGraph,
                    &m_resourceManager,
                    &m_pipelineManager,
                    frameIndex,
                    viewport,
                    scissorRect,
                    Width,
                    Height,
                    deferredFinalColor,
                    false);
            if (!toneMapOutput.texture.IsValid() ||
                !toneMapOutput.pass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred SMAA tone-map RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            passViewer.Accumulated(deferredGraph, RenderPassId::PostProcess, toneMapOutput.texture, RenderPassViewer::Mapping::LDR);
            deferredGraph.AddPassDependency(
                toneMapOutput.pass,
                deferredFinalColorProducer);

            RDGTextureHandle backBuffer =
                deferredGraph.RegisterExternalTextureOutput(
                    m_deviceContext.GetRenderTarget(frameIndex),
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_PRESENT,
                    "BackBuffer");

            SMAAPass::Input smaaInput = {};
            smaaInput.color = toneMapOutput.texture;
            smaaInput.output = backBuffer;
            SMAAPass::Output smaaOutput = SMAAPass::AddToGraph(
                deferredGraph,
                &m_resourceManager,
                &m_pipelineManager,
                Width,
                Height,
                smaaInput);
            if (!smaaOutput.color.IsValid() ||
                !smaaOutput.neighborhoodPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred SMAA RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            passViewer.Accumulated(deferredGraph, RenderPassId::AntiAliasing, smaaOutput.color, RenderPassViewer::Mapping::LDR);
            deferredGraph.AddPassDependency(
                smaaOutput.edgePass,
                toneMapOutput.pass);
        }
        else
        {
            RDGPassHandle postProcessPass = PostProcessPass::AddFinalToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                viewport,
                scissorRect,
                deferredFinalColor,
                enablePostProcessSharpen);
            if (!postProcessPass.IsValid())
            {
                OutputDebugStringA(
                    "Error: Deferred post-process RDG construction failed; rendering stopped instead of degrading.\n");
                Running = false;
                EndFrame();
                return;
            }
            deferredGraph.AddPassDependencies(
                postProcessPass,
                { deferredFinalColorProducer });
        }

        // Execute the entire graph after all passes have been added
        if (m_antiAliasingMode != AntiAliasingMode::SMAA && passViewer.Wants(RenderPassId::PostProcess))
        {
            const auto finalColor = deferredGraph.RegisterExternalTexture(m_deviceContext.GetRenderTarget(frameIndex),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");
            passViewer.Accumulated(deferredGraph, RenderPassId::PostProcess, finalColor, RenderPassViewer::Mapping::LDR);
        }
        if (pureGIView)
        {
            const auto finalColor = deferredGraph.RegisterExternalTexture(m_deviceContext.GetRenderTarget(frameIndex),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");
            passViewer.Output(deferredGraph, RenderPassId::GI, finalColor, RenderPassViewer::Mapping::LDR);
        }
        if (!finishPassView(deferredGraph))
        {
            Running = false;
            EndFrame();
            return;
        }
        deferredGraph.Execute(m_deviceContext.GetCommandList());
        if (m_resourceManager.PassConstantsAllocationFailed(frameIndex))
        {
            ErrorLog::Write("Application: pass constant allocation failed; rendering stopped.");
            Running = false;
            EndFrame();
            return;
        }

        if (dlssEvaluatedByDeferredGraph)
        {
            if (!m_dlssManager.WasLastEvaluationSuccessful())
            {
                m_dlssHistoryValid = false;
                OutputDebugStringA(
                    "Error: Deferred DLSS evaluation failed; the frame was not submitted.\n");
                Running = false;
                EndFrame();
                return;
            }

            dlssHistoryWritten = true;
        }

        if (hbaoTemporalHandledByDeferredGraph)
        {
            hbaoHistoryWritten = true;
        }

        if (temporalHistoryWrittenByDeferredGraph)
        {
            temporalHistoryWritten = true;
        }
    }

    if (!geometryDiagnostic && !passViewer.Active()) m_editorSelectionRenderer.Record(m_deviceContext, m_resourceManager, m_editorSelection,
        frameIndex, m_currUnjitteredViewProjGpu, m_imguiRtvHandles[frameIndex], io.DisplaySize.x, io.DisplaySize.y);

    m_surfaceCacheCapture.RecordFeedbackReadback(m_deviceContext);
    if (!EndFrame())
    {
        // The command list could not be finalized; skip submission and presentation
        return;
    }

    ID3D12CommandList* lists[] = { m_deviceContext.GetCommandList() };
    // Submit recorded rendering commands to the GPU for execution
    m_deviceContext.GetCommandQueue()->ExecuteCommandLists(1, lists);
    captureFrame.executed = true;
    if (!m_worldGeometry.OnSubmitted())
    {
        ErrorLog::Write(m_worldGeometry.GetLastError());
        Running = false;
    }
    m_surfaceCacheCapture.OnSubmitted();
    m_surfaceCacheLighting.OnSubmitted();
    // Insert a signal into the queue to track GPU progress
    HRESULT hr = m_deviceContext.GetCommandQueue()->Signal(
        m_deviceContext.GetFence(frameIndex),
        ++m_deviceContext.GetFenceValue(frameIndex));
    if (FAILED(hr))
    {
        ReportFrameError("Application: failed to signal the frame fence.", hr);
    }
    else
    {
        if (!m_passCapture.OnSubmitted(frameIndex, m_deviceContext.GetFence(frameIndex),
            m_deviceContext.GetFenceValue(frameIndex))) ErrorLog::Write(m_passCapture.GetLastError());
        CommitTemporalHistories(temporalHistoryWritten, hbaoHistoryWritten, dlssHistoryWritten, ssgiHistoryWritten);
        if (ssgiPreviewHistoryWritten)
        {
            m_resourceManager.FlipSSGIHistoryIndex(true);
            m_ssgiPreviewHistory = m_ssgiHistory;
        }
        m_lastRenderedDebugPass = m_debugPass;
        m_lastRenderedPureGI = pureGIView;
        m_lastRenderedPureGISource = m_passView.giSource;
        if (ssgiHistoryWritten)
        {
            m_ssgiSources.Commit(ssgiSourceKey);
            ++m_ssgiFrameIndex;
        }
        m_editorSelectionRenderer.OnSubmitted(frameIndex, m_deviceContext.GetFenceValue(frameIndex));
    }

    // Flip the back buffer to the front screen
    hr = m_deviceContext.GetSwapChain()->Present(0, 0);
    if (FAILED(hr))
    {
        ReportFrameError("Application: swap-chain Present failed.", hr);
    }
    ++m_renderedFrames;
    if (m_passRunOptions.frameLimit && m_renderedFrames >= m_passRunOptions.frameLimit)
        Running = false;

    // Match Escape's confirmation and shutdown path, without destroying the window
    // while ImGui or the frame command list is still being recorded.
    if (quitRequested && InputManager::ConfirmExit())
    {
        Running = false;
        DestroyWindow(hwnd);
    }
}

bool D3D12App::InitLoadingRenderer()
{
    auto* device = m_deviceContext.GetDevice();
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_loadingAllocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_loadingAllocator.Get(), nullptr, IID_PPV_ARGS(&m_loadingCommandList));
    if (SUCCEEDED(hr)) hr = m_loadingCommandList->Close();
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_loadingFence));
    if (FAILED(hr)) ErrorLog::HRESULT("Loading screen: failed to initialize command resources.", hr);
    return SUCCEEDED(hr);
}

bool D3D12App::WaitForLoadingFrame()
{
    if (!m_loadingFence) return true;
    const auto completed = m_loadingFence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    if (completed >= m_loadingFenceValue) return true;
    const HRESULT hr = m_loadingFence->SetEventOnCompletion(m_loadingFenceValue, m_deviceContext.GetFenceEvent());
    if (FAILED(hr))
    {
        ErrorLog::HRESULT("Loading screen: failed to wait for its frame.", hr);
        return false;
    }
    return WaitForSingleObject(m_deviceContext.GetFenceEvent(), INFINITE) == WAIT_OBJECT_0;
}

void D3D12App::ShowLoadingProgress(float fraction, const std::string& status)
{
    MSG message{};
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT) { Running = false; break; }
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    if (!Running) throw InitializationStopped{};
    m_loadingProgress = (std::max)(m_loadingProgress, (std::clamp)(fraction, 0.0f, 1.0f));
    const ULONGLONG tick = GetTickCount64();
    // Stage/file changes must be visible before their blocking work starts.
    const std::string& stageKey = status;
    if (stageKey == m_loadingStageKey && m_loadingLastDrawTick != 0 &&
        tick - m_loadingLastDrawTick < 50 && m_loadingProgress < 1.0f) return;
    const auto check = [](HRESULT hr, const char* operation) {
        if (FAILED(hr)) { ErrorLog::HRESULT(operation, hr); throw InitializationStopped{}; }
    };
    if (!WaitForLoadingFrame()) throw InitializationStopped{};
    check(m_loadingAllocator->Reset(), "Loading screen: reset allocator.");
    check(m_loadingCommandList->Reset(m_loadingAllocator.Get(), nullptr), "Loading screen: reset command list.");
    const int slot = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    auto& io = ImGui::GetIO();
    const auto desc = m_deviceContext.GetRenderTarget(slot)->GetDesc();
    io.DisplayFramebufferScale = ImVec2(1, 1);
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(desc.Width) / io.DisplaySize.x,
            static_cast<float>(desc.Height) / io.DisplaySize.y);
    EditorUI::UpdateScale(m_uiScaleOverride > 0 ? m_uiScaleOverride :
        static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
    ImGui::NewFrame();
    EditorUICenter::LoadingScreen(m_loadingProgress, status.c_str());
    ImGui::Render();
    const float clear[] = { EditorColors::Background.x, EditorColors::Background.y, EditorColors::Background.z, 1 };
    RecordImGuiDrawData(m_loadingCommandList.Get(), slot, false, clear);
    check(m_loadingCommandList->Close(), "Loading screen: close command list.");
    ID3D12CommandList* lists[] = { m_loadingCommandList.Get() };
    m_deviceContext.GetCommandQueue()->ExecuteCommandLists(1, lists);
    const HRESULT presentResult = m_deviceContext.GetSwapChain()->Present(0, 0);
    // Present also queues work that references the swap-chain buffers. Include
    // it in the loading fence so leaving initialization can release them safely.
    check(m_deviceContext.GetCommandQueue()->Signal(m_loadingFence.Get(), ++m_loadingFenceValue),
        "Loading screen: signal frame completion.");
    check(presentResult, "Loading screen: present.");
    m_loadingLastDrawTick = tick;
    m_loadingStageKey = stageKey;
    // Loading presentation advances the swap chain independently of scene work.
    frameIndex = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();
}

// Set up Dear ImGui: dedicated descriptor heaps, platform/renderer backends, and SRV allocator callbacks
bool D3D12App::InitImGui()
{
    ID3D12Device* device = m_deviceContext.GetDevice();

    // The engine creates RTVs only inside the RDG's transient heaps, so ImGui owns
    // a tiny persistent RTV heap with one descriptor per back buffer.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = frameBufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_imguiRtvHeap));
    if (FAILED(hr))
    {
        ErrorLog::HRESULT("ImGui: failed to create the RTV descriptor heap.", hr);
        return false;
    }

    const UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_imguiRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < frameBufferCount; ++i)
    {
        device->CreateRenderTargetView(m_deviceContext.GetRenderTarget(i), nullptr, rtvHandle);
        m_imguiRtvHandles[i] = rtvHandle;
        rtvHandle.ptr += rtvDescriptorSize;
    }

    // ImGui textures must not occupy the engine's bindless heap; a small dedicated
    // shader-visible heap with a free-list keeps allocation bookkeeping trivial.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = imGuiSrvHeapSize;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_imguiSrvHeap));
    if (FAILED(hr))
    {
        ErrorLog::HRESULT("ImGui: failed to create the SRV descriptor heap.", hr);
        return false;
    }

    m_imguiSrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_imguiSrvHeapCpuStart = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    m_imguiSrvHeapGpuStart = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    m_imguiSrvFreeList.reserve(imGuiSrvHeapSize);
    for (UINT i = 0; i < imGuiSrvHeapSize; ++i)
    {
        // Reverse order so slot 0 is served first
        m_imguiSrvFreeList.push_back(imGuiSrvHeapSize - 1 - i);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Initialize the shared editor theme before the first frame using window DPI.
    RECT clientRect = {};
    GetClientRect(hwnd, &clientRect);
    EditorUI::Initialize(static_cast<float>(clientRect.right - clientRect.left),
        m_uiScaleOverride > 0.0f ? m_uiScaleOverride : static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
    ErrorLog::Write("Editor: UI initialized (fixed three-pane layout).");

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ErrorLog::Write("ImGui: Win32 platform backend initialization failed.");
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = device;
    initInfo.CommandQueue = m_deviceContext.GetCommandQueue();
    initInfo.NumFramesInFlight = frameBufferCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // Must match the swap chain format
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;        // ImGui draws to the back buffer without depth
    initInfo.UserData = this;
    initInfo.SrvDescriptorHeap = m_imguiSrvHeap.Get();
    initInfo.SrvDescriptorAllocFn = &D3D12App::ImGuiSrvAlloc;
    initInfo.SrvDescriptorFreeFn = &D3D12App::ImGuiSrvFree;
    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        ErrorLog::Write("ImGui: DX12 renderer backend initialization failed.");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_imguiInitialized = true;
    return true;
}

// Record ImGui's draw data onto the frame command list, after the RDG has finished with the back buffer
void D3D12App::RecordImGuiDrawData()
{
    RecordImGuiDrawData(m_deviceContext.GetCommandList(), frameIndex, true);
}

void D3D12App::RecordImGuiDrawData(ID3D12GraphicsCommandList* commandList, int slot,
    bool restoreEngineHeap, const float* clearColor)
{
    ID3D12Resource* backBuffer = m_deviceContext.GetRenderTarget(slot);

    // The RDG leaves the back buffer in PRESENT state; promote it to RENDER_TARGET for ImGui
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &barrier);

    commandList->OMSetRenderTargets(1, &m_imguiRtvHandles[slot], FALSE, nullptr);
    if (clearColor) commandList->ClearRenderTargetView(m_imguiRtvHandles[slot], clearColor, 0, nullptr);

    // The DX12 backend never binds a descriptor heap itself; it samples from this one
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

    // Hand the engine heap back: any HEAP_DIRECTLY_INDEXED root signature set afterwards
    // would otherwise capture ImGui's heap as its bindless index space.
    if (restoreEngineHeap)
    {
        ID3D12DescriptorHeap* engineHeaps[] = { m_resourceManager.GetMainDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, engineHeaps);
    }

    // Hand the back buffer back to the swap chain in PRESENT state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);
}

// Tear down Dear ImGui; called while the GPU is idle, before the device is released
void D3D12App::ShutdownImGui()
{
    if (!m_imguiInitialized)
    {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_imguiInitialized = false;
}

// SRV allocator callbacks handed to the DX12 backend: a free-list over m_imguiSrvHeap
void D3D12App::ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    D3D12App* app = static_cast<D3D12App*>(info->UserData);
    IM_ASSERT(!app->m_imguiSrvFreeList.empty());
    const UINT index = app->m_imguiSrvFreeList.back();
    app->m_imguiSrvFreeList.pop_back();
    outCpuHandle->ptr = app->m_imguiSrvHeapCpuStart.ptr + static_cast<SIZE_T>(index) * app->m_imguiSrvDescriptorSize;
    outGpuHandle->ptr = app->m_imguiSrvHeapGpuStart.ptr + static_cast<UINT64>(index) * app->m_imguiSrvDescriptorSize;
}

void D3D12App::ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    D3D12App* app = static_cast<D3D12App*>(info->UserData);
    const UINT index = static_cast<UINT>((cpuHandle.ptr - app->m_imguiSrvHeapCpuStart.ptr) / app->m_imguiSrvDescriptorSize);
    app->m_imguiSrvFreeList.push_back(index);
}

// Track GPU progress, prevent data updates until execution is complete, provide an 'alarm' mechanism for the CPU (via Fence Events)
void D3D12App::WaitForPreviousFrame()
{
    frameIndex = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();
    m_deviceContext.WaitForPreviousFrame(frameIndex);
}
