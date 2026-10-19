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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace DirectX;
// std::shared_ptr allocates an external reference counter, whereas ComPtr uses the internal counter of the COM object itself
using Microsoft::WRL::ComPtr;

static std::vector<ModelInstance*> g_visibleInstances;
static std::array<std::vector<ModelInstance*>, NUM_CASCADES> g_shadowVisibleInstancesByCascade;
static std::array<size_t, NUM_CASCADES> g_shadowInstanceOffsets;
struct InitializationStopped {};

// Bounded-rate Debug evidence for startup versus T round trips. The frame
// interval includes GPU waits, Present and messages; it is not a CPU/GPU timer.
static void LogSubmittedGIState(UINT sampleFrame, int slot, float frameWallSeconds,
    int requestedMode, uint32_t effectiveMode, bool historyWasValid, bool sourceReset,
    const VXGIProbeState& state, const VXGIProbePass& probes,
    const VXGIProbeFilterPass& filter, const VXGIGPUScene& scene)
{
#if defined(DEBUG) || defined(_DEBUG)
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    static auto last = start;
    static int previousRequested = -1;
    static uint32_t previousEffective = UINT32_MAX;
    const auto now = Clock::now();
    const bool changed = requestedMode != previousRequested || effectiveMode != previousEffective;
    if (!changed && now - last < std::chrono::seconds(1)) return;
    static std::ofstream output = [] {
        std::error_code error;
        std::filesystem::create_directories("tmp", error);
        std::ofstream file("tmp/VXGI-runtime-diagnostics.csv", std::ios::trunc);
        file << "elapsed_seconds,sample_frame,slot,requested_mode,effective_mode,previous_frame_wall_ms,"
            "history_was_valid,source_reset,probe_plan_frame,probe_work_count,probe_epoch,active_batches,"
            "history_copies,update_dispatches,feedback_copies,filter_active_batches,probe_count,"
            "valid,disabled,unclassified,last_probe_gpu_ms,last_filter_gpu_ms,raw_gpu_address,"
            "sdf_nodes,assets,instances,bvh_nodes\n";
        return file;
    }();
    const auto& plan = state.GetPendingPlan();
    const auto& stats = state.GetStats();
    auto* raw = probes.GetDisplayHistory();
    output << std::chrono::duration<double>(now - start).count() << ',' << sampleFrame << ',' << slot
        << ',' << requestedMode << ',' << effectiveMode << ',' << frameWallSeconds * 1000.0f
        << ',' << historyWasValid << ',' << sourceReset << ',' << plan.frameIndex << ',' << plan.count
        << ',' << plan.epoch << ',' << probes.GetActivityStats().activeBatches
        << ',' << probes.GetActivityStats().historyCopies << ',' << probes.GetActivityStats().updateDispatches
        << ',' << probes.GetActivityStats().feedbackCopies << ',' << filter.GetActivityStats().activeBatches
        << ',' << state.GetPendingSources().probeCount << ',' << stats.valid
        << ',' << stats.disabled << ',' << stats.unclassified << ',' << probes.GetGPUMilliseconds()
        << ',' << filter.GetGPUMilliseconds() << ',' << (raw ? raw->GetGPUVirtualAddress() : 0)
        << ',' << scene.GetDistanceNodeCount(slot) << ',' << scene.GetAssetCount(slot)
        << ',' << scene.GetInstanceCount(slot) << ',' << scene.GetInstanceBVHNodeCount(slot) << '\n';
    output.flush();
    if (changed)
        ErrorLog::Write("SSGI submitted: requested=" + std::to_string(requestedMode) +
            ", effective=" + std::to_string(effectiveMode) + ", probe work=" + std::to_string(plan.count) +
            ". Runtime evidence: tmp/VXGI-runtime-diagnostics.csv");
    previousRequested = requestedMode;
    previousEffective = effectiveMode;
    last = now;
#else
    (void)sampleFrame; (void)slot; (void)frameWallSeconds; (void)requestedMode; (void)effectiveMode;
    (void)historyWasValid; (void)sourceReset; (void)state; (void)probes; (void)filter; (void)scene;
#endif
}

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

    const bool verifyStartup = std::strstr(lpCmdLine, "--verify-startup") != nullptr;
    D3D12App app(hInstance);
    g_App = &app;
    const bool profileStartup = app.ConfigureDiagnostics(lpCmdLine);

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
    if (!app.Initialize((verifyStartup || profileStartup) ? SW_HIDE : nShowCmd, uiScaleOverride))
    {
        ErrorLog::Write("Application: initialization failed; WinMain is returning failure.");
        return 1;
    }

    if (!verifyStartup) app.Run();

    return 0;
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
        if (m_diagnosticsPGODevice && m_diagnosticsPGOOverride)
        {
            const HRESULT result = m_diagnosticsPGODevice->SetBackgroundProcessingMode(
                D3D12_BACKGROUND_PROCESSING_MODE_ALLOWED, D3D12_MEASUREMENTS_ACTION_KEEP_ALL, nullptr, nullptr);
            if (FAILED(result)) ErrorLog::HRESULT("Diagnostics: failed to restore default background processing.", result);
            m_diagnosticsPGOOverride = false;
        }
        m_dlssManager.Shutdown();
        // The GPU is idle here; ImGui's device objects can be released safely
        ShutdownImGui();
        if (m_diagnosticsGPUValidation)
            m_validationDiagnostics.Collect("shutdown-gpu-idle", m_diagnosticsFrame, RenderDebugWorldMode(m_debugPass));
    }
    if (m_diagnosticsGPUValidation && !m_validationDiagnostics.Finish())
        ErrorLog::Write("Diagnostics: full-program GPU validation failed; inspect the validation report.");
    // Verification can exit before Run() receives WM_DESTROY. Retire the native
    // window while its message target and all input state still exist.
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    hwnd = nullptr;
    if (g_App == this) g_App = nullptr;
}

static std::wstring g_wWindowTitle;
bool D3D12App::ConfigureDiagnostics(const char* commandLine)
{
    std::istringstream arguments(commandLine ? commandLine : "");
    std::string argument;
    while (arguments >> argument)
    {
        if (argument.rfind("--profile-startup=", 0) == 0)
        {
            const auto label = argument.substr(18);
            if (!label.empty() && label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos)
                m_diagnosticsPrefix = "tmp/VXGI-full-" + label;
        }
        else if (argument.rfind("--profile-sequence=", 0) == 0)
            m_diagnosticsSequence = argument.substr(19);
        else if (argument.rfind("--profile-seconds=", 0) == 0)
            m_diagnosticsSeconds = std::clamp(std::strtod(argument.c_str() + 18, nullptr), 1.0, 90.0);
        else if (argument == "--profile-gpu-validation")
            m_diagnosticsGPUValidation = true;
        else if (argument.rfind("--profile-mode-read=", 0) == 0)
        {
            const auto value = argument.substr(20);
            if (value == "srv") m_diagnosticsModeFromSRV = true;
            else if (value == "cbv") m_diagnosticsModeFromSRV = false;
            else m_diagnosticsOptionsValid = false;
        }
    }
    return !m_diagnosticsPrefix.empty();
}

