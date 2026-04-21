/*
IBL Pipeline Architecture (The 4-Step Lifecycle):

1. [Record]  CPU records rendering/compute commands to the Command List
2. [Execute] GPU executes the commands to bake IBL resources in VRAM
3. [Seal]    CPU blocks via Fence (Wait) and transitions resources to Read-Only
4. [Runtime] PBR Shaders sample these pre-baked resources with zero overhead
*/

/*
Pre-process IBL maps at initialization

1. Convert equirectangular HDR to an environment cubemap
2. Compute Spherical Harmonics (SH) coefficients for diffuse irradiance
3. Generate the prefiltered cubemap via importance sampling (for specular)
4. Integrate the BRDF LUT
*/

#ifndef IBL_BAKER_H
#define IBL_BAKER_H

#include <cmath>
#include <wrl/client.h>
#include "stdafx.h"
#include "PBR_Shader.h"

class IBLBaker
{
public:

    void Bake(ID3D12Device* device, ID3D12CommandQueue* commandQueue, const float* data, int w, int h)
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList));

        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        UINT64 fenceValue = 1;

        CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);

        Microsoft::WRL::ComPtr<ID3D12Resource> texHDR;
        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, 1, 1);
        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texHDR));

        UINT64 uploadSize = GetRequiredIntermediateSize(texHDR.Get(), 0, 1);
        Microsoft::WRL::ComPtr<ID3D12Resource> texUpload;
        CD3DX12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texUpload));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData = data;
        subData.RowPitch = w * 16;
        subData.SlicePitch = subData.RowPitch * h;

        UpdateSubresources(cmdList.Get(), texHDR.Get(), texUpload.Get(), 0, 0, 1, &subData);

        CD3DX12_RESOURCE_BARRIER t1 = CD3DX12_RESOURCE_BARRIER::Transition(texHDR.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &t1);

        D3D12_RESOURCE_DESC cubeDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 512, 512, 6, 1);
        cubeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &cubeDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_envCube));

        D3D12_RESOURCE_DESC preDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 128, 128, 6, 5);
        preDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &preDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_prefilterCube));

        D3D12_RESOURCE_DESC lutDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, 512, 512, 1, 1);
        lutDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &lutDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_brdfLUT));

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC rtvHDesc = {};
        rtvHDesc.NumDescriptors = 60;
        rtvHDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        device->CreateDescriptorHeap(&rtvHDesc, IID_PPV_ARGS(&rtvHeap));

        UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < 6; i++)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rdesc = {};
            rdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rdesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rdesc.Texture2DArray.FirstArraySlice = i;
            rdesc.Texture2DArray.ArraySize = 1;
            rdesc.Texture2DArray.MipSlice = 0;
            device->CreateRenderTargetView(m_envCube.Get(), &rdesc, rtvHandle);
            rtvHandle.Offset(1, rtvSize);
        }

        for (int mip = 0; mip < 5; mip++)
        {
            for (int i = 0; i < 6; i++)
            {
                D3D12_RENDER_TARGET_VIEW_DESC rdesc = {};
                rdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                rdesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rdesc.Texture2DArray.FirstArraySlice = i;
                rdesc.Texture2DArray.ArraySize = 1;
                rdesc.Texture2DArray.MipSlice = mip;
                device->CreateRenderTargetView(m_prefilterCube.Get(), &rdesc, rtvHandle);
                rtvHandle.Offset(1, rtvSize);
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC rdesc = {};
        rdesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        rdesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(m_brdfLUT.Get(), &rdesc, rtvHandle);

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> localSrvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 2;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&localSrvHeap));

        UINT srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC hdrSrvDesc = {};
        hdrSrvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        hdrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hdrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hdrSrvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hdrSrvHandle(localSrvHeap->GetCPUDescriptorHandleForHeapStart(), 0, srvSize);
        device->CreateShaderResourceView(texHDR.Get(), &hdrSrvDesc, hdrSrvHandle);

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig;
        D3D12_ROOT_PARAMETER rootParams[3];
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[0].Constants.ShaderRegister = 0;
        rootParams[0].Constants.Num32BitValues = 16;
        rootParams[0].Constants.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[1].Constants.ShaderRegister = 1;
        rootParams[1].Constants.Num32BitValues = 1;
        rootParams[1].Constants.RegisterSpace = 0;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE range;
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &range;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
        rsDesc.Init(3, rootParams, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;
        D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr);
        device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

        auto vsCube = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Cubemap.hlsl", "VSMain", "vs_5_0");
        auto psEqui = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Cubemap.hlsl", "PSMain", "ps_5_0");

        auto vsPre = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Split_Sum.hlsl", "VSMain_Prefilter", "vs_5_0");
        auto psPre = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Split_Sum.hlsl", "PSMain_Prefilter", "ps_5_0");
        auto vsBrdf = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Split_Sum.hlsl", "VSMain_BRDF", "vs_5_0");
        auto psBrdf = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_Split_Sum.hlsl", "PSMain_BRDF", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC layoutCube[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { layoutCube, 1 };
        psoDesc.pRootSignature = rootSig.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsCube.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psEqui.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> psoEqui;
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoEqui));

        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsPre.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psPre.Get());
        Microsoft::WRL::ComPtr<ID3D12PipelineState> psoPreState;
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoPreState));

        D3D12_INPUT_ELEMENT_DESC layoutQuad[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        psoDesc.InputLayout = { layoutQuad, 2 };
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBrdf.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBrdf.Get());
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> psoBrdfState;
        device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoBrdfState));

        float cubeVerts[] =
        {
            -1,1,-1, 1,1,-1, 1,-1,-1, -1,1,-1, 1,-1,-1, -1,-1,-1,
            -1,1,1, -1,-1,1, 1,-1,1, -1,1,1, 1,-1,1, 1,1,1,
            -1,1,1, 1,1,1, 1,1,-1, -1,1,1, 1,1,-1, -1,1,-1,
            -1,-1,1, -1,-1,-1, 1,-1,-1, -1,-1,1, 1,-1,-1, 1,-1,1,
            -1,1,1, -1,1,-1, -1,-1,-1, -1,1,1, -1,-1,-1, -1,-1,1,
            1,1,1, 1,-1,1, 1,-1,-1, 1,1,1, 1,-1,-1, 1,1,-1
        };

        float quadVerts[] =
        {
            -1,1,0, 0,0, 1,1,0, 1,0, -1,-1,0, 0,1,
            -1,-1,0, 0,1, 1,1,0, 1,0, 1,-1,0, 1,1
        };

        Microsoft::WRL::ComPtr<ID3D12Resource> vbCube;
        CD3DX12_RESOURCE_DESC vDescC = CD3DX12_RESOURCE_DESC::Buffer(sizeof(cubeVerts));
        device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vDescC, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbCube));
        void* pD;
        vbCube->Map(0, nullptr, &pD);
        memcpy(pD, cubeVerts, sizeof(cubeVerts));
        vbCube->Unmap(0, nullptr);
        D3D12_VERTEX_BUFFER_VIEW vbvCube = { vbCube->GetGPUVirtualAddress(), sizeof(cubeVerts), 12 };

        Microsoft::WRL::ComPtr<ID3D12Resource> vbQuad;
        CD3DX12_RESOURCE_DESC vDescQ = CD3DX12_RESOURCE_DESC::Buffer(sizeof(quadVerts));
        device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vDescQ, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbQuad));
        vbQuad->Map(0, nullptr, &pD);
        memcpy(pD, quadVerts, sizeof(quadVerts));
        vbQuad->Unmap(0, nullptr);
        D3D12_VERTEX_BUFFER_VIEW vbvQuad = { vbQuad->GetGPUVirtualAddress(), sizeof(quadVerts), 20 };

        DirectX::XMFLOAT3 targets[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
        DirectX::XMFLOAT3 ups[6] = { {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,1,0} };
        DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1.0f, 0.1f, 10.0f);

        ID3D12DescriptorHeap* heaps[] = { localSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(rootSig.Get());

        CD3DX12_GPU_DESCRIPTOR_HANDLE hdrSrvGpu(localSrvHeap->GetGPUDescriptorHandleForHeapStart(), 0, srvSize);
        D3D12_RESOURCE_BARRIER b1_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_envCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &b1_barrier);

        cmdList->SetPipelineState(psoEqui.Get());
        cmdList->SetGraphicsRootDescriptorTable(2, hdrSrvGpu);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &vbvCube);

        D3D12_VIEWPORT vp = { 0.0f, 0.0f, 512.0f, 512.0f, 0.0f, 1.0f };
        D3D12_RECT sr = { 0, 0, 512, 512 };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &sr);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvCurrent(rtvHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < 6; i++)
        {
            cmdList->OMSetRenderTargets(1, &rtvCurrent, FALSE, nullptr);
            DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 0, 0, 0), DirectX::XMLoadFloat3(&targets[i]), DirectX::XMLoadFloat3(&ups[i]));
            DirectX::XMMATRIX vpMat = DirectX::XMMatrixTranspose(view * proj);
            cmdList->SetGraphicsRoot32BitConstants(0, 16, &vpMat, 0);
            cmdList->DrawInstanced(36, 1, 0, 0);
            rtvCurrent.Offset(1, rtvSize);
        }

        D3D12_RESOURCE_BARRIER b2 = CD3DX12_RESOURCE_BARRIER::Transition(m_envCube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b2);

        D3D12_SHADER_RESOURCE_VIEW_DESC envSrvDesc = {};
        envSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        envSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        envSrvDesc.TextureCube.MipLevels = 1;
        envSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        CD3DX12_CPU_DESCRIPTOR_HANDLE envSrvHandle(localSrvHeap->GetCPUDescriptorHandleForHeapStart(), 1, srvSize);
        device->CreateShaderResourceView(m_envCube.Get(), &envSrvDesc, envSrvHandle);
        CD3DX12_GPU_DESCRIPTOR_HANDLE envSrvGpu(localSrvHeap->GetGPUDescriptorHandleForHeapStart(), 1, srvSize);
        cmdList->SetGraphicsRootDescriptorTable(2, envSrvGpu);

        D3D12_RESOURCE_BARRIER b5 = CD3DX12_RESOURCE_BARRIER::Transition(m_prefilterCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &b5);
        cmdList->SetPipelineState(psoPreState.Get());

        for (int mip = 0; mip < 5; mip++)
        {
            float mipWidth = 128.0f * std::pow(0.5f, mip);
            float mipHeight = 128.0f * std::pow(0.5f, mip);
            vp.Width = mipWidth; vp.Height = mipHeight;
            sr.right = (LONG)mipWidth; sr.bottom = (LONG)mipHeight;
            cmdList->RSSetViewports(1, &vp);
            cmdList->RSSetScissorRects(1, &sr);
            float roughness = (float)mip / 4.0f;

            for (int i = 0; i < 6; i++)
            {
                cmdList->OMSetRenderTargets(1, &rtvCurrent, FALSE, nullptr);
                DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(DirectX::XMVectorSet(0, 0, 0, 0), DirectX::XMLoadFloat3(&targets[i]), DirectX::XMLoadFloat3(&ups[i]));
                DirectX::XMMATRIX vpMat = DirectX::XMMatrixTranspose(view * proj);
                cmdList->SetGraphicsRoot32BitConstants(0, 16, &vpMat, 0);
                cmdList->SetGraphicsRoot32BitConstants(1, 1, &roughness, 0);
                cmdList->DrawInstanced(36, 1, 0, 0);
                rtvCurrent.Offset(1, rtvSize);
            }
        }

        D3D12_RESOURCE_BARRIER b6 = CD3DX12_RESOURCE_BARRIER::Transition(m_prefilterCube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b6);

        D3D12_RESOURCE_BARRIER b7 = CD3DX12_RESOURCE_BARRIER::Transition(m_brdfLUT.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &b7);
        cmdList->SetPipelineState(psoBrdfState.Get());
        cmdList->IASetVertexBuffers(0, 1, &vbvQuad);
        vp.Width = 512.0f; vp.Height = 512.0f;
        sr.right = 512; sr.bottom = 512;
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &sr);
        cmdList->OMSetRenderTargets(1, &rtvCurrent, FALSE, nullptr);
        cmdList->DrawInstanced(6, 1, 0, 0);

        D3D12_RESOURCE_BARRIER b8 = CD3DX12_RESOURCE_BARRIER::Transition(m_brdfLUT.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b8);

        // SH
        Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSig;
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
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        computeRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        computeRootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
        computeRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        computeRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        computeRootParams[2].Descriptor.ShaderRegister = 0;
        computeRootParams[2].Descriptor.RegisterSpace = 0;
        computeRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        CD3DX12_ROOT_SIGNATURE_DESC computeRSDesc;
        computeRSDesc.Init(3, computeRootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        Microsoft::WRL::ComPtr<ID3DBlob> cRsBlob;
        D3D12SerializeRootSignature(&computeRSDesc, D3D_ROOT_SIGNATURE_VERSION_1, &cRsBlob, nullptr);
        device->CreateRootSignature(0, cRsBlob->GetBufferPointer(), cRsBlob->GetBufferSize(), IID_PPV_ARGS(&computeRootSig));

        auto csShader = ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_SH_Calculate.hlsl", "CSMain", "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = computeRootSig.Get();
        computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(csShader.Get());
        Microsoft::WRL::ComPtr<ID3D12PipelineState> computePSO;
        device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&computePSO));

        D3D12_RESOURCE_DESC uavDesc = CD3DX12_RESOURCE_DESC::Buffer(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_shBuffer));

        cmdList->SetPipelineState(computePSO.Get());
        cmdList->SetComputeRootSignature(computeRootSig.Get());

        ID3D12DescriptorHeap* computeHeaps[] = { localSrvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, computeHeaps);
        cmdList->SetComputeRootDescriptorTable(1, localSrvHeap->GetGPUDescriptorHandleForHeapStart());

        UINT constants[2] = { (UINT)w, (UINT)h };
        cmdList->SetComputeRoot32BitConstants(0, 2, constants, 0);
        cmdList->SetComputeRootUnorderedAccessView(2, m_shBuffer->GetGPUVirtualAddress());

        cmdList->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER uavToCbv = CD3DX12_RESOURCE_BARRIER::Transition(m_shBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        cmdList->ResourceBarrier(1, &uavToCbv);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
        commandQueue->Signal(fence.Get(), fenceValue);

        if (fence->GetCompletedValue() < fenceValue)
        {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        CloseHandle(fenceEvent);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> GetEnvCube() { return m_envCube; }
    Microsoft::WRL::ComPtr<ID3D12Resource> GetPrefilterCube() { return m_prefilterCube; }
    Microsoft::WRL::ComPtr<ID3D12Resource> GetBRDFLUT() { return m_brdfLUT; }
    Microsoft::WRL::ComPtr<ID3D12Resource> GetSHBuffer() { return m_shBuffer; }

private:

    Microsoft::WRL::ComPtr<ID3D12Resource> m_envCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_prefilterCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_brdfLUT;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shBuffer;
};

#endif