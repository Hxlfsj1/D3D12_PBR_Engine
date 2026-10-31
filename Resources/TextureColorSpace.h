#pragma once

#include <WICTextureLoader.h>

enum class TextureType
{
    Albedo,
    Normal,
    MetallicRoughness,
    Occlusion,
    Emissive,
    Unknown
};

constexpr bool IsColorTexture(TextureType type) noexcept
{
    return type == TextureType::Albedo || type == TextureType::Emissive;
}

constexpr DirectX::WIC_LOADER_FLAGS EmbeddedTextureLoadFlags(TextureType type) noexcept
{
    // Material usage determines the transfer function, regardless of image metadata.
    // Reserve color mips for our linear-space filter; DirectXTK's GPU autogen
    // copies sRGB bytes to UNORM and filters those encoded values.
    return static_cast<DirectX::WIC_LOADER_FLAGS>(
        (IsColorTexture(type) ? DirectX::WIC_LOADER_FORCE_SRGB : DirectX::WIC_LOADER_IGNORE_SRGB) |
        DirectX::WIC_LOADER_FORCE_RGBA32 |
        (IsColorTexture(type) ? DirectX::WIC_LOADER_MIP_RESERVE : DirectX::WIC_LOADER_MIP_AUTOGEN));
}
