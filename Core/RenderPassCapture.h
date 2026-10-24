#pragma once

#include "stdafx.h"
#include "RDG.h"
#include "json.hpp"
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <limits>
#include <stdexcept>
#include <cctype>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// A capture only adds copies to the current graph. The ordinary frame fence
// retires them; Poll never signals a queue or waits for GPU completion.
class RenderPassCapture
{
public:
    using Json = nlohmann::json;
    struct RawOutput
    {
        std::string name;
        RDGTextureHandle texture;
        UINT subresource = UINT_MAX; // UINT_MAX exports every mip/array/plane.
        std::string interpretation;
    };

    explicit RenderPassCapture(UINT frameCount = 3) : m_jobs(frameCount) {}

    RDGPassHandle AddToGraph(RDGBuilder& graph, ID3D12Device* device, UINT slot,
        RDGTextureHandle previewRGBA8, std::span<const RawOutput> rawOutputs,
        const std::string& prefix, const Json& metadata)
    {
        if (!device || slot >= m_jobs.size() || IsBusy(slot) || prefix.empty())
            return FailPass("Invalid capture request or frame slot is busy.");
        auto* preview = graph.GetTextureResource(previewRGBA8);
        if (!preview || preview->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            preview->GetDesc().DepthOrArraySize != 1 || preview->GetDesc().MipLevels != 1)
            return FailPass("Capture preview must be a single RGBA8 texture.");
        try
        {
            if (std::filesystem::exists(std::filesystem::path(prefix)))
                return FailPass("Capture directory already exists; refusing to overwrite it.");
            auto job = std::make_shared<Job>();
            job->prefix = prefix;
            job->metadata = metadata;
            RDGPassParameters parameters;
            parameters.ReadCopySrc(previewRGBA8);
            if (!AddImage(device, *job, preview, 0, "preview", "display RGBA8", true)) return {};
            size_t outputIndex = 0;
            for (const auto& raw : rawOutputs)
            {
                auto* source = graph.GetTextureResource(raw.texture);
                if (!source) return FailPass("Capture source texture is unavailable.");
                const auto desc = source->GetDesc();
                D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format, 0};
                if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) || info.PlaneCount == 0)
                    return FailPass("Cannot query the capture source's format planes.");
                const UINT count = UINT(desc.MipLevels) * UINT(desc.DepthOrArraySize) * info.PlaneCount;
                if (raw.subresource != UINT_MAX && raw.subresource >= count)
                    return FailPass("Capture subresource is out of range.");
                parameters.ReadCopySrc(raw.texture);
                const UINT first = raw.subresource == UINT_MAX ? 0 : raw.subresource;
                const UINT end = raw.subresource == UINT_MAX ? count : first + 1;
                const std::string name = std::to_string(outputIndex++) + "-" + FileName(raw.name);
                for (UINT subresource = first; subresource < end; ++subresource)
                    if (!AddImage(device, *job, source, subresource,
                        name + (count > 1 ? "-sub" + std::to_string(subresource) : ""), raw.interpretation, false)) return {};
            }
            const auto pass = graph.AddPass("PassView.Capture", ERDGPassFlags::Copy | ERDGPassFlags::NeverCull,
                parameters, [job](ID3D12GraphicsCommandList* commands)
                {
                    for (const auto& image : job->images)
                    {
                        D3D12_TEXTURE_COPY_LOCATION from{};
                        from.pResource = image.source.Get();
                        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        from.SubresourceIndex = image.subresource;
                        D3D12_TEXTURE_COPY_LOCATION to{};
                        to.pResource = image.readback.Get();
                        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        to.PlacedFootprint = image.footprint;
                        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    }
                    job->recorded = true;
                });
            if (!pass.IsValid()) return FailPass("Cannot add the capture copy pass.");
            m_jobs[slot] = std::move(job);
            m_error.clear();
            return pass;
        }
        catch (const std::exception& error)
        {
            return FailPass(error.what());
        }
    }

    bool OnSubmitted(UINT slot, ID3D12Fence* fence, UINT64 value)
    {
        if (slot >= m_jobs.size()) return Fail("Capture submission slot is out of range.");
        auto& job = m_jobs[slot];
        if (!job) return true;
        if (job->submitted) return Fail("Capture was submitted twice.");
        if (!job->recorded || !fence || value == 0)
            return Fail("Capture submission requires recorded copies and a valid frame fence.");
        job->fence = fence;
        job->fenceValue = value;
        job->submitted = true;
        return true;
    }

    // Recorded but unsubmitted commands can be discarded safely. Submitted
    // copies remain alive until their fence completes, including on UI changes.
    void CancelRecorded(UINT slot)
    {
        if (slot < m_jobs.size() && m_jobs[slot] && !m_jobs[slot]->submitted) m_jobs[slot].reset();
    }

    bool Poll()
    {
        bool success = true;
        for (auto& job : m_jobs)
        {
            if (!job || !job->submitted) continue;
            const UINT64 completed = job->fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                success = Fail("Capture device was removed before readback completed.");
                job.reset();
            }
            else if (completed >= job->fenceValue)
            {
                if (!Save(*job)) success = false;
                job.reset();
            }
        }
        return success;
    }

    bool IsBusy(UINT slot) const { return slot < m_jobs.size() && bool(m_jobs[slot]); }
    const std::string& GetLastSavedPath() const { return m_lastSaved; }
    const std::string& GetLastError() const { return m_error; }

