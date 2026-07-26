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

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>

#include <array>
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

    // Construction for TAA
    m_taaJitterFrameIndex = 0;
    m_taaHistoryValid = false;

    DirectX::XMStoreFloat4x4(&m_currUnjitteredViewProjGpu, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_prevUnjitteredViewProjGpu, DirectX::XMMatrixIdentity());

    m_hasPrevUnjitteredViewProj = false;

    m_currJitterNdcX = 0.0f;
    m_currJitterNdcY = 0.0f;
}

D3D12App::~D3D12App()
{
    if (m_deviceContext.GetDevice() != nullptr)
    {
        WaitForPreviousFrame();
    }
}

static std::wstring g_wWindowTitle;
bool D3D12App::Initialize(int nShowCmd)
{
    m_settingsManager.LoadAllSettingsFromJson();

    Width = m_settingsManager.window.width;
    Height = m_settingsManager.window.height;
    FullScreen = m_settingsManager.window.fullScreen;
    m_useTAA = m_settingsManager.pipeline.useTAA;

    std::string titleStr = m_settingsManager.window.title;
    g_wWindowTitle = std::wstring(titleStr.begin(), titleStr.end());
    WindowTitle = g_wWindowTitle.c_str();

    m_inputManager.Init(Width, Height);

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
    if (!m_resourceManager.LoadAssets(&m_deviceContext, SettingsManager::LoadSceneFromJson("Settings/Scene.json"), frameBufferCount)) return false;
    m_resourceManager.BuildGlobalMaterialPool(&m_deviceContext);
    currentHDRPath = SettingsManager::GetSkyboxPathFromJson();
    if (!m_resourceManager.InitIBL(&m_deviceContext, currentHDRPath.c_str())) return false;

    if (!m_resourceManager.InitShadowResources(&m_deviceContext)) return false;

    if (!m_resourceManager.InitPostProcess(&m_deviceContext, Width, Height)) return false;

    if (!m_resourceManager.InitDepthBufferSRV(&m_deviceContext)) return false;

    m_resourceManager.SealPersistentSrvUavDescriptors();

    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Width, (float)Height);
    scissorRect = CD3DX12_RECT(0, 0, Width, Height);

    return true;
}

