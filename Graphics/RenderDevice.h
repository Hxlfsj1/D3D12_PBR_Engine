/*
1. Hardware Abstraction: Initialize the core D3D12 device, DXGI factory, and select the optimal physical adapter
2. Command Infrastructure: Orchestrate the lifecycle of command queues, allocators, and lists for GPU-side execution
3. Presentation & Sync: Govern swap chain buffers and enforce rigorous CPU-GPU synchronization using fence primitives
*/

#ifndef RENDER_DEVICE_H
#define RENDER_DEVICE_H

#include "stdafx.h"
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

class RenderDevice
{
public:
    RenderDevice()
    {
        m_fenceEvent = NULL;
    }

    ~RenderDevice()
    {
        if (m_fenceEvent != nullptr)
        {
            CloseHandle(m_fenceEvent);
        }
    }

    bool Initialize(HWND hwnd, int width, int height, int frameBufferCount)
    {
        m_renderTargets.resize(frameBufferCount);
        m_commandAllocator.resize(frameBufferCount);
        m_fence.resize(frameBufferCount);
        m_fenceValue.resize(frameBufferCount, 0);

        if (!CreateDevice()) return false;
        if (!CreateCommandObjects(frameBufferCount)) return false;
        if (!CreateSwapChain(hwnd, width, height, frameBufferCount)) return false;
        if (!CreateDepthBuffer(width, height)) return false;

        return true;
    }

    bool CreateDevice()
    {
#if defined(_DEBUG)
        // Activate the D3D12 validation layer BEFORE any device is created;
        // every subsequent API misuse is then reported to the debug output
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
        }
        else
        {
            ErrorLog::Write(
                "RenderDevice: the D3D12 debug layer is unavailable "
                "(enable the Windows Graphics Tools optional feature).");
        }
#endif

        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: CreateDXGIFactory1 failed.", hr);
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> bestAdapter;
        SIZE_T maxVram = 0;
        int adapterIndex = 0;
        bool adapterFound = false;

        // Enumerate all graphics adapters
        while ((hr = dxgiFactory->EnumAdapters1(adapterIndex, &adapter)) != DXGI_ERROR_NOT_FOUND)
        {
            if (FAILED(hr))
            {
                ErrorLog::HRESULT("RenderDevice: EnumAdapters1 failed while enumerating adapters.", hr);
                // A hard enumeration failure invalidates the adapter output; stop enumerating
                break;
            }

            DXGI_ADAPTER_DESC1 desc = {};
            hr = adapter->GetDesc1(&desc);
            if (FAILED(hr))
            {
                ErrorLog::HRESULT("RenderDevice: IDXGIAdapter1::GetDesc1 failed.", hr);
                // Skip this adapter instead of reading an uninitialized descriptor
                adapterIndex++;
                continue;
            }

            // Filter out the software rasterizer
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapterIndex++;
                continue;
            }

            // Feature Level Audit: Support for D3D11 features and D3D12 execution
            hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr);

            if (SUCCEEDED(hr))
            {
                // Select the optimal adapter based on dedicated video memory
                if (desc.DedicatedVideoMemory > maxVram)
                {
                    maxVram = desc.DedicatedVideoMemory;
                    bestAdapter = adapter;
                    adapterFound = true;
                }
            }

            adapterIndex++;
        }

        if (!adapterFound || bestAdapter == nullptr)
        {
            ErrorLog::Write(
                "RenderDevice: no compatible hardware adapter was found for D3D_FEATURE_LEVEL_11_0.");
            return false;
        }

        // The graphics device is officially initialized and operational
        hr = D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: D3D12CreateDevice failed for the selected adapter.", hr);
            return false;
        }

#if defined(_DEBUG)
        // Hook up the validation message queue. GPU corruption always breaks
        // into the debugger; ordinary API errors only break when the flag is
        // flipped to true (the break lands on the exact offending call)
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            constexpr bool kBreakOnDebugLayerError = false;
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue->SetBreakOnSeverity(
                D3D12_MESSAGE_SEVERITY_ERROR,
                kBreakOnDebugLayerError ? TRUE : FALSE);
        }
