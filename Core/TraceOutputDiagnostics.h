#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "../ThirdParty/json.hpp"

// Optional diagnostics for the production Trace UAV. The caller must invoke
// Capture immediately after its dispatch while subresource zero is still UAV.
// Capture records copies only: no allocation, submission, fence signal or wait.
// Flush requires both captured command lists to have been submitted and every
// application frame fence drained. Do not use this helper in ordinary runs.
class TraceOutputDiagnostics
{
public:
    using Json = nlohmann::json;
    static constexpr uint64_t JointSamplePeriod = 576; // LCM(STBN 64, DLSS jitter 18).

    bool Initialize(ID3D12Device* device, UINT width, UINT height,
        DXGI_FORMAT format, const std::string& outputPrefix, uint64_t firstSamplePhase = JointSamplePeriod)
    {
        if (m_initialized) return Fail("Trace output diagnostics were initialized twice.", E_UNEXPECTED);
        m_outputPrefix = outputPrefix;
        m_width = width;
        m_height = height;
        m_firstSamplePhase = firstSamplePhase;
        if (!device || !width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            format != DXGI_FORMAT_R16G16B16A16_FLOAT || outputPrefix.empty())
            return Fail("Trace output diagnostics require a device, RGBA16F dimensions and output prefix.", E_INVALIDARG);
        const uint64_t tightBytes = uint64_t(width) * uint64_t(height) * BytesPerPixel;
        if (tightBytes > (std::numeric_limits<size_t>::max)() ||
            tightBytes > uint64_t((std::numeric_limits<std::streamsize>::max)()))
            return Fail("Trace output dimensions exceed CPU/file addressable size.", E_INVALIDARG);

        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = width;
        texture.Height = height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = format;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        UINT rows = 0;
        UINT64 rowBytes = 0;
        device->GetCopyableFootprints(&texture, 0, 1, 0, &m_footprint, &rows, &rowBytes, &m_readbackBytes);
        if (m_readbackBytes == (std::numeric_limits<UINT64>::max)() || !m_readbackBytes ||
            m_readbackBytes > (std::numeric_limits<SIZE_T>::max)() || rows != height ||
            rowBytes != uint64_t(width) * BytesPerPixel || m_footprint.Footprint.RowPitch < rowBytes ||
            m_footprint.Offset > m_readbackBytes ||
            uint64_t(height - 1) * m_footprint.Footprint.RowPitch + rowBytes > m_readbackBytes - m_footprint.Offset)
            return Fail("GetCopyableFootprints returned an invalid RGBA16F readback layout.", E_FAIL);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = m_readbackBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto& capture : m_captures)
        {
            const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.readback));
            if (FAILED(hr)) return Fail("Creating a Trace output readback buffer failed.", hr);
        }
        m_initialized = true;
        return true;
    }

    bool IsEnabled() const { return m_initialized; }
    bool HasFailed() const { return m_failed; }
    const std::string& GetLastError() const { return m_error; }
    HRESULT GetLastHRESULT() const { return m_lastHRESULT; }
    size_t GetCaptureCount() const { return m_captureCount; }
    std::string GetReportPath() const { return m_outputPrefix + "-rawgi-report.json"; }

    // frameIndex is the logical sampling frame, not the triple-buffer slot.
    // The first eligible frame at/after 6 seconds and a matching modulo-576
    // frame at/after 12 seconds are captured, both with worldMode exactly 2.
    bool Capture(ID3D12GraphicsCommandList* commands, ID3D12Resource* raw,
        uint64_t frameIndex, double timeSeconds, uint32_t worldMode)
    {
        if (!m_initialized) return !m_failed;
        if (m_failed) return false;
        if (m_flushed) return Fail("Trace output capture was requested after Flush.", E_UNEXPECTED);
        if (m_captureCount == m_captures.size()) return true;
        if (!std::isfinite(timeSeconds) || timeSeconds < 0)
            return Fail("Trace output capture received an invalid elapsed time.", E_INVALIDARG);
        if (worldMode != 2 || timeSeconds < (m_captureCount == 0 ? 6.0 : 12.0)) return true;
        if (m_captureCount == 0 && m_firstSamplePhase < JointSamplePeriod &&
            frameIndex % JointSamplePeriod != m_firstSamplePhase) return true;
        if (m_captureCount == 1 && frameIndex % JointSamplePeriod != m_captures[0].frame % JointSamplePeriod)
            return true;
        if (!commands || !raw) return Fail("Trace output capture requires a command list and raw UAV.", E_POINTER);
        const auto desc = raw->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != m_width ||
            desc.Height != m_height || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            return Fail("Trace output UAV does not match the initialized readback dimensions/format.", E_INVALIDARG);

        auto& capture = m_captures[m_captureCount];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = raw;
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = raw;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = capture.readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m_footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commands->ResourceBarrier(1, &barrier);
        capture.frame = frameIndex;
        capture.timeSeconds = timeSeconds;
        capture.worldMode = worldMode;
        ++m_captureCount;
        return true;
    }

    // Call this if recorded captures were abandoned rather than submitted.
    void SubmissionFailed(const std::string& reason)
    {
        Fail("Trace output capture submission failed: " + reason, E_FAIL);
    }

    bool Flush()
    {
        if (!m_initialized && !m_failed) return true;
        if (m_flushed) return !m_failed;
        m_flushed = true;
        Json report = {
            {"schema_version", 1}, {"width", m_width}, {"height", m_height},
            {"format", "DXGI_FORMAT_R16G16B16A16_FLOAT"}, {"tight_bytes_per_pixel", BytesPerPixel},
            {"readback_row_pitch", m_footprint.Footprint.RowPitch}, {"capture_count", m_captureCount},
            {"joint_sample_period", JointSamplePeriod}, {"captures", Json::array()},
            {"recorded_captures", Json::array()},
            {"contract", "Two production RawGI snapshots. Capture only records UAV-to-COPY_SOURCE-to-UAV copies. "
                "The caller must drain all submitted frame fences before Flush. Compact binaries are little-endian "
                "RGBA16F without row padding. Same logical frame modulo 576 matches STBN64/DLSS18 phase; "
                "camera, scene and other settings must also remain unchanged. FNV-1a64 is a diagnostic hash; "
                "exact byte/pixel comparisons, rather than hash equality, establish equality."}
        };
        try
        {
            for (size_t index = 0; index < m_captureCount; ++index)
                report["recorded_captures"].push_back({{"index", index}, {"frame", m_captures[index].frame},
                    {"time_s", m_captures[index].timeSeconds}, {"world_mode", m_captures[index].worldMode},
                    {"sample_phase", m_captures[index].frame % JointSamplePeriod}});
            const auto parent = std::filesystem::path(GetReportPath()).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            if (m_captureCount != m_captures.size() && !m_failed)
                Fail("Both requested Trace output captures were not recorded before Flush.", E_FAIL);
            std::array<std::vector<uint8_t>, 2> pixels;
            if (!m_failed)
            {
                bool finite = true;
                for (size_t index = 0; index < m_captures.size(); ++index)
                {
                    if (!ReadCapture(index, pixels[index])) break;
                    const std::string path = m_outputPrefix + "-rawgi-" + std::to_string(index) + ".rgba16f";
                    if (!WriteBinary(path, pixels[index])) break;
                    auto statistics = ImageStatistics(pixels[index]);
                    finite = finite && statistics.at("finite").get<bool>();
                    statistics["frame"] = m_captures[index].frame;
                    statistics["time_s"] = m_captures[index].timeSeconds;
                    statistics["world_mode"] = m_captures[index].worldMode;
                    statistics["sample_phase"] = m_captures[index].frame % JointSamplePeriod;
                    statistics["path"] = path;
                    report["captures"].push_back(std::move(statistics));
                }
                if (!m_failed)
                {
                    report["comparison"] = Compare(pixels[0], pixels[1]);
                    report["matching_sample_phase"] = m_captures[0].frame % JointSamplePeriod ==
                        m_captures[1].frame % JointSamplePeriod;
                    if (!finite) Fail("A captured Trace output contains a nonfinite channel.", E_FAIL);
                }
            }
        }
        catch (const std::exception& error)
        {
            Fail(std::string("Trace output Flush failed: ") + error.what(), E_FAIL);
        }
        report["failed"] = m_failed;
        report["passed"] = !m_failed;
        report["error"] = m_error;
        report["hresult"] = HRESULTText(m_lastHRESULT);
        try
        {
            std::ofstream output(GetReportPath(), std::ios::binary | std::ios::trunc);
            if (!output) return Fail("Opening the Trace output JSON report failed.", E_FAIL);
            output << report.dump(2) << '\n';
            output.close();
            if (!output) return Fail("Writing the Trace output JSON report failed.", E_FAIL);
        }
        catch (const std::exception& error)
        {
            return Fail(std::string("Writing the Trace output report failed: ") + error.what(), E_FAIL);
        }
        return !m_failed;
    }

