#pragma once

#include "stdafx.h"
#include "NVIDIA/STBN/STBNConfig.h"
#include <ResourceUploadBatch.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <vector>

// Reusable, immutable sampling data. Uploaded once; RDG consumers create their own SRVs.
class BlueNoiseTexture
{
public:
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        if (texture) return true;
        constexpr size_t rowBytes = STBN_WIDTH * STBN_CHANNELS;
        constexpr size_t sliceBytes = rowBytes * STBN_HEIGHT;
        constexpr size_t byteCount = sliceBytes * STBN_FRAMES;
        std::ifstream input("ThirdParty/NVIDIA/STBN/stbn_vec2_128x128x64.rg8",
            std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(byteCount))
        {
            ErrorLog::Write("BlueNoise: missing or incorrectly sized STBN data.");
            return false;
        }
        std::vector<uint8_t> bytes(byteCount);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), byteCount))
        {
            ErrorLog::Write("BlueNoise: failed to read STBN data.");
            return false;
        }

        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8_UINT,
            STBN_WIDTH, STBN_HEIGHT, STBN_FRAMES, 1);
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        Microsoft::WRL::ComPtr<ID3D12Resource> uploaded;
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&uploaded));
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("BlueNoise: texture creation failed.", hr);
            return false;
        }
        uploaded->SetName(L"BlueNoise.STBN.Vector2");
        std::array<D3D12_SUBRESOURCE_DATA, STBN_FRAMES> slices = {};
        for (size_t frame = 0; frame < slices.size(); ++frame)
        {
            slices[frame].pData = bytes.data() + frame * sliceBytes;
            slices[frame].RowPitch = rowBytes;
            slices[frame].SlicePitch = sliceBytes;
        }
        DirectX::ResourceUploadBatch upload(device);
        upload.Begin();
        upload.Upload(uploaded.Get(), 0, slices.data(), STBN_FRAMES);
        upload.Transition(uploaded.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        upload.End(queue).get();
        texture = std::move(uploaded);
        return true;
    }

    ID3D12Resource* Get() const { return texture.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
};
