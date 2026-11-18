#pragma once

#include "SurfaceCacheLightingPass.h"

// World-space material/lighting lookup diagnostics; creates no editor widgets.
class SurfaceCacheQueryDebugRenderer
{
public:
    static constexpr UINT OutputCount=7;
    inline static constexpr const char* OutputNames[OutputCount]={"lookup-albedo-metallic","lookup-normal-roughness",
        "lookup-emissive-source","query-position","query-address","surface-position","lookup-radiance-visibility"};
    bool Initialize(RenderDevice& device,UINT frameCount)
    {
        m_constants.resize(frameCount);m_frameCount=frameCount;
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(512);
        for(auto& buffer:m_constants)
            if(!Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"create query constants"))return false;
        return CreatePipeline(device.GetDevice(),device.GetRenderTarget(0)->GetDesc().Format);
    }

    bool Record(RenderDevice& device,ResourceManager& resources,const WorldGeometryRuntime& world,
        SurfaceCacheCapturePass& atlas,const SurfaceCacheLightingPass& lighting,int slot,const DirectX::XMFLOAT4X4& inverseViewProjection,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRTV,uint32_t mode,uint32_t channel,const VXGIConfig& config)
    {
        const auto& scene=world.GetGPUScene();const auto& global=world.GetGlobalSDF().GetPass();
        const auto desc=device.GetRenderTarget(slot)->GetDesc();
        if(m_width!=desc.Width||m_height!=desc.Height)
        {
            if(m_width)for(UINT i=0;i<m_frameCount;++i)device.WaitForPreviousFrame(int(i));
            if(!CreateOutputs(device.GetDevice(),UINT(desc.Width),desc.Height))return false;
        }
        SurfaceCacheCapturePass::QueryBindings bindings;
        if(!atlas.PrepareQuery(device,resources,slot,scene.GetInstanceCount(slot),bindings))return false;
        Constants constants{};constants.inverseViewProjection=inverseViewProjection;
        const auto& grid=scene.GetGrid(slot);
        constants.gridOrigin=grid.origin;constants.cellSize=grid.cellSize;constants.cellDimensions=grid.cellDimensions;
        constants.boundsMaximum=grid.boundsMaximum;constants.rayMax=config.globalSDFTraceDistance;
        constants.materialCount=uint32_t(resources.GetGlobalMaterials().size());constants.atlasIndices=bindings.atlasIndices;
        constants.queryMode=mode;constants.channel=channel;constants.bvhNodeCount=scene.GetInstanceBVHNodeCount(slot);
        constants.inverseOutputSize={1.0f/m_width,1.0f/m_height};constants.positionTolerance=0.001f;
        constants.globalMaxSteps=uint32_t(config.globalSDFMaxSteps);constants.global=global.GetLayout().constants;
        if(!lighting.CopySRV(device,resources,constants.lightingIndex))return false;
        if(mode==28)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE target{};
            if(!resources.AllocateTransientSrvUavDescriptor(&constants.globalIndex,&target))return false;
            device.GetDevice()->CopyDescriptorsSimple(1,target,global.GetSRVCPUHandle(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(m_constants[slot]->Map(0,&noReads,&mapped),"map query constants"))return false;
        std::memcpy(mapped,&constants,sizeof(constants));m_constants[slot]->Unmap(0,nullptr);
        auto* cmd=device.GetCommandList();
        for(auto& output:m_outputs)Transition(cmd,output.Get(),m_state,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(mode==28)Transition(cmd,global.GetTexture(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,SurfaceCacheCapturePass::ReadState);
        Transition(cmd,device.GetRenderTarget(slot),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[OutputCount+1]={outputRTV};
        for(UINT i=0;i<OutputCount;++i)rtvs[i+1]=CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvs->GetCPUDescriptorHandleForHeapStart(),i,m_rtvStep);
        cmd->OMSetRenderTargets(OutputCount+1,rtvs,FALSE,nullptr);
        const D3D12_VIEWPORT viewport{0,0,float(m_width),float(m_height),0,1};
        const D3D12_RECT scissor{0,0,LONG(m_width),LONG(m_height)};
        cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetGraphicsRootSignature(m_root.Get());cmd->SetPipelineState(m_pipeline.Get());
        cmd->SetGraphicsRootConstantBufferView(0,m_constants[slot]->GetGPUVirtualAddress());
        for(UINT i=0;i<4;++i)cmd->SetGraphicsRootShaderResourceView(i+1,scene.Get(slot,i)->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(5,resources.GetMaterialBufferGPUAddress());
        for(UINT i=0;i<4;++i)cmd->SetGraphicsRootShaderResourceView(i+6,scene.Get(slot,i+4)->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(10,bindings.cards);cmd->SetGraphicsRootShaderResourceView(11,bindings.ranges);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->DrawInstanced(3,1,0,0);
        for(auto& output:m_outputs)Transition(cmd,output.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,SurfaceCacheCapturePass::ReadState);
        m_state=SurfaceCacheCapturePass::ReadState;
        Transition(cmd,device.GetRenderTarget(slot),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        if(mode==28)Transition(cmd,global.GetTexture(),SurfaceCacheCapturePass::ReadState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }
    ID3D12Resource* GetOutput(UINT index)const{return m_outputs[index].Get();}

private:
    struct Constants
    {
        DirectX::XMFLOAT4X4 inverseViewProjection;
        DirectX::XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;float rayMax;
        DirectX::XMFLOAT3 boundsMaximum;uint32_t materialCount;
        std::array<uint32_t,4> atlasIndices;
        uint32_t queryMode,channel,bvhNodeCount,globalIndex;
        DirectX::XMFLOAT2 inverseOutputSize;float positionTolerance;uint32_t globalMaxSteps;
        uint32_t lightingIndex,padding[3];
        VXGIGlobalSDF::Constants global;
    };
    static_assert(sizeof(Constants)==352 && offsetof(Constants,global)==176);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_constants;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,OutputCount> m_outputs;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvs;
    D3D12_RESOURCE_STATES m_state=D3D12_RESOURCE_STATE_COMMON;
    UINT m_width=0,m_height=0,m_frameCount=0,m_rtvStep=0;
    static DXGI_FORMAT OutputFormat(UINT i)
    {return i<3||i==6?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;}
    static bool Check(HRESULT hr,const char* operation)
    {
        if(SUCCEEDED(hr))return true;
        ErrorLog::HRESULT(std::string("Surface Cache query: ")+operation,hr);return false;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
    {
        if(before==after)return;
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);cmd->ResourceBarrier(1,&barrier);
    }
    bool CreateOutputs(ID3D12Device* device,UINT width,UINT height)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=OutputCount;
        if(!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(m_rtvs.ReleaseAndGetAddressOf())),"create query RTV heap"))return false;
        m_rtvStep=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for(UINT i=0;i<OutputCount;++i)
        {
            const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(OutputFormat(i),
                width,height,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            if(!Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
                nullptr,IID_PPV_ARGS(m_outputs[i].ReleaseAndGetAddressOf())),"create query output"))return false;
            device->CreateRenderTargetView(m_outputs[i].Get(),nullptr,
                CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvs->GetCPUDescriptorHandleForHeapStart(),i,m_rtvStep));
        }
        m_width=width;m_height=height;m_state=D3D12_RESOURCE_STATE_COMMON;return true;
    }
    bool CreatePipeline(ID3D12Device* device,DXGI_FORMAT outputFormat)
    {
        CD3DX12_ROOT_PARAMETER parameters[12];parameters[0].InitAsConstantBufferView(0,0,D3D12_SHADER_VISIBILITY_PIXEL);
        for(UINT i=0;i<5;++i)parameters[i+1].InitAsShaderResourceView(i,0,D3D12_SHADER_VISIBILITY_PIXEL);
        for(UINT i=0;i<4;++i)parameters[i+6].InitAsShaderResourceView(i,1,D3D12_SHADER_VISIBILITY_PIXEL);
        for(UINT i=0;i<2;++i)parameters[i+10].InitAsShaderResourceView(i,2,D3D12_SHADER_VISIBILITY_PIXEL);
        const auto sampler=CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const CD3DX12_ROOT_SIGNATURE_DESC root(12,parameters,1,&sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
        if(!Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize query root") ||
            !Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root)),"create query root"))return false;
        const auto vs=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheQueryDebug.hlsl",L"VSMain",L"vs_6_6",{L"VXGI_MESH_SDF=1"});
        const auto ps=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheQueryDebug.hlsl",L"PSMain",L"ps_6_6",{L"VXGI_MESH_SDF=1"});
        if(!vs||!ps)return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_root.Get();
        pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pso.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);pso.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable=FALSE;pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.NumRenderTargets=OutputCount+1;pso.RTVFormats[0]=outputFormat;
        for(UINT i=0;i<OutputCount;++i)pso.RTVFormats[i+1]=OutputFormat(i);
        pso.SampleDesc.Count=1;pso.SampleMask=UINT_MAX;pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        return Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&m_pipeline)),"create query PSO");
    }
};
