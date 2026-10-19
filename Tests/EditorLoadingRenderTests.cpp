#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "EditorUICenter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace
{
    void Require(bool success, const char* message)
    {
        if (!success) throw std::runtime_error(message);
    }
    void Check(HRESULT result, const char* message)
    {
        if (FAILED(result)) throw std::runtime_error(std::string(message) + ": " + std::to_string(static_cast<unsigned long>(result)));
    }
    void SavePNG(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgba)
    {
        ComPtr<IWICImagingFactory> factory;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "create PNG factory");
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream), "create PNG stream");
        Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open PNG stream");
        ComPtr<IWICBitmapEncoder> encoder;
        Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "create PNG encoder");
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame;
        Check(encoder->CreateNewFrame(&frame, nullptr), "create PNG frame");
        Check(frame->Initialize(nullptr), "initialize PNG frame");
        Check(frame->SetSize(width, height), "set PNG dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        Check(frame->SetPixelFormat(&format), "set PNG pixel format");
        Require(format == GUID_WICPixelFormat32bppBGRA, "PNG encoder changed BGRA pixel format");
        auto bgra = rgba;
        for (size_t offset = 0; offset < bgra.size(); offset += 4) std::swap(bgra[offset], bgra[offset + 2]);
        Check(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data()), "write PNG pixels");
        Check(frame->Commit(), "commit PNG frame");
        Check(encoder->Commit(), "commit PNG");
    }
    struct Fixture
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> messages;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
        ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        uint64_t fenceValue = 0;
        UINT srvStride = 0;
        std::array<bool, 64> descriptors{};
        bool imguiInitialized = false;

        Fixture()
        {
            ComPtr<ID3D12Debug> debug;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "get D3D12 debug layer");
            debug->EnableDebugLayer();
            Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "create offscreen device");
            Check(device.As(&messages), "query D3D12 diagnostics");
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create offscreen queue");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create offscreen allocator");
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "create offscreen command list");
            Check(commands->Close(), "close initial command list");
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 1;
            Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)), "create offscreen RTV heap");
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = static_cast<UINT>(descriptors.size());
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)), "create ImGui SRV heap");
            srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create offscreen fence");
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Require(event != nullptr, "create offscreen fence event");
        }
        ~Fixture()
        {
            if (imguiInitialized) FinishImGui();
            if (event) CloseHandle(event);
        }
        void Wait()
        {
            Check(queue->Signal(fence.Get(), ++fenceValue), "signal offscreen frame");
            if (fence->GetCompletedValue() < fenceValue)
            {
                Check(fence->SetEventOnCompletion(fenceValue, event), "wait offscreen frame");
                Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "offscreen frame timeout");
            }
            Check(device->GetDeviceRemovedReason(), "offscreen device removed");
        }
        void StartImGui(float scale)
        {
            Require(!imguiInitialized, "ImGui context already active");
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().LogFilename = nullptr;
            EditorTheme::Apply(scale);
            ImGui_ImplDX12_InitInfo info{};
            info.Device = device.Get();
            info.CommandQueue = queue.Get();
            info.NumFramesInFlight = 1;
            info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            info.SrvDescriptorHeap = srvHeap.Get();
            info.UserData = this;
            info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
            {
                auto& fixture = *static_cast<Fixture*>(info->UserData);
                const auto first = std::find(fixture.descriptors.begin(), fixture.descriptors.end(), false);
                Require(first != fixture.descriptors.end(), "ImGui descriptor heap exhausted");
                const auto offset = static_cast<UINT>(std::distance(fixture.descriptors.begin(), first));
                *first = true;
                *cpu = fixture.srvHeap->GetCPUDescriptorHandleForHeapStart();
                *gpu = fixture.srvHeap->GetGPUDescriptorHandleForHeapStart();
                cpu->ptr += static_cast<SIZE_T>(offset) * fixture.srvStride;
                gpu->ptr += static_cast<UINT64>(offset) * fixture.srvStride;
            };
            info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
            {
                auto& fixture = *static_cast<Fixture*>(info->UserData);
                const auto index = (cpu.ptr - fixture.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / fixture.srvStride;
                Require(index < fixture.descriptors.size() && fixture.descriptors[index], "release invalid ImGui descriptor");
                fixture.descriptors[index] = false;
            };
            Require(ImGui_ImplDX12_Init(&info), "initialize actual ImGui DX12 backend");
            imguiInitialized = true;
        }
        void FinishImGui()
        {
            Wait();
            ImGui_ImplDX12_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized = false;
            Require(std::find(descriptors.begin(), descriptors.end(), true) == descriptors.end(), "ImGui SRV descriptor leak");
        }
        ComPtr<ID3D12Resource> Resource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE* clear = nullptr)
        {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = type;
            properties.CreationNodeMask = properties.VisibleNodeMask = 1;
            ComPtr<ID3D12Resource> resource;
            Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initial, clear, IID_PPV_ARGS(&resource)), "create offscreen resource");
            return resource;
        }
        static void CheckForegroundBounds(const ImDrawData& draw, UINT width, UINT height)
        {
            const ImU32 foreground[] = {EditorTheme::Color(EditorColors::Foreground), EditorTheme::Color(EditorColors::ForegroundHeader), EditorTheme::Color(EditorColors::Hover2)};
            size_t foregroundVertices = 0;
            for (const ImDrawList* list : draw.CmdLists)
                for (const ImDrawCmd& command : list->CmdBuffer)
                {
                    if (command.UserCallback) continue;
                    for (unsigned int index = 0; index < command.ElemCount; ++index)
                    {
                        const auto& vertex = list->VtxBuffer[list->IdxBuffer[command.IdxOffset + index] + command.VtxOffset];
                        if (std::find(std::begin(foreground), std::end(foreground), vertex.col) == std::end(foreground)) continue;
                        ++foregroundVertices;
                        Require(vertex.pos.x >= -0.5f && vertex.pos.x <= static_cast<float>(width) + 0.5f &&
                            vertex.pos.y >= -0.5f && vertex.pos.y <= static_cast<float>(height) + 0.5f, "foreground extends beyond viewport");
                        Require(vertex.pos.x >= command.ClipRect.x - 0.5f && vertex.pos.x <= command.ClipRect.z + 0.5f &&
                            vertex.pos.y >= command.ClipRect.y - 0.5f && vertex.pos.y <= command.ClipRect.w + 0.5f, "foreground clipped by component");
                    }
                }
            Require(foregroundVertices > 30, "actual loading text was not submitted");
        }
        size_t Render(const char* name, UINT width, UINT height, float progress, const char* status)
        {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
            io.DisplayFramebufferScale = ImVec2(1, 1);
            io.DeltaTime = 1.0f / 60;
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            EditorUICenter::LoadingScreen(progress, status);
            ImGui::Render();
            Require(ImGui::GetDrawData()->TotalVtxCount > 0, "loading component produced no geometry");
            for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
                std::cout << "Draw " << name << ": " << list->_OwnerName << ", vertices=" << list->VtxBuffer.Size << '\n';
            std::string boundsError;
            try { CheckForegroundBounds(*ImGui::GetDrawData(), width, height); }
            catch (const std::exception& error) { boundsError = error.what(); }

            D3D12_RESOURCE_DESC textureDesc{};
            textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.DepthOrArraySize = textureDesc.MipLevels = 1;
            textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = textureDesc.Format;
            clear.Color[0] = clear.Color[1] = clear.Color[2] = 21.0f / 255;
            clear.Color[3] = 1;
            auto texture = Resource(textureDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
            device->CreateRenderTargetView(texture.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = bytes;
            bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            auto readback = Resource(bufferDesc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            Check(allocator->Reset(), "reset offscreen allocator");
            Check(commands->Reset(allocator.Get(), nullptr), "reset offscreen command list");
            const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            commands->ClearRenderTargetView(rtv, clear.Color, 0, nullptr);
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commands.Get());
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commands->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source{}, target{};
            source.pResource = texture.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.pResource = readback.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint = footprint;
            commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            Check(commands->Close(), "close offscreen frame");
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            Wait();
            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            void* data = nullptr;
            const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
            Check(readback->Map(0, &range, &data), "map offscreen pixels");
            for (UINT y = 0; y < height; ++y)
                std::copy_n(static_cast<uint8_t*>(data) + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    static_cast<size_t>(width) * 4, pixels.data() + static_cast<size_t>(y) * width * 4);
            const D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            SavePNG(std::filesystem::path("tmp/editor-loading-render") / (std::string(name) + ".png"), width, height, pixels);
            if (!boundsError.empty()) throw std::runtime_error(std::string(name) + ": " + boundsError);
            size_t primaryPixels = 0, textPixels = 0;
            for (size_t offset = 0; offset < pixels.size(); offset += 4)
            {
                if (pixels[offset] < 20 && pixels[offset + 1] > 80 && pixels[offset + 1] < 180 && pixels[offset + 2] > 180) ++primaryPixels;
                if (pixels[offset] > 100 && std::abs(int(pixels[offset]) - int(pixels[offset + 1])) < 3 && std::abs(int(pixels[offset]) - int(pixels[offset + 2])) < 3) ++textPixels;
            }
            Require(textPixels > 30, "loading text missing from actual GPU pixels");
            std::cout << name << ": " << width << 'x' << height << ", font=" << ImGui::GetCurrentContext()->FontSize << ", primary_pixels=" << primaryPixels << ", text_pixels=" << textPixels << '\n';
            return primaryPixels;
        }
        UINT64 CheckErrors()
        {
            UINT64 errors = 0;
            for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index)
            {
                SIZE_T length = 0;
                Check(messages->GetMessage(index, nullptr, &length), "measure D3D12 message");
                std::vector<uint8_t> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(messages->GetMessage(index, message, &length), "read D3D12 message");
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                {
                    ++errors;
                    std::cerr << "D3D12: " << message->pDescription << '\n';
                }
            }
            Require(errors == 0, "D3D12 errors during loading UI render");
            return errors;
        }
    };
}

