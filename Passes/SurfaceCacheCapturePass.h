#pragma once

#include "ResourceManager.h"
#include "WorldGeometryRuntime.h"
#include "PBR_Shader.h"
#include "SurfaceCacheFeedback.h"

// Per-instance lighting tiles alias compatible shared material/depth rectangles.
// Capture each material rectangle once with LOD0; feedback expires instance tiles.
class SurfaceCacheCapturePass
{
public:
    static constexpr uint32_t ChannelCount=4;
    static constexpr D3D12_RESOURCE_STATES ReadState=static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    struct Tile
    {
        SurfaceCacheCards::Card card;
        uint32_t instanceIndex=0,cardIndex=0,x=0,y=0,width=0,height=0,feedbackIndex=0;
        uint32_t materialX=0,materialY=0,materialInstanceIndex=0;
    };
    struct Stats { uint64_t captureBatches=0,draws=0,budgetHalvings=0,budgetRejections=0; };
    struct QueryCard
    {
        DirectX::XMFLOAT3 origin;uint32_t instanceIndex;
        DirectX::XMFLOAT3 axisX;float extentX;
        DirectX::XMFLOAT3 axisY;float extentY;
        DirectX::XMFLOAT3 axisZ;float extentZ;
        uint32_t x,y,width,height,cardIndex,feedbackIndex;
        uint32_t materialX,materialY,materialInstanceIndex;
    };
    static_assert(sizeof(QueryCard)==100 && offsetof(QueryCard,feedbackIndex)==84);
    struct QueryBindings
    {
        D3D12_GPU_VIRTUAL_ADDRESS cards=0,ranges=0;
        std::array<uint32_t,4> atlasIndices{};
    };

    bool Initialize(RenderDevice& device,UINT frameCount)
    {
        m_constants.resize(frameCount);m_constantBytes.resize(frameCount);
        m_queryBuffers.resize(frameCount);
        m_frameCount=frameCount;m_feedback.Initialize(frameCount);
        return CreatePipelines(device.GetDevice(),device.GetRenderTarget(0)->GetDesc().Format);
    }

