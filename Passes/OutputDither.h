#pragma once

#include "RDG.h"
#include "../ThirdParty/NVIDIA/STBN/STBNConfig.h"

// Share the immutable STBN allocation with GI; declare pixel-stage access in RDG.
inline RDGTextureSRVHandle ImportOutputDither(RDGBuilder& graph, ID3D12Resource* texture)
{
    if (!texture) return {};
    const auto handle = graph.RegisterExternalTexture(texture,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "BlueNoise.STBN.Vector2");
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8_UINT;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2DArray.MipLevels = 1;
    desc.Texture2DArray.ArraySize = STBN_FRAMES;
    return graph.CreateTextureSRVView(handle, &desc);
}
