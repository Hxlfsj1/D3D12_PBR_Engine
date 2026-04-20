#include "D3D12App.h"
#include "PBR_Shader.h"
#include "Window.h"
#include "RenderDevice.h"
#include "ResourceManager.h"
#include "InputManager.h"
#include "PipelineManager.h"
#include "Assets.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>

using namespace DirectX;
// std::shared_ptr allocates an external reference counter, whereas ComPtr uses the internal counter of the COM object itself.
using Microsoft::WRL::ComPtr;

// Global hook for Win32 message routing
D3D12App* g_App = nullptr;

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
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
    Width = 1920;
    Height = 1200;
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
    if (!m_pipelineManager.Initialize(&m_deviceContext, Width, Height, frameBufferCount)) return false;

    // Stream Assets & Build IBL: Load 3D models and HDR textures into VRAM and bake IBL components
    auto sceneData = Assets::GetSniperAlleyScene();
    if (!m_resourceManager.LoadAssets(&m_deviceContext, sceneData, frameBufferCount)) return false;
    m_resourceManager.InitIBL(&m_deviceContext, currentHDRPath.c_str());

    if (!m_resourceManager.InitShadowResources(&m_deviceContext)) return false;
    
    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Width, (float)Height);
    scissorRect = CD3DX12_RECT(0, 0, Width, Height);

    return true;
}

// Data is streamed directly from the Upload Heap to the GPU, utilizing a Ring Buffer mechanism (with a count of 3 to align with the Triple Buffering scheme)
void D3D12App::Update()
{
    // Handle FPS
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

    // Handle continuous input in the Update loop
    m_inputManager.Update(deltaTime, camera);

    auto& instances = m_resourceManager.GetSceneInstances();
    UINT8* cbvAddress = m_resourceManager.GetCBVAddress(frameIndex);

    // Refresh per-frame constants, including the MVP transform
    PassConstants passCb;
    passCb.camPos = camera.Position;
    passCb.lightPos = XMFLOAT3(10.0f, 20.0f, -10.0f);
    passCb.lightColor = XMFLOAT3(500.0f, 500.0f, 500.0f);

    XMVECTOR lightPosVec = XMLoadFloat3(&passCb.lightPos);
    XMVECTOR lightTarget = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPosVec, lightTarget, lightUp);
    XMMATRIX lightProj = XMMatrixOrthographicLH(50.0f, 50.0f, 1.0f, 100.0f);
    XMMATRIX lightViewProj = lightView * lightProj;
    XMStoreFloat4x4(&passCb.lightViewProj, XMMatrixTranspose(lightViewProj));

    memcpy(cbvAddress, &passCb, sizeof(PassConstants));

    InstanceData* mappedInstanceData = reinterpret_cast<InstanceData*>(cbvAddress + 256);

    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)Width / Height, 0.1f, 1000.0f);
    XMMATRIX viewProj = view * proj;

    // Pack all instance matrices sequentially into the StructuredBuffer for hardware instancing
    for (size_t i = 0; i < instances.size(); ++i)
    {
        // Update math only if dirty (CPU optimization)
        instances[i].UpdateTransform();

        XMMATRIX world = instances[i].cachedWorldMat;
        XMMATRIX normalMat = instances[i].cachedNormalMat;

        // Always flush to the current frame's CBV to prevent multi-frame ghosting
        // Do not use memcpy: It ignores SIMD 16-byte alignment and causes fatal crashes, XMStore safely offloads hardware registers
        XMStoreFloat4x4(&mappedInstanceData[i].wvpMat, XMMatrixTranspose(world * viewProj));
        XMStoreFloat4x4(&mappedInstanceData[i].worldMat, XMMatrixTranspose(world));
        XMStoreFloat4x4(&mappedInstanceData[i].normalMat, XMMatrixTranspose(normalMat));
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
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = m_pipelineManager.GetRTVHandle(frameIndex);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = m_pipelineManager.GetDSVHandle();
    m_deviceContext.GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // Clear the canvas to a solid color to prevent ghosting from the previous frame
    const float clearColor[] = { 0.2f, 0.3f, 0.4f, 1.0f };
    m_deviceContext.GetCommandList()->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_deviceContext.GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set the Viewport and Scissor Rect (for viewport transformation and clipping before rasterization)
    m_deviceContext.GetCommandList()->RSSetViewports(1, &viewport);
    m_deviceContext.GetCommandList()->RSSetScissorRects(1, &scissorRect);
}