bool D3D12App::Initialize(int nShowCmd, float uiScaleOverride)
{
    if (!m_diagnosticsOptionsValid ||
        ((m_diagnosticsGPUValidation || !m_diagnosticsModeFromSRV) && m_diagnosticsPrefix.empty()) ||
        (m_diagnosticsSequence == "validation-toggle" && !m_diagnosticsGPUValidation))
    {
        ErrorLog::Write("Diagnostics: validation/read-path overrides require a valid --profile-startup label and cbv|srv mode.");
        return false;
    }
    m_loadingHiddenVerification = nShowCmd == SW_HIDE;
    m_uiScaleOverride = uiScaleOverride;
    m_settingsManager.LoadAllSettingsFromJson();

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

        if (m_settingsManager.vxgi.enabled &&
            (!m_vxgiDebugRenderer.Initialize(m_deviceContext, frameBufferCount) ||
             !m_vxgiSDFDebugRenderer.Initialize(m_deviceContext, frameBufferCount) ||
             !m_vxgiProbeDebugRenderer.Initialize(m_deviceContext)))
        {
            ErrorLog::Write("Application: VXGI diagnostic renderer initialization failed.");
            return false;
        }

        ShowLoadingProgress(1.0f, "Ready");
        if (!WaitForLoadingFrame()) return false;
        if (m_diagnosticsGPUValidation)
        {
            m_validationDiagnostics.Collect("initialization-complete", 0, RenderDebugWorldMode(m_debugPass));
            if (m_validationDiagnostics.GetErrorCount()) return false;
        }
        if (!m_diagnosticsPrefix.empty() && m_diagnosticsSequence == "no-dlss")
            m_antiAliasingMode = AntiAliasingMode::None;
        if (!m_diagnosticsPrefix.empty() &&
            !m_frameDiagnostics.Initialize(m_deviceContext.GetDevice(), m_deviceContext.GetCommandQueue(),
                frameBufferCount, m_diagnosticsPrefix)) return false;
        if (m_frameDiagnostics.IsEnabled() &&
            (m_diagnosticsSequence == "no-pgo" || m_diagnosticsSequence == "no-background" ||
             m_diagnosticsSequence == "pgo" || m_diagnosticsSequence == "pgo-discard"))
        {
            HRESULT result = m_deviceContext.GetDevice()->QueryInterface(IID_PPV_ARGS(&m_diagnosticsPGODevice));
            if (FAILED(result))
            {
                ErrorLog::HRESULT("Diagnostics: background processing requires ID3D12Device6.", result);
                return false;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
            if (SUCCEEDED(m_deviceContext.GetDevice()->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options))))
                m_diagnosticsBackgroundSupport = options.BackgroundProcessingSupported ? 1 : 0;
            // These overrides are requested only by explicit diagnostic CLI
            // sequences. They do not change Windows developer-mode settings.
            if (m_diagnosticsSequence != "pgo-discard")
            {
                const auto mode = m_diagnosticsSequence == "no-pgo" ?
                    D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM :
                    m_diagnosticsSequence == "no-background" ?
                    D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_BACKGROUND_WORK :
                    D3D12_BACKGROUND_PROCESSING_MODE_ALLOW_INTRUSIVE_MEASUREMENTS;
                result = m_diagnosticsPGODevice->SetBackgroundProcessingMode(
                    mode, D3D12_MEASUREMENTS_ACTION_KEEP_ALL, nullptr, nullptr);
                if (FAILED(result))
                {
                    ErrorLog::HRESULT("Diagnostics: SetBackgroundProcessingMode initialization failed.", result);
                    return false;
                }
                m_diagnosticsPGOOverride = true;
            }
            ErrorLog::Write("Diagnostics: background processing sequence " + m_diagnosticsSequence +
                "; BackgroundProcessingSupported=" + std::to_string(m_diagnosticsBackgroundSupport) + '.');
        }
        if (m_frameDiagnostics.IsEnabled() && (m_diagnosticsSequence == "capture-toggle" || m_diagnosticsSequence == "capture-fixed" || m_diagnosticsSequence == "alias"))
        {
            const UINT factor = static_cast<UINT>(m_settingsManager.ssgi.traceDownsampleFactor);
            if (!m_traceOutputDiagnostics.Initialize(m_deviceContext.GetDevice(),
                (SceneWidth + factor - 1) / factor, (SceneHeight + factor - 1) / factor,
                RenderFormats::IndirectLighting, m_diagnosticsPrefix,
                m_diagnosticsSequence == "capture-fixed" ? 280 : TraceOutputDiagnostics::JointSamplePeriod)) return false;
        }
        if (m_loadingHiddenVerification)
        {
            uint64_t errors = 0, warnings = 0;
            ComPtr<ID3D12InfoQueue> info;
            if (SUCCEEDED(m_deviceContext.GetDevice()->QueryInterface(IID_PPV_ARGS(&info))))
                for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
                {
                    SIZE_T bytes = 0;
                    if (FAILED(info->GetMessage(index, nullptr, &bytes))) continue;
                    std::vector<uint8_t> storage(bytes);
                    auto* entry = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (FAILED(info->GetMessage(index, entry, &bytes))) continue;
                    if (entry->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++errors;
                    if (entry->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++warnings;
                }
            const bool ready = !m_settingsManager.vxgi.enabled ||
                (!m_vxgiProbes.GetPendingPlan().HasWork() && m_vxgiProbes.GetStats().unclassified == 0);
            const bool passed = !IsWindowVisible(hwnd) && errors == 0 && ready && m_loadingProgress == 1.0f;
            std::ofstream report("tmp/Loading-startup-verification.json");
            report << nlohmann::json{{"passed", passed}, {"window_visible", bool(IsWindowVisible(hwnd))},
                {"loading_frames", m_loadingFenceValue}, {"progress", m_loadingProgress},
                {"d3d12_errors", errors}, {"d3d12_warnings", warnings}, {"probe_cache_ready", ready},
                {"probe_work", m_vxgiProbes.GetPendingPlan().count},
                {"probe_valid", m_vxgiProbes.GetStats().valid},
                {"probe_disabled", m_vxgiProbes.GetStats().disabled}}.dump(2);
            return passed;
        }
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
    const auto diagnosticsStart = std::chrono::steady_clock::now();

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
            auto sectionStart = std::chrono::steady_clock::now();
            if (!m_diagnosticsPrefix.empty() && m_diagnosticsSequence == "serial")
                for (int slot = 0; slot < frameBufferCount; ++slot) m_deviceContext.WaitForPreviousFrame(slot);
            WaitForPreviousFrame();
            if (m_diagnosticsGPUValidation)
            {
                m_validationDiagnostics.Collect("frame-fence-complete", m_diagnosticsFrame, RenderDebugWorldMode(m_debugPass));
                if (m_validationDiagnostics.GetErrorCount())
                {
                    Running = false;
                    break;
                }
            }
            const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sectionStart).count();
            if (m_frameDiagnostics.IsEnabled())
            {
                m_frameDiagnostics.Collect(frameIndex, m_deviceContext.GetFence(frameIndex));
                m_diagnosticsElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - diagnosticsStart).count();
                m_frameDiagnostics.BeginFrame(frameIndex, m_diagnosticsFrame++, RenderDebugWorldMode(m_debugPass), deltaTime * 1000.0);
                m_frameDiagnostics.CPU("FenceWait", waitMs);
                m_frameDiagnostics.Metadata("time_s", m_diagnosticsElapsed);
                m_frameDiagnostics.Metadata("slot", frameIndex);
                m_frameDiagnostics.Metadata("submitted", false);
                m_frameDiagnostics.Metadata("gpu_validation_requested", m_diagnosticsGPUValidation);
                m_frameDiagnostics.Metadata("mode_read", m_diagnosticsModeFromSRV ? "srv" : "cbv");
                m_frameDiagnostics.Metadata("sequence", m_diagnosticsSequence);
                m_frameDiagnostics.Metadata("phase", m_diagnosticsElapsed < 8 ? "pre" : m_diagnosticsElapsed < 10 ? "off_window" : "post");
                m_frameDiagnostics.Metadata("stage", m_diagnosticsElapsed < 8 ? "pre" : m_diagnosticsElapsed < 10 ? "off_window" : "post");
            }
            // Logic update
            sectionStart = std::chrono::steady_clock::now();
            Update();
            m_frameDiagnostics.CPU("Update", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sectionStart).count());
            // Graphics commands submission (generates drawcall)
            sectionStart = std::chrono::steady_clock::now();
            Render();
            m_frameDiagnostics.CPU("Render", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sectionStart).count());
            // GBV may spend the nominal duration patching the first frame's
            // shaders. Still exercise repeated submissions through all slots.
            if (!m_diagnosticsPrefix.empty() &&
                ((m_diagnosticsSequence == "validation-toggle" && m_diagnosticsFrame >= 36) ||
                 (m_diagnosticsSequence != "validation-toggle" && m_diagnosticsElapsed >= m_diagnosticsSeconds &&
                   (m_diagnosticsSequence != "debug-pass" || m_diagnosticsFrame >= 60) &&
                  (!m_diagnosticsGPUValidation || m_diagnosticsFrame >= 9)))) Running = false;
        }
    }
    if (m_frameDiagnostics.IsEnabled())
    {
        for (int slot = 0; slot < frameBufferCount; ++slot)
        {
            m_deviceContext.WaitForPreviousFrame(slot);
            m_frameDiagnostics.Collect(slot, m_deviceContext.GetFence(slot));
        }
        m_frameDiagnostics.Flush();
        if (!m_traceOutputDiagnostics.Flush())
            ErrorLog::Write(m_traceOutputDiagnostics.GetLastError().c_str());
    }
    if (m_diagnosticsGPUValidation)
        m_validationDiagnostics.Collect("run-gpu-drained", m_diagnosticsFrame, RenderDebugWorldMode(m_debugPass));
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
    if (m_diagnosticsGPUValidation && !m_validationDiagnostics.EnableBeforeDevice())
    {
        m_validationDiagnostics.Attach(nullptr, m_diagnosticsPrefix);
        ErrorLog::Write("Diagnostics: could not enable required D3D12 GPU-based validation before device creation.");
        return false;
    }
    // Bootstrap Hardware : Initialize DXGI infrastructure, Device, Command Queue, and Swap Chain
    const auto attachValidation = [this](ID3D12Device* device)
    {
        return !m_diagnosticsGPUValidation || m_validationDiagnostics.Attach(device, m_diagnosticsPrefix);
    };
    if (!m_deviceContext.Initialize(hwnd, Width, Height, frameBufferCount,
        m_diagnosticsGPUValidation ? std::function<bool(ID3D12Device*)>(attachValidation) :
            std::function<bool(ID3D12Device*)>{}))
    {
        ErrorLog::Write("Application: RenderDevice initialization failed.");
        return false;
    }
    if (m_diagnosticsGPUValidation)
    {
        m_deviceContext.GetCommandQueue()->SetName(L"Main.GraphicsQueue");
        m_deviceContext.GetCommandList()->SetName(L"Main.SceneCommandList");
        for (int slot = 0; slot < frameBufferCount; ++slot)
        {
            const auto suffix = std::to_wstring(slot);
            m_deviceContext.GetCommandAllocator(slot)->SetName((L"Main.SceneAllocator." + suffix).c_str());
            m_deviceContext.GetFence(slot)->SetName((L"Main.SceneFence." + suffix).c_str());
        }
        m_validationDiagnostics.Collect("device-initialized", 0, RenderDebugWorldMode(m_debugPass));
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
    if (m_settingsManager.pipeline.useDeferred && m_settingsManager.ssgi.enabled &&
        (!m_pipelineManager.InitializeSSGI(&m_deviceContext, m_diagnosticsModeFromSRV) ||
         !m_resourceManager.InitSSGIHistoryResources(&m_deviceContext, SceneWidth, SceneHeight,
             static_cast<UINT>(m_settingsManager.ssgi.traceDownsampleFactor))))
    {
        ErrorLog::Write("Application: SSGI pipeline initialization failed.");
        return false;
    }
    if (m_diagnosticsGPUValidation && m_pipelineManager.GetSSGITracePSO())
    {
        m_pipelineManager.GetSSGITracePSO()->SetName(m_diagnosticsModeFromSRV ? L"SSGI.Trace.SRV" : L"SSGI.Trace.CBV");
        m_pipelineManager.GetSSGIRootSignature()->SetName(L"SSGI.SharedRootSignature");
    }
    DirectX::XMStoreFloat4x4(&m_ssgiHistory.previousViewProj, DirectX::XMMatrixIdentity());
    m_ssgiHistory.previousInvViewProj = m_ssgiHistory.previousViewProj;
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

    if (!m_resourceManager.InitVXGI(&m_deviceContext, m_settingsManager.vxgi,
        m_settingsManager.lighting, frameBufferCount, stage(0.45f, 0.93f)))
    {
        ErrorLog::Write("Application: initial VXGI resource bake failed: " +
            m_resourceManager.GetVXGI().GetLastError());
        return false;
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

    if (m_settingsManager.pipeline.useDeferred &&
        !m_resourceManager.InitializeBlueNoiseTexture(&m_deviceContext))
    {
        ErrorLog::Write("Application: blue-noise texture initialization failed.");
        return false;
    }

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
    const bool allowCameraNavigation = m_diagnosticsPrefix.empty() && !m_editorGizmo.IsDragging() &&
        (ImGui::GetCurrentContext() == nullptr ||
            (!ImGui::GetIO().WantCaptureKeyboard && !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().AppFocusLost));
    m_inputManager.Update(deltaTime, camera, allowCameraNavigation);
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

    if (!m_diagnosticsPrefix.empty() && m_diagnosticsSequence == "no-jitter") jitterEnabled = false;
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

    ShadowPass::FrameData shadowFrame = ShadowPass::PrepareFrame(shadowInput);
    passCb.lightDir = shadowFrame.lightDir;
    passCb.tanSunAngularRadius = std::tan(
        XMConvertToRadians(m_settingsManager.lighting.sunAngularRadiusDegrees));
    passCb.cascadeSplits = shadowFrame.cascadeSplits;
    passCb.cascadeOrthoWidths = shadowFrame.cascadeOrthoWidths;
    passCb.cascadeDepthRanges = shadowFrame.cascadeDepthRanges;
    std::copy(
        shadowFrame.lightViewProj.begin(),
        shadowFrame.lightViewProj.end(),
        passCb.lightViewProj);

    passCb.iblPrefilterIdx = m_resourceManager.GetIblPrefilterIdx();
    passCb.iblBRDFIdx = m_resourceManager.GetIblBRDFIdx();
    memcpy(cbvAddress, &passCb, sizeof(PassConstants));
    // Use the parameters actually uploaded for this frame, including PCSS sun
    // radius. Screen sources still change when VXGI world building is disabled.
    uint64_t sourceStamp = 14695981039346656037ull;
    const auto appendSource = [&](const auto& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) sourceStamp = (sourceStamp ^ bytes[i]) * 1099511628211ull;
    };
    appendSource(passCb.lightDir);
    appendSource(passCb.lightColor);
    appendSource(passCb.tanSunAngularRadius);
    appendSource(passCb.materialMipBias);
    appendSource(m_resourceManager.GetMaterialRevision());
    m_ssgiScreenSourceStamp = sourceStamp;

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

    // Initial data is already baked. Scene edits invalidate only affected probes.
    const auto vxgiPrepareStart = std::chrono::steady_clock::now();
    if (m_settingsManager.vxgi.enabled &&
        !m_resourceManager.GetVXGI().Prepare(m_deviceContext, instances,
            m_resourceManager.GetGlobalMaterials(), m_settingsManager.vxgi, m_settingsManager.lighting))
    {
        ErrorLog::Write("VXGI: " + m_resourceManager.GetVXGI().GetLastError());
        Running = false;
        return;
    }
    m_frameDiagnostics.CPU("VXGI.Prepare", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vxgiPrepareStart).count());

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
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Editor.Draw", true);
    RecordImGuiDrawData();
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Editor.Draw", false);
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Frame", false);
    m_frameDiagnostics.Resolve(m_deviceContext.GetCommandList());

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
    bool ssgiHistoryWritten = false;
    bool ssgiHistoryWasValid = false;
    bool ssgiSourceReset = false;
    SSGIHybridState::Key ssgiSourceKey;
    bool temporalHistoryWritten = false;
    bool hbaoHistoryWritten = false;
    bool dlssHistoryWritten = false;
    bool viewHistoryReset = false;
    bool dlssResetRequested = false;
    // Normal composition: add SSGI to scene lighting, then render sky/transparency.
    const bool useZPrepass =
        m_settingsManager.pipeline.useZPrepass &&
        m_antiAliasingMode != AntiAliasingMode::TSR;
    const bool enablePostProcessSharpen =
        m_antiAliasingMode == AntiAliasingMode::TAA ||
        m_antiAliasingMode == AntiAliasingMode::TSR;
    const bool skipDLSS = !m_diagnosticsPrefix.empty() &&
        (m_diagnosticsSequence == "skip-dlss" ||
         (m_diagnosticsSequence == "pause-dlss" && m_diagnosticsElapsed >= 8.0 && m_diagnosticsElapsed < 10.0));
    if (m_diagnosticsPGODevice && m_diagnosticsSequence == "pgo-discard" &&
        !m_diagnosticsPGODiscarded && m_diagnosticsElapsed >= 8.0)
    {
        m_diagnosticsPGODiscarded = true;
        const HRESULT result = m_diagnosticsPGODevice->SetBackgroundProcessingMode(
            D3D12_BACKGROUND_PROCESSING_MODE_ALLOWED, D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS, nullptr, nullptr);
        m_frameDiagnostics.Metadata("pgo_discard_hr", static_cast<uint32_t>(result));
        m_frameDiagnostics.Metadata("pgo_discard_at_s", m_diagnosticsElapsed);
        if (FAILED(result))
        {
            ReportFrameError("Diagnostics: background measurement discard failed.", result);
            Running = false;
            return;
        }
        m_diagnosticsPGOOverride = true;
    }
    if (!BeginFrame())
    {
        // The command infrastructure is unusable this frame (e.g. the device was removed)
        return;
    }
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Frame", true);
    auto diagnosticsSectionStart = std::chrono::steady_clock::now();

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

    if (!m_diagnosticsPrefix.empty())
    {
        // Hidden acceptance changes the same viewport selector as the menu,
        // without sending OS input or introducing another interactive shortcut.
        io.AddFocusEvent(true);
        if (m_diagnosticsSequence == "debug-pass")
        {
            m_debugPass = m_diagnosticsFrame >= 12 && m_diagnosticsFrame < 24 ? RenderDebugPass::VXGIOff :
                m_diagnosticsFrame >= 24 && m_diagnosticsFrame < 36 ? RenderDebugPass::SSGIOnly :
                m_diagnosticsFrame >= 36 && m_diagnosticsFrame < 48 ? RenderDebugPass::GIOnly : RenderDebugPass::Default;
        }
        const bool toggle = m_diagnosticsSequence == "toggle" || m_diagnosticsSequence == "immediate" ||
            m_diagnosticsSequence == "capture-toggle";
        const double first = m_diagnosticsSequence == "immediate" ? 0.05 : 8.0;
        const double second = m_diagnosticsSequence == "immediate" ? 0.10 : 10.0;
        const bool validationToggle = m_diagnosticsSequence == "validation-toggle" &&
            ((m_diagnosticsStage == 0 && m_diagnosticsFrame > 12) ||
             (m_diagnosticsStage == 1 && m_diagnosticsFrame > 24));
        if (validationToggle || (toggle && ((m_diagnosticsStage == 0 && m_diagnosticsElapsed >= first) ||
            (m_diagnosticsStage == 1 && m_diagnosticsElapsed >= second))))
        {
            m_debugPass = m_diagnosticsStage == 0 ? RenderDebugPass::SSGIOnly : RenderDebugPass::Default;
            ++m_diagnosticsStage;
        }
        if (m_diagnosticsSequence == "reset" && m_diagnosticsStage == 0 && m_diagnosticsElapsed >= 8.0)
        {
            m_ssgiHistory.valid = false;
            m_diagnosticsStage = 1;
        }
    }

    ImGui::NewFrame();

    m_editorSelectionRenderer.Poll(m_deviceContext, m_resourceManager, m_editorSelection, m_editorHistory);
    const EditorGizmo::CameraFrame gizmoCamera{ m_currUnjitteredViewProjGpu, m_currViewGpu, m_currUnjitteredProjGpu };
    // Match the unjittered projection used above: live camera.Zoom, near .1, far 1000.
    const auto cameraSnapshot = EditorCameraSnapshot::Capture(camera,
        static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), 0.1f, 1000.0f);
    const bool quitRequested = EditorUI::Draw(m_resourceManager, m_editorSelection, m_editorGizmo,
        gizmoCamera, cameraSnapshot, m_editorHistory, m_vxgiWorld, m_vxgiMeshSDFScene, m_vxgiSurface, m_vxgiProbes,
        m_vxgiProbePass.GetGPUMilliseconds(), m_vxgiProbePass.GetGPUMemoryBytes(),
        m_vxgiProbeFilterPass.GetGPUMilliseconds(), m_vxgiProbeFilterPass.GetGPUMemoryBytes(), m_settingsManager.vxgi,
        m_debugPass, m_settingsManager.pipeline.useDeferred && m_settingsManager.ssgi.enabled);
    const bool giAvailable = m_settingsManager.pipeline.useDeferred && m_settingsManager.ssgi.enabled;
    if (!giAvailable) m_debugPass = RenderDebugPass::Default;
    // Resolve after the menu so a click affects this same frame. The editor's
    // output selector never changes persistent quality settings.
    const bool showGIOnly = giAvailable && IsGIOnlyDebugPass(m_debugPass);
    SSGIConfig frameSSGI = m_settingsManager.ssgi;
    frameSSGI.giOnly = showGIOnly;
    if (m_debugPass != m_lastRenderedDebugPass)
    {
        // Color histories contain the composed viewport, unlike the GI signal's
        // own history, which already tracks screen/world source changes.
        m_temporalHistoryValid = false;
        m_dlssHistoryValid = false;
        viewHistoryReset = true;
    }
    m_frameDiagnostics.CPU("Editor.Build", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagnosticsSectionStart).count());

    diagnosticsSectionStart = std::chrono::steady_clock::now();
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "VXGI.Record", true);
    if (m_settingsManager.vxgi.enabled &&
        !m_resourceManager.GetVXGI().Record(m_deviceContext, frameIndex,
            m_resourceManager.GetMainDescriptorHeap(), m_resourceManager.GetMaterialBuffer(), m_settingsManager.vxgi))
    {
        ErrorLog::Write("VXGI: probe GPU update failed; frame was not submitted.");
        Running = false;
        EndFrame();
        return;
    }
    m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "VXGI.Record", false);
    m_frameDiagnostics.CPU("VXGI.Record", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagnosticsSectionStart).count());
    const auto diagnosticsGraphStart = std::chrono::steady_clock::now();
    const auto executeGraph = [this, diagnosticsGraphStart](RDGBuilder& graph)
    {
        if (m_frameDiagnostics.IsEnabled())
        {
            m_frameDiagnostics.CPU("Graph.Build", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagnosticsGraphStart).count());
            graph.SetPassExecutionObserver([this, start = std::chrono::steady_clock::time_point{}]
                (ID3D12GraphicsCommandList* cmd, const char* name, bool begin) mutable {
                m_frameDiagnostics.GPU(cmd, name, begin);
                if (begin) start = std::chrono::steady_clock::now();
                else m_frameDiagnostics.CPU(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            });
        }
        const auto start = std::chrono::steady_clock::now();
        m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Graph", true);
        graph.Execute(m_deviceContext.GetCommandList());
        m_frameDiagnostics.GPU(m_deviceContext.GetCommandList(), "Graph", false);
        m_frameDiagnostics.CPU("Graph.Execute", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        if (m_frameDiagnostics.IsEnabled())
        {
            const auto& stats = graph.GetCompileStats();
            m_frameDiagnostics.Metadata("rdg", {{"passes", stats.compiledPassCount}, {"textures", stats.textureCount},
                {"buffers", stats.bufferCount}, {"barriers", stats.passBarrierCount + stats.finalBarrierCount}});
        }
    };

    if (m_antiAliasingMode == AntiAliasingMode::DLSS)
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

    if (!m_settingsManager.pipeline.useDeferred)
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
            else if (!skipDLSS)
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

        executeGraph(forwardGraph);
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
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "HBAO.DisabledWhite");
            m_hbaoHistoryValid = false;
        }

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
            deferredInput,
            m_settingsManager.ssgi.enabled);
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

        if (m_settingsManager.ssgi.enabled)
        {
            SSGIPass::WorldInput worldInput;
            if (m_traceOutputDiagnostics.IsEnabled())
                worldInput.traceOutput = [this](ID3D12GraphicsCommandList* cmd, ID3D12Resource* raw, uint32_t frame, uint32_t mode) {
                    const auto before = m_traceOutputDiagnostics.GetCaptureCount();
                    if (!m_traceOutputDiagnostics.Capture(cmd, raw, frame, m_diagnosticsElapsed, mode))
                        Running = false;
                    if (before != m_traceOutputDiagnostics.GetCaptureCount())
                        m_frameDiagnostics.Metadata("rawgi_capture", before);
                };
            if (m_frameDiagnostics.IsEnabled())
                worldInput.traceExecution = [this](ID3D12GraphicsCommandList* cmd, bool begin) {
                    m_frameDiagnostics.GPU(cmd, "SSGI.Trace.Dispatch", begin);
                };
            if (m_frameDiagnostics.IsEnabled())
                worldInput.traceResources = [this](std::span<ID3D12Resource* const> resources) {
                    nlohmann::json pointers = nlohmann::json::array();
                    for (auto* resource : resources) pointers.push_back(reinterpret_cast<uintptr_t>(resource));
                    m_frameDiagnostics.Metadata("trace_textures", std::move(pointers));
                };
            if (m_frameDiagnostics.IsEnabled())
                worldInput.traceDiagnostics = [this](SSGITraceConstants& trace, D3D12_GPU_VIRTUAL_ADDRESS cbv,
                    std::span<const D3D12_GPU_VIRTUAL_ADDRESS> addresses) {
                    if (m_diagnosticsSequence == "pulse" && m_diagnosticsElapsed >= 8 && m_diagnosticsElapsed < 10)
                        trace.hybrid.worldMode = 0;
                    if (m_diagnosticsSequence == "alias" && m_diagnosticsElapsed >= 8 && m_diagnosticsElapsed < 10)
                        trace.hybrid.worldMode = 1;
                    if (m_diagnosticsSequence == "one" && trace.hybrid.worldMode != 0)
                        trace.hybrid.worldMode = 1;
                    if (m_diagnosticsSequence == "prime" && m_ssgiFrameIndex < 2)
                        trace.hybrid.worldMode = 0;
                    if (m_diagnosticsSequence == "clearstate") trace.padding2 = 1;
                    if (m_diagnosticsSequence == "barrier") trace.padding2 = 2;
                    nlohmann::json roots = nlohmann::json::array();
                    for (const auto address : addresses) roots.push_back(address);
                    m_frameDiagnostics.Metadata("trace_roots", std::move(roots));
                    m_frameDiagnostics.Metadata("trace_constants", {{"cbv", cbv}, {"mode", trace.hybrid.worldMode},
                        {"rays", trace.rayCount}, {"iterations", trace.maxIterations}, {"radius", trace.radius},
                        {"dimensions", {trace.width, trace.height, trace.outputWidth, trace.outputHeight}},
                        {"descriptors", {trace.depthIndex, trace.normalIndex, trace.ormIndex, trace.sourceIndex,
                            trace.outputIndex, trace.blueNoiseIndex, trace.albedoIndex}},
                        {"world_counts", {trace.hybrid.nodeCount, trace.hybrid.assetCount, trace.hybrid.instanceCount,
                            trace.hybrid.bvhNodeCount, trace.hybrid.probeCount}},
                        {"voxel_size", trace.hybrid.voxelSize}, {"cell_size", trace.hybrid.cellSize}});
                };
            if (m_settingsManager.vxgi.enabled)
            {
                worldInput.scene = &m_vxgiGPUScene;
                worldInput.surface = &m_vxgiSurface;
                worldInput.rawRecords = m_vxgiProbePass.GetDisplayHistory();
                worldInput.materialBuffer = m_resourceManager.GetMaterialBuffer();
                worldInput.probeCount = static_cast<uint32_t>(m_vxgiWorld.GetOccupiedCenters().size());
                worldInput.voxelSize = m_vxgiWorld.GetVoxelSize();
                worldInput.mode = RenderDebugWorldMode(m_debugPass);
            }
            ssgiSourceKey.effectiveWorldMode = SSGIPass::EffectiveWorldMode(&worldInput, frameIndex);
            if (ssgiSourceKey.effectiveWorldMode != m_lastRenderedGIWorldMode)
            {
                m_temporalHistoryValid = false;
                m_dlssHistoryValid = false;
                viewHistoryReset = true;
            }
            ssgiSourceKey.screenSourceStamp = m_ssgiScreenSourceStamp;
            ssgiSourceKey.screenRadius = m_settingsManager.ssgi.radius;
            ssgiSourceKey.quality = static_cast<uint32_t>(m_settingsManager.ssgi.quality);
            ssgiSourceKey.traceDownsampleFactor = static_cast<uint32_t>(m_settingsManager.ssgi.traceDownsampleFactor);
            ssgiSourceKey.worldGeometryRevision = ssgiSourceKey.effectiveWorldMode ? m_vxgiWorld.GetGeometryRevision() : 0;
            ssgiSourceKey.sdfGeometryRevision = m_vxgiMeshSDFScene.GetRevision();
            ssgiSourceKey.surfaceRevision = ssgiSourceKey.effectiveWorldMode ? m_vxgiSurface.GetRevision() : 0;
            // Prepare exposes the reset epoch used by the current recorded RAW buffer.
            // Batch cursor/frame changes intentionally do not invalidate screen history.
            ssgiSourceKey.probeResetEpoch = m_vxgiProbes.GetPendingPlan().epoch;
            ssgiHistoryWasValid = m_ssgiHistory.valid;
            ssgiSourceReset = m_ssgiSources.NeedsReset(ssgiSourceKey);
            if (ssgiSourceReset) m_ssgiHistory.valid = false;
            SSGIPass::Input ssgiInput = { deferredOutput.sceneColor, gbufferOutput.depth,
                gbufferOutput.normal, gbufferOutput.orm, gbufferOutput.albedo, deferredOutput.bounceSource };
            auto ssgiOutput = SSGIPass::AddToGraph(deferredGraph, &m_resourceManager,
                &m_pipelineManager, m_ssgiHistory, m_currViewGpu, m_currJitteredProjGpu,
                m_currJitteredInvProjGpu, m_currJitteredInvViewProjGpu,
                SceneWidth, SceneHeight, frameIndex, m_ssgiFrameIndex, frameSSGI, ssgiInput, &worldInput);
            if (!ssgiOutput.sceneColor.IsValid() || !ssgiOutput.pass.IsValid())
            {
                ErrorLog::Write("SSGI: RDG construction failed; rendering stopped.");
                Running = false;
                EndFrame();
                return;
            }
            sceneColorProducer = ssgiOutput.pass;
            ssgiHistoryWritten = true;
        }

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
        }

        RDGTextureHandle deferredFinalColor = deferredOutput.sceneColor;
        RDGPassHandle deferredFinalColorProducer = sceneColorProducer;
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
                    { deferredOutput.sceneColor, gbufferOutput.depth, motionOutput.motionTexture });

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
                    { sceneColorProducer, motionOutput.pass });
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
                    { deferredOutput.sceneColor, gbufferOutput.depth, motionOutput.motionTexture });

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
                    { sceneColorProducer, motionOutput.pass });
                deferredFinalColor = tsrOutput.historyTexture;
                deferredFinalColorProducer = tsrOutput.pass;
                temporalHistoryWrittenByDeferredGraph = true;
            }
            else if (!skipDLSS)
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
                    { deferredOutput.sceneColor, gbufferOutput.depth, motionOutput.motionTexture },
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
                    { sceneColorProducer, motionOutput.pass });
                deferredFinalColor = dlssOutput.outputTexture;
                deferredFinalColorProducer = dlssOutput.pass;
                dlssEvaluatedByDeferredGraph = true;
            }
        }

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
        executeGraph(deferredGraph);
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

    if (!showGIOnly && m_settingsManager.vxgi.enabled && m_settingsManager.vxgi.debugView >= 1 &&
        m_settingsManager.vxgi.debugView <= 3 &&
        !m_vxgiDebugRenderer.Record(m_deviceContext, m_resourceManager.GetMainDescriptorHeap(),
            frameIndex, m_currUnjitteredViewProjGpu, m_imguiRtvHandles[frameIndex],
            m_vxgiWorld.GetOccupiedCenters(), m_vxgiWorld.GetVoxelSize(), m_vxgiWorld.GetRevision(),
            m_settingsManager.vxgi.debugView >= 2,
            m_settingsManager.vxgi.debugView == 1 || m_settingsManager.vxgi.debugView == 3))
    {
        ErrorLog::Write("VXGI: debug drawing failed; frame was not submitted.");
        Running = false;
        EndFrame();
        return;
    }

    if (!showGIOnly && m_settingsManager.vxgi.enabled && ((m_settingsManager.vxgi.debugView >= 4 && m_settingsManager.vxgi.debugView <= 12) ||
        (m_settingsManager.vxgi.debugView >= 18 && m_settingsManager.vxgi.debugView <= 20)))
    {
        XMFLOAT3 sunTowardLight;
        XMStoreFloat3(&sunTowardLight, -XMVector3Normalize(XMLoadFloat3(&m_settingsManager.lighting.lightDir)));
        if (!m_vxgiSDFDebugRenderer.Record(m_deviceContext, m_resourceManager.GetMainDescriptorHeap(),
            frameIndex, m_currUnjitteredInvViewProjGpu, m_imguiRtvHandles[frameIndex], m_vxgiSDF, m_vxgiGPUScene,
            m_settingsManager.vxgi.debugView, m_settingsManager.vxgi.sdfSliceAxis,
            m_settingsManager.vxgi.sdfSlicePosition, sunTowardLight, &m_vxgiSurface,
            m_resourceManager.GetMaterialBufferGPUAddress(), m_vxgiWorld.GetVoxelSize(),
            static_cast<uint32_t>(m_vxgiWorld.GetOccupiedCenters().size()), m_vxgiProbePass.GetDisplayHistory()))
        {
            ErrorLog::Write("VXGI: SDF debug drawing failed; frame was not submitted.");
            Running = false;
            EndFrame();
            return;
        }
    }

    if (!showGIOnly && m_settingsManager.vxgi.enabled && m_settingsManager.vxgi.debugView >= 13 && m_settingsManager.vxgi.debugView <= 17 &&
        !m_vxgiProbeDebugRenderer.Record(m_deviceContext, m_resourceManager.GetMainDescriptorHeap(), frameIndex,
            m_currUnjitteredViewProjGpu, m_imguiRtvHandles[frameIndex], m_vxgiGPUScene,
            m_settingsManager.vxgi.debugView == 16 ? m_vxgiProbeFilterPass.GetFilteredRecords() : m_vxgiProbePass.GetDisplayHistory(),
            m_vxgiWorld.GetVoxelSize(),
            static_cast<uint32_t>(m_vxgiWorld.GetOccupiedCenters().size()),
            static_cast<uint32_t>(m_settingsManager.vxgi.debugView), m_vxgiProbePass.GetDisplayFrame(),
            m_vxgiProbeFilterPass.GetVisibilityMasks()))
    {
        ErrorLog::Write("VXGI: probe diagnostic drawing failed; frame was not submitted.");
        Running = false;
        EndFrame();
        return;
    }

    m_editorSelectionRenderer.Record(m_deviceContext, m_resourceManager, m_editorSelection,
        frameIndex, m_currUnjitteredViewProjGpu, m_imguiRtvHandles[frameIndex], io.DisplaySize.x, io.DisplaySize.y);

    if (!EndFrame())
    {
        if (m_traceOutputDiagnostics.IsEnabled()) m_traceOutputDiagnostics.SubmissionFailed("closing frame failed");
        // The command list could not be finalized; skip submission and presentation
        return;
    }

    ID3D12CommandList* lists[] = { m_deviceContext.GetCommandList() };
    diagnosticsSectionStart = std::chrono::steady_clock::now();
    // Submit recorded rendering commands to the GPU for execution
    m_deviceContext.GetCommandQueue()->ExecuteCommandLists(1, lists);
    // Insert a signal into the queue to track GPU progress
    HRESULT hr = m_deviceContext.GetCommandQueue()->Signal(
        m_deviceContext.GetFence(frameIndex),
        ++m_deviceContext.GetFenceValue(frameIndex));
    if (FAILED(hr))
    {
        ReportFrameError("Application: failed to signal the frame fence.", hr);
        if (m_traceOutputDiagnostics.IsEnabled()) m_traceOutputDiagnostics.SubmissionFailed("frame fence signal failed");
    }
    else
    {
        m_frameDiagnostics.Submitted(frameIndex, m_deviceContext.GetFenceValue(frameIndex));
        m_frameDiagnostics.Metadata("submitted", true);
        CommitTemporalHistories(temporalHistoryWritten, hbaoHistoryWritten, dlssHistoryWritten, ssgiHistoryWritten);
        m_lastRenderedDebugPass = m_debugPass;
        m_lastRenderedGIWorldMode = ssgiHistoryWritten ? ssgiSourceKey.effectiveWorldMode : 0;
        if (ssgiHistoryWritten)
        {
            m_ssgiSources.Commit(ssgiSourceKey);
            ++m_ssgiFrameIndex;
        }
        if (m_settingsManager.vxgi.enabled &&
            !m_resourceManager.GetVXGI().OnSubmitted(frameIndex, m_deviceContext.GetFenceValue(frameIndex)))
            ReportFrameError("VXGI: failed to commit the submitted probe batch.", E_FAIL);
        if (ssgiHistoryWritten)
            LogSubmittedGIState(m_ssgiFrameIndex - 1, frameIndex, deltaTime,
                RenderDebugWorldMode(m_debugPass), ssgiSourceKey.effectiveWorldMode,
                ssgiHistoryWasValid, ssgiSourceReset, m_vxgiProbes, m_vxgiProbePass,
                m_vxgiProbeFilterPass, m_vxgiGPUScene);
        m_editorSelectionRenderer.OnSubmitted(frameIndex, m_deviceContext.GetFenceValue(frameIndex));
    }
    m_frameDiagnostics.CPU("Submit.Commit", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagnosticsSectionStart).count());

    // Flip the back buffer to the front screen
    diagnosticsSectionStart = std::chrono::steady_clock::now();
    hr = m_deviceContext.GetSwapChain()->Present(0, 0);
    m_frameDiagnostics.CPU("Present", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagnosticsSectionStart).count());
    if (m_diagnosticsPGODevice && m_diagnosticsSequence == "pgo")
    {
        const auto queryStart = std::chrono::steady_clock::now();
        BOOL wantMoreFrames = TRUE;
        const HRESULT result = m_diagnosticsPGODevice->SetBackgroundProcessingMode(
            D3D12_BACKGROUND_PROCESSING_MODE_ALLOW_INTRUSIVE_MEASUREMENTS,
            D3D12_MEASUREMENTS_ACTION_KEEP_ALL, nullptr, &wantMoreFrames);
        m_frameDiagnostics.CPU("PGO.Query", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - queryStart).count());
        m_frameDiagnostics.Metadata("pgo_query_hr", static_cast<uint32_t>(result));
        if (SUCCEEDED(result)) m_frameDiagnostics.Metadata("pgo_more", bool(wantMoreFrames));
        else
        {
            ReportFrameError("Diagnostics: background measurement query failed.", result);
            Running = false;
        }
    }
    if (m_frameDiagnostics.IsEnabled())
    {
        const auto& plan = m_vxgiProbes.GetPendingPlan();
        if (m_diagnosticsPGODevice)
        {
            m_frameDiagnostics.Metadata("pgo_background_supported", m_diagnosticsBackgroundSupport);
            m_frameDiagnostics.Metadata("pgo_discarded", m_diagnosticsPGODiscarded);
        }
        m_frameDiagnostics.Metadata("actual_mode", RenderDebugWorldMode(m_debugPass));
        m_frameDiagnostics.Metadata("gi_only", showGIOnly);
        m_frameDiagnostics.Metadata("debug_pass", static_cast<int>(m_debugPass));
        m_frameDiagnostics.Metadata("view_history_reset", viewHistoryReset);
        m_frameDiagnostics.Metadata("dlss_reset", dlssResetRequested);
        m_frameDiagnostics.Metadata("dlss_evaluated", dlssHistoryWritten);
        m_frameDiagnostics.Metadata("validation_stage", m_diagnosticsStage);
        m_frameDiagnostics.Metadata("dlss_evaluated", dlssHistoryWritten);
        m_frameDiagnostics.Metadata("effective_mode", ssgiSourceKey.effectiveWorldMode);
        m_frameDiagnostics.Metadata("present_hr", static_cast<uint32_t>(hr));
        m_frameDiagnostics.Metadata("history_valid_before", ssgiHistoryWasValid);
        m_frameDiagnostics.Metadata("history_reset", ssgiSourceReset);
        m_frameDiagnostics.Metadata("ssgi_frame", m_ssgiFrameIndex);
        m_frameDiagnostics.Metadata("dlss_frame", m_dlssJitterFrameIndex);
        m_frameDiagnostics.Metadata("dlss_history", m_dlssHistoryValid);
        m_frameDiagnostics.Metadata("scene_size", {SceneWidth, SceneHeight, Width, Height});
        m_frameDiagnostics.Metadata("ssgi_config", {m_settingsManager.ssgi.quality, m_settingsManager.ssgi.traceDownsampleFactor, m_settingsManager.ssgi.radius});
        m_frameDiagnostics.Metadata("source", {ssgiSourceKey.worldGeometryRevision, ssgiSourceKey.sdfGeometryRevision,
            ssgiSourceKey.surfaceRevision, ssgiSourceKey.probeResetEpoch, ssgiSourceKey.screenSourceStamp});
        m_frameDiagnostics.Metadata("probe_work", plan.count);
        m_frameDiagnostics.Metadata("probe_epoch", plan.epoch);
        m_frameDiagnostics.Metadata("probe_batches", m_vxgiProbePass.GetActivityStats().activeBatches);
        m_frameDiagnostics.Metadata("raw_address", m_vxgiProbePass.GetDisplayHistory() ? m_vxgiProbePass.GetDisplayHistory()->GetGPUVirtualAddress() : 0);
        m_frameDiagnostics.Metadata("visible_instances", m_visibleInstanceCount);
        m_frameDiagnostics.Metadata("debug_view", m_settingsManager.vxgi.debugView);
        m_frameDiagnostics.Metadata("imgui_counts", {ImGui::GetDrawData()->TotalVtxCount, ImGui::GetDrawData()->TotalIdxCount});
    }
    if (FAILED(hr))
    {
        ReportFrameError("Application: swap-chain Present failed.", hr);
    }

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
    if (SUCCEEDED(hr) && m_diagnosticsGPUValidation)
    {
        m_loadingCommandList->SetName(L"Loading.CommandList");
        m_loadingAllocator->SetName(L"Loading.Allocator");
        m_loadingFence->SetName(L"Loading.Fence");
    }
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
    // Only the rapidly changing probe counters share a throttled stage key.
    const std::string stageKey = status.rfind("Baking VXGI probe cache:", 0) == 0 ?
        "Baking VXGI probe cache" : status;
    if (stageKey == m_loadingStageKey && m_loadingLastDrawTick != 0 &&
        tick - m_loadingLastDrawTick < 50 && m_loadingProgress < 1.0f) return;
    const auto check = [](HRESULT hr, const char* operation) {
        if (FAILED(hr)) { ErrorLog::HRESULT(operation, hr); throw InitializationStopped{}; }
    };
    if (!WaitForLoadingFrame()) throw InitializationStopped{};
    if (m_diagnosticsGPUValidation)
    {
        m_validationDiagnostics.Collect("loading: " + status, 0, RenderDebugWorldMode(m_debugPass));
        if (m_validationDiagnostics.GetErrorCount()) throw InitializationStopped{};
    }
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
    if (m_loadingHiddenVerification)
    {
        static std::ofstream trace("tmp/Loading-startup-progress.jsonl");
        trace << nlohmann::json{{"progress", m_loadingProgress}, {"status", status}}.dump() << '\n';
        trace.flush();
    }
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
    if (m_loadingHiddenVerification) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

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
