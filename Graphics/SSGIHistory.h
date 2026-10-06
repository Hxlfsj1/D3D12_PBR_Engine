#pragma once

#include "stdafx.h"
#include <wrl/client.h>
#include <cmath>

// Two persistent sets, ordered on the main direct queue. Reinitialize only with GPU idle.
class SSGIHistory
{
public:
    struct Textures
    {
        // gi.a = history length; fastGI.a = slow-history luminance second moment.
        Microsoft::WRL::ComPtr<ID3D12Resource> gi, fastGI, depth, normal;
    };

    bool Initialize(ID3D12Device* device, UINT sceneWidth, UINT sceneHeight)
    {
        valid = false;
        writeIndex = 0;
        width = sceneWidth; height = sceneHeight;
        for (auto& set : textures)
        {
            if (!Create(device, DXGI_FORMAT_R16G16B16A16_FLOAT, set.gi) ||
                !Create(device, DXGI_FORMAT_R16G16B16A16_FLOAT, set.fastGI) ||
                !Create(device, DXGI_FORMAT_R32_FLOAT, set.depth) ||
                !Create(device, DXGI_FORMAT_R16G16B16A16_FLOAT, set.normal)) return false;
        }
        DirectX::XMStoreFloat4x4(&previousViewProj, DirectX::XMMatrixIdentity());
        previousInvViewProj = previousViewProj;
        return true;
    }

    void Invalidate() { valid = false; }
    void RejectCameraCut(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& forward, float fov)
    {
        if (!valid) return;
        using namespace DirectX;
        float distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&position) - XMLoadFloat3(&previousPosition)));
        float agreement = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&forward), XMLoadFloat3(&previousForward)));
        if (distance > 5.0f || agreement < 0.5f || std::abs(fov - previousFov) > 0.001f) Invalidate();
    }

    // Publish only after this frame's command list has been submitted.
    void Commit(const DirectX::XMFLOAT4X4& viewProj, const DirectX::XMFLOAT4X4& invViewProj,
        const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& forward, float fov)
    {
        previousViewProj = viewProj; previousInvViewProj = invViewProj;
        previousPosition = position; previousForward = forward; previousFov = fov;
        writeIndex = 1 - writeIndex;
        valid = true;
    }

    const Textures& Previous() const { return textures[1 - writeIndex]; }
    const Textures& Current() const { return textures[writeIndex]; }
    bool IsValid() const { return valid; }
    UINT Width() const { return width; }
    UINT Height() const { return height; }
    const DirectX::XMFLOAT4X4& PreviousViewProj() const { return previousViewProj; }
    const DirectX::XMFLOAT4X4& PreviousInvViewProj() const { return previousInvViewProj; }

private:
    bool Create(ID3D12Device* device, DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
    {
        texture.Reset();
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, (width + 1) / 2, (height + 1) / 2,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture));
        if (FAILED(hr)) ErrorLog::HRESULT("SSGI: history allocation failed.", hr);
        return SUCCEEDED(hr);
    }

    Textures textures[2];
    UINT width = 0, height = 0, writeIndex = 0;
    bool valid = false;
    DirectX::XMFLOAT4X4 previousViewProj = {}, previousInvViewProj = {};
    DirectX::XMFLOAT3 previousPosition = {}, previousForward = {};
    float previousFov = 0;
};
