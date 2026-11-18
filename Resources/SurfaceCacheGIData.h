#pragma once

#include "SurfaceCacheLightingPass.h"

// Frame-slot bindings for SSGI. Emitters are physical instance triangles, so
// overlapping Card captures never multiply the light-sampling density.
class SurfaceCacheGIData
{
public:
    struct Constants
    {
        DirectX::XMFLOAT3 gridOrigin;float cellSize;
        std::array<uint32_t,3> cellDimensions;float meshDistance;
        DirectX::XMFLOAT3 boundsMaximum;float traceDistance;
        std::array<uint32_t,4> atlasIndices;
        uint32_t lightingIndex,globalIndex,bvhNodeCount,globalEnabled;
        float rayBias;uint32_t materialCount,emitterCount,lumen;
        uint32_t feedbackIndex,padding0,padding1,padding2;
        VXGIGlobalSDF::Constants global;
    };
    static_assert(sizeof(Constants)==288 && offsetof(Constants,global)==112);
    struct Bindings
    {
        D3D12_GPU_VIRTUAL_ADDRESS constants=0;
        std::array<D3D12_GPU_VIRTUAL_ADDRESS,12> buffers{};
        std::array<ID3D12Resource*,6> textures{}; // material 4, lighting, optional Global SDF
        uint64_t sourceStamp=0;
        uint32_t emitterCount=0;
        bool globalEnabled=false;
        ID3D12Resource* feedback=nullptr;
        void Bind(ID3D12GraphicsCommandList* cmd)const
        {
            cmd->SetComputeRootConstantBufferView(1,constants);
            for(UINT i=0;i<buffers.size();++i)cmd->SetComputeRootShaderResourceView(i+2,buffers[i]);
        }
    };
    void Initialize(UINT frameCount){m_frames.resize(frameCount);}
    uint32_t GetEmitterCount()const{return m_emitterCount;}
    bool Prepare(RenderDevice& device,ResourceManager& resources,const WorldGeometryRuntime& world,
        SurfaceCacheCapturePass& atlas,const SurfaceCacheLightingPass& light,int slot,const VXGIConfig& config,const SSGIConfig& ssgi,Bindings& result)
    {
        auto& frame=m_frames[slot];const auto& gpu=world.GetGPUScene();
        SurfaceCacheCapturePass::QueryBindings capture;
        if(!atlas.PrepareQuery(device,resources,slot,gpu.GetInstanceCount(slot),capture,true))return false;
        Constants value{};const auto& grid=gpu.GetGrid(slot);
        value.gridOrigin=grid.origin;value.cellSize=grid.cellSize;value.cellDimensions=grid.cellDimensions;value.boundsMaximum=grid.boundsMaximum;
        value.meshDistance=config.globalSDFEnabled?config.meshTraceDistance:config.globalSDFTraceDistance;
        value.lumen=ssgi.lumen?1u:0u;
        value.traceDistance=ssgi.lumen?config.globalSDFTraceDistance:ssgi.screenTraceDistance;value.atlasIndices=capture.atlasIndices;
        value.bvhNodeCount=gpu.GetInstanceBVHNodeCount(slot);value.globalEnabled=config.globalSDFEnabled&&ssgi.lumen?1u:0u;
        value.rayBias=config.surfaceRayMinDistance;value.materialCount=uint32_t(resources.GetGlobalMaterials().size());
        value.feedbackIndex=atlas.GetFeedback().GetDescriptor(slot);result.feedback=atlas.GetFeedback().GetBuffer(slot);
        if(!light.CopySRV(device,resources,value.lightingIndex))return false;
        const auto& global=world.GetGlobalSDF().GetPass();value.global=global.GetLayout().constants;
        if(value.globalEnabled!=0)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE target{};
            if(!resources.AllocateTransientSrvUavDescriptor(&value.globalIndex,&target))return false;
            device.GetDevice()->CopyDescriptorsSimple(1,target,global.GetSRVCPUHandle(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        std::vector<std::array<uint32_t,2>> emitters;
        const auto assets=VXGIMeshSDFGPUData::CollectAssets(world.GetScene());
        std::vector<uint32_t> offsets;uint32_t offset=0;
        for(const auto& asset:assets){offsets.push_back(offset);offset+=uint32_t(asset->GetSDF().GetTriangles().size());}
        const auto& instances=world.GetScene().GetInstances();const auto& materials=resources.GetGlobalMaterials();
        for(uint32_t i=0;ssgi.lumen && i<instances.size();++i)
        {
            const auto& instance=instances[i];const auto asset=std::find(assets.begin(),assets.end(),instance.asset)-assets.begin();
            const auto& triangles=instance.asset->GetSDF().GetTriangles();
            for(uint32_t t=0;t<triangles.size();++t)
            {
                const auto& material=materials[instance.materialIds[triangles[t].meshIndex]];
                if(!material.isUnlit && (material.emissiveFactor.x>0||material.emissiveFactor.y>0||material.emissiveFactor.z>0))
                    emitters.push_back({i,offsets[asset]+t});
            }
        }
        value.emitterCount=uint32_t(emitters.size());m_emitterCount=value.emitterCount;if(emitters.empty())emitters.push_back({0,0});
        if(!Upload(device.GetDevice(),frame.emitters,emitters.data(),emitters.size()*sizeof(emitters[0])) ||
            !Upload(device.GetDevice(),frame.constants,&value,sizeof(value),512))return false;
        for(UINT i=0;i<4;++i)result.buffers[i]=gpu.Get(slot,i)?gpu.Get(slot,i)->GetGPUVirtualAddress():0;
        result.buffers[4]=resources.GetMaterialBufferGPUAddress();
        for(UINT i=0;i<4;++i)result.buffers[i+5]=gpu.Get(slot,i+4)?gpu.Get(slot,i+4)->GetGPUVirtualAddress():0;
        result.buffers[9]=capture.cards;result.buffers[10]=capture.ranges;result.buffers[11]=frame.emitters.resource->GetGPUVirtualAddress();
        result.constants=frame.constants.resource->GetGPUVirtualAddress();
        for(UINT i=0;i<4;++i)result.textures[i]=atlas.GetTexture(i);
        result.textures[4]=light.GetTexture();result.textures[5]=global.GetTexture();
        result.emitterCount=value.emitterCount;result.globalEnabled=value.globalEnabled!=0;
        uint64_t stamp=14695981039346656037ull;
        const auto append=[&](const auto& x){const auto* bytes=reinterpret_cast<const uint8_t*>(&x);
            for(size_t i=0;i<sizeof(x);++i)stamp=(stamp^bytes[i])*1099511628211ull;};
        append(light.GetContentStamp());append(value.lumen);
        // Keep screen history across cache reallocations and ordinary updates.
        // World tracing parameters affect the estimator only in Lumen mode;
        // disabled Global SDF settings are not part of its active source either.
        if(value.lumen!=0)
        {
            append(value.meshDistance);append(value.traceDistance);append(value.globalEnabled);
            if(value.globalEnabled!=0)
            {append(config.globalSDFCellSize);append(config.globalSDFCells);append(config.globalSDFClipmaps);}
        }
        result.sourceStamp=stamp;return true;
    }
private:
    struct Buffer{Microsoft::WRL::ComPtr<ID3D12Resource> resource;uint64_t bytes=0;};
    struct Frame{Buffer constants,emitters;};std::vector<Frame> m_frames;
    uint32_t m_emitterCount=0;
    static bool Upload(ID3D12Device* device,Buffer& buffer,const void* data,uint64_t bytes,uint64_t allocation=0)
    {
        const uint64_t capacity=allocation?allocation:bytes;
        if(buffer.bytes!=capacity)
        {
            const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);const auto desc=CD3DX12_RESOURCE_DESC::Buffer(capacity);
            const HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,IID_PPV_ARGS(buffer.resource.ReleaseAndGetAddressOf()));
            if(FAILED(hr)){ErrorLog::HRESULT("Surface Cache GI upload",hr);return false;}buffer.bytes=capacity;
        }
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};const auto hr=buffer.resource->Map(0,&noReads,&mapped);
        if(FAILED(hr)){ErrorLog::HRESULT("Surface Cache GI map",hr);return false;}
        std::memcpy(mapped,data,bytes);buffer.resource->Unmap(0,nullptr);return true;
    }
};
