#ifndef RDG_H
#define RDG_H

#include "stdafx.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include <sstream>

#include <wrl/client.h>
#include "RenderDevice.h"
#include "RDGResourceLease.h"

enum class ERDGPassFlags : uint32_t
{
    None = 0,
    Graphics = 1 << 0,
    Compute = 1 << 1,
    Copy = 1 << 2,
    NeverCull = 1 << 8
};

inline ERDGPassFlags operator|(ERDGPassFlags lhs, ERDGPassFlags rhs)
{
    return static_cast<ERDGPassFlags>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline bool HasRDGPassFlag(ERDGPassFlags flags, ERDGPassFlags flag)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

enum class ERDGAccess
{
    Unknown,
    SRV,
    ComputeSRV,
    UAV,
    RTV,
    DSVRead,
    DSVWrite,
    CopySrc,
    CopyDst,
    Present
};

struct RDGTextureHandle
{
    uint32_t index = UINT32_MAX;

    bool IsValid() const
    {
        return index != UINT32_MAX;
    }
};

struct RDGTextureAccess
{
    RDGTextureHandle texture;
    ERDGAccess access = ERDGAccess::Unknown;
};

struct RDGBufferHandle
{
    uint32_t index = UINT32_MAX;

    bool IsValid() const
    {
        return index != UINT32_MAX;
    }
};

struct RDGBufferAccess
{
    RDGBufferHandle buffer;
    ERDGAccess access = ERDGAccess::Unknown;
};

struct RDGPassHandle
{
    uint32_t index = UINT32_MAX;

    bool IsValid() const
    {
        return index != UINT32_MAX;
    }
};

struct RDGTextureSRVHandle
{
    RDGTextureHandle texture;
    UINT descriptorIndex = UINT_MAX;

    bool IsValid() const
    {
        return texture.IsValid() && descriptorIndex != UINT_MAX;
    }
};

struct RDGTextureUAVHandle
{
    RDGTextureHandle texture;
    UINT descriptorIndex = UINT_MAX;

    bool IsValid() const
    {
        return texture.IsValid() && descriptorIndex != UINT_MAX;
    }
};

struct RDGTextureRTVHandle
{
    RDGTextureHandle texture;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

    bool IsValid() const
    {
        return texture.IsValid() && cpuHandle.ptr != 0;
    }
};

struct RDGTextureDSVHandle
{
    RDGTextureHandle texture;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

    bool IsValid() const
    {
        return texture.IsValid() && cpuHandle.ptr != 0;
    }
};

struct RDGBufferSRVHandle
{
    RDGBufferHandle buffer;
    UINT descriptorIndex = UINT_MAX;

    bool IsValid() const
    {
        return buffer.IsValid() && descriptorIndex != UINT_MAX;
    }
};

struct RDGBufferUAVHandle
{
    RDGBufferHandle buffer;
    UINT descriptorIndex = UINT_MAX;

    bool IsValid() const
    {
        return buffer.IsValid() && descriptorIndex != UINT_MAX;
    }
};

using RDGTransientSrvUavDescriptorAllocator =
    std::function<bool(UINT*, D3D12_CPU_DESCRIPTOR_HANDLE*)>;

using RDGTransientResourceAllocator =
    std::function<bool(
        const D3D12_RESOURCE_DESC&,
        D3D12_RESOURCE_STATES,
        D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*,
        RDGTransientResourceLease*)>;

struct RDGPassParameters
{
    std::vector<RDGTextureAccess> textures;
    std::vector<RDGBufferAccess> buffers;

    void ReadSRV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::SRV });
    }

    void ReadSRV(RDGTextureSRVHandle srv)
    {
        ReadSRV(srv.texture);
    }

    void ReadComputeSRV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::ComputeSRV });
    }

    void ReadComputeSRV(RDGTextureSRVHandle srv)
    {
        ReadComputeSRV(srv.texture);
    }

    void ReadSRV(RDGBufferHandle buffer)
    {
        buffers.push_back({ buffer, ERDGAccess::SRV });
    }

    void ReadSRV(RDGBufferSRVHandle srv)
    {
        ReadSRV(srv.buffer);
    }

    void WriteRTV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::RTV });
    }

    void WriteRTV(RDGTextureRTVHandle rtv)
    {
        WriteRTV(rtv.texture);
    }

    void WriteUAV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::UAV });
    }

    void WriteUAV(RDGTextureUAVHandle uav)
    {
        WriteUAV(uav.texture);
    }

    void WriteUAV(RDGBufferHandle buffer)
    {
        buffers.push_back({ buffer, ERDGAccess::UAV });
    }

    void WriteUAV(RDGBufferUAVHandle uav)
    {
        WriteUAV(uav.buffer);
    }

    void ReadDSV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::DSVRead });
    }

    void ReadDSV(RDGTextureDSVHandle dsv)
    {
        ReadDSV(dsv.texture);
    }

    void WriteDSV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::DSVWrite });
    }

    void WriteDSV(RDGTextureDSVHandle dsv)
    {
        WriteDSV(dsv.texture);
    }

    void ReadCopySrc(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::CopySrc });
    }

    void ReadCopySrc(RDGBufferHandle buffer)
    {
        buffers.push_back({ buffer, ERDGAccess::CopySrc });
    }

    void WriteCopyDst(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::CopyDst });
    }

    void WriteCopyDst(RDGBufferHandle buffer)
    {
        buffers.push_back({ buffer, ERDGAccess::CopyDst });
    }

    void Present(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::Present });
    }
};

struct RDGPass
{
    std::string name;
    ERDGPassFlags flags = ERDGPassFlags::Graphics;
    RDGPassParameters parameters;
    std::function<void(ID3D12GraphicsCommandList*)> execute;
    std::vector<uint32_t> dependencies;
};

struct RDGTextureDesc
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint16_t arraySize = 1;
    uint16_t mipLevels = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    bool hasClearValue = false;
    D3D12_CLEAR_VALUE clearValue = {};
};

struct RDGBufferDesc
{
    uint64_t sizeInBytes = 0;
    uint64_t alignment = 0;
    uint32_t structureByteStride = 0;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
};

struct RDGTexture
{
    RDGTextureDesc desc;
    RDGTransientResourceLease transientResource;
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
    bool external = false;
    bool externalOutput = false;
    uint32_t lastProducer = UINT32_MAX;
    uint32_t firstUsePass = UINT32_MAX;
    uint32_t lastUsePass = UINT32_MAX;
    std::vector<uint32_t> lastReaders;
    std::string name;
};

struct RDGBuffer
{
    RDGBufferDesc desc;
    RDGTransientResourceLease transientResource;
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
    bool external = false;
    bool externalOutput = false;
    uint32_t lastProducer = UINT32_MAX;
    uint32_t firstUsePass = UINT32_MAX;
    uint32_t lastUsePass = UINT32_MAX;
    std::vector<uint32_t> lastReaders;
    std::string name;
};

struct RDGTextureExtraction
{
    RDGTextureHandle texture;
    Microsoft::WRL::ComPtr<ID3D12Resource>* output = nullptr;
};

struct RDGBufferExtraction
{
    RDGBufferHandle buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource>* output = nullptr;
};

struct RDGCompileStats
{
    uint32_t totalPassCount = 0;
    uint32_t livePassCount = 0;
    uint32_t culledPassCount = 0;
    uint32_t compiledPassCount = 0;
    uint32_t textureCount = 0;
    uint32_t bufferCount = 0;
    uint32_t externalTextureCount = 0;
    uint32_t externalBufferCount = 0;
    uint32_t transientTextureCount = 0;
    uint32_t transientBufferCount = 0;
    uint32_t passBarrierCount = 0;
    uint32_t finalBarrierCount = 0;
    uint32_t transitionBarrierCount = 0;
    uint32_t uavBarrierCount = 0;
    uint32_t parallelBatchCount = 0;
    uint32_t maxParallelBatchSize = 0;
    bool compileSucceeded = false;
};

struct RDGCompiledPassInfo
{
    uint32_t passIndex = UINT32_MAX;
    uint32_t dependencyLevel = UINT32_MAX;
    bool live = false;
    bool culled = false;
    std::string name;
};

struct RDGCompileSnapshot
{
    RDGCompileStats stats;
    std::vector<RDGCompiledPassInfo> passes;
    std::vector<uint32_t> compiledPassOrder;
    std::vector<std::vector<uint32_t>> parallelPassBatches;
};

class RDGBuilder
{
public:
#if defined(_DEBUG)
    static constexpr bool EnableValidation = true;
    static constexpr bool EnableGraphDump = false;
    static constexpr bool EnableBarrierDump = false;
#endif

    RDGBuilder(RenderDevice* deviceContext, const char* debugName)
        : m_deviceContext(deviceContext), m_debugName(debugName ? debugName : "RDG")
    {
    }

    void SetTransientResourceAllocator(
        RDGTransientResourceAllocator allocator)
    {
        if (m_executed)
        {
            TraceLifecycleWarning("SetTransientResourceAllocator called after Execute");
            return;
        }

        m_transientResourceAllocator = std::move(allocator);
    }