void D3D12App::DrawPBRModel()
{
    m_deviceContext.GetCommandList()->RSSetViewports(1, &viewport);
    m_deviceContext.GetCommandList()->RSSetScissorRects(1, &scissorRect);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv = m_pipelineManager.GetRTVHandle(frameIndex);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = m_pipelineManager.GetDSVHandle();
    m_deviceContext.GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    m_deviceContext.GetCommandList()->SetPipelineState(m_pipelineManager.GetPBR_PSO());

    auto& instances = m_resourceManager.GetSceneInstances();
    if (instances.empty()) return;

    // Set the Root Signature for data binding
    m_deviceContext.GetCommandList()->SetGraphicsRootSignature(m_pipelineManager.GetRootSignature());

    // Bind the Constant Buffer View (CBV) to the GPU to provide the resource locations
    ID3D12DescriptorHeap* heaps[] = { m_resourceManager.GetMainDescriptorHeap() };
    m_deviceContext.GetCommandList()->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = m_resourceManager.GetCBVGPUAddress(frameIndex);
    m_deviceContext.GetCommandList()->SetGraphicsRootConstantBufferView(0, baseGpuAddress);

    // Bind the per-object Constant Buffer (CBV) containing the MVP matrices and environment data
    m_deviceContext.GetCommandList()->SetGraphicsRootConstantBufferView(8, m_resourceManager.GetSHBufferGPUAddress());

    // Set the Primitive Topology (e.g., Triangle List) to define how vertices are connected
    m_deviceContext.GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind the IBL (Image-Based Lighting) textures
    CD3DX12_GPU_DESCRIPTOR_HANDLE hStart(m_resourceManager.GetMainDescriptorHeap()->GetGPUDescriptorHandleForHeapStart());
    UINT srvDescSize = m_resourceManager.GetSrvDescriptorSize();

    m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(5, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, m_resourceManager.GetIblPrefilterIdx(), srvDescSize));
    m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(6, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, m_resourceManager.GetIblBRDFIdx(), srvDescSize));

    m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(10, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, m_resourceManager.GetShadowSrvIdx(), srvDescSize));

    Model* currentModel = nullptr;
    UINT instanceStartOffset = 0;
    UINT currentInstanceCount = 0;

    // Iterate through all model instances that store transformations and model data addresses
    for (size_t i = 0; i <= instances.size(); ++i)
    {
        bool isEnd = (i == instances.size());
        Model* thisModel = isEnd ? nullptr : instances[i].pModel;

        if ((isEnd || thisModel != currentModel) && currentInstanceCount > 0 && currentModel != nullptr)
        {
            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (instanceStartOffset * sizeof(InstanceData));
            m_deviceContext.GetCommandList()->SetGraphicsRootShaderResourceView(9, srvAddress);

            for (auto& mesh : currentModel->meshes)
            {
                // Configure fallback textures for the GPU to prevent errors in case of loading failures
                UINT srvIdx[4] = { m_resourceManager.GetDummyAlbedoIdx(), m_resourceManager.GetDummyNormalIdx(), m_resourceManager.GetDummyORMIdx(), m_resourceManager.GetDummyEmissiveIdx() };
                bool hasMap[4] = { false, false, false, false };

                // Locate the texture assets and swap the placeholders with the actual model textures if available
                for (auto& tex : mesh.textures)
                {
                    if (tex.type == "texture_diffuse") { srvIdx[0] = m_resourceManager.GetTextureSrvIdx(tex.Resource.Get()); hasMap[0] = true; }
                    else if (tex.type == "texture_normal") { srvIdx[1] = m_resourceManager.GetTextureSrvIdx(tex.Resource.Get()); hasMap[1] = true; }
                    else if (tex.type == "texture_metallicRoughness" || tex.type == "texture_ao") { srvIdx[2] = m_resourceManager.GetTextureSrvIdx(tex.Resource.Get()); hasMap[2] = true; }
                    else if (tex.type == "texture_emissive") { srvIdx[3] = m_resourceManager.GetTextureSrvIdx(tex.Resource.Get()); hasMap[3] = true; } // 新增
                }

                // Pass a 'hasTexture' boolean flag to the shader to dictate whether to sample the texture or fallback to the default material properties
                UINT32 flags[4] = { (UINT32)hasMap[0], (UINT32)hasMap[1], (UINT32)hasMap[2], (UINT32)hasMap[3] };
                m_deviceContext.GetCommandList()->SetGraphicsRoot32BitConstants(7, 4, flags, 0);

                // Bind the final resolved texture
                m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(1, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[0], srvDescSize));
                m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(2, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[1], srvDescSize));
                m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(3, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[2], srvDescSize));
                m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(4, CD3DX12_GPU_DESCRIPTOR_HANDLE(hStart, srvIdx[3], srvDescSize));

                mesh.Draw(m_deviceContext.GetCommandList(), currentInstanceCount);
            }
        }

        if (!isEnd)
        {
            if (thisModel != currentModel)
            {
                currentModel = thisModel;
                instanceStartOffset = i;
                currentInstanceCount = 1;
            }
            else
            {
                currentInstanceCount++;
            }
        }
    }
}

