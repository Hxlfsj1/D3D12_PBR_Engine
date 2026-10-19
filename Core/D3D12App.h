// Wrapping up the main functions and variables

// 1. Abstract the foundational boilerplate of the Win32 windowing system.
// 2. Encapsulate the intricacies of Direct3D 12 hardware initialization and synchronization.
// 3. Centralize the lifecycle management of system memory and VRAM resources.
// 4. Orchestrate the configuration and transitions of the rendering pipeline states.
// 5. Govern the primary execution loop and process user interaction events.

#ifndef D3D12APP_H
#define D3D12APP_H

#include <cstdint>
#include <string>
#include <vector>
#include "stdafx.h"
#include "Camera.h"
#include "DLSSManager.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "InputManager.h"
#include "PipelineManager.h"
#include "RenderStructs.h"
#include "EditorSelectionRenderer.h"
#include "EditorGizmo.h"
#include "VXGIWorld.h"
#include "VXGIDebugRenderer.h"
#include "VXGISDF.h"
#include "VXGIMeshSDF.h"
#include "VXGIMeshSDFScene.h"
#include "VXGISurface.h"
#include "VXGISDFDebugRenderer.h"
#include "VXGIGPUScene.h"
#include "VXGIProbeState.h"
#include "VXGIProbeCache.h"
#include "VXGIProbePass.h"
#include "VXGIProbeFilterPass.h"
#include "VXGIProbeTopology.h"
#include "VXGIProbeDebugRenderer.h"
#include "SSGIHybridState.h"
#include "RenderDebugPass.h"
#include "SceneEditState.h"
#include "FrameDiagnostics.h"
#include "TraceOutputDiagnostics.h"
#include "D3D12ValidationDiagnostics.h"

// Forward declaration keeps Dear ImGui headers out of this header
struct ImGui_ImplDX12_InitInfo;

class D3D12App
{
public:

    // Bootstrapping the engine and initializing state
    D3D12App(HINSTANCE hInstance);
    ~D3D12App();
    // Initialize the engine and activate all core functionalities
    bool Initialize(int nShowCmd, float uiScaleOverride = 0.0f);
    // Execute the Game Loop
    void Run();
    bool ConfigureDiagnostics(const char* commandLine);
    // Handle window messages and user-input-driven state changes
    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:

    // Engine boot-strapping internals for full-feature readiness
    bool InitializeWindow(int nShowCmd);
    bool InitD3D();

    // Internal routines invoked per frame during runtime
    void Update();
    void InvalidateTemporalHistories();
    void UpdateTemporalHistoryValidity();
    void CommitTemporalHistories(bool temporalWritten, bool hbaoWritten, bool dlssWritten, bool ssgiWritten);

    // Render passes
    bool BeginFrame();
    void DrawShadowMap();
    void DrawPBRModel();
    void DrawSkybox();
    void DrawPostProcess();
    bool EndFrame();

    void Render();
    void WaitForPreviousFrame();