int main()
{
    try
    {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "initialize WIC COM");
        std::filesystem::create_directories("tmp/editor-loading-render");
        Fixture fixture;
        constexpr const char* longStatus = "正在构建 VXGI 探针缓存：生成间接光照并更新表面数据，完成初始化后进入场景。";
        std::ofstream report("tmp/editor-loading-render/report.json");
        report << "{\n  \"component\": \"EditorUICenter::LoadingScreen\",\n  \"renderer\": \"actual ImGui DX12 backend, RGBA8 offscreen texture\",\n  \"present_calls\": 0,\n  \"native_windows_created\": 0,\n  \"cases\": [\n";
        bool first = true;
        for (float scale : {1.0f, 1.5f})
        {
            fixture.StartImGui(scale);
            const std::string suffix = scale == 1 ? "100" : "150";
            const UINT width = static_cast<UINT>(640 * scale), height = static_cast<UINT>(360 * scale);
            const auto beginPixels = fixture.Render(("start-" + suffix).c_str(), width, height, 0, "Loading scene resources...");
            const float fontAtWide = ImGui::GetCurrentContext()->FontSize;
            const auto middlePixels = fixture.Render(("middle-" + suffix).c_str(), width, height, .62f, longStatus);
            const auto endPixels = fixture.Render(("complete-" + suffix).c_str(), width, height, 1, "Initialization complete");
            Require(beginPixels < middlePixels && middlePixels < endPixels, "actual GPU progress fill is not monotonic");
            fixture.FinishImGui();
            // Narrow layouts must also render correctly on their very first
            // frame, including the first dynamic CJK font atlas upload.
            fixture.StartImGui(scale);
            fixture.Render(("narrow-" + suffix).c_str(), static_cast<UINT>(320 * scale), static_cast<UINT>(240 * scale), .62f, longStatus);
            Require(std::abs(fontAtWide - ImGui::GetCurrentContext()->FontSize) < .01f, "window width changed font size");
            if (!first) report << ",\n";
            first = false;
            report << "    {\"dpi_scale\": " << scale << ", \"font_pixels\": " << fontAtWide << ", \"wide_size\": [" << width << ", " << height
                << "], \"narrow_size\": [" << static_cast<UINT>(320 * scale) << ", " << static_cast<UINT>(240 * scale)
                << "], \"progress_fill_pixels\": [" << beginPixels << ", " << middlePixels << ", " << endPixels << "], \"foreground_bounds_passed\": true, \"width_preserves_font_size\": true, \"wide_and_narrow_first_frames_passed\": true}";
            fixture.FinishImGui();
        }
        fixture.CheckErrors();
        report << "\n  ],\n  \"d3d12_errors\": 0,\n  \"completed\": true\n}\n";
        CoUninitialize();
        std::cout << "Actual loading UI render acceptance passed; no HWND, swapchain or Present.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Editor loading render acceptance failed: " << error.what() << '\n';
        return 1;
    }
}