private:
    static constexpr uint32_t BytesPerPixel = 8;
    struct Snapshot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        uint64_t frame = 0;
        double timeSeconds = 0;
        uint32_t worldMode = 0;
    };

    bool Fail(const std::string& error, HRESULT hr)
    {
        if (!m_failed) { m_error = error; m_lastHRESULT = hr; }
        m_failed = true;
        return false;
    }

    static std::string HRESULTText(HRESULT hr)
    {
        std::ostringstream text;
        text << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << uint32_t(hr);
        return text.str();
    }

    bool ReadCapture(size_t index, std::vector<uint8_t>& output)
    {
        const size_t rowBytes = size_t(m_width) * BytesPerPixel;
        output.resize(rowBytes * m_height);
        D3D12_RANGE readRange{ SIZE_T(m_footprint.Offset), SIZE_T(m_footprint.Offset +
            uint64_t(m_height - 1) * m_footprint.Footprint.RowPitch + rowBytes) };
        void* mapped = nullptr;
        const HRESULT hr = m_captures[index].readback->Map(0, &readRange, &mapped);
        if (FAILED(hr)) return Fail("Mapping a completed Trace output readback failed.", hr);
        if (!mapped)
        {
            const D3D12_RANGE noWrites{ 0, 0 };
            m_captures[index].readback->Unmap(0, &noWrites);
            return Fail("Mapping a Trace output readback returned a null pointer.", E_POINTER);
        }
        const auto* source = static_cast<const uint8_t*>(mapped) + m_footprint.Offset;
        for (UINT y = 0; y < m_height; ++y)
            std::memcpy(output.data() + size_t(y) * rowBytes,
                source + size_t(y) * m_footprint.Footprint.RowPitch, rowBytes);
        const D3D12_RANGE noWrites{ 0, 0 };
        m_captures[index].readback->Unmap(0, &noWrites);
        return true;
    }

    bool WriteBinary(const std::string& path, const std::vector<uint8_t>& pixels)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return Fail("Opening a compact Trace output binary failed.", E_FAIL);
        output.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
        output.close();
        return output ? true : Fail("Writing a compact Trace output binary failed.", E_FAIL);
    }

    static float Half(uint16_t bits)
    {
        const uint32_t exponent = (bits >> 10) & 31u;
        const uint32_t mantissa = bits & 1023u;
        float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
            exponent == 31 ? (mantissa ? (std::numeric_limits<float>::quiet_NaN)() :
                (std::numeric_limits<float>::infinity)()) : std::ldexp(float(1024u + mantissa), int(exponent) - 25);
        return bits & 0x8000u ? -value : value;
    }

    static uint16_t Channel(const std::vector<uint8_t>& bytes, size_t channel)
    {
        return uint16_t(uint16_t(bytes[channel * 2]) | (uint16_t(bytes[channel * 2 + 1]) << 8));
    }

    static std::string Hash(const std::vector<uint8_t>& bytes)
    {
        uint64_t hash = 14695981039346656037ull;
        for (const uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
        std::ostringstream text;
        text << std::hex << std::setw(16) << std::setfill('0') << hash;
        return text.str();
    }

    static Json ImageStatistics(const std::vector<uint8_t>& bytes)
    {
        const size_t count = bytes.size() / BytesPerPixel;
        uint64_t finitePixels = 0, nonzeroRGB = 0, nonzeroHit = 0, positiveHit = 0, invalidHitRange = 0;
        std::array<double, 4> sums{};
        std::array<uint64_t, 4> finiteChannels{};
        for (size_t pixel = 0; pixel < count; ++pixel)
        {
            bool finite = true, finiteRGB = true, nonzero = false;
            for (size_t channel = 0; channel < 4; ++channel)
            {
                const float value = Half(Channel(bytes, pixel * 4 + channel));
                const bool good = std::isfinite(value);
                finite = finite && good;
                if (good) { sums[channel] += value; ++finiteChannels[channel]; }
                if (channel < 3) { finiteRGB = finiteRGB && good; nonzero = nonzero || (good && value != 0); }
                else if (good)
                {
                    nonzeroHit += value != 0;
                    positiveHit += value > 0;
                    invalidHitRange += value < 0 || value > 1;
                }
            }
            finitePixels += finite;
            nonzeroRGB += finiteRGB && nonzero;
        }
        Json means = Json::array();
        for (size_t channel = 0; channel < 4; ++channel)
            means.push_back(finiteChannels[channel] ? Json(sums[channel] / double(finiteChannels[channel])) : Json(nullptr));
        return {{"pixels", count}, {"bytes", bytes.size()}, {"finite", finitePixels == count},
            {"finite_pixels", finitePixels}, {"nonfinite_pixels", count - finitePixels},
            {"finite_channel_counts", finiteChannels}, {"mean_rgba_over_finite_channels", std::move(means)},
            {"nonzero_rgb_pixels", nonzeroRGB}, {"nonzero_hit_fraction_pixels", nonzeroHit},
            {"positive_hit_fraction_pixels", positiveHit}, {"hit_fraction_outside_unit_range_pixels", invalidHitRange},
            {"fnv1a64", Hash(bytes)}};
    }

    static Json Compare(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after)
    {
        uint64_t exactPixels = 0, exactChannels = 0, exactBytes = 0;
        std::array<double, 4> absolute{}, squared{}, maximum{};
        std::array<uint64_t, 4> finitePairs{};
        const size_t pixels = before.size() / BytesPerPixel;
        for (size_t index = 0; index < before.size(); ++index) exactBytes += before[index] == after[index];
        for (size_t pixel = 0; pixel < pixels; ++pixel)
        {
            exactPixels += std::memcmp(before.data() + pixel * BytesPerPixel,
                after.data() + pixel * BytesPerPixel, BytesPerPixel) == 0;
            for (size_t channel = 0; channel < 4; ++channel)
            {
                const uint16_t a = Channel(before, pixel * 4 + channel), b = Channel(after, pixel * 4 + channel);
                exactChannels += a == b;
                const double first = Half(a), last = Half(b);
                if (!std::isfinite(first) || !std::isfinite(last)) continue;
                const double difference = std::abs(last - first);
                ++finitePairs[channel];
                absolute[channel] += difference;
                squared[channel] += difference * difference;
                if (difference > maximum[channel]) maximum[channel] = difference;
            }
        }
        Json mae = Json::array(), rmse = Json::array();
        for (size_t channel = 0; channel < 4; ++channel)
        {
            mae.push_back(finitePairs[channel] ? Json(absolute[channel] / double(finitePairs[channel])) : Json(nullptr));
            rmse.push_back(finitePairs[channel] ? Json(std::sqrt(squared[channel] / double(finitePairs[channel]))) : Json(nullptr));
        }
        const double rgbAbsolute = absolute[0] + absolute[1] + absolute[2];
        const uint64_t rgbPairs = finitePairs[0] + finitePairs[1] + finitePairs[2];
        return {{"exact_bytes", exactBytes}, {"exact_channels", exactChannels}, {"exact_pixels", exactPixels},
            {"different_pixels", pixels - exactPixels}, {"byte_identical", exactBytes == before.size()},
            {"finite_channel_pairs", finitePairs}, {"mean_absolute_error_rgba", std::move(mae)},
            {"root_mean_square_error_rgba", std::move(rmse)}, {"maximum_absolute_error_rgba", maximum},
            {"rgb_mean_absolute_error", rgbPairs ? Json(rgbAbsolute / double(rgbPairs)) : Json(nullptr)}};
    }

    std::array<Snapshot, 2> m_captures;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint{};
    UINT64 m_readbackBytes = 0;
    UINT m_width = 0, m_height = 0;
    uint64_t m_firstSamplePhase = JointSamplePeriod;
    size_t m_captureCount = 0;
    std::string m_outputPrefix, m_error;
    HRESULT m_lastHRESULT = S_OK;
    bool m_initialized = false, m_failed = false, m_flushed = false;
};