    void SetTransientSrvUavDescriptorAllocator(
        RDGTransientSrvUavDescriptorAllocator allocator)
    {
        if (m_executed)
        {
            TraceLifecycleWarning("SetTransientSrvUavDescriptorAllocator called after Execute");
            return;
        }

        m_transientSrvUavDescriptorAllocator = std::move(allocator);
    }

    RDGTextureHandle RegisterExternalTexture(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGTextureHandle existingHandle = FindRegisteredExternalTexture(resource);
        if (existingHandle.IsValid())
        {
            RDGTexture& texture = m_textures[existingHandle.index];
            texture.finalState = finalState;
            return existingHandle;
        }

        RDGTexture texture;
        texture.resource = resource;

        if (resource != nullptr)
        {
            const D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
            texture.desc.width = static_cast<uint32_t>(resourceDesc.Width);
            texture.desc.height = static_cast<uint32_t>(resourceDesc.Height);
            texture.desc.arraySize = static_cast<uint16_t>(resourceDesc.DepthOrArraySize);
            texture.desc.mipLevels = static_cast<uint16_t>(resourceDesc.MipLevels);
            texture.desc.format = resourceDesc.Format;
            texture.desc.flags = resourceDesc.Flags;
        }

        texture.initialState = initialState;
        texture.currentState = initialState;
        texture.finalState = finalState;
        texture.external = true;
        texture.name = name ? name : "ExternalTexture";

        RDGTextureHandle handle;
        handle.index = static_cast<uint32_t>(m_textures.size());
        m_textures.push_back(texture);
        return handle;
    }

    RDGTextureHandle RegisterExternalTextureOutput(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGTextureHandle handle = RegisterExternalTexture(resource, initialState, finalState, name);
        MarkTextureAsOutput(handle);
        return handle;
    }

    RDGTextureHandle CreateTexture(
        const RDGTextureDesc& desc,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGTextureHandle handle;

        if (m_deviceContext == nullptr || m_deviceContext->GetDevice() == nullptr)
        {
            return handle;
        }

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            desc.format,
            desc.width,
            desc.height,
            desc.arraySize,
            desc.mipLevels,
            1,
            0,
            desc.flags);

        RDGTexture texture;
        texture.desc = desc;
        texture.initialState = initialState;
        texture.currentState = initialState;
        texture.finalState = finalState;
        texture.external = false;
        texture.name = name ? name : "TransientTexture";

        const D3D12_CLEAR_VALUE* clearValue = desc.hasClearValue ? &desc.clearValue : nullptr;

        if (!AllocateTransientResource(
            resourceDesc,
            initialState,
            finalState,
            clearValue,
            &texture.transientResource))
        {
            return handle;
        }

        texture.resource = texture.transientResource->resource.Get();

