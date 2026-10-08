#ifndef RENDER_STRUCTS_H
#define RENDER_STRUCTS_H

#include "stdafx.h"

struct RenderFormats
{
    static constexpr DXGI_FORMAT SceneColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT DepthResource = DXGI_FORMAT_R32_TYPELESS;
    static constexpr DXGI_FORMAT DepthDSV = DXGI_FORMAT_D32_FLOAT;
    static constexpr DXGI_FORMAT DepthSRV = DXGI_FORMAT_R32_FLOAT;
    static constexpr DXGI_FORMAT GBufferAlbedo = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT GBufferNormal = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT GBufferORM = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT GBufferEmissive = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT ScalarSignal = DXGI_FORMAT_R16_FLOAT;
    static constexpr DXGI_FORMAT MotionVector = DXGI_FORMAT_R16G16_FLOAT;
    static constexpr DXGI_FORMAT PostProcess = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT SMAAEdges = DXGI_FORMAT_R8G8_UNORM;
    static constexpr DXGI_FORMAT SMAAWeights = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT SMAAOutput = PostProcess;
    static constexpr DXGI_FORMAT IndirectLighting = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT LuminanceMoments = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT DepthBounds = DXGI_FORMAT_R32G32_FLOAT;
};

// CPU history metadata. Matrices correspond to the last submitted SSGI history.
struct SSGIHistoryState
{
    bool valid = false;
    DirectX::XMFLOAT4X4 previousViewProj = {};
    DirectX::XMFLOAT4X4 previousInvViewProj = {};
    DirectX::XMFLOAT3 previousPosition = {};
    DirectX::XMFLOAT3 previousForward = {};
    float previousFov = 0;
};

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
    float materialMipBias;
    DirectX::XMFLOAT3 cameraForward;
    float paddingCameraForward;
    DirectX::XMFLOAT3 lightDir;
    float environmentIntensity;
    DirectX::XMFLOAT3 lightColor;
    float tanSunAngularRadius;

    DirectX::XMFLOAT4X4 lightViewProj[NUM_CASCADES];
    DirectX::XMFLOAT4 cascadeSplits;
    DirectX::XMFLOAT4 cascadeOrthoWidths;
    DirectX::XMFLOAT4 cascadeDepthRanges;

    UINT iblPrefilterIdx;
    UINT iblBRDFIdx;
    UINT shadowMapIdx;

    UINT padTo256[33];
};

constexpr UINT64 kPassConstantsAlignedSize = AlignConstantBufferSize(sizeof(PassConstants));

struct MaterialData
{
    UINT albedoIdx;
    UINT normalIdx;
    UINT ormIdx;
    UINT emissiveIdx;
    DirectX::XMFLOAT4 baseColorFactor;
    UINT isUnlit;
    DirectX::XMFLOAT3 emissiveFactor; // Linear factor including emissive strength; retains 48-byte stride.
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
    UINT gbufferEmissiveIdx;

    UINT hbaoIdx;
    UINT padTo256[42];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) HBAOConstants
{
    DirectX::XMFLOAT4X4 projMat;
    DirectX::XMFLOAT4X4 invProjMat;
    DirectX::XMFLOAT4X4 viewMat;

    float radius;
    float bias;
    float intensity;
    float resolutionX;

    float resolutionY;
    UINT temporalFrameIndex;
    UINT quality;
    UINT padTo256[9];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) MotionVectorConstants
{
    DirectX::XMFLOAT4X4 currJitteredInvViewProj;
    DirectX::XMFLOAT4X4 currUnjitteredViewProj;
    DirectX::XMFLOAT4X4 prevUnjitteredViewProj;

    UINT depthTextureIdx;
    UINT pad[3];
};

#endif
