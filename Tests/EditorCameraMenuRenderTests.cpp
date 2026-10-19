#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "EditorCameraSnapshot.h"
#include <cstdarg>
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

// Test-only submission hooks record the real center-created controls. They add
// no widgets and never send operating-system input or touch its clipboard.
struct CameraMenuItem
{
    ImGuiID id{};
    ImRect bounds{};
    std::string label, window;
};
static std::vector<CameraMenuItem> cameraMenuItems;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id, const ImRect& bounds, const ImGuiLastItemData*)
{
    if (id) cameraMenuItems.push_back({id, bounds, {}, context->CurrentWindow ? context->CurrentWindow->Name : ""});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    for (auto item = cameraMenuItems.rbegin(); item != cameraMenuItems.rend(); ++item)
        if (item->id == id) { item->label = label ? label : ""; break; }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id)
{
    for (auto item = cameraMenuItems.rbegin(); item != cameraMenuItems.rend(); ++item)
        if (item->id == id) return item->label.c_str();
    return nullptr;
}

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
        std::string clipboard;
        UINT width = 0, height = 0;
        bool quit = false;
        float menuHeight = 0;

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
            auto* context = ImGui::GetCurrentContext();
            context->TestEngineHookItems = true;
            context->PlatformIO.Platform_ClipboardUserData = &clipboard;
            context->PlatformIO.Platform_SetClipboardTextFn = [](ImGuiContext* ctx, const char* text)
            { *static_cast<std::string*>(ctx->PlatformIO.Platform_ClipboardUserData) = text ? text : ""; };
            context->PlatformIO.Platform_GetClipboardTextFn = [](ImGuiContext* ctx)
            { return static_cast<std::string*>(ctx->PlatformIO.Platform_ClipboardUserData)->c_str(); };
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
            Require(foregroundVertices > 30, "actual menu text was not submitted");
        }

        void Frame(const EditorCameraSnapshot& snapshot, float delta = 1.0f / 30.0f)
        {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
            io.DisplayFramebufferScale = ImVec2(1, 1);
            io.DeltaTime = delta;
            ImGui_ImplDX12_NewFrame();
            ImGui::NewFrame();
            cameraMenuItems.clear();
            quit = EditorUICenter::MainMenu(menuHeight, snapshot);
            ImGui::Render();
            Require(ImGui::GetDrawData()->TotalVtxCount > 0, "camera menu produced no geometry");
            Capture(nullptr);
        }
        CameraMenuItem Item(const char* token) const
        {
            for (const auto& item : cameraMenuItems)
                if (item.label == token || item.label.find(token) != std::string::npos) return item;
            for (const auto& item : cameraMenuItems)
                std::cerr << "Submitted item: " << item.label << " in " << item.window << '\n';
            throw std::runtime_error(std::string("actual menu item was not submitted: ") + token);
        }
        void Hover(const CameraMenuItem& item, const EditorCameraSnapshot& snapshot, unsigned frames = 12)
        {
            const auto center = item.bounds.GetCenter();
            ImGui::GetIO().AddMousePosEvent(center.x, center.y);
            for (unsigned frame = 0; frame < frames; ++frame) Frame(snapshot);
        }
        bool Click(const CameraMenuItem& item, const EditorCameraSnapshot& snapshot)
        {
            Hover(item, snapshot, 2);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Frame(snapshot);
            bool activated = quit;
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Frame(snapshot);
            return activated || quit;
        }
        void OutsideClick(const EditorCameraSnapshot& snapshot)
        {
            ImGui::GetIO().AddMousePosEvent(static_cast<float>(width) - 4, static_cast<float>(height) - 4);
            Frame(snapshot);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Frame(snapshot);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Frame(snapshot);
        }
        size_t Capture(const char* name)
        {
            CheckForegroundBounds(*ImGui::GetDrawData(), width, height);
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
            if (name) SavePNG(std::filesystem::path("tmp/editor-camera-menu-render") / (std::string(name) + ".png"), width, height, pixels);
            size_t textPixels = 0;
            for (size_t offset = 0; offset < pixels.size(); offset += 4)
                if (pixels[offset] > 100 && std::abs(int(pixels[offset]) - int(pixels[offset + 1])) < 3 &&
                    std::abs(int(pixels[offset]) - int(pixels[offset + 2])) < 3) ++textPixels;
            Require(textPixels > 30, "camera menu text missing from actual GPU pixels");
            if (name) std::cout << name << ": " << width << 'x' << height << ", font=" << ImGui::GetCurrentContext()->FontSize
                << ", text_pixels=" << textPixels << '\n';
            return textPixels;
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
            Require(errors == 0, "D3D12 errors during camera menu render");
            return errors;
        }
    };
}

