#pragma once

#include "SurfaceCacheCapturePass.h"

// Camera-independent outgoing diffuse source Lo0, using the screen source BRDF
// contract and world geometry visibility. No environment, specular or GI feedback.
class SurfaceCacheLightingPass
{
public:
    bool Initialize(RenderDevice& device,UINT frameCount)
    {
        m_constants.resize(frameCount);m_retired.resize(frameCount);
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(256);
        for(auto& buffer:m_constants)
            if(!Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"create lighting constants"))return false;
        CD3DX12_ROOT_PARAMETER parameters[13];parameters[0].InitAsConstantBufferView(0);
        for(UINT i=0;i<5;++i)parameters[i+1].InitAsShaderResourceView(i);
        for(UINT i=0;i<4;++i)parameters[i+6].InitAsShaderResourceView(i,1);
        for(UINT i=0;i<2;++i)parameters[i+10].InitAsShaderResourceView(i,2);
        parameters[12].InitAsConstants(9,1);
        const auto sampler=CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const CD3DX12_ROOT_SIGNATURE_DESC root(13,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
        if(!Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize lighting root") ||
            !Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_root)),"create lighting root"))return false;
        const auto cs=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheLighting.hlsl",L"CSMain",L"cs_6_6",{L"VXGI_MESH_SDF=1"});
        if(!cs)return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_root.Get();pso.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        if(!Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&m_pipeline)),"create lighting PSO"))return false;
        const auto resample=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheLighting.hlsl",L"ResampleCS",L"cs_6_6",{L"VXGI_MESH_SDF=1"});
        if(!resample)return false;
        pso.CS={resample->GetBufferPointer(),resample->GetBufferSize()};
        return Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&m_resamplePipeline)),"create resample PSO");
    }

    bool Record(RenderDevice& device,ResourceManager& resources,const WorldGeometryRuntime& world,
        SurfaceCacheCapturePass& atlas,int slot,const LightingConfig& light,const VXGIConfig& config)
    {
        const Source source{world.GetScene().GetRevision(),world.GetCardBuildCount(),resources.GetMaterialRevision(),atlas.GetRecordingRevision(),
            {light.lightDir.x,light.lightDir.y,light.lightDir.z},{light.lightColor.x,light.lightColor.y,light.lightColor.z},
            config.globalSDFTraceDistance,config.surfaceRayMinDistance};
        // This frame slot has retired before Record. Keep replaced atlases alive
        // until the slot retires again, including their resample reads this frame.
        m_retired[slot].Reset();
        const bool changed=!m_valid || !(source==m_source);
        if(!changed && m_nextUpdate==m_updates.size())return true;
        const auto& tiles=atlas.GetTiles();
        const bool sameCards=m_valid && source.scene==m_source.scene && source.cards==m_source.cards;
        const bool layoutChanged=!sameCards || m_width!=atlas.GetWidth() || m_height!=atlas.GetHeight() ||
            tiles.size()!=m_tiles.size() || !std::equal(tiles.begin(),tiles.end(),m_tiles.begin(),[](const auto& a,const auto& b)
            {return a.instanceIndex==b.instanceIndex && a.cardIndex==b.cardIndex && a.x==b.x && a.y==b.y &&
                a.width==b.width && a.height==b.height;});
        uint32_t historyIndex=0;
        if(layoutChanged)
        {
            if(sameCards && !CopySRV(device,resources,historyIndex))return false;
            m_retired[slot]=std::move(m_texture);
            if(!CreateTexture(device.GetDevice(),atlas.GetWidth(),atlas.GetHeight()))return false;
        }
        if(changed){m_updates.clear();m_nextUpdate=0;}
        SurfaceCacheCapturePass::QueryBindings bindings;
        const auto& scene=world.GetGPUScene();
        if(!atlas.PrepareQuery(device,resources,slot,scene.GetInstanceCount(slot),bindings))return false;
        Constants constants{};constants.atlasIndices=bindings.atlasIndices;constants.historyIndex=historyIndex;
        DirectX::XMStoreFloat3(&constants.directionTowardLight,DirectX::XMVector3Normalize(
            DirectX::XMVectorNegate(DirectX::XMLoadFloat3(&light.lightDir))));
        constants.cardCount=uint32_t(atlas.GetTiles().size());constants.sunRadiance=light.lightColor;
        constants.rayBias=config.surfaceRayMinDistance;constants.rayMax=config.globalSDFTraceDistance;
        const auto& grid=scene.GetGrid(slot);constants.gridOrigin=grid.origin;constants.cellSize=grid.cellSize;
        constants.cellDimensions=grid.cellDimensions;constants.boundsMaximum=grid.boundsMaximum;
        constants.materialCount=uint32_t(resources.GetGlobalMaterials().size());
        D3D12_CPU_DESCRIPTOR_HANDLE target{};
        if(!resources.AllocateTransientSrvUavDescriptor(&constants.outputIndex,&target))return false;
        device.GetDevice()->CopyDescriptorsSimple(1,target,CPUHandle(1),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(m_constants[slot]->Map(0,&noReads,&mapped),"map lighting constants"))return false;
        std::memcpy(mapped,&constants,sizeof(constants));m_constants[slot]->Unmap(0,nullptr);
        auto* cmd=device.GetCommandList();m_beforeState=m_state;m_pending=true;m_pendingSource=source;
        m_pendingNext=m_nextUpdate;m_pendingMigration=layoutChanged && sameCards;
        Transition(cmd,m_texture.Get(),m_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);
        if(layoutChanged)
        {
            const auto outputGPU=CD3DX12_GPU_DESCRIPTOR_HANDLE(heaps[0]->GetGPUDescriptorHandleForHeapStart(),
                constants.outputIndex,m_viewStep);
            const float clear[4]={0,0,0,0};
            cmd->ClearUnorderedAccessViewFloat(outputGPU,CPUHandle(1),m_texture.Get(),clear,0,nullptr);
            const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(m_texture.Get());cmd->ResourceBarrier(1,&barrier);
        }
        cmd->SetComputeRootSignature(m_root.Get());cmd->SetPipelineState(m_pipeline.Get());
        cmd->SetComputeRootConstantBufferView(0,m_constants[slot]->GetGPUVirtualAddress());
        for(UINT i=0;i<4;++i){auto* buffer=scene.Get(slot,i);cmd->SetComputeRootShaderResourceView(i+1,buffer?buffer->GetGPUVirtualAddress():0);}
        cmd->SetComputeRootShaderResourceView(5,resources.GetMaterialBufferGPUAddress());
        for(UINT i=0;i<4;++i){auto* buffer=scene.Get(slot,i+4);cmd->SetComputeRootShaderResourceView(i+6,buffer?buffer->GetGPUVirtualAddress():0);}
        cmd->SetComputeRootShaderResourceView(10,bindings.cards);cmd->SetComputeRootShaderResourceView(11,bindings.ranges);
        if(changed)for(uint32_t i=0;i<tiles.size();++i)
        {
            const auto& tile=tiles[i];
            const auto old=std::find_if(m_tiles.begin(),m_tiles.end(),[&](const auto& previous)
                {return previous.instanceIndex==tile.instanceIndex && previous.cardIndex==tile.cardIndex;});
            const bool hasHistory=sameCards && old!=m_tiles.end();
            if(layoutChanged && hasHistory)
            {
                const uint32_t values[9]={i,0,0,tile.width,tile.height,old->x,old->y,old->width,old->height};
                cmd->SetPipelineState(m_resamplePipeline.Get());
                cmd->SetComputeRoot32BitConstants(12,9,values,0);
                cmd->Dispatch(tile.width/8,tile.height/8,1);
            }
            if(!hasHistory)
            {
                // First capture / newly visible Card has nothing to migrate.
                cmd->SetPipelineState(m_pipeline.Get());
                Dispatch(cmd,{i,0,0,tile.width,tile.height});
            }
            else for(uint32_t y=0;y<tile.height;y+=UpdateBlockSize)
                for(uint32_t x=0;x<tile.width;x+=UpdateBlockSize)
                    m_updates.push_back({i,x,y,(std::min)(UpdateBlockSize,tile.width-x),(std::min)(UpdateBlockSize,tile.height-y)});
        }
        if(m_pendingNext<m_updates.size())
        {
            // Resampling writes the same UAV before the first fresh block.
            if(layoutChanged){const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(m_texture.Get());cmd->ResourceBarrier(1,&barrier);}
            cmd->SetPipelineState(m_pipeline.Get());Dispatch(cmd,m_updates[m_pendingNext++]);
        }
        m_pendingTiles=tiles;
        Transition(cmd,m_texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,SurfaceCacheCapturePass::ReadState);
        m_state=SurfaceCacheCapturePass::ReadState;return true;
    }
    void OnSubmitted()
    {
        if(!m_pending)return;
        m_pending=false;m_valid=true;m_source=m_pendingSource;m_tiles=std::move(m_pendingTiles);
        m_nextUpdate=m_pendingNext;++m_batches;if(m_pendingMigration)++m_migrations;
    }
    void CancelRecording(){if(m_pending)m_state=m_beforeState;m_pending=false;}
    ID3D12Resource* GetTexture()const{return m_texture.Get();}
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPUHandle()const{return CPUHandle(0);}
    uint64_t GetRelightBatches()const{return m_batches;}
    uint64_t GetMigrationBatches()const{return m_migrations+(m_pending && m_pendingMigration?1u:0u);}
    size_t GetRemainingBlocks()const{return m_updates.size()-(m_pending?m_pendingNext:m_nextUpdate);}
    size_t GetUpdateBlockCount()const{return m_updates.size();}
    static constexpr uint32_t UpdateBlockSize=64;
    uint64_t GetContentStamp()const
    {
        // Match the source being recorded this frame, before submission. Card
        // layouts and capture/relight counters are not lighting-content changes.
        const auto& source=m_pending?m_pendingSource:m_source;
        uint64_t stamp=14695981039346656037ull;
        const auto append=[&](const auto& value){const auto* bytes=reinterpret_cast<const uint8_t*>(&value);
            for(size_t i=0;i<sizeof(value);++i)stamp=(stamp^bytes[i])*1099511628211ull;};
        append(source.scene);append(source.materials);
        append(source.direction);append(source.color);append(source.distance);append(source.bias);
        return stamp;
    }
    bool CopySRV(RenderDevice& device,ResourceManager& resources,uint32_t& index)const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE target{};
        if(!resources.AllocateTransientSrvUavDescriptor(&index,&target))return false;
        device.GetDevice()->CopyDescriptorsSimple(1,target,CPUHandle(0),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return true;
    }

