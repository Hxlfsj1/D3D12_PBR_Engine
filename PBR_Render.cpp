#include "D3D12App.h"
#include "PBR_Shader.h"
#include "Window.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "InputManager.h"
#include "PipelineManager.h"

#include "Assets.h"

#include "RenderStructs.h"
#include "ShadowPass.h"
#include "SkyboxPass.h"
#include "PostProcessPass.h"
#include "PBRPass.h"
#include "GBufferPass.h"
#include "DeferredLightingPass.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>

using namespace DirectX;
// std::shared_ptr allocates an external reference counter, whereas ComPtr uses the internal counter of the COM object itself
using Microsoft::WRL::ComPtr;

static std::vector<ModelInstance*> g_visibleInstances;
static std::vector<ModelInstance*> g_shadowVisibleInstances;

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
    WindowTitle = L"PBR IBL Model Viewer";
    Width = 2240;
    Height = 1400;
    FullScreen = false;
    Running = true;

    frameIndex = 0;
    deltaTime = 0.0f;

    m_inputManager.Init(Width, Height);
}

D3D12App::~D3D12App()
{
    if (m_deviceContext.GetDevice() != nullptr)
    {
        WaitForPreviousFrame();
    }
}

bool D3D12App::Initialize(int nShowCmd)
{
    currentHDRPath = Assets::GetSkyboxPath();

    // Ask the system for a window
    if (!InitializeWindow(nShowCmd))
    {
        return false;
    }

    // "Fuel" the engine
    if (!InitD3D())
    {
        return false;
    }

    m_resourceManager.FreeUploadHeaps();

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
    if (!m_deviceContext.Initialize(hwnd, Width, Height, frameBufferCount)) return false;

    frameIndex = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();

    // Compile Pipeline States: Precompute Root Signatures and PSOs for both Graphics and Compute pipelines
    if (!m_pipelineManager.Initialize(&m_deviceContext)) return false;

    // Stream Assets & Build IBL: Load 3D models and HDR textures into VRAM and bake IBL components
    auto sceneData = Assets::GetSniperAlleyScene();
    if (!m_resourceManager.LoadAssets(&m_deviceContext, sceneData, frameBufferCount)) return false;
    m_resourceManager.BuildGlobalMaterialPool(&m_deviceContext);
    m_resourceManager.InitIBL(&m_deviceContext, currentHDRPath.c_str());

    if (!m_resourceManager.InitShadowResources(&m_deviceContext)) return false;

    if (!m_resourceManager.InitPostProcess(&m_deviceContext, Width, Height)) return false;

    if (!m_resourceManager.InitGBuffer(&m_deviceContext, Width, Height)) return false;

    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Width, (float)Height);
    scissorRect = CD3DX12_RECT(0, 0, Width, Height);

    return true;
}

