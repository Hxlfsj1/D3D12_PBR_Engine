#pragma once

#include "TextureColorSpace.h"
#include <ResourceUploadBatch.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace MaterialTextureDetail
{
    inline const std::array<double, 256>& SRGBDecodeTable()
    {
        static const auto table = [] {
            std::array<double, 256> result{};
            for (size_t i = 0; i < result.size(); ++i)
            {
                const double encoded = double(i) / 255.0;
                result[i] = encoded <= 0.04045 ? encoded / 12.92 :
                    std::pow((encoded + 0.055) / 1.055, 2.4);
            }
            return result;
        }();
        return table;
    }

    inline uint8_t EncodeSRGB(double linear)
    {
        linear = std::clamp(linear, 0.0, 1.0);
        const double encoded = linear <= 0.0031308 ? linear * 12.92 :
            1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        return static_cast<uint8_t>(std::lround(encoded * 255.0));
    }

    inline std::vector<uint8_t> DownsampleSRGB(const uint8_t* source, size_t rowPitch,
        uint32_t width, uint32_t height, uint32_t nextWidth, uint32_t nextHeight)
    {
        const auto& decode = SRGBDecodeTable();
        std::vector<uint8_t> result(size_t(nextWidth) * nextHeight * 4);
        const double scaleX = double(width) / nextWidth;
        const double scaleY = double(height) / nextHeight;
        // Area weights include the final row/column of odd-sized and 1D images.
        for (uint32_t y = 0; y < nextHeight; ++y)
        {
            const double top = y * scaleY, bottom = (y + 1) * scaleY;
            const auto endY = (std::min)(height, static_cast<uint32_t>(std::ceil(bottom)));
            for (uint32_t x = 0; x < nextWidth; ++x)
            {
                const double left = x * scaleX, right = (x + 1) * scaleX;
                const auto endX = (std::min)(width, static_cast<uint32_t>(std::ceil(right)));
                double sum[4]{};
                for (auto sy = static_cast<uint32_t>(top); sy < endY; ++sy)
                {
                    const double weightY = (std::min)(bottom, double(sy + 1)) - (std::max)(top, double(sy));
                    for (auto sx = static_cast<uint32_t>(left); sx < endX; ++sx)
                    {
                        const double weight = weightY *
                            ((std::min)(right, double(sx + 1)) - (std::max)(left, double(sx)));
                        const auto* pixel = source + size_t(sy) * rowPitch + size_t(sx) * 4;
                        for (size_t c = 0; c < 3; ++c) sum[c] += decode[pixel[c]] * weight;
                        sum[3] += pixel[3] * weight; // Alpha is always linear.
                    }
                }
                const double area = scaleX * scaleY;
                auto* pixel = result.data() + (size_t(y) * nextWidth + x) * 4;
                for (size_t c = 0; c < 3; ++c) pixel[c] = EncodeSRGB(sum[c] / area);
                pixel[3] = static_cast<uint8_t>(std::lround(std::clamp(sum[3] / area, 0.0, 255.0)));
            }
        }
        return result;
    }
}

// The returned texture is ready for material sampling from both pixel and
// compute shaders once the caller submits and waits for the upload batch.
inline HRESULT CreateEmbeddedMaterialTexture(ID3D12Device* device, DirectX::ResourceUploadBatch& upload,
    const uint8_t* bytes, size_t byteCount, TextureType type, ID3D12Resource** texture)
{
    if (!texture) return E_INVALIDARG;
    *texture = nullptr;
    const auto flags = EmbeddedTextureLoadFlags(type);
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr;
    const auto sampleState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!IsColorTexture(type))
    {
        hr = DirectX::CreateWICTextureFromMemoryEx(device, upload, bytes, byteCount, 0,
            D3D12_RESOURCE_FLAG_NONE, flags, resource.GetAddressOf());
        if (FAILED(hr)) return hr;
        upload.Transition(resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sampleState);
    }
    else
    {
        std::unique_ptr<uint8_t[]> decoded;
        D3D12_SUBRESOURCE_DATA data{};
        hr = DirectX::LoadWICTextureFromMemoryEx(device, bytes, byteCount, 0,
            D3D12_RESOURCE_FLAG_NONE, flags, resource.GetAddressOf(), decoded, data);
        if (FAILED(hr)) return hr;
        const auto desc = resource->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return E_UNEXPECTED;
        upload.Upload(resource.Get(), 0, &data, 1);
        auto width = static_cast<uint32_t>(desc.Width);
        auto height = desc.Height;
        const auto* source = decoded.get();
        auto rowPitch = static_cast<size_t>(data.RowPitch);
        std::vector<uint8_t> previous;
        for (UINT mip = 1; mip < desc.MipLevels; ++mip)
        {
            const auto nextWidth = (std::max)(1u, width / 2);
            const auto nextHeight = (std::max)(1u, height / 2);
            auto next = MaterialTextureDetail::DownsampleSRGB(source, rowPitch,
                width, height, nextWidth, nextHeight);
            data.pData = next.data();
            data.RowPitch = static_cast<LONG_PTR>(size_t(nextWidth) * 4);
            data.SlicePitch = data.RowPitch * nextHeight;
            upload.Upload(resource.Get(), mip, &data, 1);
            // Upload copies the bytes immediately; keep only the source mip.
            previous = std::move(next);
            decoded.reset();
            source = previous.data();
            rowPitch = static_cast<size_t>(data.RowPitch);
            width = nextWidth;
            height = nextHeight;
        }
        upload.Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, sampleState);
    }
    *texture = resource.Detach();
    return S_OK;
}