private:
    struct Image
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> source, readback;
        D3D12_RESOURCE_DESC description{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT subresource = 0, rows = 0;
        UINT64 rowBytes = 0, bytes = 0;
        std::string name, interpretation;
        bool preview = false;
    };
    struct Job
    {
        std::vector<Image> images;
        std::string prefix;
        Json metadata;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 fenceValue = 0;
        bool recorded = false, submitted = false;
    };

    static std::string FileName(const std::string& text)
    {
        std::string result;
        for (const unsigned char character : text)
            result += std::isalnum(character) || character == '-' || character == '_' ? char(character) : '_';
        return result.empty() ? "resource" : result;
    }
    bool Fail(const std::string& error)
    {
        m_error = "Pass capture: " + error;
        ErrorLog::Write(m_error);
        return false;
    }
    RDGPassHandle FailPass(const std::string& error) { Fail(error); return {}; }
    static void Check(HRESULT result, const char* message)
    {
        if (FAILED(result)) throw std::runtime_error(std::string(message) + " (HRESULT " + std::to_string(uint32_t(result)) + ")");
    }

    bool AddImage(ID3D12Device* device, Job& job, ID3D12Resource* source, UINT subresource,
        const std::string& name, const std::string& interpretation, bool preview)
    {
        Image image;
        image.source = source;
        image.description = source->GetDesc();
        image.subresource = subresource;
        image.name = name;
        image.interpretation = interpretation;
        image.preview = preview;
        if (image.description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || image.description.SampleDesc.Count != 1)
            return Fail("Capture supports single-sample 2D textures and arrays.");
        device->GetCopyableFootprints(&image.description, subresource, 1, 0, &image.footprint,
            &image.rows, &image.rowBytes, &image.bytes);
        if (!image.bytes || image.bytes == UINT64_MAX || image.rowBytes == 0 || image.rows == 0 ||
            image.rowBytes > image.footprint.Footprint.RowPitch ||
            image.rowBytes > SIZE_MAX / image.rows || image.bytes > SIZE_MAX)
            return Fail("Capture returned an invalid texture readback layout.");
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(image.bytes);
        const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image.readback));
        if (FAILED(result)) return Fail("Cannot allocate the capture readback buffer.");
        job.images.push_back(std::move(image));
        return true;
    }

    static std::vector<uint8_t> Read(const Image& image)
    {
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(image.bytes)};
        Check(image.readback->Map(0, &range, &mapped), "Cannot map the completed readback");
        std::vector<uint8_t> pixels(static_cast<size_t>(image.rowBytes) * image.rows);
        for (UINT row = 0; row < image.rows; ++row)
            std::memcpy(pixels.data() + static_cast<size_t>(row) * image.rowBytes,
                static_cast<const uint8_t*>(mapped) + image.footprint.Offset +
                static_cast<size_t>(row) * image.footprint.Footprint.RowPitch, static_cast<size_t>(image.rowBytes));
        const D3D12_RANGE none{0, 0};
        image.readback->Unmap(0, &none);
        return pixels;
    }

    static void WritePNG(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& pixels)
    {
        if (uint64_t(width) * height * 4 != pixels.size() || pixels.size() > UINT_MAX)
            throw std::runtime_error("PNG preview dimensions exceed WIC's supported buffer size.");
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> properties;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create WIC factory");
        Check(factory->CreateStream(&stream), "Create PNG stream");
        Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG preview");
        Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        Check(encoder->CreateNewFrame(&frame, &properties), "Create PNG frame");
        Check(frame->Initialize(properties.Get()), "Initialize PNG frame");
        Check(frame->SetSize(width, height), "Set PNG size");
        // The Windows PNG encoder natively accepts BGRA. Swap only its byte
        // order; the stored PNG still decodes to the original RGBA values.
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        Check(frame->SetPixelFormat(&format), "Set PNG BGRA format");
        if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed the requested BGRA format.");
        auto encoded = pixels;
        for (size_t pixel = 0; pixel < encoded.size(); pixel += 4)
            std::swap(encoded[pixel], encoded[pixel + 2]);
        Check(frame->WritePixels(height, width * 4, static_cast<UINT>(encoded.size()), encoded.data()), "Write PNG pixels");
        Check(frame->Commit(), "Commit PNG frame");
        Check(encoder->Commit(), "Commit PNG file");
    }

    bool Save(const Job& job)
    {
        try
        {
            const std::filesystem::path directory(job.prefix);
            if (!std::filesystem::create_directories(directory))
                return Fail("Capture directory already exists; refusing to overwrite it.");
            Json metadata = job.metadata;
            if (!metadata.is_object()) metadata = Json{{"context", metadata}};
            metadata["schema_version"] = 1;
            metadata["completed"] = true;
            metadata["readback_contract"] = "Original little-endian DXGI texture bytes, tightly packed rows without D3D12 padding. PNG is the displayed RGBA8 preview. No tone mapping, clamping or conversion is applied to raw files.";
            metadata["outputs"] = Json::array();
            for (const auto& image : job.images)
            {
                const auto pixels = Read(image);
                const std::string fileName = image.preview ? "preview.png" : image.name + ".raw";
                if (image.preview) WritePNG(directory / fileName, image.footprint.Footprint.Width,
                    image.footprint.Footprint.Height, pixels);
                else
                {
                    std::ofstream file(directory / fileName, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                    file.close();
                    if (!file) throw std::runtime_error("Cannot write capture raw file.");
                }
                const UINT mip = image.subresource % image.description.MipLevels;
                const UINT slice = (image.subresource / image.description.MipLevels) % image.description.DepthOrArraySize;
                const UINT plane = image.subresource / (UINT(image.description.MipLevels) * image.description.DepthOrArraySize);
                metadata["outputs"].push_back({{"file", fileName}, {"preview", image.preview},
                    {"format_dxgi", static_cast<UINT>(image.description.Format)},
                    {"width", image.footprint.Footprint.Width}, {"height", image.footprint.Footprint.Height},
                    {"subresource", image.subresource}, {"mip", mip}, {"array_slice", slice}, {"plane", plane},
                    {"row_bytes", image.rowBytes}, {"row_count", image.rows}, {"byte_count", pixels.size()},
                    {"interpretation", image.interpretation}});
            }
            std::ofstream output(directory / "metadata.json", std::ios::binary | std::ios::trunc);
            output << metadata.dump(2) << '\n';
            output.close();
            if (!output) throw std::runtime_error("Cannot write capture metadata.");
            m_lastSaved = (directory / "metadata.json").string();
            ErrorLog::Write("Pass capture saved: " + m_lastSaved);
            return true;
        }
        catch (const std::exception& error)
        {
            return Fail(error.what());
        }
    }

    std::vector<std::shared_ptr<Job>> m_jobs;
    std::string m_error, m_lastSaved;
};
