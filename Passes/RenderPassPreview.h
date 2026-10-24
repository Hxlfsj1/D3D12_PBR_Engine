#pragma once

#include "stdafx.h"
#include "RDG.h"
#include "PBR_Shader.h"
#include <algorithm>
#include <array>
#include <string>

// Debug presentation reads a snapshot of a stage; it never redirects normal
// lighting or temporal history. No resources are allocated until a view is used.
class RenderPassPreview
{
public:
    enum class Mapping : uint32_t
    {
        HDR, LDR, Normal, Scalar, Depth, Motion, Shadow, DeltaHDR, RawDifference, LinearColor
    };
    struct Source
    {
        RDGTextureHandle texture;
        Mapping mapping = Mapping::HDR;
        RDGTextureHandle beforeTexture;
        uint32_t channel = 0;
        uint32_t slice = UINT_MAX; // Shadow arrays: UINT_MAX displays a 2x2 atlas.
        float nearZ = 0.1f;
        float farZ = 1000.0f;
    };
    struct Output
    {
        RDGTextureHandle texture;
        RDGPassHandle pass;
        bool IsValid() const { return texture.IsValid() && pass.IsValid(); }
    };

    bool Initialize(RenderDevice& device)
    {
        if (m_root && m_display && m_difference) return true;
        auto* gpu = device.GetDevice();
        if (!gpu) return false;
        CD3DX12_ROOT_PARAMETER parameter;
        parameter.InitAsConstants(12, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        const CD3DX12_ROOT_SIGNATURE_DESC desc(1, &parameter, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        if (!Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &blob, &errors), "serialize root signature")) return false;
        if (!Check(gpu->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&m_root)), "create root signature")) return false;
        const std::wstring path = L"Shaders/Shaders_For_RenderPassPreview.hlsl";
        const auto vs = ShaderCompiler::CompileFromFile(path, L"VSMain", L"vs_6_6");
        const auto ps = ShaderCompiler::CompileFromFile(path, L"PSMain", L"ps_6_6");
        if (!vs || !ps) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_root.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.SampleDesc.Count = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!Check(gpu->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_display)), "create display pipeline")) return false;
        pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return Check(gpu->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_difference)), "create difference pipeline");
    }

    // Insert exactly at the stage boundary. Declared CopySrc/CopyDst accesses
    // preserve the old contents before a later pass writes the same SceneColor.
    static Output Snapshot(RDGBuilder& graph, RDGTextureHandle source, const char* name = "PassView.Snapshot")
    {
        const auto* input = graph.GetTextureDesc(source);
        if (!input) return {};
        auto desc = *input;
        desc.flags = D3D12_RESOURCE_FLAG_NONE;
        desc.hasClearValue = false;
        const auto target = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON, name);
        if (!target.IsValid()) return {};
        auto* src = graph.GetTextureResource(source);
        auto* dst = graph.GetTextureResource(target);
        if (!src || !dst) return {};
        RDGPassParameters params;
        params.ReadCopySrc(source);
        params.WriteCopyDst(target);
        const auto pass = graph.AddPass(name, ERDGPassFlags::Copy, params,
            [src, dst](ID3D12GraphicsCommandList* cmd) { cmd->CopyResource(dst, src); });
        return {target, pass};
    }

    Output AddToGraph(RDGBuilder& graph, ID3D12DescriptorHeap* heap, const Source& source,
        UINT width, UINT height, const char* name = "PassView.Visualize") const
    {
        return CreateOutput(graph, heap, source, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, name);
    }

    // Raw signed color change in linear HDR units. This supports exact exports
    // even when a transparent pass darkens the existing background.
    Output Difference(RDGBuilder& graph, ID3D12DescriptorHeap* heap,
        RDGTextureHandle after, RDGTextureHandle before, const char* name = "PassView.Difference") const
    {
        const auto* desc = graph.GetTextureDesc(after);
        const auto* previous = graph.GetTextureDesc(before);
        if (!desc || !previous || desc->width != previous->width || desc->height != previous->height ||
            desc->arraySize != 1 || previous->arraySize != 1) return {};
        Source source{after, Mapping::RawDifference, before};
        return CreateOutput(graph, heap, source, desc->width, desc->height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, name);
    }

    // The caller registers the back buffer as an output with its actual initial
    // and final state. This pass follows the normal final scene pass.
    RDGPassHandle PresentToGraph(RDGBuilder& graph, ID3D12DescriptorHeap* heap,
        RDGTextureHandle preview, RDGTextureHandle target, UINT width, UINT height) const
    {
        return RenderInto(graph, heap, Source{preview, Mapping::LDR}, target, width, height,
            DXGI_FORMAT_R8G8B8A8_UNORM, "PassView.Present");
    }

