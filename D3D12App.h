// Wrapping up the main functions and variables

// 1. Abstract the foundational boilerplate of the Win32 windowing system.
// 2. Encapsulate the intricacies of Direct3D 12 hardware initialization and synchronization.
// 3. Centralize the lifecycle management of system memory and VRAM resources.
// 4. Orchestrate the configuration and transitions of the rendering pipeline states.
// 5. Govern the primary execution loop and process user interaction events.

#ifndef D3D12APP_H
#define D3D12APP_H

#include <string>
#include "stdafx.h"
#include "Camera.h"
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
    ResourceManager m_resourceManager;
    InputManager m_inputManager;
    PipelineManager m_pipelineManager;
    SettingsManager m_settingsManager;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissorRect;

    // Runtime game state, user input, and spatial transformations
    Camera camera;
    float deltaTime;

    // FPS
    int frameCount = 0;
    float timeElapsed = 0.0f;

    int m_visibleInstanceCount = 0;
    bool m_useTAA;
    UINT m_taaFrameCounter;
    bool m_taaHistoryValid = false;

    DirectX::XMFLOAT4X4 m_viewMat;
    DirectX::XMFLOAT4X4 m_unjitteredProjMat;
    DirectX::XMFLOAT4X4 m_projMat;
    DirectX::XMFLOAT4X4 m_unjitteredViewProj;
    DirectX::XMFLOAT4X4 m_viewProjMat;
    DirectX::XMFLOAT4X4 m_invViewProjMat;
    DirectX::XMFLOAT4X4 m_invProjMat;

    DirectX::XMFLOAT4X4 m_prevViewProj;
    bool m_hasPrevViewProj = false;
    float m_jitterX = 0.0f;
    float m_jitterY = 0.0f;
};

#endif
