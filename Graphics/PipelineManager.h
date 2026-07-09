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
        if (!BuildShadowPipeline(dc)) return false;
        if (!BuildPostProcessPipeline(dc)) return false;
        if (!BuildDeferredPipeline(dc)) return false;
        if (!BuildHBAOPipeline(dc)) return false;
        if (!BuildTAAPipeline(dc)) return false;

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

    ID3D12PipelineState* GetZPrepassCutout_PSO()
    {
        return psoZPrepassCutout.Get();
    }

    ID3D12PipelineState* GetPBRCutout_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoPBRCutout[lodLevel].Get();
    }

    ID3D12PipelineState* GetPBR_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoPBR[lodLevel].Get();
    }

    ID3D12PipelineState* GetPBRBuildDepth_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoPBRBuildDepth[lodLevel].Get();
    }

    ID3D12PipelineState* GetPBRCutoutBuildDepth_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoPBRCutoutBuildDepth[lodLevel].Get();
    }

    ID3D12PipelineState* GetGBuffer_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoGBuffer[lodLevel].Get();
    }

    ID3D12PipelineState* GetGBufferCutout_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoGBufferCutout[lodLevel].Get();
    }

    ID3D12PipelineState* GetGBufferAfterZPrepass_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoGBufferAfterZPrepass[lodLevel].Get();
    }

    ID3D12PipelineState* GetGBufferCutoutAfterZPrepass_PSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoGBufferCutoutAfterZPrepass[lodLevel].Get();
    }

    ID3D12PipelineState* GetSkybox_PSO()
    {
        return psoSkybox.Get();
    }

    ID3D12RootSignature* GetShadowRootSignature()
    {
        return shadowRootSignature.Get();
    }

    ID3D12PipelineState* GetShadowPSO()
    {
        return shadowPSO.Get();
    }

    ID3D12PipelineState* GetShadowCutoutPSO()
    {
        return shadowCutoutPSO.Get();
    }

    ID3D12PipelineState* GetTransparentPSO(int lodLevel = 0)
    {
        if (lodLevel < 0) lodLevel = 0;
        if (lodLevel > 2) lodLevel = 2;
        return psoTransparent[lodLevel].Get();
    }

    ID3D12RootSignature* GetPostProcessRootSignature()
    {
        return postProcessRootSignature.Get();
    }

    ID3D12PipelineState* GetPostProcessPSO()
    {
        return postProcessPSO.Get();
    }

    ID3D12RootSignature* GetDeferredRootSignature()
    {
        return deferredRootSignature.Get();
    }

    ID3D12PipelineState* GetDeferredPSO()
    {
        return deferredPSO.Get();
    }

    ID3D12RootSignature* GetHBAORootSignature()
    {
        return hbaoRootSignature.Get();
    }

    ID3D12PipelineState* GetHBAOPSO()
    {
        return hbaoPSO.Get();
    }

    ID3D12PipelineState* GetHBAOBlurPSO()
    {
        return hbaoBlurPSO.Get();
    }

    ID3D12RootSignature* GetTAARootSignature()
    {
        return taaRootSignature.Get();
    }

    ID3D12PipelineState* GetTAAPSO()
    {
        return psoTAA.Get();
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
        // ====================================================================================================
        // MACROS & INPUT LAYOUTS
        // ====================================================================================================
        std::vector<std::wstring> lod0Macros = { L"LOD_LEVEL=0" };
        std::vector<std::wstring> lod1Macros = { L"LOD_LEVEL=1" };
        std::vector<std::wstring> lod2Macros = { L"LOD_LEVEL=2" };

        std::vector<std::wstring> lod0Cutout = { L"LOD_LEVEL=0", L"ALPHA_TEST=1" };
        std::vector<std::wstring> lod1Cutout = { L"LOD_LEVEL=1", L"ALPHA_TEST=1" };
        std::vector<std::wstring> lod2Cutout = { L"LOD_LEVEL=2", L"ALPHA_TEST=1" };

        std::vector<std::wstring> lod0Transparent = { L"LOD_LEVEL=0", L"TRANSPARENT_PASS=1" };
        std::vector<std::wstring> lod1Transparent = { L"LOD_LEVEL=1", L"TRANSPARENT_PASS=1" };
        std::vector<std::wstring> lod2Transparent = { L"LOD_LEVEL=2", L"TRANSPARENT_PASS=1" };

        std::vector<std::wstring> cutoutMacros = { L"ALPHA_TEST=1" };

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

        D3D12_INPUT_ELEMENT_DESC layoutSky[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

        // ====================================================================================================
        // SHADER COMPILATION
        // ====================================================================================================
        auto vsZPrepass = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_ZPrepass.hlsl", L"VSMain", L"vs_6_6");
        auto psZPrepassCutout = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_ZPrepass.hlsl", L"PSMain", L"ps_6_6", cutoutMacros);

        auto vs0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod0Macros);
        auto vs1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod1Macros);
        auto vs2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"VSMain", L"vs_6_6", lod2Macros);

        auto ps0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod0Macros);
        auto ps1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod1Macros);
        auto ps2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod2Macros);

        auto psPBRCutout0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod0Cutout);
        auto psPBRCutout1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod1Cutout);
        auto psPBRCutout2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod2Cutout);

        auto psTransparent0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod0Transparent);
        auto psTransparent1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod1Transparent);
        auto psTransparent2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_PBR.hlsl", L"PSMain", L"ps_6_6", lod2Transparent);

        auto vsGBuffer0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod0Macros);
        auto vsGBuffer1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod1Macros);
        auto vsGBuffer2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"VSMain", L"vs_6_6", lod2Macros);

        auto psGBuffer0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod0Macros);
        auto psGBuffer1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod1Macros);
        auto psGBuffer2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod2Macros);

        auto psGBufferCutout0 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod0Cutout);
        auto psGBufferCutout1 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod1Cutout);
        auto psGBufferCutout2 = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_GBuffer.hlsl", L"PSMain", L"ps_6_6", lod2Cutout);

        auto vsSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", L"VSMain", L"vs_6_6");
        auto psSky = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Sky_Box.hlsl", L"PSMain", L"ps_6_6");

        // ====================================================================================================
        // Z-PREPASS PSO
        // ====================================================================================================
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

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&zPrepassPsoDesc, IID_PPV_ARGS(&psoZPrepass)))) return false;

        // ====================================================================================================
        // Z-PREPASS CUTOUT PSO
        // ====================================================================================================
        zPrepassPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psZPrepassCutout->GetBufferPointer(), psZPrepassCutout->GetBufferSize());
        zPrepassPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&zPrepassPsoDesc, IID_PPV_ARGS(&psoZPrepassCutout)))) return false;

        // ====================================================================================================
        // FORWARD PBR PSOs
        // ====================================================================================================
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
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[0])))) return false;

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps1->GetBufferPointer(), ps1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[1])))) return false;

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(ps2->GetBufferPointer(), ps2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPBR[2])))) return false;

        // ====================================================================================================
        // FORWARD PBR CUTOUT PSOs
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pbrCutoutDesc = psoDesc;
        pbrCutoutDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        pbrCutoutDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        pbrCutoutDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout0->GetBufferPointer(), psPBRCutout0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutDesc, IID_PPV_ARGS(&psoPBRCutout[0])))) return false;

        pbrCutoutDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        pbrCutoutDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout1->GetBufferPointer(), psPBRCutout1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutDesc, IID_PPV_ARGS(&psoPBRCutout[1])))) return false;

        pbrCutoutDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        pbrCutoutDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout2->GetBufferPointer(), psPBRCutout2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutDesc, IID_PPV_ARGS(&psoPBRCutout[2])))) return false;

        // ====================================================================================================
        // FORWARD PBR PSOs WITHOUT Z-PREPASS
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pbrBuildDepthDesc = psoDesc;
        pbrBuildDepthDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pbrBuildDepthDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        pbrBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        pbrBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(ps0->GetBufferPointer(), ps0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrBuildDepthDesc, IID_PPV_ARGS(&psoPBRBuildDepth[0])))) return false;

        pbrBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        pbrBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(ps1->GetBufferPointer(), ps1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrBuildDepthDesc, IID_PPV_ARGS(&psoPBRBuildDepth[1])))) return false;

        pbrBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        pbrBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(ps2->GetBufferPointer(), ps2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrBuildDepthDesc, IID_PPV_ARGS(&psoPBRBuildDepth[2])))) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pbrCutoutBuildDepthDesc = pbrBuildDepthDesc;
        pbrCutoutBuildDepthDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        pbrCutoutBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        pbrCutoutBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout0->GetBufferPointer(), psPBRCutout0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutBuildDepthDesc, IID_PPV_ARGS(&psoPBRCutoutBuildDepth[0])))) return false;

        pbrCutoutBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        pbrCutoutBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout1->GetBufferPointer(), psPBRCutout1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutBuildDepthDesc, IID_PPV_ARGS(&psoPBRCutoutBuildDepth[1])))) return false;

        pbrCutoutBuildDepthDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        pbrCutoutBuildDepthDesc.PS = CD3DX12_SHADER_BYTECODE(psPBRCutout2->GetBufferPointer(), psPBRCutout2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&pbrCutoutBuildDepthDesc, IID_PPV_ARGS(&psoPBRCutoutBuildDepth[2])))) return false;

        // ====================================================================================================
        // G-BUFFER PSOs
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = psoDesc;
        gbufferPsoDesc.NumRenderTargets = 4;
        gbufferPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gbufferPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gbufferPsoDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gbufferPsoDesc.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gbufferPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        gbufferPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer0->GetBufferPointer(), vsGBuffer0->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer0->GetBufferPointer(), psGBuffer0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[0])))) return false;

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer1->GetBufferPointer(), vsGBuffer1->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer1->GetBufferPointer(), psGBuffer1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[1])))) return false;

        gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer2->GetBufferPointer(), vsGBuffer2->GetBufferSize());
        gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer2->GetBufferPointer(), psGBuffer2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&psoGBuffer[2])))) return false;

        // ====================================================================================================
        // G-BUFFER CUTOUT PSOs
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferCutoutPsoDesc = gbufferPsoDesc;
        gbufferCutoutPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        gbufferCutoutPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer0->GetBufferPointer(), vsGBuffer0->GetBufferSize());
        gbufferCutoutPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout0->GetBufferPointer(), psGBufferCutout0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutPsoDesc, IID_PPV_ARGS(&psoGBufferCutout[0])))) return false;

        gbufferCutoutPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer1->GetBufferPointer(), vsGBuffer1->GetBufferSize());
        gbufferCutoutPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout1->GetBufferPointer(), psGBufferCutout1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutPsoDesc, IID_PPV_ARGS(&psoGBufferCutout[1])))) return false;

        gbufferCutoutPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer2->GetBufferPointer(), vsGBuffer2->GetBufferSize());
        gbufferCutoutPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout2->GetBufferPointer(), psGBufferCutout2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutPsoDesc, IID_PPV_ARGS(&psoGBufferCutout[2])))) return false;

        // ====================================================================================================
        // G-BUFFER PSOs AFTER Z-PREPASS
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferAfterZPrepassDesc = gbufferPsoDesc;
        gbufferAfterZPrepassDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gbufferAfterZPrepassDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;

        gbufferAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer0->GetBufferPointer(), vsGBuffer0->GetBufferSize());
        gbufferAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer0->GetBufferPointer(), psGBuffer0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferAfterZPrepass[0])))) return false;

        gbufferAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer1->GetBufferPointer(), vsGBuffer1->GetBufferSize());
        gbufferAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer1->GetBufferPointer(), psGBuffer1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferAfterZPrepass[1])))) return false;

        gbufferAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer2->GetBufferPointer(), vsGBuffer2->GetBufferSize());
        gbufferAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBuffer2->GetBufferPointer(), psGBuffer2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferAfterZPrepass[2])))) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferCutoutAfterZPrepassDesc = gbufferAfterZPrepassDesc;
        gbufferCutoutAfterZPrepassDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        gbufferCutoutAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer0->GetBufferPointer(), vsGBuffer0->GetBufferSize());
        gbufferCutoutAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout0->GetBufferPointer(), psGBufferCutout0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferCutoutAfterZPrepass[0])))) return false;

        gbufferCutoutAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer1->GetBufferPointer(), vsGBuffer1->GetBufferSize());
        gbufferCutoutAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout1->GetBufferPointer(), psGBufferCutout1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferCutoutAfterZPrepass[1])))) return false;

        gbufferCutoutAfterZPrepassDesc.VS = CD3DX12_SHADER_BYTECODE(vsGBuffer2->GetBufferPointer(), vsGBuffer2->GetBufferSize());
        gbufferCutoutAfterZPrepassDesc.PS = CD3DX12_SHADER_BYTECODE(psGBufferCutout2->GetBufferPointer(), psGBufferCutout2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&gbufferCutoutAfterZPrepassDesc, IID_PPV_ARGS(&psoGBufferCutoutAfterZPrepass[2])))) return false;

        // ====================================================================================================
        // TRANSPARENT PSOs
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentDesc = psoDesc;
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

        transparentDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
        transparentDesc.DepthStencilState.DepthEnable = TRUE;
        transparentDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        transparentDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        transparentDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

        transparentDesc.VS = CD3DX12_SHADER_BYTECODE(vs0->GetBufferPointer(), vs0->GetBufferSize());
        transparentDesc.PS = CD3DX12_SHADER_BYTECODE(psTransparent0->GetBufferPointer(), psTransparent0->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&transparentDesc, IID_PPV_ARGS(&psoTransparent[0])))) return false;

        transparentDesc.VS = CD3DX12_SHADER_BYTECODE(vs1->GetBufferPointer(), vs1->GetBufferSize());
        transparentDesc.PS = CD3DX12_SHADER_BYTECODE(psTransparent1->GetBufferPointer(), psTransparent1->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&transparentDesc, IID_PPV_ARGS(&psoTransparent[1])))) return false;

        transparentDesc.VS = CD3DX12_SHADER_BYTECODE(vs2->GetBufferPointer(), vs2->GetBufferSize());
        transparentDesc.PS = CD3DX12_SHADER_BYTECODE(psTransparent2->GetBufferPointer(), psTransparent2->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&transparentDesc, IID_PPV_ARGS(&psoTransparent[2])))) return false;

        // ====================================================================================================
        // SKYBOX PSO
        // ====================================================================================================
        D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = psoDesc;
        skyPsoDesc.InputLayout = { layoutSky, 1 };
        skyPsoDesc.VS = CD3DX12_SHADER_BYTECODE(vsSky->GetBufferPointer(), vsSky->GetBufferSize());
        skyPsoDesc.PS = CD3DX12_SHADER_BYTECODE(psSky->GetBufferPointer(), psSky->GetBufferSize());
        skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&psoSkybox)))) return false;

        return true;
    }

    bool BuildShadowPipeline(RenderDevice* dc)
    {
        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Shadow.hlsl", L"VSMain", L"vs_6_6");

        std::vector<std::wstring> cutoutMacros = { L"ALPHA_TEST=1" };
        auto psCutout = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Shadow.hlsl", L"PSMain", L"ps_6_6", cutoutMacros);

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

        auto rasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rasterizerState.DepthBias = 500;
        rasterizerState.DepthBiasClamp = 0.0f;
        rasterizerState.SlopeScaledDepthBias = 1.0f;
        rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState = rasterizerState;

        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        psoDesc.NumRenderTargets = 0;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs->GetBufferPointer(), vs->GetBufferSize());
        psoDesc.PS = { nullptr, 0 };
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&shadowPSO)))) return false;

        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psCutout->GetBufferPointer(), psCutout->GetBufferSize());
        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&shadowCutoutPSO)))) return false;

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

    bool BuildDeferredPipeline(RenderDevice* dc)
    {
        D3D12_ROOT_PARAMETER rootParameters[3];

        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].Descriptor.ShaderRegister = 1;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[2].Descriptor.ShaderRegister = 2;
        rootParameters[2].Descriptor.RegisterSpace = 0;
        rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
            1,
            D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER);
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(3, rootParameters, 2, samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

        ComPtr<ID3DBlob> rsBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr))) return false;
        if (FAILED(dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&deferredRootSignature)))) return false;

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Deferred.hlsl", L"VSMain", L"vs_6_6");
        auto ps = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Deferred.hlsl", L"PSMain", L"ps_6_6");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = deferredRootSignature.Get();
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
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&deferredPSO)))) return false;

        return true;
    }

    bool BuildHBAOPipeline(RenderDevice* dc)
    {
        D3D12_ROOT_PARAMETER rootParameters[2];

        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[1].Constants.ShaderRegister = 1;
        rootParameters[1].Constants.Num32BitValues = 4;
        rootParameters[1].Constants.RegisterSpace = 0;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2];

        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(
            0,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(
            1,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(
            2,
            rootParameters,
            2,
            samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        );

        ComPtr<ID3DBlob> rsBlob;

        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
        {
            return false;
        }

        if (FAILED(dc->GetDevice()->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&hbaoRootSignature))))
        {
            return false;
        }

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_HBAO.hlsl", L"VSMain", L"vs_6_6");
        auto psHBAO = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_HBAO.hlsl", L"PSMain_HBAO", L"ps_6_6");
        auto psBlur = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_HBAO.hlsl", L"PSMain_Blur", L"ps_6_6");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = hbaoRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vs->GetBufferPointer(), vs->GetBufferSize());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psHBAO->GetBufferPointer(), psHBAO->GetBufferSize());

        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&hbaoPSO))))
        {
            return false;
        }

        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlur->GetBufferPointer(), psBlur->GetBufferSize());

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&hbaoBlurPSO))))
        {
            return false;
        }

        return true;
    }

    bool BuildTAAPipeline(RenderDevice* dc)
    {
        CD3DX12_ROOT_PARAMETER rootParameters[1];
        rootParameters[0].InitAsConstantBufferView(0);

        D3D12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, rootParameters, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

        ComPtr<ID3DBlob> serializedRootSig = nullptr;
        ComPtr<ID3DBlob> errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob);
        if (FAILED(hr)) return false;

        hr = dc->GetDevice()->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&taaRootSignature));
        if (FAILED(hr)) return false;

        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_TAA.hlsl", L"VSMain", L"vs_6_6");
        auto ps = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_TAA.hlsl", L"PSMain", L"ps_6_6");

        if (!vs || !ps)
        {
            MessageBox(NULL, L"TAA Shader compilation failed! Please check if Shaders_For_TAA.hlsl exists in the Shaders directory.", L"Engine Error", MB_OK);
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { nullptr, 0 };
        psoDesc.pRootSignature = taaRootSignature.Get();
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
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        if (FAILED(dc->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoTAA))))
        {
            MessageBox(NULL, L"Failed to create TAA PSO!", L"Engine Error", MB_OK);
            return false;
        }

        return true;
    }

