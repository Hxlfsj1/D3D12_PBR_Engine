#pragma once
#include "RenderDevice.h"
#include "PBR_Shader.h"
#include "VXGIGPUScene.h"

// X-ray diagnostics of actual GPU records at immutable world cell centers.
class VXGIProbeDebugRenderer
{
public:
    bool Initialize(RenderDevice& device)
    {
        CD3DX12_ROOT_PARAMETER roots[5];roots[0].InitAsConstants(20,0,0,D3D12_SHADER_VISIBILITY_VERTEX);
        roots[1].InitAsShaderResourceView(0,0,D3D12_SHADER_VISIBILITY_VERTEX);
        roots[2].InitAsShaderResourceView(1,0,D3D12_SHADER_VISIBILITY_VERTEX);
        roots[3].InitAsShaderResourceView(2,0,D3D12_SHADER_VISIBILITY_VERTEX);
        roots[4].InitAsShaderResourceView(3,0,D3D12_SHADER_VISIBILITY_VERTEX);
        const CD3DX12_ROOT_SIGNATURE_DESC desc(5,roots,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) ||
            FAILED(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root)))) return false;
        const auto vs=ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_VXGI_Probe_Debug.hlsl",L"VSMain",L"vs_6_6");
        const auto ps=ShaderCompiler::CompileFromFile(L"Shaders/Shaders_For_VXGI_Probe_Debug.hlsl",L"PSMain",L"ps_6_6");
        if(!vs || !ps) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_root.Get();
        pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pso.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);pso.SampleMask=UINT_MAX;
        pso.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);pso.DepthStencilState.DepthEnable=FALSE;
        pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pso.NumRenderTargets=1;
        pso.RTVFormats[0]=device.GetRenderTarget(0)->GetDesc().Format;pso.SampleDesc.Count=1;
        pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        if(FAILED(device.GetDevice()->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&m_lines)))) return false;
        pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        return SUCCEEDED(device.GetDevice()->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&m_octahedra)));
    }
    bool Record(RenderDevice& device,ID3D12DescriptorHeap* heap,int slot,const DirectX::XMFLOAT4X4& matrix,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,const VXGIGPUScene& scene,ID3D12Resource* records,
        float voxelSize,uint32_t count,uint32_t mode,uint32_t frameIndex,ID3D12Resource* visibility=nullptr)
    {
        if(!count) return true;if(!records || !heap || !scene.Get(slot,4)) return false;
        auto* neighbors=scene.Get(slot,6);
        if(mode==17 && (!neighbors || !visibility)) return false;
        auto* cmd=device.GetCommandList();auto* target=device.GetRenderTarget(slot);const auto desc=target->GetDesc();
        auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1,&barrier);cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};
        const D3D12_RECT scissor{0,0,LONG(desc.Width),LONG(desc.Height)};cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
        ID3D12DescriptorHeap* heaps[]={heap};cmd->SetDescriptorHeaps(1,heaps);cmd->SetGraphicsRootSignature(m_root.Get());
        cmd->SetGraphicsRoot32BitConstants(0,16,&matrix,0);cmd->SetGraphicsRoot32BitConstants(0,1,&voxelSize,16);
        cmd->SetGraphicsRoot32BitConstants(0,1,&mode,17);cmd->SetGraphicsRoot32BitConstants(0,1,&frameIndex,18);
        cmd->SetGraphicsRoot32BitConstants(0,1,&count,19);
        cmd->SetGraphicsRootShaderResourceView(1,scene.Get(slot,4)->GetGPUVirtualAddress());cmd->SetGraphicsRootShaderResourceView(2,records->GetGPUVirtualAddress());
        // Unused bindings still receive valid resource addresses for old views.
        cmd->SetGraphicsRootShaderResourceView(3,(neighbors?neighbors:records)->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(4,(visibility?visibility:records)->GetGPUVirtualAddress());
        const bool shView=mode==14 || mode==16;
        cmd->SetPipelineState(shView?m_octahedra.Get():m_lines.Get());
        cmd->IASetPrimitiveTopology(shView?D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cmd->DrawInstanced(shView?24u:mode==17?12u:6u,count,0,0);
        barrier=CD3DX12_RESOURCE_BARRIER::Transition(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1,&barrier);return true;
    }
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lines,m_octahedra;
};
