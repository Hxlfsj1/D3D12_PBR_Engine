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

using namespace DirectX;
// std::shared_ptr allocates an external reference counter, whereas ComPtr uses the internal counter of the COM object itself
using Microsoft::WRL::ComPtr;

static std::vector<ModelInstance*> g_visibleInstances;
static std::array<std::vector<ModelInstance*>, NUM_CASCADES> g_shadowVisibleInstancesByCascade;
static std::array<size_t, NUM_CASCADES> g_shadowInstanceOffsets;

// Global hook for Win32 message routing
D3D12App* g_App = nullptr;

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
    // Disable system DPI scaling to unlock raw GPU performance
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    D3D12App app(hInstance);
    g_App = &app;

    if (!app.Initialize(nShowCmd))
    {
        ErrorLog::Write("Application: initialization failed; WinMain is returning failure.");
        return 1;
    }

    app.Run();

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
        WaitForPreviousFrame();
        m_dlssManager.Shutdown();
        // The GPU is idle here; ImGui's device objects can be released safely
        ShutdownImGui();
    }
}

static std::wstring g_wWindowTitle;
bool D3D12App::Initialize(int nShowCmd)
{
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
    if (!InitD3D())
    {
        ErrorLog::Write("Application: Direct3D initialization failed.");
        return false;
    }

    m_resourceManager.FreeUploadHeaps();

    // ImGui rides on the fully initialized device and swap chain; it owns no engine resources
    if (!InitImGui())
    {
        ErrorLog::Write("Application: ImGui initialization failed.");
        return false;
    }

    if (!m_editorSelectionRenderer.Initialize(m_deviceContext, frameBufferCount))
    {
        ErrorLog::Write("Application: editor selection initialization failed.");
        return false;
    }

    return true;
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
        }
    }
}

// Handle user input
LRESULT D3D12App::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_RBUTTONUP || msg == WM_KILLFOCUS || msg == WM_CAPTURECHANGED)
        m_inputManager.EndMouseLook();
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

    // Compile Pipeline States: Precompute Root Signatures and PSOs for both Graphics and Compute pipelines
    if (!m_pipelineManager.Initialize(&m_deviceContext))
    {
        ErrorLog::Write("Application: main pipeline initialization failed.");
        return false;
    }
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

    // Stream Assets & Build IBL: Load 3D models and HDR textures into VRAM and bake IBL components
    if (!m_resourceManager.LoadAssets(&m_deviceContext, SettingsManager::LoadSceneFromJson("Settings/Scene.json"), frameBufferCount))
    {
        ErrorLog::Write("Application: resource loading failed.");
        return false;
    }
    m_resourceManager.BuildGlobalMaterialPool(&m_deviceContext);
    currentHDRPath = SettingsManager::GetSkyboxPathFromJson();
    if (!m_resourceManager.InitIBL(&m_deviceContext, currentHDRPath.c_str()))
    {
        ErrorLog::Write("Application: image-based-lighting initialization failed.");
        return false;
    }

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

    m_resourceManager.SealPersistentSrvUavDescriptors();

    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Width, (float)Height);
    scissorRect = CD3DX12_RECT(0, 0, Width, Height);
    sceneViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)SceneWidth, (float)SceneHeight);
    sceneScissorRect = CD3DX12_RECT(0, 0, SceneWidth, SceneHeight);

    return true;
}

