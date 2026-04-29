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
    float padTo256[36];
};

struct InstanceData
{
    DirectX::XMFLOAT4X4 wvpMat;
    DirectX::XMFLOAT4X4 worldMat;
    DirectX::XMFLOAT4X4 normalMat;
};

#endif