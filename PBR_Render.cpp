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
#include "TAAPass.h"

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
    Running = true;

    frameIndex = 0;
    deltaTime = 0.0f;
    m_taaFrameCounter = 0;
    m_taaHistoryValid = false;

    DirectX::XMStoreFloat4x4(&m_unjitteredViewProj, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&m_prevViewProj, DirectX::XMMatrixIdentity());
    m_hasPrevViewProj = false;
    m_jitterX = 0.0f;
    m_jitterY = 0.0f;
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

    if (!m_resourceManager.InitGBuffer(&m_deviceContext, Width, Height)) return false;

    if (!m_resourceManager.InitHBAO(&m_deviceContext, Width, Height)) return false;

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
    // Calculate V * P matrix
    // ====================================================================================================
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX unjitteredProj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)Width / Height, 0.1f, 1000.0f);
    XMMATRIX unjitteredViewProj = view * unjitteredProj;

    if (m_hasPrevViewProj)
    {
        m_prevViewProj = m_unjitteredViewProj;
    }
    else
    {
        XMStoreFloat4x4(&m_prevViewProj, XMMatrixTranspose(unjitteredViewProj));
        m_hasPrevViewProj = true;
    }

    if (m_useTAA)
    {
        static const float haltonX[8] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0f };
        static const float haltonY[8] = { 0.333333f, 0.666667f, 0.111111f, 0.444444f, 0.777778f, 0.222222f, 0.555556f, 0.888889f };

        float jX = haltonX[m_taaFrameCounter % 8] - 0.5f;
        float jY = haltonY[m_taaFrameCounter % 8] - 0.5f;
        m_jitterX = (jX * 2.0f) / Width;
        m_jitterY = (jY * 2.0f) / Height;
        m_taaFrameCounter++;
    }
    else
    {
        m_jitterX = 0.0f;
        m_jitterY = 0.0f;
    }

    DirectX::XMFLOAT4X4 projF;
    DirectX::XMStoreFloat4x4(&projF, unjitteredProj);
    projF._31 += m_jitterX;
    projF._32 += m_jitterY;
    XMMATRIX jitteredProj = DirectX::XMLoadFloat4x4(&projF);

    XMMATRIX jitteredViewProj = view * jitteredProj;

    XMVECTOR det;
    XMMATRIX invViewProj = XMMatrixInverse(&det, jitteredViewProj);
    XMMATRIX invProj = XMMatrixInverse(&det, jitteredProj);

    XMStoreFloat4x4(&m_viewMat, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_unjitteredProjMat, XMMatrixTranspose(unjitteredProj));
    XMStoreFloat4x4(&m_projMat, XMMatrixTranspose(jitteredProj));
    XMStoreFloat4x4(&m_unjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
    XMStoreFloat4x4(&m_viewProjMat, XMMatrixTranspose(jitteredViewProj));
    XMStoreFloat4x4(&m_invViewProjMat, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&m_invProjMat, XMMatrixTranspose(invProj));

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
    XMVECTOR dirVec = XMLoadFloat3(&m_settingsManager.lighting.lightDir);
    dirVec = XMVector3Normalize(dirVec);
    XMStoreFloat3(&passCb.lightDir, dirVec);

    passCb.lightColor = m_settingsManager.lighting.lightColor;
    float shadowRadius = m_settingsManager.lighting.shadowRadius;
    float shadowMaxDistance = 100.0f;
    BoundingFrustum worldFrustum = camera.GetWorldSpaceFrustum((float)Width / Height, 0.1f, shadowMaxDistance);
    XMFLOAT3 frustumCorners[8];
    worldFrustum.GetCorners(frustumCorners);
    XMVECTOR camPosVec = XMLoadFloat3(&camera.Position);

    // Anchor to the camera and pull back along the reverse light direction to position the virtual 'sun camera' for shadow capture
    XMVECTOR lightPosVec = XMVectorSubtract(camPosVec, XMVectorScale(dirVec, 200.0f));
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (abs(XMVectorGetY(dirVec)) > 0.99f)
    {
        lightUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }
    XMMATRIX lightView = XMMatrixLookAtLH(lightPosVec, XMVectorAdd(lightPosVec, dirVec), lightUp);

    float minZ = FLT_MAX;
    float maxZ = -FLT_MAX;

    // Transform the 8 camera frustum corners into the light's view space to calculate the tightest possible bounding box
    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR vWorld = XMLoadFloat3(&frustumCorners[i]);
        XMVECTOR vLight = XMVector3Transform(vWorld, lightView);

        XMFLOAT3 pLight;
        XMStoreFloat3(&pLight, vLight);

        minZ = std::min(minZ, pLight.z);
        maxZ = max(maxZ, pLight.z);
    }

    // Extend Z-bounds both ways to prevent missing shadows
    minZ -= 50.0f;
    maxZ += 50.0f;

    passCb.padding3 = shadowRadius * 2.0f;
    float nearClip = 0.1f;
    float cascadeSplits[5] = { nearClip, 5.0f, 15.0f, 50.0f, shadowMaxDistance };
    passCb.cascadeSplits = XMFLOAT4(cascadeSplits[1], cascadeSplits[2], cascadeSplits[3], cascadeSplits[4]);
    float orthoWidths[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    for (int cascadeIdx = 0; cascadeIdx < NUM_CASCADES; ++cascadeIdx)
    {
        BoundingFrustum subFrustum = camera.GetWorldSpaceFrustum((float)Width / Height, cascadeSplits[cascadeIdx], cascadeSplits[cascadeIdx + 1]);
        XMFLOAT3 subCorners[8];
        subFrustum.GetCorners(subCorners);

        XMVECTOR frustumCenter = XMVectorZero();
        for (int i = 0; i < 8; ++i)
        {
            frustumCenter = XMVectorAdd(frustumCenter, XMLoadFloat3(&subCorners[i]));
        }
        frustumCenter = XMVectorScale(frustumCenter, 1.0f / 8.0f);

        float sphereRadius = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            XMVECTOR distVec = XMVector3Length(XMVectorSubtract(XMLoadFloat3(&subCorners[i]), frustumCenter));
            sphereRadius = max(sphereRadius, XMVectorGetX(distVec));
        }

        sphereRadius = std::ceil(sphereRadius * 16.0f) / 16.0f;

        XMVECTOR lightSpaceCenter = XMVector3Transform(frustumCenter, lightView);

        float orthoWidthOrHeight = sphereRadius * 2.0f;

        // Texel snapping technique to eliminate edge flickering (enforced by calculating the actual world-space length of each shadow map texel)
        float shadowMapSize = 4096.0f;
        float fixedTexelSize = orthoWidthOrHeight / shadowMapSize;

        float snappedX = std::floor(XMVectorGetX(lightSpaceCenter) / fixedTexelSize) * fixedTexelSize;
        float snappedY = std::floor(XMVectorGetY(lightSpaceCenter) / fixedTexelSize) * fixedTexelSize;
        float snappedZ = XMVectorGetZ(lightSpaceCenter);

        float subMinX = snappedX - sphereRadius;
        float subMaxX = snappedX + sphereRadius;
        float subMinY = snappedY - sphereRadius;
        float subMaxY = snappedY + sphereRadius;

        float subMinZ = snappedZ - sphereRadius - 50.0f;
        float subMaxZ = snappedZ + sphereRadius;

        // Generate orthographic projection matrix and upload all packed data to GPU memory
        XMMATRIX subLightProj = XMMatrixOrthographicOffCenterLH(subMinX, subMaxX, subMinY, subMaxY, subMinZ, subMaxZ);
        XMStoreFloat4x4(&passCb.lightViewProj[cascadeIdx], XMMatrixTranspose(lightView * subLightProj));
        orthoWidths[cascadeIdx] = orthoWidthOrHeight;
    }

    passCb.cascadeOrthoWidths = XMFLOAT4(orthoWidths[0], orthoWidths[1], orthoWidths[2], orthoWidths[3]);

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

    std::sort(g_shadowVisibleInstances.begin(), g_shadowVisibleInstances.end(), [](ModelInstance* a, ModelInstance* b)
        {
            if (a->pModel != b->pModel)
            {
                return a->pModel < b->pModel;
            }
            return a->currentLodLevel < b->currentLodLevel;
        });

    // ====================================================================================================
    // Instance data Submission and TAA offset matirx calculating
    // ====================================================================================================
    InstanceData* mappedInstanceData = reinterpret_cast<InstanceData*>(cbvAddress + kPassConstantsAlignedSize);

    for (size_t i = 0; i < g_visibleInstances.size(); ++i)
    {
        XMMATRIX world = g_visibleInstances[i]->cachedWorldMat;
        XMMATRIX normalMat = g_visibleInstances[i]->cachedNormalMat;

        XMStoreFloat4x4(&mappedInstanceData[i].wvpMat, XMMatrixTranspose(world * jitteredViewProj));
        XMStoreFloat4x4(&mappedInstanceData[i].worldMat, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mappedInstanceData[i].normalMat, XMMatrixTranspose(normalMat));

        mappedInstanceData[i].customMaterialID = g_visibleInstances[i]->customMaterialID;
    }

    size_t shadowOffset = g_visibleInstances.size();
    for (size_t i = 0; i < g_shadowVisibleInstances.size(); ++i)
    {
        XMMATRIX world = g_shadowVisibleInstances[i]->cachedWorldMat;
        XMStoreFloat4x4(&mappedInstanceData[shadowOffset + i].worldMat, XMMatrixTranspose(world));

        mappedInstanceData[shadowOffset + i].customMaterialID = g_shadowVisibleInstances[i]->customMaterialID;
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

    if (!m_settingsManager.pipeline.useDeferred)
    {
        transparentIdx = PBRPass::ExecuteOpaque(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances);
    }
    else
    {
        transparentIdx = GBufferPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances);

        HBAOPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, m_viewMat, m_projMat, m_invProjMat, Width, Height, frameIndex);

        DeferredLightingPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, m_invViewProjMat, Width, Height, frameIndex);
    }

    SkyboxPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, camera, viewport, scissorRect, Width, Height);

    PBRPass::ExecuteTransparent(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, g_visibleInstances, transparentIdx);

    UINT finalPostInputSRV = m_resourceManager.GetPostProcessSrvIdx();

    if (m_useTAA)
    {
        finalPostInputSRV = TAAPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, m_invViewProjMat, m_prevViewProj, m_jitterX, m_jitterY, frameIndex, Width, Height, m_taaHistoryValid);
        m_taaHistoryValid = true;
    }

    PostProcessPass::Execute(&m_deviceContext, &m_resourceManager, &m_pipelineManager, frameIndex, viewport, scissorRect, finalPostInputSRV);

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
