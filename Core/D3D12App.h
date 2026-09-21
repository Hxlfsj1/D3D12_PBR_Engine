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

// Forward declaration keeps Dear ImGui headers out of this header
struct ImGui_ImplDX12_InitInfo;

class D3D12App
{
public:

    // Bootstrapping the engine and initializing state
    D3D12App(HINSTANCE hInstance);
    ~D3D12App();
    // Initialize the engine and activate all core functionalities
    bool Initialize(int nShowCmd);
    // Execute the Game Loop
    void Run();
    // Handle window messages and user-input-driven state changes
    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:

    // Engine boot-strapping internals for full-feature readiness
    bool InitializeWindow(int nShowCmd);
    bool InitD3D();

    // Internal routines invoked per frame during runtime
    void Update();

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
    SettingsManager m_settingsManager;
    EditorSelection m_editorSelection;
    EditorSelectionRenderer m_editorSelectionRenderer;

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
    UINT m_hbaoTemporalFrameIndex;
    bool m_temporalHistoryValid = false;
    bool m_hbaoHistoryValid = false;
    bool m_dlssHistoryValid = false;

    DirectX::XMFLOAT4X4 m_currViewGpu;
    DirectX::XMFLOAT4X4 m_currUnjitteredProjGpu;
    DirectX::XMFLOAT4X4 m_currJitteredProjGpu;
    DirectX::XMFLOAT4X4 m_currUnjitteredViewProjGpu;
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