// Data is streamed directly from the Upload Heap to the GPU, utilizing a Ring Buffer mechanism (with a count of 3 to align with the Triple Buffering scheme)
void D3D12App::Update()
{
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
            L"    |    Hz: 300";

        SetWindowText(hwnd, windowText.c_str());

        frameCount = 0;
        timeElapsed -= 1.0f;
    }

    // ====================================================================================================
    // Input polling and environment setup
    // ====================================================================================================
    m_inputManager.Update(deltaTime, camera);

    auto& instances = m_resourceManager.GetSceneInstances();
    // Get the world-space view frustum of the current frame's camera
    BoundingFrustum frustum = camera.GetWorldSpaceFrustum((float)Width / Height, 0.1f, 1000.0f);
    // Bind CBVs to prepare for subsequent data updates to the GPU
    UINT8* cbvAddress = m_resourceManager.GetCBVAddress(frameIndex);

    // ====================================================================================================
    // Directional light matrix and shadow stability calculations
    // ====================================================================================================
    // Initialize passed data (camera position, light attributes, etc.)
    PassConstants passCb;
    passCb.camPos = camera.Position;

    XMVECTOR dirVec = XMVectorSet(-0.5f, -1.0f, 0.5f, 0.0f);
    dirVec = XMVector3Normalize(dirVec);
    XMStoreFloat3(&passCb.lightDir, dirVec);

    passCb.lightColor = XMFLOAT3(5.0f, 5.0f, 5.0f);

    // Initialize shadow volume attributes: a cube located in front of the player's view
    XMVECTOR camPosVec = XMLoadFloat3(&camera.Position);
    XMVECTOR camFrontVec = XMLoadFloat3(&camera.Front);
    XMVECTOR centerVec = XMVectorAdd(camPosVec, XMVectorScale(camFrontVec, 25.0f));
    float shadowRadius = 40.0f;

    // Initialize shadow volume attributes: a cube located in front of the player's view
    XMVECTOR lightPosVec = XMVectorSubtract(centerVec, XMVectorScale(dirVec, 500.0f));
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    if (abs(XMVectorGetY(dirVec)) > 0.99f)
    {
        lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    XMMATRIX lightView = XMMatrixLookAtLH(lightPosVec, centerVec, lightUp);

    // Texel snapping technique to eliminate edge flickering
    float shadowMapSize = 4096.0f;
    float texelSize = (shadowRadius * 2.0f) / shadowMapSize;

    XMVECTOR centerLS = XMVector3TransformCoord(centerVec, lightView);
    float cx = XMVectorGetX(centerLS);
    float cy = XMVectorGetY(centerLS);

    float snappedCx = floor(cx / texelSize) * texelSize;
    float snappedCy = floor(cy / texelSize) * texelSize;

    lightView.r[3] = XMVectorAdd(lightView.r[3], XMVectorSet(snappedCx - cx, snappedCy - cy, 0.0f, 0.0f));

    // Generate orthographic projection matrix and upload all packed data to GPU memory
    float minZ = 0.0f;
    float maxZ = 1000.0f;

    XMMATRIX lightProj = XMMatrixOrthographicLH(shadowRadius * 2.0f, shadowRadius * 2.0f, minZ, maxZ);
    XMMATRIX lightViewProj = lightView * lightProj;

    passCb.padding3 = shadowRadius * 2.0f;

    XMStoreFloat4x4(&passCb.lightViewProj, XMMatrixTranspose(lightViewProj));

    passCb.iblPrefilterIdx = m_resourceManager.GetIblPrefilterIdx();
    passCb.iblBRDFIdx = m_resourceManager.GetIblBRDFIdx();
    passCb.shadowMapIdx = m_resourceManager.GetShadowSrvIdx();

    memcpy(cbvAddress, &passCb, sizeof(PassConstants));

    // ====================================================================================================
    // Frustum culling and directional light shadow volume culling
    // ====================================================================================================
    BoundingBox shadowArea;
    shadowArea.Center = XMFLOAT3(0.0f, 0.0f, (minZ + maxZ) * 0.5f);
    shadowArea.Extents = XMFLOAT3(shadowRadius, shadowRadius, (maxZ - minZ) * 0.5f);

    g_visibleInstances.clear();
    g_shadowVisibleInstances.clear();

    for (size_t i = 0; i < instances.size(); ++i)
    {
        // LOD
        XMVECTOR objPosVec = XMLoadFloat3(&instances[i].translation);
        float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(camPosVec, objPosVec)));

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

        // Use the existing Intersects library function to determine if an object should be added to the render queue or the shadow queue
        if (instances[i].pModel == nullptr || instances[i].pModel->meshes.empty())
        {
            instances[i].isVisible = false;
            continue;
        }

        instances[i].UpdateTransform();

        BoundingBox worldBox;
        instances[i].pModel->boundingBox.Transform(worldBox, instances[i].cachedWorldMat);

        // Introduce an isVisible variable to enhance scalability
        if (frustum.Intersects(worldBox))
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

        if (shadowArea.Intersects(lightSpaceBox))
        {
            g_shadowVisibleInstances.push_back(&instances[i]);
        }
    }

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
                if (a->pModel != b->pModel)
                {
                    return a->pModel < b->pModel;
                }
                return a->currentLodLevel < b->currentLodLevel;
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

    std::sort(g_shadowVisibleInstances.begin(), g_shadowVisibleInstances.end(), [](ModelInstance* a, ModelInstance* b)
        {
            if (a->pModel != b->pModel)
            {
                return a->pModel < b->pModel;
            }
            return a->currentLodLevel < b->currentLodLevel;
        });

    // ====================================================================================================
    // Instance data packing and submission
    // ====================================================================================================
    InstanceData* mappedInstanceData = reinterpret_cast<InstanceData*>(cbvAddress + 256);

    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)Width / Height, 0.1f, 1000.0f);
    XMMATRIX viewProj = view * proj;

    // Pack all instance matrices sequentially into the StructuredBuffer for hardware instancing
    for (size_t i = 0; i < g_visibleInstances.size(); ++i)
    {
        // Update math only if dirty (CPU optimization)
        XMMATRIX world = g_visibleInstances[i]->cachedWorldMat;
        XMMATRIX normalMat = g_visibleInstances[i]->cachedNormalMat;

        // Always flush to the current frame's CBV to prevent multi-frame ghosting
        // Do not use memcpy: It ignores SIMD 16-byte alignment and causes fatal crashes, XMStore safely offloads hardware registers
        XMStoreFloat4x4(&mappedInstanceData[i].wvpMat, XMMatrixTranspose(world * viewProj));
        XMStoreFloat4x4(&mappedInstanceData[i].worldMat, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mappedInstanceData[i].normalMat, XMMatrixTranspose(normalMat));
    }

    size_t shadowOffset = g_visibleInstances.size();
    for (size_t i = 0; i < g_shadowVisibleInstances.size(); ++i)
    {
        XMMATRIX world = g_shadowVisibleInstances[i]->cachedWorldMat;
        XMStoreFloat4x4(&mappedInstanceData[shadowOffset + i].worldMat, XMMatrixTranspose(world));
    }
}