void D3D12App::DrawSkybox()
{
    // Bind the PSO (Pipeline State Object) for Skybox rendering
    m_deviceContext.GetCommandList()->SetPipelineState(m_pipelineManager.GetSkybox_PSO());

    // Bind the vertex data directly
    D3D12_VERTEX_BUFFER_VIEW skyboxVBV = m_resourceManager.GetSkyboxVBV();
    m_deviceContext.GetCommandList()->IASetVertexBuffers(0, 1, &skyboxVBV);

    // Ensure the skybox remains centered relative to the camera
    XMMATRIX view = camera.GetViewMatrix();
    view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(camera.Zoom), (float)Width / Height, 0.1f, 1000.0f);
    XMMATRIX skyVPMat = XMMatrixTranspose(view * proj);

    // Pass the Skybox MVP matrix via registers (Root Constants)
    m_deviceContext.GetCommandList()->SetGraphicsRoot32BitConstants(7, 16, &skyVPMat, 0);

    // Bind the skybox texture (Cubemap)
    CD3DX12_GPU_DESCRIPTOR_HANDLE hEnvCube(m_resourceManager.GetMainDescriptorHeap()->GetGPUDescriptorHandleForHeapStart(), m_resourceManager.GetIblEnvCubeIdx(), m_resourceManager.GetSrvDescriptorSize());
    m_deviceContext.GetCommandList()->SetGraphicsRootDescriptorTable(1, hEnvCube);

    m_deviceContext.GetCommandList()->DrawInstanced(36, 1, 0, 0);
}

void D3D12App::DrawShadowMap()
{
    auto cmdList = m_deviceContext.GetCommandList();

    CD3DX12_RESOURCE_BARRIER toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resourceManager.GetShadowMap(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->ResourceBarrier(1, &toDepthWrite);

    cmdList->SetGraphicsRootSignature(m_pipelineManager.GetShadowRootSignature());
    cmdList->SetPipelineState(m_pipelineManager.GetShadowPSO());

    D3D12_VIEWPORT shadowViewport = { 0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f };
    D3D12_RECT shadowScissor = { 0, 0, 2048, 2048 };
    cmdList->RSSetViewports(1, &shadowViewport);
    cmdList->RSSetScissorRects(1, &shadowScissor);

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv = m_resourceManager.GetShadowDsvHandle();
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_GPU_VIRTUAL_ADDRESS baseGpuAddress = m_resourceManager.GetCBVGPUAddress(frameIndex);
    cmdList->SetGraphicsRootConstantBufferView(0, baseGpuAddress);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto& instances = m_resourceManager.GetSceneInstances();
    Model* currentModel = nullptr;
    UINT instanceStartOffset = 0;
    UINT currentInstanceCount = 0;

    for (size_t i = 0; i <= instances.size(); ++i)
    {
        bool isEnd = (i == instances.size());
        Model* thisModel = isEnd ? nullptr : instances[i].pModel;

        if ((isEnd || thisModel != currentModel) && currentInstanceCount > 0 && currentModel != nullptr)
        {
            D3D12_GPU_VIRTUAL_ADDRESS srvAddress = baseGpuAddress + 256 + (instanceStartOffset * sizeof(InstanceData));
            cmdList->SetGraphicsRootShaderResourceView(1, srvAddress);

            for (auto& mesh : currentModel->meshes)
            {
                mesh.Draw(cmdList, currentInstanceCount);
            }
        }

        if (!isEnd)
        {
            if (thisModel != currentModel)
            {
                currentModel = thisModel;
                instanceStartOffset = i;
                currentInstanceCount = 1;
            }
            else currentInstanceCount++;
        }
    }

    CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resourceManager.GetShadowMap(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSrv);
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
    DrawShadowMap();
    DrawPBRModel();
    DrawSkybox();
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