    bool Record(RenderDevice& device,ResourceManager& resources,const WorldGeometryRuntime& world,int slot,
        const SurfaceCacheConfig& config,const DirectX::XMFLOAT3& cameraPosition)
    {
        const Source source{world.GetScene().GetRevision(),world.GetCardBuildCount(),resources.GetMaterialRevision(),config};
        std::vector<Tile> tiles;
        const auto& instances=world.GetScene().GetInstances();
        size_t count=0;
        for(const auto& instance:instances)for(const auto& asset:world.GetCardAssets())
            if(asset.geometry==instance.asset)count+=asset.data.cards.size();
        const bool reset=!m_valid || source.scene!=m_source.scene || source.cards!=m_source.cards;
        const bool capacityChanged=!m_valid || config.atlasSize!=m_source.config.atlasSize ||
            config.minCardResolution!=m_source.config.minCardResolution || config.maxCardResolution!=m_source.config.maxCardResolution;
        if(!m_feedback.Begin(device,resources,slot,count,reset))return false;
        if(reset || capacityChanged){m_allocations.assign(count,{});++m_budgetRevision;}
        m_queryTiles.clear();m_residentIndices.resize(count);uint32_t feedbackIndex=0;
        for(uint32_t i=0;i<instances.size();++i)
            for(const auto& asset:world.GetCardAssets())if(asset.geometry==instances[i].asset)
                for(uint32_t c=0;c<asset.data.cards.size();++c)
                {
                    Tile tile;tile.card=asset.data.cards[c];tile.instanceIndex=i;tile.cardIndex=c;tile.feedbackIndex=feedbackIndex++;
                    // Canonical handedness keeps winding and UV orientation identical
                    // for all six capture directions. Bounds are unaffected by the flip.
                    tile.card.axisY=SurfaceCacheCards::Cross(tile.card.axisX,tile.card.axisZ);
                    const float distance=CardWorldDistance(tile.card,instances[i].worldFromLocal,cameraPosition);
                    const bool inRange=distance<=config.maxDistance;
                    const bool resident=m_feedback.Select(tile.feedbackIndex,inRange);
                    if(!inRange){m_allocations[tile.feedbackIndex].width=m_allocations[tile.feedbackIndex].height=0;continue;}
                    const auto transform=DirectX::XMLoadFloat4x4(&instances[i].worldFromLocal);
                    const auto resolution=[&](const DirectX::XMFLOAT3& axis,float extent)
                    {
                        const float worldSize=2*extent*DirectX::XMVectorGetX(DirectX::XMVector3Length(
                            DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&axis),transform)));
                        return DesiredResolution(worldSize,distance,config);
                    };
                    tile.width=resolution(tile.card.axisX,tile.card.extent.x);
                    tile.height=resolution(tile.card.axisY,tile.card.extent.y);
                    auto& allocation=m_allocations[tile.feedbackIndex];
                    const bool targetChanged=allocation.desiredWidth!=tile.width || allocation.desiredHeight!=tile.height;
                    allocation.desiredWidth=tile.width;allocation.desiredHeight=tile.height;
                    if(targetChanged)allocation.rejectedRevision=UINT64_MAX;
                    if(!resident)allocation.width=allocation.height=0;
                    else if(allocation.width==0 && allocation.rejectedRevision==m_budgetRevision)m_feedback.Reject(tile.feedbackIndex);
                    else
                    {
                        if(allocation.width!=0 && !targetChanged){tile.width=allocation.width;tile.height=allocation.height;}
                        tiles.push_back(tile);
                    }
                    tile.width=tile.height=0;
                    m_queryTiles.push_back(tile);
                }
        uint32_t width=m_width,height=m_height;std::vector<uint32_t> rejected;
        if(capacityChanged || !SameTiles(tiles,m_tiles))AdmitTiles(config,tiles,width,height,rejected);
        const bool same=SameTiles(tiles,m_tiles);
        if(!same)++m_budgetRevision;
        for(uint32_t index:rejected)m_allocations[index].rejectedRevision=m_budgetRevision;
        for(uint32_t i=0;i<tiles.size();++i)
        {
            auto& tile=tiles[i];auto& allocation=m_allocations[tile.feedbackIndex];
            allocation.width=tile.width;allocation.height=tile.height;
            m_residentIndices[tile.feedbackIndex]=i;m_feedback.Accept(tile.feedbackIndex);
            if(same && !capacityChanged){tile.x=m_tiles[i].x;tile.y=m_tiles[i].y;}
        }
        for(auto& tile:m_queryTiles)
        {const auto& allocation=m_allocations[tile.feedbackIndex];tile.width=allocation.width;tile.height=allocation.height;}
        // Remember admitted sizes and declined requests. Ordinary feedback does
        // not retry a full Atlas or recapture an unchanged resident set.
        if(m_valid && source==m_source && same)return true;
        m_tiles=std::move(tiles);
        if(same && !capacityChanged){width=m_width;height=m_height;}
        uint32_t materialWidth=width,materialHeight=height;
        BuildSharedMaterialTiles(world,config,materialWidth,materialHeight);
        m_width=width;m_height=height;
        if(!m_textures[0] || materialWidth!=m_materialWidth || materialHeight!=m_materialHeight)
        {
            if(m_textures[0])for(UINT i=0;i<m_frameCount;++i)device.WaitForPreviousFrame(int(i));
            if(!CreateTextures(device.GetDevice(),materialWidth,materialHeight))return false;
        }
        const uint64_t bytes=(std::max)(uint64_t(256),uint64_t(m_tiles.size())*256);
        auto& constants=m_constants[slot];
        if(m_constantBytes[slot]!=bytes)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if(!Check(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(constants.ReleaseAndGetAddressOf())),"create capture constants"))return false;
            m_constantBytes[slot]=bytes;
        }
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(constants->Map(0,&noReads,&mapped),"map capture constants"))return false;
        for(uint32_t i=0;i<m_materialTiles.size();++i)
        {
            const auto& tile=m_materialTiles[i];
            const auto& card=tile.card;const auto& instance=instances[tile.instanceIndex];
            Constants value{};
            value.origin=card.origin;value.right=card.axisX;value.up=card.axisY;value.normal=card.axisZ;
            value.inverseExtentX=1/card.extent.x;value.inverseExtentY=1/card.extent.y;value.inverseDepthSpan=0.5f/card.extent.z;
            value.worldFromLocal=instance.worldFromLocal;value.normalFromLocal=instance.normalFromLocal;
            std::memcpy(static_cast<uint8_t*>(mapped)+i*256,&value,sizeof(value));
        }
        constants->Unmap(0,nullptr);
        auto* cmd=device.GetCommandList();
        m_beforeState=m_state;m_pending=true;m_pendingSource=source;m_batchDraws=0;
        for(UINT i=0;i<ChannelCount;++i)Transition(cmd,m_textures[i].Get(),m_state,
            i==3?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4]={0,0,0,0};
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3];
        for(UINT i=0;i<3;++i){rtvs[i]=RTV(i);cmd->ClearRenderTargetView(rtvs[i],clear,0,nullptr);}
        cmd->ClearDepthStencilView(m_dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        const auto dsv=m_dsv->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(3,rtvs,FALSE,&dsv);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};
        cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetGraphicsRootSignature(m_captureRoot.Get());cmd->SetPipelineState(m_capturePSO.Get());
        cmd->SetGraphicsRootShaderResourceView(1,resources.GetMaterialBufferGPUAddress());
        auto& models=resources.GetSceneInstances();
        for(uint32_t i=0;i<m_materialTiles.size();++i)
        {
            const auto& tile=m_materialTiles[i];const auto& instance=instances[tile.instanceIndex];
            const D3D12_VIEWPORT viewport{float(tile.x),float(tile.y),float(tile.width),float(tile.height),0,1};
            const D3D12_RECT scissor{LONG(tile.x),LONG(tile.y),LONG(tile.x+tile.width),LONG(tile.y+tile.height)};
            cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
            cmd->SetGraphicsRootConstantBufferView(0,constants->GetGPUVirtualAddress()+i*256);
            auto& meshes=models[instance.sourceInstanceIndex].pModel->meshes;
            for(uint32_t mesh=0;mesh<meshes.size();++mesh)
            {
                const uint32_t draw[2]={instance.materialIds[mesh],instance.isCutout?1u:0u};
                cmd->SetGraphicsRoot32BitConstants(2,2,draw,0);meshes[mesh].Draw(cmd,1,0);++m_batchDraws;
            }
        }
        for(UINT i=0;i<ChannelCount;++i)Transition(cmd,m_textures[i].Get(),
            i==3?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET,ReadState);
        m_state=ReadState;
        return true;
    }

    void OnSubmitted()
    {
        m_feedback.OnSubmitted();
        if(!m_pending)return;
        m_source=m_pendingSource;m_valid=true;m_pending=false;
        ++m_stats.captureBatches;m_stats.draws+=m_batchDraws;
    }
    void CancelRecording(){if(m_pending)m_state=m_beforeState;m_pending=false;m_feedback.CancelRecording();}
    void RecordFeedbackReadback(RenderDevice& device){m_feedback.RecordReadback(device);}
    const SurfaceCacheFeedback& GetFeedback()const{return m_feedback;}

    // Call after Record and after retiring this frame slot. Metadata and atlas
    // use the same Card frame; no material or triangle lookup is needed to read it.
    bool PrepareQuery(RenderDevice& device,ResourceManager& resources,int slot,uint32_t instanceCount,QueryBindings& bindings,bool includeEvicted=false)
    {
        std::vector<QueryCard> cards;
        std::vector<std::array<uint32_t,2>> ranges((std::max)(1u,instanceCount));
        for(auto tile:includeEvicted?m_queryTiles:m_tiles)
        {
            if(includeEvicted && tile.width!=0)tile=m_tiles[m_residentIndices[tile.feedbackIndex]];
            auto& range=ranges[tile.instanceIndex];if(range[1]++==0)range[0]=uint32_t(cards.size());
            const auto& c=tile.card;
            cards.push_back({c.origin,tile.instanceIndex,c.axisX,c.extent.x,c.axisY,c.extent.y,c.axisZ,c.extent.z,
                tile.x,tile.y,tile.width,tile.height,tile.cardIndex,tile.feedbackIndex,
                tile.materialX,tile.materialY,tile.materialInstanceIndex});
        }
        if(cards.empty())cards.emplace_back();
        auto& buffers=m_queryBuffers[slot];
        const UINT base=includeEvicted?2u:0u;
        if(!UploadQuery(device.GetDevice(),cards.data(),cards.size()*sizeof(QueryCard),buffers[base]) ||
            !UploadQuery(device.GetDevice(),ranges.data(),ranges.size()*sizeof(ranges[0]),buffers[base+1]))return false;
        bindings.cards=buffers[base].resource->GetGPUVirtualAddress();bindings.ranges=buffers[base+1].resource->GetGPUVirtualAddress();
        return CopyAtlasDescriptors(device,resources,bindings.atlasIndices);
    }

    bool RecordPreview(RenderDevice& device,ResourceManager& resources,int slot,
        D3D12_CPU_DESCRIPTOR_HANDLE outputRTV,uint32_t mode,D3D12_CPU_DESCRIPTOR_HANDLE lightingSRV={})
    {
        std::array<uint32_t,4> indices;
        if(!CopyAtlasDescriptors(device,resources,indices))return false;
        uint32_t lightingIndex=0;
        if(mode==29 || mode==30)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE target{};
            if(!resources.AllocateTransientSrvUavDescriptor(&lightingIndex,&target))return false;
            device.GetDevice()->CopyDescriptorsSimple(1,target,lightingSRV,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        auto* cmd=device.GetCommandList();auto* output=device.GetRenderTarget(slot);const auto desc=output->GetDesc();
        Transition(cmd,output,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1,&outputRTV,FALSE,nullptr);
        const D3D12_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};
        const D3D12_RECT scissor{0,0,LONG(desc.Width),LONG(desc.Height)};
        cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&scissor);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetGraphicsRootSignature(m_previewRoot.Get());cmd->SetPipelineState(m_previewPSO.Get());
        const uint32_t values[12]={indices[0],indices[1],indices[2],indices[3],mode,
            uint32_t(desc.Width),desc.Height,(mode==29 || mode==30)?m_width:m_materialWidth,
            (mode==29 || mode==30)?m_height:m_materialHeight,lightingIndex,0,0};
        cmd->SetGraphicsRoot32BitConstants(0,12,values,0);cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3,1,0,0);
        Transition(cmd,output,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        return true;
    }

    ID3D12Resource* GetTexture(UINT channel)const{return m_textures[channel].Get();}
    const std::vector<Tile>& GetTiles()const{return m_tiles;}
    const Stats& GetStats()const{return m_stats;}
    uint32_t GetWidth()const{return m_width;}
    uint32_t GetHeight()const{return m_height;}
    uint32_t GetMaterialWidth()const{return m_materialWidth;}
    uint32_t GetMaterialHeight()const{return m_materialHeight;}
    size_t GetMaterialTileCount()const{return m_materialTiles.size();}
    uint64_t GetRecordingRevision()const{return m_stats.captureBatches+(m_pending?1u:0u);}
    const std::string& GetLastError()const{return m_error;}