        handle.index = static_cast<uint32_t>(m_textures.size());
        m_textures.push_back(std::move(texture));
        return handle;
    }

    RDGBufferHandle RegisterExternalBuffer(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGBufferHandle existingHandle = FindRegisteredExternalBuffer(resource);
        if (existingHandle.IsValid())
        {
            RDGBuffer& buffer = m_buffers[existingHandle.index];
            buffer.finalState = finalState;
            return existingHandle;
        }

        RDGBuffer buffer;
        buffer.resource = resource;

        if (resource != nullptr)
        {
            const D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
            buffer.desc.sizeInBytes = resourceDesc.Width;
            buffer.desc.alignment = resourceDesc.Alignment;
            buffer.desc.flags = resourceDesc.Flags;
        }

        buffer.initialState = initialState;
        buffer.currentState = initialState;
        buffer.finalState = finalState;
        buffer.external = true;
        buffer.name = name ? name : "ExternalBuffer";

        RDGBufferHandle handle;
        handle.index = static_cast<uint32_t>(m_buffers.size());
        m_buffers.push_back(buffer);
        return handle;
    }

    RDGBufferHandle CreateBuffer(
        const RDGBufferDesc& desc,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGBufferHandle handle;

        if (desc.sizeInBytes == 0 ||
            m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr)
        {
            return handle;
        }

        D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(
            desc.sizeInBytes,
            desc.flags,
            desc.alignment);

        RDGBuffer buffer;
        buffer.desc = desc;
        buffer.initialState = initialState;
        buffer.currentState = initialState;
        buffer.finalState = finalState;
        buffer.external = false;
        buffer.name = name ? name : "TransientBuffer";

        if (!AllocateTransientResource(
            resourceDesc,
            initialState,
            finalState,
            nullptr,
            &buffer.transientResource))
        {
            return handle;
        }

        buffer.resource = buffer.transientResource->resource.Get();

        handle.index = static_cast<uint32_t>(m_buffers.size());
        m_buffers.push_back(std::move(buffer));
        return handle;
    }

    void MarkTextureAsOutput(RDGTextureHandle texture)
    {
        if (!texture.IsValid() || texture.index >= m_textures.size())
        {
            return;
        }

        m_textures[texture.index].externalOutput = true;
    }

    void MarkBufferAsOutput(RDGBufferHandle buffer)
    {
        if (!buffer.IsValid() || buffer.index >= m_buffers.size())
        {
            return;
        }

        m_buffers[buffer.index].externalOutput = true;
    }

    void QueueTextureExtraction(
        RDGTextureHandle texture,
        Microsoft::WRL::ComPtr<ID3D12Resource>* output)
    {
        if (!texture.IsValid() || texture.index >= m_textures.size() || output == nullptr)
        {
            return;
        }

        MarkTextureAsOutput(texture);
        m_textureExtractions.push_back({ texture, output });
    }

    void QueueBufferExtraction(
        RDGBufferHandle buffer,
        Microsoft::WRL::ComPtr<ID3D12Resource>* output)
    {
        if (!buffer.IsValid() || buffer.index >= m_buffers.size() || output == nullptr)
        {
            return;
        }

        MarkBufferAsOutput(buffer);
        m_bufferExtractions.push_back({ buffer, output });
    }

    ID3D12Resource* GetTextureResource(RDGTextureHandle texture) const
    {
        if (!texture.IsValid() || texture.index >= m_textures.size())
        {
            return nullptr;
        }

        return m_textures[texture.index].resource;
    }

    const RDGTextureDesc* GetTextureDesc(RDGTextureHandle texture) const
    {
        if (!texture.IsValid() || texture.index >= m_textures.size())
        {
            return nullptr;
        }

        return &m_textures[texture.index].desc;
    }

    ID3D12Resource* GetBufferResource(RDGBufferHandle buffer) const
    {
        if (!buffer.IsValid() || buffer.index >= m_buffers.size())
        {
            return nullptr;
        }

        return m_buffers[buffer.index].resource;
    }

    const RDGBufferDesc* GetBufferDesc(RDGBufferHandle buffer) const
    {
        if (!buffer.IsValid() || buffer.index >= m_buffers.size())
        {
            return nullptr;
        }

        return &m_buffers[buffer.index].desc;
    }

    const RDGCompileStats& GetCompileStats() const
    {
        return m_compileStats;
    }

    const RDGCompileSnapshot& GetLastCompileSnapshot() const
    {
        return m_lastCompileSnapshot;
    }

    RDGTextureSRVHandle CreateTextureSRVView(
        RDGTextureHandle texture,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* overrideDesc = nullptr)
    {
        RDGTextureSRVHandle view;
        UINT descriptorIndex = UINT_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!AllocateTransientSrvUavDescriptor(&descriptorIndex, &cpuHandle))
        {
            TraceLifecycleWarning("CreateTextureSRVView failed to allocate a descriptor");
            return view;
        }

        if (!CreateTextureSRV(texture, cpuHandle, overrideDesc))
        {
            TraceLifecycleWarning("CreateTextureSRVView failed to create the SRV");
            return view;
        }

        view.texture = texture;
        view.descriptorIndex = descriptorIndex;
        return view;
    }

    bool CreateTextureSRV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* overrideDesc = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        ID3D12Resource* resource = m_textures[texture.index].resource;
        if (resource == nullptr)
        {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        if (overrideDesc != nullptr)
        {
            srvDesc = *overrideDesc;
        }
        else
        {
            srvDesc = BuildDefaultTextureSRVDesc(m_textures[texture.index].desc);
        }

        m_deviceContext->GetDevice()->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
        return true;
    }

    RDGTextureUAVHandle CreateTextureUAVView(
        RDGTextureHandle texture,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* overrideDesc = nullptr)
    {
        RDGTextureUAVHandle view;
        UINT descriptorIndex = UINT_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!AllocateTransientSrvUavDescriptor(&descriptorIndex, &cpuHandle))
        {
            TraceLifecycleWarning("CreateTextureUAVView failed to allocate a descriptor");
            return view;
        }

        if (!CreateTextureUAV(texture, cpuHandle, overrideDesc))
        {
            TraceLifecycleWarning("CreateTextureUAVView failed to create the UAV");
            return view;
        }

        view.texture = texture;
        view.descriptorIndex = descriptorIndex;
        return view;
    }

    bool CreateTextureUAV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* overrideDesc = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        const RDGTexture& rdgTexture = m_textures[texture.index];
        if (rdgTexture.resource == nullptr ||
            (rdgTexture.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        {
            return false;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        if (overrideDesc != nullptr)
        {
            uavDesc = *overrideDesc;
        }
        else
        {
            uavDesc = BuildDefaultTextureUAVDesc(rdgTexture.desc);
        }

        m_deviceContext->GetDevice()->CreateUnorderedAccessView(
            rdgTexture.resource,
            nullptr,
            &uavDesc,
            cpuHandle);

        return true;
    }

    RDGBufferSRVHandle CreateBufferSRVView(
        RDGBufferHandle buffer,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* overrideDesc = nullptr)
    {
        RDGBufferSRVHandle view;
        UINT descriptorIndex = UINT_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!AllocateTransientSrvUavDescriptor(&descriptorIndex, &cpuHandle))
        {
            TraceLifecycleWarning("CreateBufferSRVView failed to allocate a descriptor");
            return view;
        }

        if (!CreateBufferSRV(buffer, cpuHandle, overrideDesc))
        {
            TraceLifecycleWarning("CreateBufferSRVView failed to create the SRV");
            return view;
        }

        view.buffer = buffer;
        view.descriptorIndex = descriptorIndex;
        return view;
    }

    bool CreateBufferSRV(
        RDGBufferHandle buffer,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_SHADER_RESOURCE_VIEW_DESC* overrideDesc = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !buffer.IsValid() ||
            buffer.index >= m_buffers.size())
        {
            return false;
        }

        const RDGBuffer& rdgBuffer = m_buffers[buffer.index];
        if (rdgBuffer.resource == nullptr)
        {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        if (overrideDesc != nullptr)
        {
            srvDesc = *overrideDesc;
        }
        else
        {
            if (rdgBuffer.desc.structureByteStride == 0)
            {
                return false;
            }

            srvDesc = BuildDefaultBufferSRVDesc(rdgBuffer.desc);
        }

        m_deviceContext->GetDevice()->CreateShaderResourceView(rdgBuffer.resource, &srvDesc, cpuHandle);
        return true;
    }

    RDGBufferUAVHandle CreateBufferUAVView(
        RDGBufferHandle buffer,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* overrideDesc = nullptr,
        ID3D12Resource* counterResource = nullptr)
    {
        RDGBufferUAVHandle view;
        UINT descriptorIndex = UINT_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!AllocateTransientSrvUavDescriptor(&descriptorIndex, &cpuHandle))
        {
            TraceLifecycleWarning("CreateBufferUAVView failed to allocate a descriptor");
            return view;
        }

        if (!CreateBufferUAV(buffer, cpuHandle, overrideDesc, counterResource))
        {
            TraceLifecycleWarning("CreateBufferUAVView failed to create the UAV");
            return view;
        }

        view.buffer = buffer;
        view.descriptorIndex = descriptorIndex;
        return view;
    }

    bool CreateBufferUAV(
        RDGBufferHandle buffer,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* overrideDesc = nullptr,
        ID3D12Resource* counterResource = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !buffer.IsValid() ||
            buffer.index >= m_buffers.size())
        {
            return false;
        }

        const RDGBuffer& rdgBuffer = m_buffers[buffer.index];
        if (rdgBuffer.resource == nullptr ||
            (rdgBuffer.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        {
            return false;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        if (overrideDesc != nullptr)
        {
            uavDesc = *overrideDesc;
        }
        else
        {
            if (rdgBuffer.desc.structureByteStride == 0)
            {
                return false;
            }

            uavDesc = BuildDefaultBufferUAVDesc(rdgBuffer.desc);
        }

        m_deviceContext->GetDevice()->CreateUnorderedAccessView(
            rdgBuffer.resource,
            counterResource,
            &uavDesc,
            cpuHandle);

        return true;
    }

    RDGTextureRTVHandle CreateTextureRTVView(
        RDGTextureHandle texture,
        const D3D12_RENDER_TARGET_VIEW_DESC* overrideDesc = nullptr)
    {
        RDGTextureRTVHandle view;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!CreateTransientTextureRTV(texture, &cpuHandle, overrideDesc))
        {
            TraceLifecycleWarning("CreateTextureRTVView failed to create the RTV");
            return view;
        }

        view.texture = texture;
        view.cpuHandle = cpuHandle;
        return view;
    }

    bool CreateTextureRTV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_RENDER_TARGET_VIEW_DESC* overrideDesc = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        const RDGTexture& rdgTexture = m_textures[texture.index];
        if (rdgTexture.resource == nullptr ||
            (rdgTexture.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
        {
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        if (overrideDesc != nullptr)
        {
            rtvDesc = *overrideDesc;
        }
        else
        {
            rtvDesc = BuildDefaultTextureRTVDesc(rdgTexture.desc);
        }

        m_deviceContext->GetDevice()->CreateRenderTargetView(
            rdgTexture.resource,
            &rtvDesc,
            cpuHandle);

        return true;
    }

    bool AllocateTransientRTV(D3D12_CPU_DESCRIPTOR_HANDLE* outHandle)
    {
        if (outHandle == nullptr)
        {
            return false;
        }

        if (!m_transientRTVHeap)
        {
            CreateTransientRTVHeap();
        }

        if (!m_transientRTVHeap)
        {
            return false;
        }

        if (m_transientRTVCount >= MaxTransientRTVDescriptors)
        {
            return false;
        }

        *outHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            m_transientRTVHeap->GetCPUDescriptorHandleForHeapStart(),
            m_transientRTVCount,
            m_transientRTVDescriptorSize);

        ++m_transientRTVCount;
        return true;
    }

    bool CreateTransientTextureRTV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE* outHandle,
        const D3D12_RENDER_TARGET_VIEW_DESC* overrideDesc = nullptr)
    {
        if (outHandle == nullptr ||
            m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        const RDGTexture& rdgTexture = m_textures[texture.index];
        if (rdgTexture.resource == nullptr ||
            (rdgTexture.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
        {
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        if (!AllocateTransientRTV(&rtvHandle))
        {
            return false;
        }

        if (!CreateTextureRTV(texture, rtvHandle, overrideDesc))
        {
            return false;
        }

        *outHandle = rtvHandle;
        return true;
    }

    bool AllocateTransientDSV(D3D12_CPU_DESCRIPTOR_HANDLE* outHandle)
    {
        if (outHandle == nullptr)
        {
            return false;
        }

        if (!m_transientDSVHeap)
        {
            CreateTransientDSVHeap();
        }

        if (!m_transientDSVHeap)
        {
            return false;
        }

        if (m_transientDSVCount >= MaxTransientDSVDescriptors)
        {
            return false;
        }

        *outHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            m_transientDSVHeap->GetCPUDescriptorHandleForHeapStart(),
            m_transientDSVCount,
            m_transientDSVDescriptorSize);

        ++m_transientDSVCount;
        return true;
    }

    RDGTextureDSVHandle CreateTextureDSVView(
        RDGTextureHandle texture,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* overrideDesc = nullptr)
    {
        RDGTextureDSVHandle view;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};

        if (!CreateTransientTextureDSV(texture, &cpuHandle, overrideDesc))
        {
            TraceLifecycleWarning("CreateTextureDSVView failed to create the DSV");
            return view;
        }

        view.texture = texture;
        view.cpuHandle = cpuHandle;
        return view;
    }

    bool CreateTextureDSV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* overrideDesc = nullptr) const
    {
        if (m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        const RDGTexture& rdgTexture = m_textures[texture.index];
        if (rdgTexture.resource == nullptr ||
            (rdgTexture.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0)
        {
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        if (overrideDesc != nullptr)
        {
            dsvDesc = *overrideDesc;
        }
        else
        {
            dsvDesc = BuildDefaultTextureDSVDesc(rdgTexture.desc);
        }

        m_deviceContext->GetDevice()->CreateDepthStencilView(
            rdgTexture.resource,
            &dsvDesc,
            cpuHandle);

        return true;
    }

    bool CreateTransientTextureDSV(
        RDGTextureHandle texture,
        D3D12_CPU_DESCRIPTOR_HANDLE* outHandle,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* overrideDesc = nullptr)
    {
        if (outHandle == nullptr ||
            m_deviceContext == nullptr ||
            m_deviceContext->GetDevice() == nullptr ||
            !texture.IsValid() ||
            texture.index >= m_textures.size())
        {
            return false;
        }

        const RDGTexture& rdgTexture = m_textures[texture.index];
        if (rdgTexture.resource == nullptr ||
            (rdgTexture.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0)
        {
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        if (!AllocateTransientDSV(&dsvHandle))
        {
            return false;
        }

        if (!CreateTextureDSV(texture, dsvHandle, overrideDesc))
        {
            return false;
        }

        *outHandle = dsvHandle;
        return true;
    }

    // The first two loops establish dependencies between passes based on resource dependencies,
    // while the last two loops update the resource read/write tracking state
    RDGPassHandle AddPass(
        const char* name,
        ERDGPassFlags flags,
        const RDGPassParameters& parameters,
        std::function<void(ID3D12GraphicsCommandList*)> execute)
    {
        if (m_executed)
        {
            TraceLifecycleWarning("AddPass called after Execute");
            return {};
        }

        const uint32_t passIndex = static_cast<uint32_t>(m_passes.size());

        RDGPass pass;
        pass.name = name ? name : "UnnamedPass";
        pass.flags = flags;
        pass.parameters = parameters;
        pass.execute = execute;

        for (const RDGTextureAccess& access : parameters.textures)
        {
            if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
            {
                continue;
            }

            const RDGTexture& texture = m_textures[access.texture.index];

            // If the current pass reads a resource, it must add the resource's last writer as a dependency
            if (IsReadAccess(access.access))
            {
                AddPassDependency(pass, texture.lastProducer);
            }

            // If the current pass writes to a resource, it must add the last writer and all preceding readers as dependencies
            if (IsWriteAccess(access.access))
            {
                AddPassDependency(pass, texture.lastProducer);

                for (uint32_t readerPassIndex : texture.lastReaders)
                {
                    AddPassDependency(pass, readerPassIndex);
                }
            }
        }

        // Samiliar as above
        for (const RDGBufferAccess& access : parameters.buffers)
        {
            if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
            {
                continue;
            }

            const RDGBuffer& buffer = m_buffers[access.buffer.index];

            if (IsReadAccess(access.access))
            {
                AddPassDependency(pass, buffer.lastProducer);
            }

            if (IsWriteAccess(access.access))
            {
                AddPassDependency(pass, buffer.lastProducer);

                for (uint32_t readerPassIndex : buffer.lastReaders)
                {
                    AddPassDependency(pass, readerPassIndex);
                }
            }
        }

        for (const RDGTextureAccess& access : parameters.textures)
        {
            if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
            {
                continue;
            }

            RDGTexture& texture = m_textures[access.texture.index];

            // If the current pass reads a resource, add it to the list of latest readers
            if (IsReadAccess(access.access))
            {
                texture.lastReaders.push_back(passIndex);
            }

            // If the current pass writes to a resource, it becomes the new last writer
            if (IsWriteAccess(access.access))
            {
                texture.lastProducer = passIndex;
                texture.lastReaders.clear();
            }
        }

        // Samiliar as above
        for (const RDGBufferAccess& access : parameters.buffers)
        {
            if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
            {
                continue;
            }

            RDGBuffer& buffer = m_buffers[access.buffer.index];

            if (IsReadAccess(access.access))
            {
                buffer.lastReaders.push_back(passIndex);
            }

            if (IsWriteAccess(access.access))
            {
                buffer.lastProducer = passIndex;
                buffer.lastReaders.clear();
            }
        }

        m_passes.push_back(std::move(pass));

        RDGPassHandle handle;
        handle.index = passIndex;
        return handle;
    }

    void AddPassDependency(RDGPassHandle pass, RDGPassHandle dependency)
    {
        if (!pass.IsValid() ||
            !dependency.IsValid() ||
            pass.index >= m_passes.size() ||
            dependency.index >= m_passes.size() ||
            pass.index == dependency.index)
        {
            return;
        }

        AddPassDependency(m_passes[pass.index], dependency.index);
    }

    void AddPassDependencies(
        RDGPassHandle pass,
        std::initializer_list<RDGPassHandle> dependencies)
    {
        for (RDGPassHandle dependency : dependencies)
        {
            AddPassDependency(pass, dependency);
        }
    }

    RDGPassHandle AddPassAfter(
        RDGPassHandle dependency,
        const char* name,
        ERDGPassFlags flags,
        const RDGPassParameters& parameters,
        std::function<void(ID3D12GraphicsCommandList*)> execute)
    {
        RDGPassHandle pass = AddPass(name, flags, parameters, execute);
        AddPassDependency(pass, dependency);
        return pass;
    }

    void Execute(ID3D12GraphicsCommandList* cmdList)
    {
        if (m_executed)
        {
            TraceLifecycleWarning("Execute called more than once");
            return;
        }

        if (cmdList == nullptr)
        {
            TraceLifecycleWarning("Execute called with null command list");
            return;
        }

        m_executed = true;

#if defined(_DEBUG)
        if constexpr (EnableValidation)
        {
            ValidateGraph();
        }
#endif
        // The function returns false if a dependency cycle exists
        const bool compileSucceeded = CompileGraph();
        BuildResourceLifetimes();
        BuildBarrierPlan();
        BuildCompileStats(compileSucceeded);
        BuildCompileSnapshot();

#if defined(_DEBUG)
        if constexpr (EnableGraphDump)
        {
            DumpGraph();
        }
#endif
        // Execute the passes sequentially in order
        for (uint32_t passIndex : m_compiledPassOrder)
        {
            if (passIndex >= m_passes.size())
            {
                continue;
            }

            SubmitBarriers(cmdList, m_passBarriers[passIndex]);

            RDGPass& pass = m_passes[passIndex];

            if (pass.execute)
            {
                pass.execute(cmdList);
            }
        }

        SubmitBarriers(cmdList, m_finalBarriers);
        ExtractResources();

        m_finalBarriers.clear();
        m_passBarriers.clear();
        m_livePasses.clear();
        m_compiledPassOrder.clear();
        m_passDependencyLevels.clear();
        m_parallelPassBatches.clear();
        m_passes.clear();
        m_textureExtractions.clear();
        m_bufferExtractions.clear();
    }

private:
    bool AllocateTransientResource(
        const D3D12_RESOURCE_DESC& resourceDesc,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const D3D12_CLEAR_VALUE* clearValue,
        RDGTransientResourceLease* outResource)
    {
        if (outResource == nullptr || !m_transientResourceAllocator)
        {
            TraceLifecycleWarning("CreateTexture/CreateBuffer called without a transient resource allocator");
            return false;
        }

        return m_transientResourceAllocator(
            resourceDesc,
            initialState,
            finalState,
            clearValue,
            outResource);
    }

    bool AllocateTransientSrvUavDescriptor(
        UINT* outDescriptorIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle)
    {
        if (outDescriptorIndex == nullptr ||
            outCpuHandle == nullptr ||
            !m_transientSrvUavDescriptorAllocator)
        {
            return false;
        }

        return m_transientSrvUavDescriptorAllocator(outDescriptorIndex, outCpuHandle);
    }

    void CreateTransientRTVHeap()
    {
        if (m_deviceContext == nullptr || m_deviceContext->GetDevice() == nullptr)
        {
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = MaxTransientRTVDescriptors;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(m_deviceContext->GetDevice()->CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(&m_transientRTVHeap))))
        {
            m_transientRTVHeap.Reset();
            return;
        }

        m_transientRTVDescriptorSize =
            m_deviceContext->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void CreateTransientDSVHeap()
    {
        if (m_deviceContext == nullptr || m_deviceContext->GetDevice() == nullptr)
        {
            return;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = MaxTransientDSVDescriptors;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(m_deviceContext->GetDevice()->CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(&m_transientDSVHeap))))
        {
            m_transientDSVHeap.Reset();
            return;
        }

        m_transientDSVDescriptorSize =
            m_deviceContext->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }

    static D3D12_SHADER_RESOURCE_VIEW_DESC BuildDefaultTextureSRVDesc(const RDGTextureDesc& desc)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = desc.mipLevels;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        return srvDesc;
    }

    static D3D12_UNORDERED_ACCESS_VIEW_DESC BuildDefaultTextureUAVDesc(const RDGTextureDesc& desc)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        uavDesc.Texture2D.PlaneSlice = 0;
        return uavDesc;
    }

    static D3D12_SHADER_RESOURCE_VIEW_DESC BuildDefaultBufferSRVDesc(const RDGBufferDesc& desc)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(desc.sizeInBytes / desc.structureByteStride);
        srvDesc.Buffer.StructureByteStride = desc.structureByteStride;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        return srvDesc;
    }

    static D3D12_UNORDERED_ACCESS_VIEW_DESC BuildDefaultBufferUAVDesc(const RDGBufferDesc& desc)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = static_cast<UINT>(desc.sizeInBytes / desc.structureByteStride);
        uavDesc.Buffer.StructureByteStride = desc.structureByteStride;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        return uavDesc;
    }

    static D3D12_RENDER_TARGET_VIEW_DESC BuildDefaultTextureRTVDesc(const RDGTextureDesc& desc)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = desc.format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Texture2D.PlaneSlice = 0;
        return rtvDesc;
    }

    static D3D12_DEPTH_STENCIL_VIEW_DESC BuildDefaultTextureDSVDesc(const RDGTextureDesc& desc)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = ToDefaultDSVFormat(desc.format);
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

        if (desc.arraySize > 1)
        {
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.MipSlice = 0;
            dsvDesc.Texture2DArray.FirstArraySlice = 0;
            dsvDesc.Texture2DArray.ArraySize = desc.arraySize;
        }
        else
        {
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;
        }

        return dsvDesc;
    }

    static DXGI_FORMAT ToDefaultDSVFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_D16_UNORM;
        default:
            return format;
        }
    }

    void ExtractResources()
    {
        for (const RDGTextureExtraction& extraction : m_textureExtractions)
        {
            if (extraction.output == nullptr ||
                !extraction.texture.IsValid() ||
                extraction.texture.index >= m_textures.size())
            {
                continue;
            }

            const RDGTexture& texture = m_textures[extraction.texture.index];

            if (texture.transientResource && texture.transientResource->resource)
            {
                texture.transientResource->reusable = false;
                *extraction.output = texture.transientResource->resource;
            }
        }

        for (const RDGBufferExtraction& extraction : m_bufferExtractions)
        {
            if (extraction.output == nullptr ||
                !extraction.buffer.IsValid() ||
                extraction.buffer.index >= m_buffers.size())
            {
                continue;
            }

            const RDGBuffer& buffer = m_buffers[extraction.buffer.index];

            if (buffer.transientResource && buffer.transientResource->resource)
            {
                buffer.transientResource->reusable = false;
                *extraction.output = buffer.transientResource->resource;
            }
        }
    }

    bool CompileGraph()
    {
        m_livePasses.clear();
        m_compiledPassOrder.clear();
        m_passDependencyLevels.clear();
        m_parallelPassBatches.clear();

        const uint32_t passCount = static_cast<uint32_t>(m_passes.size());
        const uint32_t livePassCount = CullUnusedPasses(passCount);
        m_passDependencyLevels.assign(passCount, UINT32_MAX);

        // dependencyCount[i] indicates how many prerequisites of the i-th pass remain unprocessed
        std::vector<uint32_t> dependencyCount(passCount, 0);
        // dependents[i] stores all passes that directly depend on pass i
        std::vector<std::vector<uint32_t>> dependents(passCount);

        for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
        {
            if (!m_livePasses[passIndex])
            {
                continue;
            }

            for (uint32_t dependencyIndex : m_passes[passIndex].dependencies)
            {
                if (dependencyIndex >= passCount || !m_livePasses[dependencyIndex])
                {
                    continue;
                }

                ++dependencyCount[passIndex];
                dependents[dependencyIndex].push_back(passIndex);
            }
        }

        // Seed the ready queue with live passes that have no unresolved dependencies
        std::vector<uint32_t> readyPasses;
        for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
        {
            if (m_livePasses[passIndex] && dependencyCount[passIndex] == 0)
            {
                m_passDependencyLevels[passIndex] = 0;
                readyPasses.push_back(passIndex);
            }
        }

        /*
        Start the topological sort
        This loop continuously enqueues new passes, so a dynamically sized container is used
         
        Once a ready pass is added to the final execution order, use dependents[passIndex]
        to find every pass waiting on it and decrement their remaining dependency counts,
        Any pass whose count reaches zero becomes ready. Repeat until no ready passes remain
        */
        for (size_t readyIndex = 0; readyIndex < readyPasses.size(); ++readyIndex)
        {
            uint32_t passIndex = readyPasses[readyIndex];
            // Final pass execution order
            m_compiledPassOrder.push_back(passIndex);
            const uint32_t passLevel = m_passDependencyLevels[passIndex] == UINT32_MAX ? 0 : m_passDependencyLevels[passIndex];

            for (uint32_t dependentIndex : dependents[passIndex])
            {
                const uint32_t dependentLevel = passLevel + 1;
                if (m_passDependencyLevels[dependentIndex] == UINT32_MAX ||
                    m_passDependencyLevels[dependentIndex] < dependentLevel)
                {
                    m_passDependencyLevels[dependentIndex] = dependentLevel;
                }

                if (dependencyCount[dependentIndex] > 0)
                {
                    --dependencyCount[dependentIndex];
                }

                if (dependencyCount[dependentIndex] == 0)
                {
                    readyPasses.push_back(dependentIndex);
                }
            }
        }

        if (m_compiledPassOrder.size() == livePassCount)
        {
            BuildParallelPassBatches();
            return true;
        }

        m_compiledPassOrder.clear();
        m_passDependencyLevels.assign(passCount, UINT32_MAX);

        for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
        {
            if (m_livePasses[passIndex])
            {
                m_passDependencyLevels[passIndex] = static_cast<uint32_t>(m_compiledPassOrder.size());
                m_compiledPassOrder.push_back(passIndex);
            }
        }

#if defined(_DEBUG)
        OutputDebugStringA("[RDG][Compile] Dependency cycle detected, falling back to AddPass order.\n");
#endif

        BuildParallelPassBatches();
        return false;
    }

    void BuildParallelPassBatches()
    {
        m_parallelPassBatches.clear();

        for (uint32_t passIndex : m_compiledPassOrder)
        {
            if (passIndex >= m_passDependencyLevels.size() ||
                m_passDependencyLevels[passIndex] == UINT32_MAX)
            {
                continue;
            }

            const uint32_t level = m_passDependencyLevels[passIndex];
            if (level >= m_parallelPassBatches.size())
            {
                m_parallelPassBatches.resize(static_cast<size_t>(level) + 1);
            }

            m_parallelPassBatches[level].push_back(passIndex);
        }
    }

    void BuildCompileStats(bool compileSucceeded)
    {
        m_compileStats = {};
        m_compileStats.compileSucceeded = compileSucceeded;
        m_compileStats.totalPassCount = static_cast<uint32_t>(m_passes.size());
        m_compileStats.compiledPassCount = static_cast<uint32_t>(m_compiledPassOrder.size());
        m_compileStats.textureCount = static_cast<uint32_t>(m_textures.size());
        m_compileStats.bufferCount = static_cast<uint32_t>(m_buffers.size());
        m_compileStats.parallelBatchCount = static_cast<uint32_t>(m_parallelPassBatches.size());

        for (bool isLive : m_livePasses)
        {
            if (isLive)
            {
                ++m_compileStats.livePassCount;
            }
        }

        m_compileStats.culledPassCount = m_compileStats.totalPassCount - m_compileStats.livePassCount;

        for (const RDGTexture& texture : m_textures)
        {
            if (texture.external)
            {
                ++m_compileStats.externalTextureCount;
            }
            else
            {
                ++m_compileStats.transientTextureCount;
            }
        }

        for (const RDGBuffer& buffer : m_buffers)
        {
            if (buffer.external)
            {
                ++m_compileStats.externalBufferCount;
            }
            else
            {
                ++m_compileStats.transientBufferCount;
            }
        }

        for (const std::vector<D3D12_RESOURCE_BARRIER>& barriers : m_passBarriers)
        {
            m_compileStats.passBarrierCount += static_cast<uint32_t>(barriers.size());

            for (const D3D12_RESOURCE_BARRIER& barrier : barriers)
            {
                CountBarrierType(barrier, m_compileStats);
            }
        }

        m_compileStats.finalBarrierCount = static_cast<uint32_t>(m_finalBarriers.size());
        for (const D3D12_RESOURCE_BARRIER& barrier : m_finalBarriers)
        {
            CountBarrierType(barrier, m_compileStats);
        }

        for (const std::vector<uint32_t>& batch : m_parallelPassBatches)
        {
            const uint32_t batchSize = static_cast<uint32_t>(batch.size());
            if (m_compileStats.maxParallelBatchSize < batchSize)
            {
                m_compileStats.maxParallelBatchSize = batchSize;
            }
        }
    }

    static void CountBarrierType(
        const D3D12_RESOURCE_BARRIER& barrier,
        RDGCompileStats& stats)
    {
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        {
            ++stats.uavBarrierCount;
        }
        else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            ++stats.transitionBarrierCount;
        }
    }

    void BuildCompileSnapshot()
    {
        m_lastCompileSnapshot = {};
        m_lastCompileSnapshot.stats = m_compileStats;
        m_lastCompileSnapshot.compiledPassOrder = m_compiledPassOrder;
        m_lastCompileSnapshot.parallelPassBatches = m_parallelPassBatches;
        m_lastCompileSnapshot.passes.reserve(m_passes.size());

        for (uint32_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
        {
            RDGCompiledPassInfo passInfo = {};
            passInfo.passIndex = passIndex;
            passInfo.name = m_passes[passIndex].name;
            passInfo.live = passIndex < m_livePasses.size() && m_livePasses[passIndex];
            passInfo.culled = !passInfo.live;

            if (passIndex < m_passDependencyLevels.size())
            {
                passInfo.dependencyLevel = m_passDependencyLevels[passIndex];
            }

            m_lastCompileSnapshot.passes.push_back(std::move(passInfo));
        }
    }

    uint32_t CullUnusedPasses(uint32_t passCount)
    {
        m_livePasses.assign(passCount, false);

        std::vector<uint32_t> workList;

        // Passes marked with NeverCull and passes that write to the final RDG output are never culled
        for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
        {
            if (HasRDGPassFlag(m_passes[passIndex].flags, ERDGPassFlags::NeverCull) ||
                PassWritesExternalOutput(m_passes[passIndex]))
            {
                m_livePasses[passIndex] = true;
                workList.push_back(passIndex);
            }
        }

        // If all passes are eligible for culling, keep them all as a safety measure
        if (workList.empty())
        {
            for (uint32_t passIndex = 0; passIndex < passCount; ++passIndex)
            {
                m_livePasses[passIndex] = true;
            }

            return passCount;
        }

        /*
        Starting from each culling root,
        traverse the graph backward and recursively mark the root and all of its dependencies as live;
        mark all other passes as cullable.

        The topological sort then considers only the live passes when generating the final execution order.
        */
        for (size_t workIndex = 0; workIndex < workList.size(); ++workIndex)
        {
            uint32_t passIndex = workList[workIndex];

            for (uint32_t dependencyIndex : m_passes[passIndex].dependencies)
            {
                if (dependencyIndex >= passCount || m_livePasses[dependencyIndex])
                {
                    continue;
                }

                m_livePasses[dependencyIndex] = true;
                workList.push_back(dependencyIndex);
            }
        }

        uint32_t livePassCount = 0;

        for (bool isLive : m_livePasses)
        {
            if (isLive)
            {
                ++livePassCount;
            }
        }

        return livePassCount;
    }

    bool PassWritesExternalOutput(const RDGPass& pass) const
    {
        for (const RDGTextureAccess& access : pass.parameters.textures)
        {
            if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
            {
                continue;
            }

            if (IsWriteAccess(access.access) &&
                m_textures[access.texture.index].externalOutput)
            {
                return true;
            }
        }

        for (const RDGBufferAccess& access : pass.parameters.buffers)
        {
            if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
            {
                continue;
            }

            if (IsWriteAccess(access.access) &&
                m_buffers[access.buffer.index].externalOutput)
            {
                return true;
            }
        }

        return false;
    }

    /*
    Queue a transition barrier when a resource's current state differs from the state required by the pass,
    if the resource remains in UAV state, queue a UAV barrier only after a previous UAV access,
    after all passes, queue final transitions that restore resources to their declared final states.
    */
    void BuildBarrierPlan()
    {
        m_passBarriers.clear();
        m_passBarriers.resize(m_passes.size());
        m_finalBarriers.clear();

        for (RDGTexture& texture : m_textures)
        {
            texture.currentState = texture.initialState;
        }

        for (RDGBuffer& buffer : m_buffers)
        {
            buffer.currentState = buffer.initialState;
        }

        std::vector<bool> textureHadUAVAccess(m_textures.size(), false);
        std::vector<bool> bufferHadUAVAccess(m_buffers.size(), false);

        for (uint32_t passIndex : m_compiledPassOrder)
        {
            if (passIndex >= m_passes.size())
            {
                continue;
            }

            RDGPass& pass = m_passes[passIndex];
            std::vector<D3D12_RESOURCE_BARRIER>& barriers = m_passBarriers[passIndex];

            for (const RDGTextureAccess& access : pass.parameters.textures)
            {
                if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
                {
                    continue;
                }

                RDGTexture& texture = m_textures[access.texture.index];

                if (texture.resource == nullptr)
                {
                    continue;
                }

                D3D12_RESOURCE_STATES desiredState = ToD3D12State(access.access);

                if (texture.currentState != desiredState)
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    barrier.Transition.pResource = texture.resource;
                    barrier.Transition.StateBefore = texture.currentState;
                    barrier.Transition.StateAfter = desiredState;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    barriers.push_back(barrier);
                    TraceBarrier(pass.name.c_str(), texture.name, texture.currentState, desiredState);
                    texture.currentState = desiredState;
                }
                else if (access.access == ERDGAccess::UAV &&
                    textureHadUAVAccess[access.texture.index])
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    barrier.UAV.pResource = texture.resource;

                    barriers.push_back(barrier);
                    TraceUAVBarrier(pass.name.c_str(), texture.name);
                }

                if (access.access == ERDGAccess::UAV)
                {
                    textureHadUAVAccess[access.texture.index] = true;
                }
            }

            for (const RDGBufferAccess& access : pass.parameters.buffers)
            {
                if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
                {
                    continue;
                }

                RDGBuffer& buffer = m_buffers[access.buffer.index];

                if (buffer.resource == nullptr)
                {
                    continue;
                }

                D3D12_RESOURCE_STATES desiredState = ToD3D12State(access.access);

                if (buffer.currentState != desiredState)
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    barrier.Transition.pResource = buffer.resource;
                    barrier.Transition.StateBefore = buffer.currentState;
                    barrier.Transition.StateAfter = desiredState;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    barriers.push_back(barrier);
                    TraceBarrier(pass.name.c_str(), buffer.name, buffer.currentState, desiredState);
                    buffer.currentState = desiredState;
                }
                else if (access.access == ERDGAccess::UAV &&
                    bufferHadUAVAccess[access.buffer.index])
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    barrier.UAV.pResource = buffer.resource;

                    barriers.push_back(barrier);
                    TraceUAVBarrier(pass.name.c_str(), buffer.name);
                }

                if (access.access == ERDGAccess::UAV)
                {
                    bufferHadUAVAccess[access.buffer.index] = true;
                }
            }
        }

        for (RDGTexture& texture : m_textures)
        {
            if (texture.resource == nullptr)
            {
                continue;
            }

            if (texture.currentState != texture.finalState)
            {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = texture.resource;
                barrier.Transition.StateBefore = texture.currentState;
                barrier.Transition.StateAfter = texture.finalState;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                m_finalBarriers.push_back(barrier);
                TraceBarrier("FinalState", texture.name, texture.currentState, texture.finalState);
                texture.currentState = texture.finalState;
            }
        }

        for (RDGBuffer& buffer : m_buffers)
        {
            if (buffer.resource == nullptr)
            {
                continue;
            }

            if (buffer.currentState != buffer.finalState)
            {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = buffer.resource;
                barrier.Transition.StateBefore = buffer.currentState;
                barrier.Transition.StateAfter = buffer.finalState;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                m_finalBarriers.push_back(barrier);
                TraceBarrier("FinalState", buffer.name, buffer.currentState, buffer.finalState);
                buffer.currentState = buffer.finalState;
            }
        }
    }

    void BuildResourceLifetimes()
    {
        for (RDGTexture& texture : m_textures)
        {
            texture.firstUsePass = UINT32_MAX;
            texture.lastUsePass = UINT32_MAX;
        }

        for (RDGBuffer& buffer : m_buffers)
        {
            buffer.firstUsePass = UINT32_MAX;
            buffer.lastUsePass = UINT32_MAX;
        }

        for (uint32_t passIndex : m_compiledPassOrder)
        {
            if (passIndex >= m_passes.size())
            {
                continue;
            }

            const RDGPass& pass = m_passes[passIndex];

            for (const RDGTextureAccess& access : pass.parameters.textures)
            {
                if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
                {
                    continue;
                }

                RDGTexture& texture = m_textures[access.texture.index];

                if (texture.firstUsePass == UINT32_MAX)
                {
                    texture.firstUsePass = passIndex;
                }

                texture.lastUsePass = passIndex;
            }

            for (const RDGBufferAccess& access : pass.parameters.buffers)
            {
                if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
                {
                    continue;
                }

                RDGBuffer& buffer = m_buffers[access.buffer.index];

                if (buffer.firstUsePass == UINT32_MAX)
                {
                    buffer.firstUsePass = passIndex;
                }

                buffer.lastUsePass = passIndex;
            }
        }
    }

    bool AreLifetimesDisjoint(
        uint32_t firstA,
        uint32_t lastA,
        uint32_t firstB,
        uint32_t lastB) const
    {
        if (firstA == UINT32_MAX || lastA == UINT32_MAX ||
            firstB == UINT32_MAX || lastB == UINT32_MAX)
        {
            return false;
        }

        return lastA < firstB || lastB < firstA;
    }

    bool CanAliasTextures(const RDGTexture& a, const RDGTexture& b) const
    {
        if (a.external || b.external)
        {
            return false;
        }

        if (!AreLifetimesDisjoint(a.firstUsePass, a.lastUsePass, b.firstUsePass, b.lastUsePass))
        {
            return false;
        }

        return a.desc.width == b.desc.width &&
            a.desc.height == b.desc.height &&
            a.desc.arraySize == b.desc.arraySize &&
            a.desc.mipLevels == b.desc.mipLevels &&
            a.desc.format == b.desc.format &&
            a.desc.flags == b.desc.flags;
    }

    bool CanAliasBuffers(const RDGBuffer& a, const RDGBuffer& b) const
    {
        if (a.external || b.external)
        {
            return false;
        }

        if (!AreLifetimesDisjoint(a.firstUsePass, a.lastUsePass, b.firstUsePass, b.lastUsePass))
        {
            return false;
        }

        return a.desc.sizeInBytes == b.desc.sizeInBytes &&
            a.desc.alignment == b.desc.alignment &&
            a.desc.flags == b.desc.flags;
    }

    void SubmitBarriers(
        ID3D12GraphicsCommandList* cmdList,
        const std::vector<D3D12_RESOURCE_BARRIER>& barriers) const
    {
        if (!barriers.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    void AddPassDependency(RDGPass& pass, uint32_t dependencyPassIndex) const
    {
        if (dependencyPassIndex == UINT32_MAX)
        {
            return;
        }

        for (uint32_t existingDependency : pass.dependencies)
        {
            if (existingDependency == dependencyPassIndex)
            {
                return;
            }
        }

        pass.dependencies.push_back(dependencyPassIndex);
    }

    RDGTextureHandle FindRegisteredExternalTexture(ID3D12Resource* resource) const
    {
        RDGTextureHandle handle;

        if (resource == nullptr)
        {
            return handle;
        }

        for (size_t textureIndex = 0; textureIndex < m_textures.size(); ++textureIndex)
        {
            if (m_textures[textureIndex].resource == resource)
            {
                handle.index = static_cast<uint32_t>(textureIndex);
                return handle;
            }
        }

        return handle;
    }

    RDGBufferHandle FindRegisteredExternalBuffer(ID3D12Resource* resource) const
    {
        RDGBufferHandle handle;

        if (resource == nullptr)
        {
            return handle;
        }

        for (size_t bufferIndex = 0; bufferIndex < m_buffers.size(); ++bufferIndex)
        {
            if (m_buffers[bufferIndex].resource == resource)
            {
                handle.index = static_cast<uint32_t>(bufferIndex);
                return handle;
            }
        }

        return handle;
    }

    bool IsReadAccess(ERDGAccess access) const
    {
        return access == ERDGAccess::SRV ||
            access == ERDGAccess::ComputeSRV ||
            access == ERDGAccess::DSVRead ||
            access == ERDGAccess::CopySrc;
    }

    bool IsWriteAccess(ERDGAccess access) const
    {
        return access == ERDGAccess::UAV ||
            access == ERDGAccess::RTV ||
            access == ERDGAccess::DSVWrite ||
            access == ERDGAccess::CopyDst ||
            access == ERDGAccess::Present;
    }

    const char* ToString(ERDGAccess access) const
    {
        switch (access)
        {
        case ERDGAccess::SRV: return "SRV";
        case ERDGAccess::ComputeSRV: return "ComputeSRV";
        case ERDGAccess::UAV: return "UAV";
        case ERDGAccess::RTV: return "RTV";
        case ERDGAccess::DSVRead: return "DSVRead";
        case ERDGAccess::DSVWrite: return "DSVWrite";
        case ERDGAccess::CopySrc: return "CopySrc";
        case ERDGAccess::CopyDst: return "CopyDst";
        case ERDGAccess::Present: return "Present";
        default: return "Unknown";
        }
    }

    const char* ToString(D3D12_RESOURCE_STATES state) const
    {
        if (state == D3D12_RESOURCE_STATE_COMMON)
            return "COMMON/PRESENT";

        if (state == D3D12_RESOURCE_STATE_RENDER_TARGET)
            return "RENDER_TARGET";

        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            return "UNORDERED_ACCESS";

        if (state == D3D12_RESOURCE_STATE_DEPTH_WRITE)
            return "DEPTH_WRITE";

        if (state == D3D12_RESOURCE_STATE_DEPTH_READ)
            return "DEPTH_READ";

        if (state == D3D12_RESOURCE_STATE_COPY_SOURCE)
            return "COPY_SOURCE";

        if (state == D3D12_RESOURCE_STATE_COPY_DEST)
            return "COPY_DEST";

        if (state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            return "PIXEL_SHADER_RESOURCE";

        if (state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            return "NON_PIXEL_SHADER_RESOURCE";

        if (state == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
            return "SHADER_RESOURCE";

        return "UNKNOWN_STATE";
    }

    void TraceBarrier(
        const char* phase,
        const std::string& textureName,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after) const
    {
    #if defined(_DEBUG)
        if constexpr (EnableBarrierDump)
        {
            std::ostringstream oss;
            oss << "[RDG][Barrier] Graph '" << m_debugName
                << "' " << phase
                << ": " << textureName
                << " " << ToString(before)
                << " -> " << ToString(after)
                << "\n";

            OutputDebugStringA(oss.str().c_str());
        }
    #endif
    }

    void TraceUAVBarrier(
        const char* phase,
        const std::string& resourceName) const
    {
    #if defined(_DEBUG)
        if constexpr (EnableBarrierDump)
        {
            std::ostringstream oss;
            oss << "[RDG][Barrier] Graph '" << m_debugName
                << "' " << phase
                << ": UAV barrier for " << resourceName
                << "\n";

            OutputDebugStringA(oss.str().c_str());
        }
    #endif
    }

    void TraceLifecycleWarning(const char* message) const
    {
    #if defined(_DEBUG)
        std::ostringstream oss;
        oss << "[RDG][Lifecycle] Graph '" << m_debugName
            << "': " << (message ? message : "Unknown lifecycle warning")
            << "\n";

        OutputDebugStringA(oss.str().c_str());
    #endif
    }

    void ValidateGraph() const
    {
        std::ostringstream oss;
        bool hasIssue = false;

        for (size_t textureIndex = 0; textureIndex < m_textures.size(); ++textureIndex)
        {
            const RDGTexture& texture = m_textures[textureIndex];

            if (texture.resource == nullptr)
            {
                hasIssue = true;
                oss << "[RDG][Validation] Graph '" << m_debugName
                    << "' registered null texture at index " << textureIndex
                    << " (" << texture.name << ")\n";
            }
        }

        for (size_t bufferIndex = 0; bufferIndex < m_buffers.size(); ++bufferIndex)
        {
            const RDGBuffer& buffer = m_buffers[bufferIndex];

            if (buffer.resource == nullptr)
            {
                hasIssue = true;
                oss << "[RDG][Validation] Graph '" << m_debugName
                    << "' registered null buffer at index " << bufferIndex
                    << " (" << buffer.name << ")\n";
            }
        }

        for (size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
        {
            const RDGPass& pass = m_passes[passIndex];

            for (size_t accessIndex = 0; accessIndex < pass.parameters.textures.size(); ++accessIndex)
            {
                const RDGTextureAccess& access = pass.parameters.textures[accessIndex];

                if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
                {
                    hasIssue = true;
                    oss << "[RDG][Validation] Graph '" << m_debugName
                        << "', pass '" << pass.name
                        << "' uses invalid texture handle at access " << accessIndex << "\n";
                    continue;
                }

                if (access.access == ERDGAccess::Unknown)
                {
                    hasIssue = true;
                    oss << "[RDG][Validation] Graph '" << m_debugName
                        << "', pass '" << pass.name
                        << "' uses Unknown access for texture '"
                        << m_textures[access.texture.index].name << "'\n";
                }

                bool thisIsRead = IsReadAccess(access.access);
                bool thisIsWrite = IsWriteAccess(access.access);

                for (size_t prevIndex = 0; prevIndex < accessIndex; ++prevIndex)
                {
                    const RDGTextureAccess& previous = pass.parameters.textures[prevIndex];

                    if (!previous.texture.IsValid() || previous.texture.index != access.texture.index)
                    {
                        continue;
                    }

                    bool previousIsRead = IsReadAccess(previous.access);
                    bool previousIsWrite = IsWriteAccess(previous.access);

                    if (thisIsWrite && previousIsWrite)
                    {
                        hasIssue = true;
                        oss << "[RDG][Validation] Graph '" << m_debugName
                            << "', pass '" << pass.name
                            << "' writes texture '" << m_textures[access.texture.index].name
                            << "' more than once\n";
                    }

                    if ((thisIsRead && previousIsWrite) || (thisIsWrite && previousIsRead))
                    {
                        hasIssue = true;
                        oss << "[RDG][Validation] Graph '" << m_debugName
                            << "', pass '" << pass.name
                            << "' reads and writes texture '"
                            << m_textures[access.texture.index].name
                            << "' in the same pass\n";
                    }
                }
            }

            for (size_t accessIndex = 0; accessIndex < pass.parameters.buffers.size(); ++accessIndex)
            {
                const RDGBufferAccess& access = pass.parameters.buffers[accessIndex];

                if (!access.buffer.IsValid() || access.buffer.index >= m_buffers.size())
                {
                    hasIssue = true;
                    oss << "[RDG][Validation] Graph '" << m_debugName
                        << "', pass '" << pass.name
                        << "' uses invalid buffer handle at access " << accessIndex << "\n";
                    continue;
                }

                if (access.access == ERDGAccess::Unknown)
                {
                    hasIssue = true;
                    oss << "[RDG][Validation] Graph '" << m_debugName
                        << "', pass '" << pass.name
                        << "' uses Unknown access for buffer '"
                        << m_buffers[access.buffer.index].name << "'\n";
                }

                bool thisIsRead = IsReadAccess(access.access);
                bool thisIsWrite = IsWriteAccess(access.access);

                for (size_t prevIndex = 0; prevIndex < accessIndex; ++prevIndex)
                {
                    const RDGBufferAccess& previous = pass.parameters.buffers[prevIndex];

                    if (!previous.buffer.IsValid() || previous.buffer.index != access.buffer.index)
                    {
                        continue;
                    }

                    bool previousIsRead = IsReadAccess(previous.access);
                    bool previousIsWrite = IsWriteAccess(previous.access);

                    if (thisIsWrite && previousIsWrite)
                    {
                        hasIssue = true;
                        oss << "[RDG][Validation] Graph '" << m_debugName
                            << "', pass '" << pass.name
                            << "' writes buffer '" << m_buffers[access.buffer.index].name
                            << "' more than once\n";
                    }

                    if ((thisIsRead && previousIsWrite) || (thisIsWrite && previousIsRead))
                    {
                        hasIssue = true;
                        oss << "[RDG][Validation] Graph '" << m_debugName
                            << "', pass '" << pass.name
                            << "' reads and writes buffer '"
                            << m_buffers[access.buffer.index].name
                            << "' in the same pass\n";
                    }
                }
            }
        }

        if (hasIssue)
        {
            OutputDebugStringA(oss.str().c_str());
        }
    }

    void DumpGraph() const
    {
        std::ostringstream oss;
        oss << "[RDG] Graph: " << m_debugName << "\n";
        oss << "  Stats:"
            << " passes=" << m_compileStats.totalPassCount
            << " live=" << m_compileStats.livePassCount
            << " culled=" << m_compileStats.culledPassCount
            << " compiled=" << m_compileStats.compiledPassCount
            << " textures=" << m_compileStats.textureCount
            << " buffers=" << m_compileStats.bufferCount
            << " passBarriers=" << m_compileStats.passBarrierCount
            << " finalBarriers=" << m_compileStats.finalBarrierCount
            << " transitions=" << m_compileStats.transitionBarrierCount
            << " uavBarriers=" << m_compileStats.uavBarrierCount
            << " batches=" << m_compileStats.parallelBatchCount
            << " maxBatch=" << m_compileStats.maxParallelBatchSize
            << " compile=" << (m_compileStats.compileSucceeded ? "ok" : "fallback")
            << "\n";

        if (!m_compiledPassOrder.empty())
        {
            oss << "  CompiledOrder:";

            for (uint32_t passIndex : m_compiledPassOrder)
            {
                oss << " " << passIndex;

                if (passIndex < m_passes.size())
                {
                    oss << "(" << m_passes[passIndex].name << ")";
                }
            }

            oss << "\n";
        }

        if (!m_passDependencyLevels.empty())
        {
            oss << "  DependencyLevels:";

            for (uint32_t passIndex : m_compiledPassOrder)
            {
                if (passIndex < m_passDependencyLevels.size() &&
                    m_passDependencyLevels[passIndex] != UINT32_MAX)
                {
                    oss << " " << passIndex << "=" << m_passDependencyLevels[passIndex];
                }
            }

            oss << "\n";
        }

        if (!m_parallelPassBatches.empty())
        {
            oss << "  ParallelBatches:\n";

            for (size_t batchIndex = 0; batchIndex < m_parallelPassBatches.size(); ++batchIndex)
            {
                const std::vector<uint32_t>& batch = m_parallelPassBatches[batchIndex];
                if (batch.empty())
                {
                    continue;
                }

                oss << "    Batch " << batchIndex << ":";

                for (uint32_t passIndex : batch)
                {
                    oss << " " << passIndex;

                    if (passIndex < m_passes.size())
                    {
                        oss << "(" << m_passes[passIndex].name << ")";
                    }
                }

                oss << "\n";
            }
        }

        for (size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
        {
            const RDGPass& pass = m_passes[passIndex];
            oss << "  Pass " << passIndex << ": " << pass.name;

            if (passIndex < m_livePasses.size() && !m_livePasses[passIndex])
            {
                oss << " [Culled]";
            }

            oss << "\n";

            if (passIndex < m_passBarriers.size() && !m_passBarriers[passIndex].empty())
            {
                oss << "    BarriersBefore: " << m_passBarriers[passIndex].size() << "\n";
            }

            if (!pass.dependencies.empty())
            {
                oss << "    DependsOn:";

                for (uint32_t dependencyIndex : pass.dependencies)
                {
                    oss << " " << dependencyIndex;

                    if (dependencyIndex < m_passes.size())
                    {
                        oss << "(" << m_passes[dependencyIndex].name << ")";
                    }
                }

                oss << "\n";
            }

            for (const RDGTextureAccess& access : pass.parameters.textures)
            {
                const char* textureName = "InvalidTexture";

                if (access.texture.IsValid() && access.texture.index < m_textures.size())
                {
                    textureName = m_textures[access.texture.index].name.c_str();
                }

                oss << "    " << textureName << " -> " << ToString(access.access) << "\n";
            }

            for (const RDGBufferAccess& access : pass.parameters.buffers)
            {
                const char* bufferName = "InvalidBuffer";

                if (access.buffer.IsValid() && access.buffer.index < m_buffers.size())
                {
                    bufferName = m_buffers[access.buffer.index].name.c_str();
                }

                oss << "    " << bufferName << " -> " << ToString(access.access) << "\n";
            }
        }

        for (size_t textureIndex = 0; textureIndex < m_textures.size(); ++textureIndex)
        {
            const RDGTexture& texture = m_textures[textureIndex];

            if (texture.firstUsePass != UINT32_MAX)
            {
                oss << "  TextureLifetime " << textureIndex
                    << "(" << texture.name << "): "
                    << texture.firstUsePass << " -> " << texture.lastUsePass << "\n";
            }
        }

        for (size_t bufferIndex = 0; bufferIndex < m_buffers.size(); ++bufferIndex)
        {
            const RDGBuffer& buffer = m_buffers[bufferIndex];

            if (buffer.firstUsePass != UINT32_MAX)
            {
                oss << "  BufferLifetime " << bufferIndex
                    << "(" << buffer.name << "): "
                    << buffer.firstUsePass << " -> " << buffer.lastUsePass << "\n";
            }
        }

        for (size_t a = 0; a < m_textures.size(); ++a)
        {
            for (size_t b = a + 1; b < m_textures.size(); ++b)
            {
                if (CanAliasTextures(m_textures[a], m_textures[b]))
                {
                    oss << "  TextureAliasCandidate "
                        << a << "(" << m_textures[a].name << ") <-> "
                        << b << "(" << m_textures[b].name << ")\n";
                }
            }
        }

        for (size_t a = 0; a < m_buffers.size(); ++a)
        {
            for (size_t b = a + 1; b < m_buffers.size(); ++b)
            {
                if (CanAliasBuffers(m_buffers[a], m_buffers[b]))
                {
                    oss << "  BufferAliasCandidate "
                        << a << "(" << m_buffers[a].name << ") <-> "
                        << b << "(" << m_buffers[b].name << ")\n";
                }
            }
        }

        if (!m_finalBarriers.empty())
        {
            oss << "  FinalBarriers: " << m_finalBarriers.size() << "\n";
        }

        OutputDebugStringA(oss.str().c_str());
    }

    static D3D12_RESOURCE_STATES ToD3D12State(ERDGAccess access)
    {
        switch (access)
        {
        case ERDGAccess::SRV:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ERDGAccess::ComputeSRV:
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ERDGAccess::UAV:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ERDGAccess::RTV:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ERDGAccess::DSVRead:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ERDGAccess::DSVWrite:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ERDGAccess::CopySrc:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ERDGAccess::CopyDst:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case ERDGAccess::Present:
            return D3D12_RESOURCE_STATE_PRESENT;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    RenderDevice* m_deviceContext = nullptr;
    std::string m_debugName;
    bool m_executed = false;

    static constexpr UINT MaxTransientRTVDescriptors = 64;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_transientRTVHeap;
    UINT m_transientRTVDescriptorSize = 0;
    UINT m_transientRTVCount = 0;

    static constexpr UINT MaxTransientDSVDescriptors = 64;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_transientDSVHeap;
    UINT m_transientDSVDescriptorSize = 0;
    UINT m_transientDSVCount = 0;

    RDGTransientResourceAllocator m_transientResourceAllocator;
    RDGTransientSrvUavDescriptorAllocator m_transientSrvUavDescriptorAllocator;

    std::vector<RDGTexture> m_textures;
    std::vector<RDGBuffer> m_buffers;
    std::vector<RDGPass> m_passes;
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> m_passBarriers;
    std::vector<D3D12_RESOURCE_BARRIER> m_finalBarriers;
    std::vector<bool> m_livePasses;
    std::vector<uint32_t> m_compiledPassOrder;
    std::vector<uint32_t> m_passDependencyLevels;
    std::vector<std::vector<uint32_t>> m_parallelPassBatches;
    RDGCompileStats m_compileStats;
    RDGCompileSnapshot m_lastCompileSnapshot;
    std::vector<RDGTextureExtraction> m_textureExtractions;
    std::vector<RDGBufferExtraction> m_bufferExtractions;
};

#endif