private:

    ComPtr<ID3D12PipelineState> psoZPrepass;
    ComPtr<ID3D12PipelineState> psoZPrepassCutout;
    ComPtr<ID3D12PipelineState> psoPBRCutout[3];
    ComPtr<ID3D12PipelineState> psoPBR[3];
    ComPtr<ID3D12PipelineState> psoPBRCutoutBuildDepth[3];
    ComPtr<ID3D12PipelineState> psoPBRBuildDepth[3];
    ComPtr<ID3D12PipelineState> psoGBuffer[3];
    ComPtr<ID3D12PipelineState> psoGBufferCutout[3];
    ComPtr<ID3D12PipelineState> psoGBufferAfterZPrepass[3];
    ComPtr<ID3D12PipelineState> psoGBufferCutoutAfterZPrepass[3];
    ComPtr<ID3D12PipelineState> psoSkybox;
    ComPtr<ID3D12RootSignature> rootSignature;

    ComPtr<ID3D12RootSignature> shadowRootSignature;
    ComPtr<ID3D12PipelineState> shadowPSO;
    ComPtr<ID3D12PipelineState> shadowCutoutPSO;

    ComPtr<ID3D12PipelineState> psoTransparent[3];

    ComPtr<ID3D12RootSignature> postProcessRootSignature;
    ComPtr<ID3D12PipelineState> postProcessPSO;

    ComPtr<ID3D12RootSignature> deferredRootSignature;
    ComPtr<ID3D12PipelineState> deferredPSO;

    ComPtr<ID3D12RootSignature> hbaoRootSignature;
    ComPtr<ID3D12PipelineState> hbaoPSO;
    ComPtr<ID3D12PipelineState> hbaoBlurPSO;

    ComPtr<ID3D12RootSignature> taaRootSignature;
    ComPtr<ID3D12PipelineState> psoTAA;
};

#endif