private:
    struct Source
    {
        uint64_t scene,cards,materials;
        SurfaceCacheConfig config;
        bool operator==(const Source&)const=default;
    };
    struct Constants
    {
        DirectX::XMFLOAT3 origin;float padding;
        DirectX::XMFLOAT3 right;float inverseExtentX;
        DirectX::XMFLOAT3 up;float inverseExtentY;
        DirectX::XMFLOAT3 normal;float inverseDepthSpan;
        DirectX::XMFLOAT4X4 worldFromLocal,normalFromLocal;
    };
    static_assert(sizeof(Constants)==192 && offsetof(Constants,worldFromLocal)==64);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_captureRoot,m_previewRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_capturePSO,m_previewPSO;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> m_textures;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtv,m_dsv,m_srv;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_constants;
    std::vector<uint64_t> m_constantBytes;
    struct QueryBuffer { Microsoft::WRL::ComPtr<ID3D12Resource> resource;uint64_t bytes=0; };
    std::vector<std::array<QueryBuffer,4>> m_queryBuffers;
    std::vector<Tile> m_tiles;
    std::vector<Tile> m_materialTiles;
    std::vector<Tile> m_queryTiles;
    std::vector<uint32_t> m_residentIndices;
    SurfaceCacheFeedback m_feedback;
    struct Allocation
    {
        uint32_t desiredWidth=0,desiredHeight=0,width=0,height=0;
        uint64_t rejectedRevision=UINT64_MAX;
    };
    std::vector<Allocation> m_allocations;
    uint64_t m_budgetRevision=0;
    Source m_source{},m_pendingSource{};
    Stats m_stats;
    uint64_t m_batchDraws=0;
    uint32_t m_width=0,m_height=0,m_frameCount=0,m_rtvStep=0,m_srvStep=0;
    uint32_t m_materialWidth=0,m_materialHeight=0;
    D3D12_RESOURCE_STATES m_state=D3D12_RESOURCE_STATE_COMMON,m_beforeState=D3D12_RESOURCE_STATE_COMMON;
    bool m_valid=false,m_pending=false;
    std::string m_error;
    void BuildSharedMaterialTiles(const WorldGeometryRuntime& world,const SurfaceCacheConfig& config,
        uint32_t& width,uint32_t& height)
    {
        // Like UE's compatible-card capture reuse, but alias the material rectangle
        // itself. Lighting rectangles and feedback remain owned by each instance.
        // Equal signed scale preserves the normal-map tangent frame under rotation.
        const auto& instances=world.GetScene().GetInstances();
        m_materialTiles.clear();std::vector<size_t> owners;
        for(const auto& tile:m_tiles)
        {
            const auto& instance=instances[tile.instanceIndex];
            size_t owner=0;
            for(;owner<m_materialTiles.size();++owner)
            {
                const auto& other=m_materialTiles[owner];const auto& source=instances[other.instanceIndex];
                if(tile.cardIndex==other.cardIndex && tile.width==other.width && tile.height==other.height &&
                    instance.asset==source.asset && instance.materialIds==source.materialIds && instance.isCutout==source.isCutout &&
                    instance.scale.x==source.scale.x && instance.scale.y==source.scale.y && instance.scale.z==source.scale.z)break;
            }
            if(owner==m_materialTiles.size())m_materialTiles.push_back(tile);
            owners.push_back(owner);
        }
        // The original lighting layout is always a valid fallback for this subset.
        auto packed=m_materialTiles;uint32_t packedWidth=0,packedHeight=0;
        if(TryPackTiles(config,packed,packedWidth,packedHeight) && uint64_t(packedWidth)*packedHeight<=uint64_t(width)*height)
        {m_materialTiles=std::move(packed);width=packedWidth;height=packedHeight;}
        for(size_t i=0;i<m_tiles.size();++i)
        {
            const auto& owner=m_materialTiles[owners[i]];
            m_tiles[i].materialX=owner.x;m_tiles[i].materialY=owner.y;m_tiles[i].materialInstanceIndex=owner.instanceIndex;
        }
    }
    static float CardWorldDistance(const SurfaceCacheCards::Card& card,const DirectX::XMFLOAT4X4& worldFromLocal,
        const DirectX::XMFLOAT3& cameraPosition)
    {
        // Six-axis Cards and the instance SRT form a world OBB. Measure to its
        // closest point, not its center, so a large floor stays detailed nearby.
        using namespace DirectX;
        const auto transform=XMLoadFloat4x4(&worldFromLocal);
        const auto offset=XMVectorSubtract(XMLoadFloat3(&cameraPosition),
            XMVector3TransformCoord(XMLoadFloat3(&card.origin),transform));
        const XMFLOAT3 axes[]={card.axisX,card.axisY,card.axisZ};
        const float extents[]={card.extent.x,card.extent.y,card.extent.z};
        float squaredDistance=0;
        for(uint32_t i=0;i<3;++i)
        {
            const auto axis=XMVector3TransformNormal(XMLoadFloat3(&axes[i]),transform);
            const float scale=XMVectorGetX(XMVector3Length(axis));
            const float projection=XMVectorGetX(XMVector3Dot(offset,axis))/scale;
            const float outside=(std::max)(std::abs(projection)-extents[i]*scale,0.0f);
            squaredDistance+=outside*outside;
        }
        return std::sqrt(squaredDistance);
    }
    static uint32_t DesiredResolution(float worldSize,float distance,const SurfaceCacheConfig& config)
    {
        // UE's ordinary near-field base density, in meters and at scale 1:
        // target texel = max(10cm, 0.02 * max(OBB distance, 1m)). As in UE,
        // truncate the requested texel count before snapping to a power of two.
        const float targetTexel=(std::max)(config.worldTexelSize,0.02f*(std::max)(distance,1.0f));
        const float texels=std::floor(worldSize/targetTexel);
        uint32_t size=config.minCardResolution;
        while(size<config.maxCardResolution && float(size)<texels)size*=2;
        return size;
    }
    static bool SameTiles(const std::vector<Tile>& a,const std::vector<Tile>& b)
    {
        return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y)
        {return x.instanceIndex==y.instanceIndex && x.cardIndex==y.cardIndex && x.feedbackIndex==y.feedbackIndex &&
            x.width==y.width && x.height==y.height;});
    }
    void AdmitTiles(const SurfaceCacheConfig& config,std::vector<Tile>& tiles,uint32_t& width,uint32_t& height,
        std::vector<uint32_t>& rejected)
    {
        if(TryPackTiles(config,tiles,width,height))return;
        // Preserve current allocations first; a failed upgrade keeps its old
        // resolution. New requests alone may be halved, at most twice.
        std::vector<Tile> admitted;
        for(auto tile:tiles)
        {
            const auto& allocation=m_allocations[tile.feedbackIndex];
            if(allocation.width==0)continue;
            tile.width=(std::min)(tile.width,allocation.width);tile.height=(std::min)(tile.height,allocation.height);
            admitted.push_back(tile);
        }
        if(!TryPackTiles(config,admitted,width,height))
        {
            // A smaller resident subset always has its previous valid layout,
            // even if the packing heuristic doesn't rediscover that layout.
            width=m_width;height=m_height;
            for(auto& tile:admitted)
            {
                const auto old=std::find_if(m_tiles.begin(),m_tiles.end(),[&](const auto& value){return value.feedbackIndex==tile.feedbackIndex;});
                tile.x=old->x;tile.y=old->y;
            }
        }
        for(auto tile:tiles)
        {
            const auto found=std::find_if(admitted.begin(),admitted.end(),[&](const auto& value){return value.feedbackIndex==tile.feedbackIndex;});
            const size_t index=size_t(found-admitted.begin());const bool existing=found!=admitted.end();
            if(existing && found->width==tile.width && found->height==tile.height)continue;
            for(uint32_t halvings=0;;++halvings)
            {
                auto attempt=admitted;
                if(existing)attempt[index]=tile;else attempt.push_back(tile);
                uint32_t attemptWidth=0,attemptHeight=0;
                if(TryPackTiles(config,attempt,attemptWidth,attemptHeight))
                {
                    admitted=std::move(attempt);width=attemptWidth;height=attemptHeight;
                    m_stats.budgetHalvings+=halvings;break;
                }
                if(existing)break;
                const uint32_t nextWidth=(std::max)(config.minCardResolution,tile.width/2);
                const uint32_t nextHeight=(std::max)(config.minCardResolution,tile.height/2);
                if(halvings==2 || (nextWidth==tile.width && nextHeight==tile.height))
                {
                    rejected.push_back(tile.feedbackIndex);m_feedback.Reject(tile.feedbackIndex);++m_stats.budgetRejections;break;
                }
                tile.width=nextWidth;tile.height=nextHeight;
            }
        }
        std::sort(admitted.begin(),admitted.end(),[](const auto& a,const auto& b){return a.feedbackIndex<b.feedbackIndex;});
        tiles=std::move(admitted);
    }
    static bool TryPackTiles(const SurfaceCacheConfig& config,std::vector<Tile>& tiles,uint32_t& width,uint32_t& height)
    {
        // Greedy best-short-side fit in free rectangles (no Card rotation).
        // Sort indices only: per-instance query ranges keep their original order.
        std::vector<uint32_t> order(tiles.size());
        uint64_t tileArea=0;
        for(uint32_t i=0;i<order.size();++i)order[i]=i;
        for(const auto& tile:tiles)tileArea+=uint64_t(tile.width)*tile.height;
        const auto sortOrder=[&](bool heightFirst){std::stable_sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b)
        {
            const auto& x=tiles[a];const auto& y=tiles[b];
            if(heightFirst && x.height!=y.height)return x.height>y.height;
            const uint64_t areaX=uint64_t(x.width)*x.height,areaY=uint64_t(y.width)*y.height;
            if(areaX!=areaY)return areaX>areaY;
            const auto edgeX=(std::max)(x.width,x.height),edgeY=(std::max)(y.width,y.height);
            if(edgeX!=edgeY)return edgeX>edgeY;
            return x.height!=y.height?x.height>y.height:x.width>y.width;
        });};
        // Prefer the smallest atlas area, then the most balanced dimensions.
        std::vector<std::array<uint32_t,2>> sizes;
        for(uint32_t w=config.minCardResolution;w<=config.atlasSize;w*=2)
            for(uint32_t h=config.minCardResolution;h<=config.atlasSize;h*=2)
                if(uint64_t(w)*h>=tileArea)sizes.push_back({w,h});
        std::sort(sizes.begin(),sizes.end(),[](const auto& a,const auto& b)
        {
            const uint64_t areaA=uint64_t(a[0])*a[1],areaB=uint64_t(b[0])*b[1];
            if(areaA!=areaB)return areaA<areaB;
            const auto edgeA=(std::max)(a[0],a[1]),edgeB=(std::max)(b[0],b[1]);
            return edgeA!=edgeB?edgeA<edgeB:a[0]<b[0];
        });
        struct Rect {uint32_t x,y,width,height;};
        std::vector<std::array<uint32_t,2>> positions(tiles.size());
        for(const auto& size:sizes)
        for(uint32_t strategy=0;strategy<2;++strategy)
        {
            // Area/short-side fit fills gaps; height/bottom-left fit also covers
            // long strips efficiently. Keep the first fit in atlas-area order.
            sortOrder(strategy==1);
            std::vector<Rect> free{{0,0,size[0],size[1]}};
            bool fits=true;
            for(uint32_t index:order)
            {
                const auto& tile=tiles[index];
                uint32_t best=UINT32_MAX;
                std::array<uint32_t,4> bestScore{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
                for(uint32_t i=0;i<free.size();++i)
                {
                    const auto& r=free[i];
                    if(tile.width>r.width || tile.height>r.height)continue;
                    const uint32_t dx=r.width-tile.width,dy=r.height-tile.height;
                    const std::array<uint32_t,4> score=strategy==0?
                        std::array<uint32_t,4>{(std::min)(dx,dy),(std::max)(dx,dy),r.y,r.x}:
                        std::array<uint32_t,4>{r.y,r.x,0,0};
                    if(score<bestScore){best=i;bestScore=score;}
                }
                if(best==UINT32_MAX){fits=false;break;}
                const Rect placed{free[best].x,free[best].y,tile.width,tile.height};
                positions[index]={placed.x,placed.y};
                // Free rectangles may overlap each other, but never a placed tile.
                // Split every intersecting rectangle so gaps remain available.
                std::vector<Rect> next;
                for(const auto& r:free)
                {
                    if(placed.x>=r.x+r.width || placed.x+placed.width<=r.x ||
                        placed.y>=r.y+r.height || placed.y+placed.height<=r.y)
                    {next.push_back(r);continue;}
                    if(placed.x>r.x)next.push_back({r.x,r.y,placed.x-r.x,r.height});
                    if(placed.x+placed.width<r.x+r.width)
                        next.push_back({placed.x+placed.width,r.y,r.x+r.width-placed.x-placed.width,r.height});
                    if(placed.y>r.y)next.push_back({r.x,r.y,r.width,placed.y-r.y});
                    if(placed.y+placed.height<r.y+r.height)
                        next.push_back({r.x,placed.y+placed.height,r.width,r.y+r.height-placed.y-placed.height});
                }
                // Drop duplicates and rectangles fully contained in another.
                for(size_t i=0;i<next.size();)
                {
                    const auto r=next[i];bool contained=false;
                    for(size_t j=0;j<next.size();++j)if(i!=j)
                    {
                        const auto& outer=next[j];
                        if(r.x>=outer.x && r.y>=outer.y && r.x+r.width<=outer.x+outer.width &&
                            r.y+r.height<=outer.y+outer.height){contained=true;break;}
                    }
                    if(contained)next.erase(next.begin()+i);else ++i;
                }
                free=std::move(next);
            }
            if(!fits)continue;
            width=size[0];height=size[1];
            for(uint32_t i=0;i<tiles.size();++i){tiles[i].x=positions[i][0];tiles[i].y=positions[i][1];}
            return true;
        }
        return false;
    }
    bool Check(HRESULT result,const char* operation)
    {
        if(SUCCEEDED(result))return true;
        m_error=std::string("Surface Cache capture: ")+operation;
        ErrorLog::HRESULT(m_error,result);return false;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
    {
        if(before==after)return;
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,before,after);cmd->ResourceBarrier(1,&barrier);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(UINT i)const
    {return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtv->GetCPUDescriptorHandleForHeapStart(),i,m_rtvStep);}
    D3D12_CPU_DESCRIPTOR_HANDLE SRV(UINT i)const
    {return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_srv->GetCPUDescriptorHandleForHeapStart(),i,m_srvStep);}

    bool CopyAtlasDescriptors(RenderDevice& device,ResourceManager& resources,std::array<uint32_t,4>& indices)
    {
        for(UINT i=0;i<4;++i)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE target{};
            if(!resources.AllocateTransientSrvUavDescriptor(&indices[i],&target))return false;
            device.GetDevice()->CopyDescriptorsSimple(1,target,SRV(i),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return true;
    }
    bool UploadQuery(ID3D12Device* device,const void* data,uint64_t bytes,QueryBuffer& buffer)
    {
        if(buffer.bytes!=bytes)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if(!Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,IID_PPV_ARGS(buffer.resource.ReleaseAndGetAddressOf())),"create query metadata"))return false;
            buffer.bytes=bytes;
        }
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(!Check(buffer.resource->Map(0,&noReads,&mapped),"map query metadata"))return false;
        std::memcpy(mapped,data,bytes);buffer.resource->Unmap(0,nullptr);return true;
    }

    bool CreateTextures(ID3D12Device* device,UINT width,UINT height)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=3;
        if(!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(m_rtv.ReleaseAndGetAddressOf())),"create RTV heap"))return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;heap.NumDescriptors=1;
        if(!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(m_dsv.ReleaseAndGetAddressOf())),"create DSV heap"))return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=4;
        if(!Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(m_srv.ReleaseAndGetAddressOf())),"create CPU SRV heap"))return false;
        m_rtvStep=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_srvStep=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(UINT i=0;i<4;++i)
        {
            const bool depth=i==3;const auto properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(depth?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R16G16B16A16_FLOAT,
                width,height,1,1,1,0,depth?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            D3D12_CLEAR_VALUE clear{};clear.Format=depth?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(depth)clear.DepthStencil.Depth=1;
            if(!Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
                &clear,IID_PPV_ARGS(m_textures[i].ReleaseAndGetAddressOf())),"create atlas texture"))return false;
            if(depth)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
                device->CreateDepthStencilView(m_textures[i].Get(),&dsv,m_dsv->GetCPUDescriptorHandleForHeapStart());
            }
            else device->CreateRenderTargetView(m_textures[i].Get(),nullptr,RTV(i));
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels=1;device->CreateShaderResourceView(m_textures[i].Get(),&srv,SRV(i));
        }
        m_materialWidth=width;m_materialHeight=height;m_state=D3D12_RESOURCE_STATE_COMMON;
        return true;
    }

    bool CreatePipelines(ID3D12Device* device,DXGI_FORMAT outputFormat)
    {
        CD3DX12_ROOT_PARAMETER parameters[3];parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsShaderResourceView(0,0,D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[2].InitAsConstants(2,1,0,D3D12_SHADER_VISIBILITY_PIXEL);
        const auto sampler=CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        const auto flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        const CD3DX12_ROOT_SIGNATURE_DESC root(3,parameters,1,&sampler,flags);
        Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
        if(!Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize capture root") ||
            !Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_captureRoot)),"create capture root"))return false;
        const auto vs=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheCapture.hlsl",L"VSCapture",L"vs_6_6");
        const auto ps=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheCapture.hlsl",L"PSCapture",L"ps_6_6");
        if(!vs||!ps)return false;
        const D3D12_INPUT_ELEMENT_DESC input[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TANGENT",0,DXGI_FORMAT_R32G32B32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BITANGENT",0,DXGI_FORMAT_R32G32B32_FLOAT,0,44,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=m_captureRoot.Get();pso.InputLayout={input,5};
        pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pso.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);pso.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);pso.SampleMask=UINT_MAX;
        pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.SampleDesc.Count=1;
        pso.NumRenderTargets=3;for(UINT i=0;i<3;++i)pso.RTVFormats[i]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        pso.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        if(!Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&m_capturePSO)),"create capture PSO"))return false;
        CD3DX12_ROOT_PARAMETER previewParameter;previewParameter.InitAsConstants(12,0);
        const CD3DX12_ROOT_SIGNATURE_DESC previewRoot(1,&previewParameter,0,nullptr,flags);
        if(!Check(D3D12SerializeRootSignature(&previewRoot,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize preview root") ||
            !Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&m_previewRoot)),"create preview root"))return false;
        const auto pvs=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheAtlasPreview.hlsl",L"VSMain",L"vs_6_6");
        const auto pps=ShaderCompiler::CompileFromFile(L"Shaders/SurfaceCacheAtlasPreview.hlsl",L"PSMain",L"ps_6_6");
        if(!pvs||!pps)return false;
        pso={};pso.pRootSignature=m_previewRoot.Get();pso.VS={pvs->GetBufferPointer(),pvs->GetBufferSize()};pso.PS={pps->GetBufferPointer(),pps->GetBufferSize()};
        pso.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);pso.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable=FALSE;pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.SampleMask=UINT_MAX;pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.SampleDesc.Count=1;
        pso.NumRenderTargets=1;pso.RTVFormats[0]=outputFormat;
        return Check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&m_previewPSO)),"create preview PSO");
    }
};
