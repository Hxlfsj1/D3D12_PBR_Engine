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
    std::vector<ComPtr<ID3D12Fence>> m_fence;
    std::vector<UINT64> m_fenceValue;
    HANDLE m_fenceEvent;
};

#endif