#endif

        return true;
    }

    bool CreateCommandObjects(int frameBufferCount)
    {
        // Define the main Command Queue
        D3D12_COMMAND_QUEUE_DESC cqDesc = {};
        cqDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        HRESULT hr = device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&commandQueue));

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: CreateCommandQueue failed.", hr);
            return false;
        }

        // Command Allocator : A backing buffer for CPU - side command recording
        for (int i = 0; i < frameBufferCount; i++)
        {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator[i]));

            if (FAILED(hr))
            {
                ErrorLog::HRESULT(
                    "RenderDevice: CreateCommandAllocator failed for frame index " +
                    std::to_string(i) + '.',
                    hr);
                return false;
            }
        }

        // Manage Command Allocators using a Command List (similar to a swapchain mechanism)
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator[0].Get(), NULL, IID_PPV_ARGS(&commandList));

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: CreateCommandList failed.", hr);
            return false;
        }

        // Set up fences to ensure synchronization
        for (int i = 0; i < frameBufferCount; i++)
        {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence[i]));

            if (FAILED(hr))
            {
                ErrorLog::HRESULT(
                    "RenderDevice: CreateFence failed for frame index " +
                    std::to_string(i) + '.',
                    hr);
                return false;
            }

            m_fenceValue[i] = 0;
        }

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        if (!m_fenceEvent)
        {
            ErrorLog::Win32("RenderDevice: CreateEvent failed.", GetLastError());
            return false;
        }

        return true;
    }

    bool CreateSwapChain(HWND hwnd, int width, int height, int frameBufferCount)
    {
        DXGI_MODE_DESC backBufferDesc = {};
        backBufferDesc.Width = width;
        backBufferDesc.Height = height;
        backBufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        DXGI_SAMPLE_DESC sampleDesc = {};
        sampleDesc.Count = 1;

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = frameBufferCount;
        swapChainDesc.BufferDesc = backBufferDesc;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // Implement "Flip Discard" mode rather than manual pixel blitting
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // The interface point between the API and the underlying OS
        swapChainDesc.OutputWindow = hwnd;
        swapChainDesc.SampleDesc = sampleDesc;
        swapChainDesc.Windowed = true;

        ComPtr<IDXGISwapChain> tempSwapChain;
        // Retrieve the submission progress from the Command Queue, ensure completion before swapchain presentation
        HRESULT hr = dxgiFactory->CreateSwapChain(commandQueue.Get(), &swapChainDesc, &tempSwapChain);

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: CreateSwapChain failed.", hr);
            return false;
        }

        // Acquire an idle back buffer and signal the GPU to begin rendering
        hr = tempSwapChain.As(&swapChain);
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: failed to query IDXGISwapChain3.", hr);
            // Continuing with a null swap chain would crash in GetBuffer below
            return false;
        }

        for (int i = 0; i < frameBufferCount; i++)
        {
            hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
            if (FAILED(hr))
            {
                ErrorLog::HRESULT(
                    "RenderDevice: GetBuffer failed for back-buffer index " +
                    std::to_string(i) + '.',
                    hr);
                // A partially populated back-buffer set is not presentable
                return false;
            }
        }

        return true;
    }

    bool CreateDepthBuffer(int width, int height)
    {
        D3D12_CLEAR_VALUE depthClearValue = {};
        depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthClearValue.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES dsvHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC dsvResDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS,
            width,
            height,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        const HRESULT hr = device->CreateCommittedResource(
            &dsvHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &dsvResDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(&depthStencilBuffer));

        if (FAILED(hr))
        {
            ErrorLog::HRESULT("RenderDevice: CreateCommittedResource failed for the depth buffer.", hr);
            return false;
        }

        return true;
    }

    // Track GPU progress, prevent data updates until execution is complete
    void WaitForPreviousFrame(int frameIndex)
    {
        if (m_fence[frameIndex]->GetCompletedValue() < m_fenceValue[frameIndex])
        {
            const HRESULT hr = m_fence[frameIndex]->SetEventOnCompletion(
                m_fenceValue[frameIndex],
                m_fenceEvent);
            if (FAILED(hr))
            {
                ErrorLog::HRESULT(
                    "RenderDevice: SetEventOnCompletion failed while waiting for frame " +
                    std::to_string(frameIndex) + '.',
                    hr);
                // The event was never armed; waiting on it would hang the CPU indefinitely
                return;
            }
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    ID3D12Device* GetDevice() { return device.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() { return commandList.Get(); }
    ID3D12CommandQueue* GetCommandQueue() { return commandQueue.Get(); }
    IDXGISwapChain3* GetSwapChain() { return swapChain.Get(); }
    ID3D12Resource* GetRenderTarget(int i) { return m_renderTargets[i].Get(); }
    ID3D12CommandAllocator* GetCommandAllocator(int i) { return m_commandAllocator[i].Get(); }
    ID3D12Resource* GetDepthStencilBuffer() { return depthStencilBuffer.Get(); }

    ID3D12Fence* GetFence(int i) { return m_fence[i].Get(); }
    UINT64& GetFenceValue(int i) { return m_fenceValue[i]; }
    HANDLE GetFenceEvent() { return m_fenceEvent; }

public:
    ComPtr<IDXGIFactory4> dxgiFactory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<IDXGISwapChain3> swapChain;

private:
    std::vector<ComPtr<ID3D12Resource>> m_renderTargets;
    std::vector<ComPtr<ID3D12CommandAllocator>> m_commandAllocator;

    ComPtr<ID3D12Resource> depthStencilBuffer;

    std::vector<ComPtr<ID3D12Fence>> m_fence;
    std::vector<UINT64> m_fenceValue;
    HANDLE m_fenceEvent;
};

#endif
