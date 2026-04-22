/*
1. Root Signatures: Define the register layouts and data binding interfaces between CPU and GPU
2. PSOs: Pre-compile shaders and fixed-function states into immutable hardware blueprints
*/

#ifndef PIPELINE_MANAGER_H
#define PIPELINE_MANAGER_H

#include "stdafx.h"
#include "RenderDevice.h"
#include "PBR_Shader.h"
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class PipelineManager
{
public:
    PipelineManager()
    {}

    ~PipelineManager()
    {}

    bool Initialize(RenderDevice* dc)
    {
        if (!BuildRootSignature(dc)) return false;
        if (!BuildPipelineStates(dc)) return false;
        if (!BuildComputePipeline(dc)) return false;
        if (!BuildShadowPipeline(dc)) return false;

        return true;
    }

    ID3D12RootSignature* GetRootSignature()
    {
        return rootSignature.Get();
    }

    ID3D12PipelineState* GetPBR_PSO()
    {
        return pipelineStateObject.Get();
    }

    ID3D12PipelineState* GetSkybox_PSO()
    {
        return psoSkybox.Get();
    }

    ID3D12RootSignature* GetComputeRootSignature()
    {
        return computeRootSignature.Get();
    }

    ID3D12PipelineState* GetComputePSO()
    {
        return computePSO.Get();
    }

    ID3D12RootSignature* GetShadowRootSignature()
    {
        return shadowRootSignature.Get();
    }

    ID3D12PipelineState* GetShadowPSO()
    {
        return shadowPSO.Get();
    }

    ID3D12PipelineState* GetTransparentPSO_DepthOnly()
    {
        return psoTransparent_DepthOnly.Get();
    }
    ID3D12PipelineState* GetTransparentPSO_Color()
    {
        return psoTransparent_Color.Get();
    }

private:

    // Root Signature: Defines the data binding layout for GPU submissions
    bool BuildRootSignature(RenderDevice* dc)
    {
        // Define texture sampling rules
        D3D12_DESCRIPTOR_RANGE ranges[7];

        for (int i = 0; i < 6; i++)
        {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = 1;
            ranges[i].BaseShaderRegister = i;
            ranges[i].RegisterSpace = 0;
            ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }

        ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[6].NumDescriptors = 1;
        ranges[6].BaseShaderRegister = 7;
        ranges[6].RegisterSpace = 0;
        ranges[6].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // Allocate 8 root parameters
        D3D12_ROOT_PARAMETER rootParameters[11];

        // Global Constant Matrix (Root CBV), typically the MVP matrix
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor = { 0, 0 };
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Bind the descriptor table for the 6 previously defined texture sampling rules
        for (int i = 1; i <= 6; i++)
        {
            rootParameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParameters[i].DescriptorTable.NumDescriptorRanges = 1;
            rootParameters[i].DescriptorTable.pDescriptorRanges = &ranges[i - 1];
            rootParameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        // High-performance Root Constants
        rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[7].Constants.ShaderRegister = 1;
        rootParameters[7].Constants.Num32BitValues = 16;
        rootParameters[7].Constants.RegisterSpace = 0;
        rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[8].Descriptor.ShaderRegister = 2;
        rootParameters[8].Descriptor.RegisterSpace = 0;
        rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[9].Descriptor.ShaderRegister = 6;
        rootParameters[9].Descriptor.RegisterSpace = 0;
        rootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        rootParameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[10].DescriptorTable.NumDescriptorRanges = 1;
        rootParameters[10].DescriptorTable.pDescriptorRanges = &ranges[6];
        rootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Define static samplers for texture filtering and addressing
        D3D12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_ANISOTROPIC);
        samplers[0].MaxAnisotropy = 16;

        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC
        (1,
            D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;

        // Serialize the Root Signature
        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(11, rootParameters, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> rsBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr);

        if (FAILED(hr))
        {
            return false;
        }

        hr = dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));

        if (FAILED(hr))
        {
            return false;
        }

        return true;
    }

    // PSO : A pre-baked, immutable snapshot of the entire graphics pipeline state
    bool BuildPipelineStates(RenderDevice* dc)
    {
        // PSO for PBR object
        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", "VSMain", "vs_5_0");
        auto ps = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", "PSMain", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_SINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { layout, _countof(layout) };
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps.Get());

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStateObject));

        // PSO for transparent objects
        // Depth-Only Pass : Enable depth writing and disable color output
        D3D12_GRAPHICS_PIPELINE_STATE_DESC depthOnlyDesc = psoDesc;
        depthOnlyDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        depthOnlyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthOnlyDesc.DepthStencilState.DepthEnable = TRUE;
        depthOnlyDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        depthOnlyDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

        dc->GetDevice()->CreateGraphicsPipelineState(&depthOnlyDesc, IID_PPV_ARGS(&psoTransparent_DepthOnly));

        // Color-Blend Pass : Enable color output and alpha blending, while disabling depth writes
        D3D12_GRAPHICS_PIPELINE_STATE_DESC colorDesc = psoDesc;

        D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc = {};
        transparencyBlendDesc.BlendEnable = TRUE;
        transparencyBlendDesc.LogicOpEnable = FALSE;
        transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        colorDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;

        colorDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        colorDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        colorDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

        dc->GetDevice()->CreateGraphicsPipelineState(&colorDesc, IID_PPV_ARGS(&psoTransparent_Color));

        // PSO for sky box
        auto vsSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", "VSMain", "vs_5_0");
        auto psSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", "PSMain", "ps_5_0");

        // Derive a new PSO from the existing template
        D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = psoDesc;
        D3D12_INPUT_ELEMENT_DESC layoutSky[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

        skyPsoDesc.InputLayout = { layoutSky, 1 };
        skyPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsSky.Get());
        skyPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psSky.Get());
        skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        dc->GetDevice()->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&psoSkybox));

        return true;
    }

    // A data compute pipeline with future-proof extensibility
    // Provide the Root Signature, Textures, Output UAV Buffer, and PSO for GPU-based Spherical Harmonics (SH) computation
    bool BuildComputePipeline(RenderDevice* dc)
    {
        D3D12_ROOT_PARAMETER computeRootParams[3];

        computeRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        computeRootParams[0].Constants.ShaderRegister = 0;
        computeRootParams[0].Constants.Num32BitValues = 2;
        computeRootParams[0].Constants.RegisterSpace = 0;
        computeRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE srvRange;
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        computeRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        computeRootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
        computeRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE uavRange;
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        computeRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        computeRootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
        computeRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        CD3DX12_ROOT_SIGNATURE_DESC computeRSDesc;
        computeRSDesc.Init(3, computeRootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rsBlob, errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&computeRSDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errorBlob);
        if (FAILED(hr)) return false;

        hr = dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&computeRootSignature));
        if (FAILED(hr)) return false;

        auto cs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_SH_Calculate.hlsl", "CSMain", "cs_5_0");

        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = computeRootSignature.Get();
        computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cs.Get());
        computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        hr = dc->GetDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&computePSO));
        if (FAILED(hr)) return false;

        return true;
    }

    bool BuildShadowPipeline(RenderDevice* dc)
    {
        D3D12_ROOT_PARAMETER rootParams[2];

        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[1].Descriptor.ShaderRegister = 0;
        rootParams[1].Descriptor.RegisterSpace = 0;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(2, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> rsBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr))) return false;
        if (FAILED(dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&shadowRootSignature)))) return false;

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Shadow.hlsl", "VSMain", "vs_5_0");

        D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_SINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { layout, _countof(layout) };
        psoDesc.pRootSignature = shadowRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
        psoDesc.PS = { nullptr, 0 };

        auto rasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rasterizerState.DepthBias = 100000;
        rasterizerState.DepthBiasClamp = 0.0f;
        rasterizerState.SlopeScaledDepthBias = 1.0f;
        psoDesc.RasterizerState = rasterizerState;

        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        psoDesc.NumRenderTargets = 0;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&shadowPSO)))) return false;

        return true;
    }

private:

    ComPtr<ID3D12PipelineState> pipelineStateObject;
    ComPtr<ID3D12PipelineState> psoSkybox;
    ComPtr<ID3D12RootSignature> rootSignature;

    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12PipelineState> computePSO;

    ComPtr<ID3D12RootSignature> shadowRootSignature;
    ComPtr<ID3D12PipelineState> shadowPSO;

    ComPtr<ID3D12PipelineState> psoTransparent_DepthOnly;
    ComPtr<ID3D12PipelineState> psoTransparent_Color;
};

#endif