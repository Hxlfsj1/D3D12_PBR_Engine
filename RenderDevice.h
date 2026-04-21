/*
1. Hardware Abstraction: Initialize the core D3D12 device, DXGI factory, and select the optimal physical adapter
2. Command Infrastructure: Orchestrate the lifecycle of command queues, allocators, and lists for GPU-side execution
3. Presentation & Sync: Govern swap chain buffers and enforce rigorous CPU-GPU synchronization using fence primitives
*/

#ifndef RENDER_DEVICE_H
#define RENDER_DEVICE_H

#include "stdafx.h"
#include <wrl/client.h>
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
        if (!CreateFrameViews(width, height, frameBufferCount)) return false;

        return true;
    }

    bool CreateDevice()
    {
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

        if (FAILED(hr))
        {
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> bestAdapter;
        SIZE_T maxVram = 0;
        int adapterIndex = 0;
        bool adapterFound = false;

        // Enumerate all graphics adapters
        while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

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
            return false;
        }

        // The graphics device is officially initialized and operational
        hr = D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

        if (FAILED(hr))
        {
            return false;
        }

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
            return false;
        }

        // Command Allocator : A backing buffer for CPU - side command recording
        for (int i = 0; i < frameBufferCount; i++)
        {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator[i]));

            if (FAILED(hr))
            {
                return false;
            }
        }

        // Manage Command Allocators using a Command List (similar to a swapchain mechanism)
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator[0].Get(), NULL, IID_PPV_ARGS(&commandList));

        if (FAILED(hr))
        {
            return false;
        }

        // Set up fences to ensure synchronization
        for (int i = 0; i < frameBufferCount; i++)
        {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence[i]));

            if (FAILED(hr))
            {
                return false;
            }

            m_fenceValue[i] = 0;
        }

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        if (!m_fenceEvent)
        {
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
            return false;
        }

        // Acquire an idle back buffer and signal the GPU to begin rendering
        tempSwapChain.As(&swapChain);

        for (int i = 0; i < frameBufferCount; i++)
        {
            swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        }

        return true;
    }

    bool CreateFrameViews(int width, int height, int frameBufferCount)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = frameBufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvDescriptorHeap));
        if (FAILED(hr))
        {
            return false;
        }

        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < frameBufferCount; i++)
        {
            device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, rtvDescriptorSize);
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsDescriptorHeap));
        if (FAILED(hr))
        {
            return false;
        }

        D3D12_CLEAR_VALUE depthClearValue = {};
        depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthClearValue.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES dsvHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC dsvResDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_D32_FLOAT,
            width,
            height,
            1,
            0,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        hr = device->CreateCommittedResource(
            &dsvHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &dsvResDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(&depthStencilBuffer));

        if (FAILED(hr))
        {
            return false;
        }

        device->CreateDepthStencilView(
            depthStencilBuffer.Get(),
            nullptr,
            dsDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        return true;
    }


    // Track GPU progress, prevent data updates until execution is complete
    void WaitForPreviousFrame(int frameIndex)
    {
        if (m_fence[frameIndex]->GetCompletedValue() < m_fenceValue[frameIndex])
        {
            m_fence[frameIndex]->SetEventOnCompletion(m_fenceValue[frameIndex], m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    ID3D12Device* GetDevice() { return device.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() { return commandList.Get(); }
    ID3D12CommandQueue* GetCommandQueue() { return commandQueue.Get(); }
    IDXGISwapChain3* GetSwapChain() { return swapChain.Get(); }
    ID3D12Resource* GetRenderTarget(int i) { return m_renderTargets[i].Get(); }
    ID3D12CommandAllocator* GetCommandAllocator(int i) { return m_commandAllocator[i].Get(); }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetRTVHandle(int frameIndex)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetDSVHandle()
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(dsDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    }

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

    UINT rtvDescriptorSize = 0;
    ComPtr<ID3D12DescriptorHeap> rtvDescriptorHeap;
    ComPtr<ID3D12Resource> depthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> dsDescriptorHeap;

    std::vector<ComPtr<ID3D12Fence>> m_fence;
    std::vector<UINT64> m_fenceValue;
    HANDLE m_fenceEvent;
};

#endif