// Data is streamed directly from the Upload Heap to the GPU, utilizing a Ring Buffer mechanism (with a count of 3 to align with the Triple Buffering scheme)
void D3D12App::Update()
{
    auto& instances = m_resourceManager.GetSceneInstances();

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
    if (m_useTAA)
    {
        static const float haltonX[8] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f };
        static const float haltonY[8] = { 0.333333f, 0.666667f, 0.111111f, 0.444444f, 0.777778f, 0.222222f, 0.555556f, 0.888889f };
        constexpr float jitterScale = 0.75f;

        float currJitterPixelX = (haltonX[m_taaJitterFrameIndex % 8] - 0.5f) * jitterScale;
        float currJitterPixelY = (haltonY[m_taaJitterFrameIndex % 8] - 0.5f) * jitterScale;
        m_currJitterNdcX = (currJitterPixelX * 2.0f) / Width;
        m_currJitterNdcY = (currJitterPixelY * 2.0f) / Height;
        m_taaJitterFrameIndex++;
    }
    else
    {
        m_currJitterNdcX = 0.0f;
        m_currJitterNdcY = 0.0f;
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
    passCb.lightColor = m_settingsManager.lighting.lightColor;

    ShadowPass::FramePreparationInput shadowInput = {};
    shadowInput.camera = &camera;
    shadowInput.lightDir = m_settingsManager.lighting.lightDir;
    shadowInput.aspectRatio = static_cast<float>(Width) / Height;
    shadowInput.shadowRadius = m_settingsManager.lighting.shadowRadius;

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
    passCb.shadowMapIdx = m_resourceManager.GetShadowSrvIdx();

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
            if (shadowFrame.shadowArea.Intersects(lightSpaceBox) &&
                shadowFrame.cascadeShadowAreas[cascadeIdx].Intersects(lightSpaceBox))
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
    // Instance data Submission and TAA offset matirx calculating
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

void D3D12App::BeginFrame(bool backBufferHandledByFrameGraph)
{
    m_resourceManager.ResetTransientSrvUavDescriptors(frameIndex);
    m_resourceManager.BeginRDGFrame(frameIndex);

    // Reset the command sequence from the previous frame
    m_deviceContext.GetCommandAllocator(frameIndex)->Reset();
    m_deviceContext.GetCommandList()->Reset(m_deviceContext.GetCommandAllocator(frameIndex), m_pipelineManager.GetPBR_PSO());

    if (!backBufferHandledByFrameGraph)
    {
        ID3D12Resource* currentBuffer = m_deviceContext.GetRenderTarget(frameIndex);

        // Define the required framebuffers as 'canvases' rather than 'presentation states'
        CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(currentBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_deviceContext.GetCommandList()->ResourceBarrier(1, &b);
    }

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

void D3D12App::EndFrame(bool backBufferAlreadyPresent)
{
    if (!backBufferAlreadyPresent)
    {
        // Define the required framebuffers as 'presentation states' rather than 'canvases'
        ID3D12Resource* currentBuffer = m_deviceContext.GetRenderTarget(frameIndex);
        CD3DX12_RESOURCE_BARRIER p = CD3DX12_RESOURCE_BARRIER::Transition(currentBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_deviceContext.GetCommandList()->ResourceBarrier(1, &p);
    }

    // Close the Command List to finalize recording, no further commands can be added until the next Reset
    // CPU recording is complete, but the GPU has yet to begin execution; therefore
    // A Fence must be signaled to track GPU progress, ensuring the CPU waits before reusing this memory in the NEXT frame
    m_deviceContext.GetCommandList()->Close();
}

void D3D12App::Render()
{
    const bool backBufferHandledByFrameGraph = m_settingsManager.pipeline.useDeferred;
    const bool useZPrepass = m_settingsManager.pipeline.useZPrepass;
    BeginFrame(backBufferHandledByFrameGraph);

    size_t transparentIdx = 0;
    bool taaHandledByDeferredGraph = false;
    bool postProcessHandledByDeferredGraph = false;

    if (!m_settingsManager.pipeline.useDeferred)
    {
        ShadowPass::ExecuteRDG(
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            g_shadowVisibleInstancesByCascade,
            g_shadowInstanceOffsets,
            g_visibleInstances.size());

        transparentIdx = PBRPass::ExecuteOpaque(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances, useZPrepass);
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
                Microsoft::WRL::ComPtr<ID3D12Resource>* outResource)
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
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            g_shadowVisibleInstancesByCascade,
            g_shadowInstanceOffsets,
            g_visibleInstances.size());

        PBRPass::ZPrepassOutput zPrepassOutput = {};
        if (useZPrepass)
        {
            zPrepassOutput = PBRPass::AddZPrepassToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                viewport,
                scissorRect,
                g_visibleInstances,
                transparentStartIndex);
        }

        GBufferPass::Output gbufferOutput = GBufferPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            g_visibleInstances,
            transparentStartIndex,
            useZPrepass,
            zPrepassOutput.depth);

        HBAOPass::Output hbaoOutput = HBAOPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            m_currViewGpu,
            m_currJitteredProjGpu,
            m_currJitteredInvProjGpu,
            Width,
            Height,
            frameIndex,
            { gbufferOutput.depth, gbufferOutput.normal });
        deferredGraph.AddPassDependencies(hbaoOutput.rawPass, { gbufferOutput.pass });
        deferredGraph.AddPassDependencies(hbaoOutput.blurPass, { hbaoOutput.rawPass });

        DeferredLightingPass::Input deferredInput = {};
        deferredInput.gbufferAlbedo = gbufferOutput.albedo;
        deferredInput.gbufferNormal = gbufferOutput.normal;
        deferredInput.gbufferORM = gbufferOutput.orm;
        deferredInput.gbufferEmissive = gbufferOutput.emissive;
        deferredInput.depth = gbufferOutput.depth;
        deferredInput.hbaoBlurred = hbaoOutput.blurredTexture;
        deferredInput.shadowMap = shadowOutput.shadowMapSrv;

        DeferredLightingPass::Output deferredOutput = DeferredLightingPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            m_currJitteredInvViewProjGpu,
            Width,
            Height,
            frameIndex,
            deferredInput);
        deferredGraph.AddPassDependencies(deferredOutput.pass, { shadowOutput.pass, hbaoOutput.blurPass });
        RDGPassHandle sceneColorProducer = deferredOutput.pass;

        RDGPassHandle skyboxPass = SkyboxPass::AddToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            camera,
            viewport,
            scissorRect,
            Width,
            Height,
            { deferredOutput.sceneColor, gbufferOutput.depth });
        deferredGraph.AddPassDependencies(skyboxPass, { sceneColorProducer });
        sceneColorProducer = skyboxPass;

        RDGPassHandle transparentPass = PBRPass::AddTransparentToGraph(
            deferredGraph,
            &m_deviceContext,
            &m_resourceManager,
            &m_pipelineManager,
            frameIndex,
            viewport,
            scissorRect,
            g_visibleInstances,
            transparentStartIndex,
            { deferredOutput.sceneColor, gbufferOutput.depth, shadowOutput.shadowMapSrv });
        sceneColorProducer = transparentPass;

        if (m_useTAA)
        {
            MotionVectorPass::Output motionOutput = MotionVectorPass::AddToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                m_currJitteredInvViewProjGpu,
                m_prevUnjitteredViewProjGpu,
                Width,
                Height,
                frameIndex,
                { gbufferOutput.depth });

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
                m_taaHistoryValid,
                { deferredOutput.sceneColor, gbufferOutput.depth, motionOutput.motionTexture });
            deferredGraph.AddPassDependencies(taaOutput.pass, { sceneColorProducer });
            sceneColorProducer = taaOutput.pass;

            RDGPassHandle postProcessPass = PostProcessPass::AddFinalToGraph(
                deferredGraph,
                &m_deviceContext,
                &m_resourceManager,
                &m_pipelineManager,
                frameIndex,
                viewport,
                scissorRect,
                taaOutput.historyTexture);
            deferredGraph.AddPassDependencies(postProcessPass, { sceneColorProducer });

            taaHandledByDeferredGraph = true;
            postProcessHandledByDeferredGraph = true;
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
                deferredOutput.sceneColor);
            deferredGraph.AddPassDependencies(postProcessPass, { sceneColorProducer });

            postProcessHandledByDeferredGraph = true;
        }

        // Execute the entire graph after all passes have been added
        deferredGraph.Execute(m_deviceContext.GetCommandList());

        if (taaHandledByDeferredGraph)
        {
            m_resourceManager.FlipTAAHistoryIndex();
            m_taaHistoryValid = true;
        }

        transparentIdx = transparentStartIndex;
    }

    if (!m_settingsManager.pipeline.useDeferred)
    {
        SkyboxPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, camera, viewport, scissorRect, Width, Height);

        PBRPass::ExecuteTransparent(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances, transparentIdx);
    }

    UINT finalPostInputSRV = m_resourceManager.GetPostProcessSrvIdx();

    if (m_useTAA && !taaHandledByDeferredGraph)
    {
        finalPostInputSRV = TAAPass::ExecuteRDG(&m_deviceContext, &m_resourceManager, &m_pipelineManager, m_currJitteredInvViewProjGpu, m_prevUnjitteredViewProjGpu, m_currJitterNdcX, m_currJitterNdcY, frameIndex, Width, Height, m_taaHistoryValid);
        m_taaHistoryValid = true;
    }

    if (!postProcessHandledByDeferredGraph)
    {
        if (!m_useTAA)
        {
            PostProcessPass::ExecuteRDG(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect);
        }
        else
        {
            PostProcessPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, finalPostInputSRV);
        }
    }

    EndFrame(postProcessHandledByDeferredGraph);

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