// Data is streamed directly from the Upload Heap to the GPU, utilizing a Ring Buffer mechanism (with a count of 3 to align with the Triple Buffering scheme)
void D3D12App::Update()
{
    auto& instances = m_resourceManager.GetSceneInstances();

    // UI edits mark transforms dirty after the preceding frame's instance upload.
    // Rebuild matrices/culling below as one coherent update, and discard temporal
    // history because the current motion vectors only describe camera motion.
    if (std::any_of(instances.begin(), instances.end(), [](const ModelInstance& instance) { return instance.isDirty; }))
    {
        m_temporalHistoryValid = false;
        m_hbaoHistoryValid = false;
        m_dlssHistoryValid = false;
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
            L"    |    Frustum: " + std::to_wstring(m_frustumInstanceCount) +
            L"    |    Hz: 300";

        SetWindowText(hwnd, windowText.c_str());

        frameCount = 0;
        timeElapsed -= 1.0f;
    }

    // Continuous keyboard movement must update the camera before building frame matrices and culling volumes.
    if (!m_editorGizmo.IsDragging() &&
        (ImGui::GetCurrentContext() == nullptr || !ImGui::GetIO().WantCaptureKeyboard))
        m_inputManager.Update(deltaTime, camera);

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

    ShadowPass::FramePreparationInput shadowInput = {};
    shadowInput.camera = &camera;
    shadowInput.lightDir = m_settingsManager.lighting.lightDir;
    shadowInput.aspectRatio = static_cast<float>(Width) / Height;

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
    EditorUI::UpdateScale(static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);

    ImGui::NewFrame();
    m_editorSelectionRenderer.Poll(m_deviceContext, m_resourceManager, m_editorSelection, m_editorHistory);
    const EditorGizmo::CameraFrame gizmoCamera{ m_currUnjitteredViewProjGpu, m_currViewGpu, m_currUnjitteredProjGpu };
    const bool quitRequested = EditorUI::Draw(m_resourceManager, m_editorSelection, m_editorGizmo, gizmoCamera, m_editorHistory);

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
            else
            {
                DLSSPass::Output dlssOutput = DLSSPass::AddToGraph(
                    forwardGraph,
                    &m_dlssManager,
                    &m_resourceManager,
                    m_currJitterPixelX,
                    m_currJitterPixelY,
                    (std::max)(deltaTime * 1000.0f, 0.0f),
                    !m_dlssHistoryValid,
                    { opaqueOutput.sceneColor, opaqueOutput.depth, motionOutput.motionTexture },
                    [this](ID3D12GraphicsCommandList* commandList)
                    {
                        commandList->SetGraphicsRootSignature(
                            m_pipelineManager.GetPostProcessRootSignature());
                        commandList->SetPipelineState(
                            m_pipelineManager.GetPostProcessPSO(false));
                        commandList->RSSetViewports(1, &viewport);
                        commandList->RSSetScissorRects(1, &scissorRect);
                        commandList->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                        ID3D12DescriptorHeap* heaps[] =
                        {
                            m_resourceManager.GetMainDescriptorHeap()
                        };
                        commandList->SetDescriptorHeaps(1, heaps);
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
                    false,
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
                false,
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

            m_dlssHistoryValid = true;
        }

        if (temporalHistoryWrittenByForwardGraph)
        {
            m_resourceManager.FlipTemporalHistoryIndex();
            m_temporalHistoryValid = true;
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

        RDGTextureHandle hbaoForLighting = hbaoOutput.blurredTexture;
        RDGPassHandle hbaoProducer = hbaoOutput.blurPass;
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

        {
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
            deferredGraph.AddPassDependencies(deferredOutput.pass, { shadowOutput.pass, hbaoProducer });
            RDGPassHandle sceneColorProducer = deferredOutput.pass;

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
                else
                {
                    DLSSPass::Output dlssOutput = DLSSPass::AddToGraph(
                        deferredGraph,
                        &m_dlssManager,
                        &m_resourceManager,
                        m_currJitterPixelX,
                        m_currJitterPixelY,
                        (std::max)(deltaTime * 1000.0f, 0.0f),
                        !m_dlssHistoryValid,
                        { deferredOutput.sceneColor, gbufferOutput.depth, motionOutput.motionTexture },
                        [this](ID3D12GraphicsCommandList* commandList)
                        {
                            commandList->SetGraphicsRootSignature(
                                m_pipelineManager.GetPostProcessRootSignature());
                            commandList->SetPipelineState(
                                m_pipelineManager.GetPostProcessPSO(false));
                            commandList->RSSetViewports(1, &viewport);
                            commandList->RSSetScissorRects(1, &scissorRect);
                            commandList->IASetPrimitiveTopology(
                                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                            ID3D12DescriptorHeap* heaps[] =
                            {
                                m_resourceManager.GetMainDescriptorHeap()
                            };
                            commandList->SetDescriptorHeaps(1, heaps);
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
                        false,
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
                    false,
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
        }

        // Execute the entire graph after all passes have been added
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

            m_dlssHistoryValid = true;
        }

        if (hbaoTemporalHandledByDeferredGraph)
        {
            m_resourceManager.FlipHBAOHistoryIndex();
            m_hbaoHistoryValid = true;
        }

        if (temporalHistoryWrittenByDeferredGraph)
        {
            m_resourceManager.FlipTemporalHistoryIndex();
            m_temporalHistoryValid = true;
        }
    }

    m_editorSelectionRenderer.Record(m_deviceContext, m_resourceManager, m_editorSelection,
        frameIndex, m_currUnjitteredViewProjGpu, m_imguiRtvHandles[frameIndex], io.DisplaySize.x, io.DisplaySize.y);

    if (!EndFrame())
    {
        // The command list could not be finalized; skip submission and presentation
        return;
    }

    ID3D12CommandList* lists[] = { m_deviceContext.GetCommandList() };
    // Submit recorded rendering commands to the GPU for execution
    m_deviceContext.GetCommandQueue()->ExecuteCommandLists(1, lists);
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
        m_editorSelectionRenderer.OnSubmitted(frameIndex, m_deviceContext.GetFenceValue(frameIndex));
    }

    // Flip the back buffer to the front screen
    hr = m_deviceContext.GetSwapChain()->Present(0, 0);
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
    EditorUI::Initialize(static_cast<float>(clientRect.right - clientRect.left), static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
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
    ID3D12GraphicsCommandList* commandList = m_deviceContext.GetCommandList();
    ID3D12Resource* backBuffer = m_deviceContext.GetRenderTarget(frameIndex);

    // The RDG leaves the back buffer in PRESENT state; promote it to RENDER_TARGET for ImGui
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &barrier);

    commandList->OMSetRenderTargets(1, &m_imguiRtvHandles[frameIndex], FALSE, nullptr);

    // The DX12 backend never binds a descriptor heap itself; it samples from this one
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

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
