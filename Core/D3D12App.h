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
    void BeginFrame();
    void DrawShadowMap();
    void DrawPBRModel();
    void DrawSkybox();
    void DrawPostProcess();
    void EndFrame();

    void Render();
    void WaitForPreviousFrame();

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