private:
    struct Constants
    {
        uint32_t sourceIndex, beforeIndex, mode, channel;
        uint32_t slice, arraySize, width, height;
        float nearZ, farZ, exposure, motionScale;
    };
    static_assert(sizeof(Constants) == 48);

    static bool Check(HRESULT hr, const char* action)
    {
        if (SUCCEEDED(hr)) return true;
        ErrorLog::HRESULT(std::string("Pass preview: failed to ") + action + '.', hr);
        return false;
    }

    static RDGTextureSRVHandle CreateView(RDGBuilder& graph, RDGTextureHandle texture)
    {
        const auto* input = graph.GetTextureDesc(texture);
        if (!input) return {};
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = input->format;
        if (srv.Format == DXGI_FORMAT_R32_TYPELESS || srv.Format == DXGI_FORMAT_D32_FLOAT)
            srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (input->arraySize > 1)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = 1;
            srv.Texture2DArray.ArraySize = input->arraySize;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
        }
        return graph.CreateTextureSRVView(texture, &srv);
    }

    Output CreateOutput(RDGBuilder& graph, ID3D12DescriptorHeap* heap, const Source& source,
        UINT width, UINT height, DXGI_FORMAT format, const char* name) const
    {
        if (!width || !height || !source.texture.IsValid()) return {};
        RDGTextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto target = graph.CreateTexture(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, name);
        if (!target.IsValid()) return {};
        const auto pass = RenderInto(graph, heap, source, target, width, height, format, name);
        return {target, pass};
    }

    RDGPassHandle RenderInto(RDGBuilder& graph, ID3D12DescriptorHeap* heap, const Source& source,
        RDGTextureHandle target, UINT width, UINT height, DXGI_FORMAT format, const char* name) const
    {
        if (!m_root || !m_display || !m_difference || !heap || !width || !height ||
            !target.IsValid() || target.index == source.texture.index) return {};
        const auto* desc = graph.GetTextureDesc(source.texture);
        if (!desc) return {};
        const auto input = CreateView(graph, source.texture);
        const auto before = source.beforeTexture.IsValid() ? CreateView(graph, source.beforeTexture) : input;
        const auto output = graph.CreateTextureRTVView(target);
        if (!input.IsValid() || !before.IsValid() || !output.IsValid()) return {};
        Constants cb{input.descriptorIndex, before.descriptorIndex, static_cast<uint32_t>(source.mapping),
            (std::min)(source.channel, 3u), source.slice, desc->arraySize, desc->width, desc->height,
            source.nearZ, source.farZ, 1.0f, 32.0f};
        RDGPassParameters params;
        params.ReadSRV(input);
        if (source.beforeTexture.IsValid()) params.ReadSRV(before);
        params.WriteRTV(output);
        auto* root = m_root.Get();
        auto* pipeline = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? m_difference.Get() : m_display.Get();
        const auto pass = graph.AddPass(name, ERDGPassFlags::Graphics, params,
            [=](ID3D12GraphicsCommandList* cmd)
            {
                ID3D12DescriptorHeap* heaps[]{heap};
                cmd->SetDescriptorHeaps(1, heaps);
                cmd->SetGraphicsRootSignature(root);
                cmd->SetPipelineState(pipeline);
                cmd->SetGraphicsRoot32BitConstants(0, 12, &cb, 0);
                cmd->OMSetRenderTargets(1, &output.cpuHandle, FALSE, nullptr);
                const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
                cmd->RSSetViewports(1, &viewport);
                cmd->RSSetScissorRects(1, &scissor);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->DrawInstanced(3, 1, 0, 0);
            });
        return pass;
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_display, m_difference;
};
