#ifndef RENDER_STRUCTS_H
#define RENDER_STRUCTS_H

#include "stdafx.h"

// Dynamic CPU-to-GPU data payloads updated per frame (Constant Buffers)
struct alignas(256) PassConstants
{
    DirectX::XMFLOAT3 camPos;
    float padding1;
    DirectX::XMFLOAT3 lightDir;
    float padding2;
    DirectX::XMFLOAT3 lightColor;
    float padding3;
    DirectX::XMFLOAT4X4 lightViewProj;

    UINT iblPrefilterIdx;
    UINT iblBRDFIdx;
    UINT shadowMapIdx;
    UINT padTo256[33];
};

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

struct alignas(256) DeferredConstants
{
    DirectX::XMFLOAT4X4 invViewProj;

    UINT gbufferAlbedoIdx;
    UINT gbufferNormalIdx;
    UINT gbufferORMIdx;
    UINT depthBufferIdx;

    UINT hbaoIdx;
    UINT padTo256[43];
};

struct alignas(256) HBAOConstants
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

#endif