void D3D12App::BeginFrame()
{
    // Reset the command sequence from the previous frame
    m_deviceContext.GetCommandAllocator(frameIndex)->Reset();
    m_deviceContext.GetCommandList()->Reset(m_deviceContext.GetCommandAllocator(frameIndex), m_pipelineManager.GetPBR_PSO());

    ID3D12Resource* currentBuffer = m_deviceContext.GetRenderTarget(frameIndex);

    // Define the required framebuffers as 'canvases' rather than 'presentation states'
    CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(currentBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_deviceContext.GetCommandList()->ResourceBarrier(1, &b);

    // Bind the Render Target View (RTV) and Depth Stencil View (DSV) for the current frame
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = m_resourceManager.GetPostProcessRtvHandle();
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = m_deviceContext.GetDSVHandle();
    m_deviceContext.GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Clear the canvas to a solid color to prevent ghosting from the previous frame
    const float clearColor[] = { 0.2f, 0.3f, 0.4f, 1.0f };
    m_deviceContext.GetCommandList()->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_deviceContext.GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set the Viewport and Scissor Rect (for viewport transformation and clipping before rasterization)
    m_deviceContext.GetCommandList()->RSSetViewports(1, &viewport);
    m_deviceContext.GetCommandList()->RSSetScissorRects(1, &scissorRect);
}

void D3D12App::EndFrame()
{
    // Define the required framebuffers as 'presentation states' rather than 'canvases'
    ID3D12Resource* currentBuffer = m_deviceContext.GetRenderTarget(frameIndex);
    CD3DX12_RESOURCE_BARRIER p = CD3DX12_RESOURCE_BARRIER::Transition(currentBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_deviceContext.GetCommandList()->ResourceBarrier(1, &p);

    // Close the Command List to finalize recording, no further commands can be added until the next Reset
    // CPU recording is complete, but the GPU has yet to begin execution; therefore
    // A Fence must be signaled to track GPU progress, ensuring the CPU waits before reusing this memory in the NEXT frame
    m_deviceContext.GetCommandList()->Close();
}

void D3D12App::Render()
{
    BeginFrame();

    ShadowPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, g_shadowVisibleInstances, g_visibleInstances.size());

    size_t transparentIdx = 0;

    static bool useDeferredPath = true;

    if (!useDeferredPath)
    {
        transparentIdx = PBRPass::ExecuteOpaque(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances);
    }
    else
    {
        transparentIdx = GBufferPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances);

        DeferredLightingPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, camera, Width, Height, frameIndex);
    }

    SkyboxPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, camera, viewport, scissorRect, Width, Height);

    PBRPass::ExecuteTransparent(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances, transparentIdx);

    PostProcessPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect);

    EndFrame();

    ID3D12CommandList* lists[] = { m_deviceContext.GetCommandList() };
    // Submit recorded rendering commands to the GPU for execution
    m_deviceContext.GetCommandQueue()->ExecuteCommandLists(1, lists);
    // Insert a signal into the queue to track GPU progress
    m_deviceContext.GetCommandQueue()->Signal(m_deviceContext.GetFence(frameIndex), ++m_deviceContext.GetFenceValue(frameIndex));

    // Flip the back buffer to the front screen
    m_deviceContext.GetSwapChain()->Present(0, 0);
}

// Track GPU progress, prevent data updates until execution is complete, provide an 'alarm' mechanism for the CPU (via Fence Events)
void D3D12App::WaitForPreviousFrame()
{
    frameIndex = m_deviceContext.GetSwapChain()->GetCurrentBackBufferIndex();
    m_deviceContext.WaitForPreviousFrame(frameIndex);
}