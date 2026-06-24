#ifndef RDG_H
#define RDG_H

#include "stdafx.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <sstream>

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
    std::vector<uint32_t> dependencies;
};

struct RDGTexture
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
    bool external = false;
    uint32_t lastProducer = UINT32_MAX;
    std::string name;
};

class RDGBuilder
{
public:
#if defined(_DEBUG)
    static constexpr bool EnableValidation = true;
    static constexpr bool EnableGraphDump = true;
    static constexpr bool EnableBarrierDump = true;
#endif

    RDGBuilder(RenderDevice* deviceContext, const char* debugName)
        : m_deviceContext(deviceContext), m_debugName(debugName ? debugName : "RDG")
    {}

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

            if ((IsReadAccess(access.access) || IsWriteAccess(access.access)) &&
                texture.lastProducer != UINT32_MAX)
            {
                AddPassDependency(pass, texture.lastProducer);
            }
        }

        for (const RDGTextureAccess& access : parameters.textures)
        {
            if (!access.texture.IsValid() || access.texture.index >= m_textures.size())
            {
                continue;
            }

            if (IsWriteAccess(access.access))
            {
                m_textures[access.texture.index].lastProducer = passIndex;
            }
        }

        m_passes.push_back(std::move(pass));
    }

    void Execute(ID3D12GraphicsCommandList* cmdList)
    {
    #if defined(_DEBUG)
        if constexpr (EnableValidation)
        {
            ValidateGraph();
        }

        if constexpr (EnableGraphDump)
        {
            DumpGraph();
        }
    #endif

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
                TraceBarrier(pass.name.c_str(), texture.name, texture.currentState, desiredState);
                texture.currentState = desiredState;
            }
        }

        if (!barriers.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
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
                TraceBarrier("FinalState", texture.name, texture.currentState, texture.finalState);
                texture.currentState = texture.finalState;
            }
        }

        if (!barriers.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    bool IsReadAccess(ERDGAccess access) const
    {
        return access == ERDGAccess::SRV ||
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

        for (size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
        {
            const RDGPass& pass = m_passes[passIndex];
            oss << "  Pass " << passIndex << ": " << pass.name << "\n";

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
        }

        OutputDebugStringA(oss.str().c_str());
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