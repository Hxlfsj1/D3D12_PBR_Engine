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
#include <vector>
#include <string>

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
        if (!BuildPostProcessPipeline(dc)) return false;

        return true;
    }

    ID3D12RootSignature* GetRootSignature()
    {
        return rootSignature.Get();
    }

    ID3D12PipelineState* GetZPrepass_PSO()
    {
        return psoZPrepass.Get();
    }

    ID3D12PipelineState* GetPBR_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoPBR[lodLevel].Get();
    }

    ID3D12PipelineState* GetGBuffer_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoGBuffer[lodLevel].Get();
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

    ID3D12RootSignature* GetPostProcessRootSignature()
    {
        return postProcessRootSignature.Get();
    }

    ID3D12PipelineState* GetPostProcessPSO()
    {
        return postProcessPSO.Get();
    }

private:

    bool BuildRootSignature(RenderDevice* dc)
    {
        D3D12_ROOT_PARAMETER rootParameters[5];

        // Pass the GPU virtual address of the CBV
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Pass the starting address of the instance world matrices
        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[1].Descriptor.ShaderRegister = 6;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Pass the Spherical Harmonic (SH) lighting coefficients
        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[2].Descriptor.ShaderRegister = 2;
        rootParameters[2].Descriptor.RegisterSpace = 0;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Pass indices for bindless textures (PBR), or MVP matrix + texture index (Skybox)
        rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[3].Descriptor.ShaderRegister = 7;
        rootParameters[3].Descriptor.RegisterSpace = 0;
        rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[4].Constants.ShaderRegister = 1;
        rootParameters[4].Constants.Num32BitValues = 17;
        rootParameters[4].Constants.RegisterSpace = 0;
        rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Two samplers: the former is an anisotropic sampler for high-quality texture sampling;
        // and the latter is a comparison sampler for PCSS shadow calculations
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

        // Enable instancing and bindless resources
        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(5, rootParameters, 2, samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

        ComPtr<ID3DBlob> rsBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr);
        if (FAILED(hr)) return false;

        hr = dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
        if (FAILED(hr)) return false;

        return true;
    }

    bool BuildPipelineStates(RenderDevice* dc)
    {
        std::vector<std::wstring> lod0Macros = { L"LOD_LEVEL=0" };
        std::vector<std::wstring> lod1Macros = { L"LOD_LEVEL=1" };
        std::vector<std::wstring> lod2Macros = { L"LOD_LEVEL=2" };

        auto vs0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod0Macros);
        auto vs1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod1Macros);
        auto vs2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod2Macros);

        auto ps0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod0Macros);
        auto ps1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod1Macros);
        auto ps2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod2Macros);

        auto vsGBuffer0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod0Macros);
        auto vsGBuffer1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod1Macros);
        auto vsGBuffer2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod2Macros);

        auto psGBuffer0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod0Macros);
        auto psGBuffer1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod1Macros);
        auto psGBuffer2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod2Macros);

        auto vsZPrepass = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_ZPrepass.hlsl", L"VSMain", L"vs_6_6");

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

        D3D12_GRAPHICS_PIPELINE_STATE_DESC zPrepassPsoDesc = {};
        zPrepassPsoDesc.InputLayout = { layout, _countof(layout) };
        zPrepassPsoDesc.pRootSignature = rootSignature.Get();
        zPrepassPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsZPrepass->GetBufferPointer(), vsZPrepass->GetBufferSize());
        zPrepassPsoDesc.PS = { nullptr, 0 };
        zPrepassPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        zPrepassPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        zPrepassPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        zPrepassPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        zPrepassPsoDesc.DepthStencilState.DepthEnable = TRUE;
        zPrepassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        zPrepassPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        zPrepassPsoDesc.SampleMask = UINT_MAX;
        zPrepassPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        zPrepassPsoDesc.NumRenderTargets = 0;
        zPrepassPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        zPrepassPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        zPrepassPsoDesc.SampleDesc.Count = 1;

        dc->GetDevice()->CreateGraphicsPipelineState(&zPrepassPsoDesc, IID_PPV_ARGS(&psoZPrepass));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { layout, _countof(layout) };
        psoDesc.pRootSignature = rootSignature.Get();

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps0->GetBufferPointer(), ps0->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[0]));

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps1->GetBufferPointer(), ps1->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[1]));

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps2->GetBufferPointer(), ps2->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[2]));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = psoDesc;
        gbufferPsoDesc.NumRenderTargets = 3;
        gbufferPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gbufferPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gbufferPsoDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer0->GetBufferPointer(), vsGBuffer0->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer0->GetBufferPointer(), psGBuffer0->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[0]));

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer1->GetBufferPointer(), vsGBuffer1->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer1->GetBufferPointer(), psGBuffer1->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[1]));

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer2->GetBufferPointer(), vsGBuffer2->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer2->GetBufferPointer(), psGBuffer2->GetBufferSize());
        dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[2]));

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps0->GetBufferPointer(), ps0->GetBufferSize());

        D3D12_GRAPHICS_PIPELINE_STATE_DESC depthOnlyDesc = psoDesc;
        depthOnlyDesc.PS = { nullptr, 0 };
        depthOnlyDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        depthOnlyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthOnlyDesc.DepthStencilState.DepthEnable = TRUE;
        depthOnlyDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        depthOnlyDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

        dc->GetDevice()->CreateGraphicsPipelineState(&depthOnlyDesc, IID_PPV_ARGS(&psoTransparent_DepthOnly));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC colorDesc = psoDesc;

        D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc = {};
        transparencyBlendDesc.BlendEnable = TRUE;
        transparencyBlendDesc.LogicOpEnable = FALSE;
        transparencyBlendDesc.SrcBlend = D3D12_BLEND_ONE;
        transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        colorDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;

        colorDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        colorDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        colorDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

        dc->GetDevice()->CreateGraphicsPipelineState(&colorDesc, IID_PPV_ARGS(&psoTransparent_Color));

        auto vsSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", L"VSMain", L"vs_6_6");
        auto psSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", L"PSMain", L"ps_6_6");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = psoDesc;
        D3D12_INPUT_ELEMENT_DESC layoutSky[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

        skyPsoDesc.InputLayout = { layoutSky, 1 };
        skyPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsSky->GetBufferPointer(), vsSky->GetBufferSize());
        skyPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psSky->GetBufferPointer(), psSky->GetBufferSize());
        skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        dc->GetDevice()->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&psoSkybox));

        return true;
    }

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

        auto cs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_SH_Calculate.hlsl", L"CSMain", L"cs_6_6");

        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = computeRootSignature.Get();
        computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cs->GetBufferPointer(), cs->GetBufferSize());
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

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Shadow.hlsl", L"VSMain", L"vs_6_6");

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
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs->GetBufferPointer(), vs->GetBufferSize());
        psoDesc.PS = { nullptr, 0 };

        auto rasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rasterizerState.DepthBias = 500;
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

    bool BuildPostProcessPipeline(RenderDevice* dc)
    {
        CD3DX12_ROOT_PARAMETER rootParam;
        rootParam.InitAsConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        D3D12_STATIC_SAMPLER_DESC sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(
            1,
            &rootParam,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        );

        Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;

        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
        {
            return false;
        }

        if (FAILED(dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&postProcessRootSignature))))
        {
            return false;
        }

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PostProcess.hlsl", L"VSMain", L"vs_6_6");
        auto ps = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PostProcess.hlsl", L"PSMain", L"ps_6_6");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = postProcessRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs->GetBufferPointer(), vs->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps->GetBufferPointer(), ps->GetBufferSize());

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&postProcessPSO))))
        {
            return false;
        }

        return true;
    }

private:

    ComPtr<ID3D12PipelineState> psoZPrepass;
    ComPtr<ID3D12PipelineState> psoPBR[3];
    ComPtr<ID3D12PipelineState> psoGBuffer[3];
    ComPtr<ID3D12PipelineState> psoSkybox;
    ComPtr<ID3D12RootSignature> rootSignature;

    ComPtr<ID3D12RootSignature> computeRootSignature;
    ComPtr<ID3D12PipelineState> computePSO;

    ComPtr<ID3D12RootSignature> shadowRootSignature;
    ComPtr<ID3D12PipelineState> shadowPSO;

    ComPtr<ID3D12PipelineState> psoTransparent_DepthOnly;
    ComPtr<ID3D12PipelineState> psoTransparent_Color;

    ComPtr<ID3D12RootSignature> postProcessRootSignature;
    ComPtr<ID3D12PipelineState> postProcessPSO;
};

#endif