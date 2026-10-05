#pragma once

#include "PBR_Shader.h"

// SSGI owns its compute pipeline; it does not share AO or anti-aliasing state.
class SSGIPipeline
{
public:
    bool Initialize(ID3D12Device* device)
    {
        CD3DX12_ROOT_PARAMETER parameter;
        parameter.InitAsConstantBufferView(0);
        CD3DX12_ROOT_SIGNATURE_DESC desc(1, &parameter, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("SSGI: root signature creation failed.", hr);
            return false;
        }
        return Build(device, L"Shaders/SSGIPyramid.hlsl", pyramid) &&
            Build(device, L"Shaders/SSGITrace.hlsl", trace) &&
            Build(device, L"Shaders/SSGIReconstruct.hlsl", reconstruct) &&
            Build(device, L"Shaders/SSGITemporal.hlsl", temporal) && BuildComposite(device);
    }

    ID3D12RootSignature* Root() const { return root.Get(); }
    ID3D12PipelineState* Pyramid() const { return pyramid.Get(); }
    ID3D12PipelineState* Trace() const { return trace.Get(); }
    ID3D12PipelineState* Reconstruct() const { return reconstruct.Get(); }
    ID3D12PipelineState* Temporal() const { return temporal.Get(); }
    ID3D12PipelineState* Composite() const { return composite.Get(); }

private:
    bool BuildComposite(ID3D12Device* device)
    {
        auto vs = ShaderCompiler::CompileFromFile(L"Shaders/SSGIComposite.hlsl", L"VSMain", L"vs_6_6");
        auto ps = ShaderCompiler::CompileFromFile(L"Shaders/SSGIComposite.hlsl", L"PSMain", L"ps_6_6");
        if (!vs || !ps) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_ONE;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO; blend.DestBlendAlpha = D3D12_BLEND_ONE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&composite));
        if (FAILED(hr)) ErrorLog::HRESULT("SSGI: composite PSO creation failed.", hr);
        return SUCCEEDED(hr);
    }

    bool Build(ID3D12Device* device, const wchar_t* path,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& output)
    {
        auto shader = ShaderCompiler::CompileFromFile(path, L"CSMain", L"cs_6_6");
        if (!shader) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root.Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output));
        if (FAILED(hr)) ErrorLog::HRESULT("SSGI: compute PSO creation failed.", hr);
        return SUCCEEDED(hr);
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pyramid, trace, reconstruct, temporal, composite;
};
