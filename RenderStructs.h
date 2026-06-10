#ifndef RENDER_STRUCTS_H
#define RENDER_STRUCTS_H

#include "stdafx.h"

constexpr UINT NUM_CASCADES = 4;

constexpr UINT64 AlignConstantBufferSize(UINT64 byteSize)
{
    constexpr UINT64 alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    return (byteSize + alignment - 1) & ~(alignment - 1);
}

// Dynamic CPU-to-GPU data payloads updated per frame (Constant Buffers)
struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) PassConstants
{
    DirectX::XMFLOAT3 camPos;
    float padding1;
    DirectX::XMFLOAT3 lightDir;
    float padding2;
    DirectX::XMFLOAT3 lightColor;
    float padding3;

    DirectX::XMFLOAT4X4 lightViewProj[NUM_CASCADES];
    DirectX::XMFLOAT4 cascadeSplits;

    UINT iblPrefilterIdx;
    UINT iblBRDFIdx;
    UINT shadowMapIdx;

    UINT padTo256[45];
};

constexpr UINT64 kPassConstantsAlignedSize = AlignConstantBufferSize(sizeof(PassConstants));

struct MaterialData
{
    UINT albedoIdx;
    UINT normalIdx;
    UINT ormIdx;
    UINT emissiveIdx;
    UINT isUnlit;

    UINT pad[3];
};

struct InstanceData
{
    DirectX::XMFLOAT4X4 wvpMat;
    DirectX::XMFLOAT4X4 worldMat;
    DirectX::XMFLOAT4X4 normalMat;

    UINT customMaterialID;
    UINT pad[3];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) DeferredConstants
{
    DirectX::XMFLOAT4X4 invViewProj;

    UINT gbufferAlbedoIdx;
    UINT gbufferNormalIdx;
    UINT gbufferORMIdx;
    UINT depthBufferIdx;

    UINT hbaoIdx;
    UINT padTo256[43];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) HBAOConstants
{
    DirectX::XMFLOAT4X4 projMat;
    DirectX::XMFLOAT4X4 invProjMat;
    DirectX::XMFLOAT4X4 viewMat;

    float radius;
    float bias;
    float power;
    float resolutionX;

    float resolutionY;
    UINT padTo256[11];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) TAAConstants
{
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT4X4 prevViewProj;
    DirectX::XMFLOAT2 jitterOffset;
    float blendAlpha;
    float varianceScale;

    UINT colorTextureIdx;
    UINT historyTextureIdx;
    UINT depthTextureIdx;
    UINT pad;
};

#endif