    // Dear ImGui integration: context, backends, and GPU-side plumbing.
    // No business UI is built yet; the pipeline is kept alive and verified end-to-end.
    bool InitImGui();
    void RecordImGuiDrawData();
    void RecordImGuiDrawData(ID3D12GraphicsCommandList* commandList, int slot,
        bool restoreEngineHeap, const float* clearColor = nullptr);
    bool InitLoadingRenderer();
    bool WaitForLoadingFrame();
    void ShowLoadingProgress(float fraction, const std::string& status);
    void ShutdownImGui();
    // SRV allocator callbacks required by the 1.92 DX12 backend (SrvDescriptorAllocFn/FreeFn)
    static void ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);
    static void ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

    // Log a per-frame HRESULT failure; if the D3D12 device has been removed,
    // report the underlying removal reason and stop the main loop cleanly
    void ReportFrameError(const char* operation, HRESULT hr);

    // Populated when D3D12App() is invoked
    HINSTANCE mhAppInst;
    HWND hwnd;
    LPCTSTR WindowName;
    LPCTSTR WindowTitle;
    int Width;
    int Height;
    int SceneWidth;
    int SceneHeight;
    bool FullScreen;
    bool Running;
    // Populated when Initialize() is invoked
    std::string currentHDRPath;
    // Compile-time constants
    static const int frameBufferCount = 3;
    // Data populated dynamically at runtime
    int frameIndex;

    // Core Managers
    RenderDevice m_deviceContext;
    DLSSManager m_dlssManager;
    ResourceManager m_resourceManager;
    InputManager m_inputManager;
    PipelineManager m_pipelineManager;
    SSGIHistoryState m_ssgiHistory;
    SSGIHybridState m_ssgiSources;
    SceneEditState m_sceneEdits;
    uint64_t m_ssgiScreenSourceStamp = 0;
    SettingsManager m_settingsManager;
    RenderDebugPass m_debugPass = RenderDebugPass::Default;
    RenderDebugPass m_lastRenderedDebugPass = RenderDebugPass::Default;
    uint32_t m_lastRenderedGIWorldMode = 0;
    EditorSelection m_editorSelection;
    EditorHistory m_editorHistory;
    EditorGizmo m_editorGizmo;
    EditorSelectionRenderer m_editorSelectionRenderer;
    // The initial bake and persistent cache belong to ResourceManager.
    VXGIWorld& m_vxgiWorld = m_resourceManager.GetVXGI().world;
    VXGIDebugRenderer m_vxgiDebugRenderer;
    VXGISDF& m_vxgiSDF = m_resourceManager.GetVXGI().legacySDF;
    VXGIMeshSDFScene& m_vxgiMeshSDFScene = m_resourceManager.GetVXGI().meshSDFScene;
    VXGISurface& m_vxgiSurface = m_resourceManager.GetVXGI().surface;
    VXGISDFDebugRenderer m_vxgiSDFDebugRenderer;
    VXGIGPUScene& m_vxgiGPUScene = m_resourceManager.GetVXGI().gpuScene;
    VXGIProbeState& m_vxgiProbes = m_resourceManager.GetVXGI().probes;
    VXGIProbePass& m_vxgiProbePass = m_resourceManager.GetVXGI().probePass;
    VXGIProbeFilterPass& m_vxgiProbeFilterPass = m_resourceManager.GetVXGI().filterPass;
    VXGIProbeDebugRenderer m_vxgiProbeDebugRenderer;
    float m_uiScaleOverride = 0.0f;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissorRect;
    D3D12_VIEWPORT sceneViewport;
    D3D12_RECT sceneScissorRect;

    // Runtime game state, user input, and spatial transformations
    Camera camera;
    float deltaTime;

    // FPS
    int frameCount = 0;
    float timeElapsed = 0.0f;

    // Dear ImGui state
    bool m_imguiInitialized = false;
    ComPtr<ID3D12CommandAllocator> m_loadingAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_loadingCommandList;
    ComPtr<ID3D12Fence> m_loadingFence;
    UINT64 m_loadingFenceValue = 0;
    FrameDiagnostics m_frameDiagnostics;
    TraceOutputDiagnostics m_traceOutputDiagnostics;
    D3D12ValidationDiagnostics m_validationDiagnostics;
    std::string m_diagnosticsPrefix;
    std::string m_diagnosticsSequence = "control";
    double m_diagnosticsSeconds = 32;
    double m_diagnosticsElapsed = 0;
    uint64_t m_diagnosticsFrame = 0;
    uint32_t m_diagnosticsStage = 0;
    bool m_diagnosticsGPUValidation = false;
    bool m_diagnosticsModeFromSRV = true;
    bool m_diagnosticsOptionsValid = true;
    ComPtr<ID3D12Device6> m_diagnosticsPGODevice;
    bool m_diagnosticsPGOOverride = false;
    bool m_diagnosticsPGODiscarded = false;
    int m_diagnosticsBackgroundSupport = -1;
    ULONGLONG m_loadingLastDrawTick = 0;
    float m_loadingProgress = 0.0f;
    std::string m_loadingStageKey;
    bool m_loadingHiddenVerification = false;
    ComPtr<ID3D12DescriptorHeap> m_imguiRtvHeap;                              // One RTV per back buffer; the engine keeps no persistent RTV heap
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;                              // Dedicated shader-visible heap for ImGui textures (font atlas, ...)
    D3D12_CPU_DESCRIPTOR_HANDLE m_imguiRtvHandles[frameBufferCount] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_imguiSrvHeapCpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_imguiSrvHeapGpuStart = {};
    UINT m_imguiSrvDescriptorSize = 0;
    std::vector<UINT> m_imguiSrvFreeList;                                     // Free descriptor slots inside m_imguiSrvHeap
    static const UINT imGuiSrvHeapSize = 64;

    int m_visibleInstanceCount = 0;
    int m_frustumInstanceCount = 0;
    AntiAliasingMode m_antiAliasingMode = AntiAliasingMode::None;
    UINT m_temporalJitterFrameIndex = 0;
    UINT m_dlssJitterFrameIndex = 0;
    UINT m_ssgiFrameIndex = 0;
    UINT m_hbaoTemporalFrameIndex;
    bool m_temporalHistoryValid = false;
    bool m_hbaoHistoryValid = false;
    bool m_dlssHistoryValid = false;

    DirectX::XMFLOAT4X4 m_currViewGpu;
    DirectX::XMFLOAT4X4 m_currUnjitteredProjGpu;
    DirectX::XMFLOAT4X4 m_currJitteredProjGpu;
    DirectX::XMFLOAT4X4 m_currUnjitteredViewProjGpu;
    DirectX::XMFLOAT4X4 m_currUnjitteredInvViewProjGpu;
    DirectX::XMFLOAT4X4 m_currJitteredViewProjGpu;
    DirectX::XMFLOAT4X4 m_currJitteredInvViewProjGpu;
    DirectX::XMFLOAT4X4 m_currJitteredInvProjGpu;

    DirectX::XMFLOAT4X4 m_prevUnjitteredViewProjGpu;
    bool m_hasPrevUnjitteredViewProj = false;
    float m_currJitterNdcX = 0.0f;
    float m_currJitterNdcY = 0.0f;
    float m_currJitterPixelX = 0.0f;
    float m_currJitterPixelY = 0.0f;
};

#endif
