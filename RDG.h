#ifndef RDG_H
#define RDG_H

#include "stdafx.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class RenderDevice;

enum class ERDGPassFlags
{
    Graphics,
    Compute,
    Copy
};

enum class ERDGAccess
{
    Unknown,
    SRV,
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

struct RDGPassParameters
{
    std::vector<RDGTextureAccess> textures;

    void ReadSRV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::SRV });
    }

    void WriteRTV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::RTV });
    }

    void WriteUAV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::UAV });
    }

    void WriteDSV(RDGTextureHandle texture)
    {
        textures.push_back({ texture, ERDGAccess::DSVWrite });
    }
};

struct RDGPass
{
    std::string name;
    ERDGPassFlags flags = ERDGPassFlags::Graphics;
    RDGPassParameters parameters;
    std::function<void(ID3D12GraphicsCommandList*)> execute;
};

struct RDGTexture
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
    bool external = false;
    std::string name;
};

class RDGBuilder
{
public:
    RDGBuilder(RenderDevice* deviceContext, const char* debugName)
        : m_deviceContext(deviceContext), m_debugName(debugName ? debugName : "RDG")
    {}

    RDGTextureHandle RegisterExternalTexture(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const char* name)
    {
        RDGTexture texture;
        texture.resource = resource;
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

    void AddPass(
        const char* name,
        ERDGPassFlags flags,
        const RDGPassParameters& parameters,
        std::function<void(ID3D12GraphicsCommandList*)> execute)
    {
        RDGPass pass;
        pass.name = name ? name : "UnnamedPass";
        pass.flags = flags;
        pass.parameters = parameters;
        pass.execute = execute;
        m_passes.push_back(std::move(pass));
    }

    void Execute(ID3D12GraphicsCommandList* cmdList)
    {
        for (RDGPass& pass : m_passes)
        {
            TransitionResourcesForPass(cmdList, pass);

            if (pass.execute)
            {
                pass.execute(cmdList);
            }
        }

        TransitionResourcesToFinalStates(cmdList);

        m_passes.clear();
    }

private:
    void TransitionResourcesForPass(ID3D12GraphicsCommandList* cmdList, const RDGPass& pass)
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

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
                texture.currentState = desiredState;
            }
        }

        if (!barriers.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    void TransitionResourcesToFinalStates(ID3D12GraphicsCommandList* cmdList)
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;

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

                barriers.push_back(barrier);
                texture.currentState = texture.finalState;
            }
        }

        if (!barriers.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    static D3D12_RESOURCE_STATES ToD3D12State(ERDGAccess access)
    {
        switch (access)
        {
        case ERDGAccess::SRV:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
    std::vector<RDGTexture> m_textures;
    std::vector<RDGPass> m_passes;
};

#endif