private:
    struct Source
    {
        uint64_t scene,cards,materials,capture;
        std::array<float,3> direction,color;float distance,bias;
        bool operator==(const Source&)const=default;
    };
    struct Constants
    {
        std::array<uint32_t,4> atlasIndices;
        uint32_t outputIndex,historyIndex,padding1,padding2;
        DirectX::XMFLOAT3 directionTowardLight;uint32_t cardCount;
        DirectX::XMFLOAT3 sunRadiance;float rayBias;
        DirectX::XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;float rayMax;
        DirectX::XMFLOAT3 boundsMaximum;uint32_t materialCount;
    };
    static_assert(sizeof(Constants)==112);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    struct Update {uint32_t card,x,y,width,height;};
    static void Dispatch(ID3D12GraphicsCommandList* cmd,const Update& update)
    {
        const uint32_t values[9]={update.card,update.x,update.y,update.width,update.height,0,0,0,0};
        cmd->SetComputeRoot32BitConstants(12,9,values,0);
        cmd->Dispatch((update.width+7)/8,(update.height+7)/8,1);
    }
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline,m_resamplePipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_views;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_constants;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_retired;
    std::vector<SurfaceCacheCapturePass::Tile> m_tiles,m_pendingTiles;
    std::vector<Update> m_updates;
    size_t m_nextUpdate=0,m_pendingNext=0;
    Source m_source{},m_pendingSource{};
    D3D12_RESOURCE_STATES m_state=D3D12_RESOURCE_STATE_COMMON,m_beforeState=D3D12_RESOURCE_STATE_COMMON;
    uint32_t m_width=0,m_height=0,m_viewStep=0;
    uint64_t m_batches=0,m_migrations=0;bool m_valid=false,m_pending=false,m_pendingMigration=false;
    static bool Check(HRESULT hr,const char* operation)
    {
        if(SUCCEEDED(hr))return true;ErrorLog::HRESULT(std::string("Surface Cache lighting: ")+operation,hr);return false;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
    {
        if(before==after)return;
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);cmd->ResourceBarrier(1,&barrier);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(UINT index)const
    {return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_views->GetCPUDescriptorHandleForHeapStart(),index,m_viewStep);}
    bool CreateTexture(ID3D12Device* device,UINT width,UINT height)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=2;
        if(!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(m_views.ReleaseAndGetAddressOf())),"create lighting views"))return false;
        m_viewStep=device->GetDescriptorHandleIncrementSize(heap.Type);
        const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(m_texture.ReleaseAndGetAddressOf())),"create lighting atlas"))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=desc.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(m_texture.Get(),&srv,CPUHandle(0));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=desc.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_texture.Get(),nullptr,&uav,CPUHandle(1));
        m_width=width;m_height=height;m_state=D3D12_RESOURCE_STATE_COMMON;return true;
    }
};