int main()
{
    try
    {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "initialize WIC COM");
        std::filesystem::create_directories("tmp/editor-camera-menu-render");
        Fixture fixture;
        const Camera camera({-0.3f, 1.05f, -1.1f}, {0.0f, 1.0f, 0.0f}, 116.565f, 68.542f);
        const auto snapshot = EditorCameraSnapshot::Capture(camera, 1707, 1067);
        const auto expected = snapshot.ToJson();
        std::ofstream report("tmp/editor-camera-menu-render/report.json");
        report << "{\n  \"component\": \"EditorUICenter::MainMenu and CameraTransformContent\",\n"
            << "  \"renderer\": \"actual ImGui DX12 backend, RGBA8 offscreen texture\",\n"
            << "  \"native_windows_created\": 0,\n  \"present_calls\": 0,\n  \"os_input_events\": 0,\n"
            << "  \"system_clipboard_writes\": 0,\n  \"cases\": [\n";
        bool first = true;
        for (float scale : {1.0f, 1.5f})
        {
            float fontAtWide = 0;
            for (UINT logicalWidth : {960u, 320u})
            {
                fixture.StartImGui(scale);
                fixture.width = static_cast<UINT>(logicalWidth * scale);
                fixture.height = static_cast<UINT>(720 * scale);
                fixture.clipboard = "test-local clipboard sentinel";
                ImGui::GetIO().AddMousePosEvent(-1000, -1000);
                fixture.Frame(snapshot);
                Require(!fixture.quit, "idle menu requested quit");
                fixture.Item("File");
                const auto edit = fixture.Item("Edit");
                fixture.Click(edit, snapshot);
                const auto camera = fixture.Item("##camera-transform-menu");
                fixture.Hover(camera, snapshot);
                const auto copy = fixture.Item("##camera-copy-json");
                Require(ImGui::GetCurrentContext()->OpenPopupStack.Size >= 2, "hover did not open camera submenu");
                fixture.Hover(copy, snapshot);
                Require(ImGui::GetCurrentContext()->OpenPopupStack.Size >= 2, "camera submenu closed while mouse entered it");
                const float font = ImGui::GetCurrentContext()->FontSize;
                if (logicalWidth == 960) fontAtWide = font;
                Require(std::abs(font - fontAtWide) < .01f, "window width changed menu font size");
                const std::string name = std::string(logicalWidth == 960 ? "wide-" : "narrow-") + (scale == 1 ? "100" : "150");
                fixture.Capture(name.c_str());
                fixture.Click(fixture.Item("##camera-copy-json"), snapshot);
                Require(fixture.clipboard == expected, "copy action did not export the live camera JSON exactly");
                Require(ImGui::GetCurrentContext()->OpenPopupStack.Size >= 2, "copy unexpectedly closed the temporary camera submenu");
                Require(snapshot.ToJson() == expected, "read-only camera content changed the supplied transform");
                std::ofstream exported(std::filesystem::path("tmp/editor-camera-menu-render") / (name + "-camera.json"));
                exported << fixture.clipboard;
                auto changedSnapshot = snapshot;
                changedSnapshot.position[0] = 17.125f;
                changedSnapshot.target[0] = 17.4336788f;
                fixture.Frame(changedSnapshot);
                fixture.Click(fixture.Item("##camera-copy-json"), changedSnapshot);
                Require(fixture.clipboard == changedSnapshot.ToJson(), "open camera submenu exported a stale camera snapshot");
                fixture.OutsideClick(snapshot);
                Require(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0, "clicking outside did not close temporary menus");
                fixture.Click(fixture.Item("File"), snapshot);
                Require(fixture.Click(fixture.Item("quit"), snapshot), "File quit no longer requests application exit");
                if (!first) report << ",\n";
                first = false;
                report << "    {\"dpi_scale\": " << scale << ", \"logical_width\": " << logicalWidth
                    << ", \"size\": [" << fixture.width << ", " << fixture.height << "], \"font_pixels\": " << font
                    << ", \"hover_submenu_passed\": true, \"enter_submenu_passed\": true, \"copy_json_exact\": true, "
                    << "\"copy_keeps_submenu_open\": true, \"live_snapshot_refresh_passed\": true, \"outside_click_closes\": true, \"file_quit_preserved\": true, "
                    << "\"foreground_bounds_passed\": true, \"width_preserves_font_size\": true}";
                fixture.FinishImGui();
            }
        }
        fixture.CheckErrors();
        report << "\n  ],\n  \"d3d12_errors\": 0,\n  \"completed\": true\n}\n";
        CoUninitialize();
        std::cout << "Actual camera menu acceptance passed; no HWND, swapchain, Present, OS input or system clipboard writes.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Editor camera menu render acceptance failed: " << error.what() << '\n';
        return 1;
    }
}
