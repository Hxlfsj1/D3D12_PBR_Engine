#pragma once

#include "RenderDevice.h"
#include "VXGIWorld.h"
#include "VXGISDF.h"
#include "VXGIMeshSDFGPUData.h"
#include <array>
#include <cstring>
#include <vector>

class VXGIGPUScene
{
public:
    using GpuTriangle = VXGIMeshSDFGPUData::Triangle;
    using GpuAsset = VXGIMeshSDFGPUData::Asset;
    using GpuInstance = VXGIMeshSDFGPUData::Instance;
    using GpuInstanceBVHNode = VXGIMeshSDFGPUData::InstanceBVHNode;
    struct MeshUploadStats
    {
        uint64_t assetUploads=0,instanceUploads=0,materialMapUploads=0,instanceBVHUploads=0;
        uint64_t assetBytes=0,instanceBytes=0,materialMapBytes=0,instanceBVHBytes=0;
    };
    static_assert(sizeof(GpuTriangle)==208);
    static constexpr D3D12_RESOURCE_STATES ReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    void Initialize(UINT frameCount,bool useAcceleration=true)
    {
        m_frames.resize(frameCount);
        if(m_useAcceleration!=useAcceleration)
            for(auto& frame:m_frames) { frame.geometryRevision=UINT64_MAX;frame.assetOwners.clear(); }
        m_useAcceleration=useAcceleration;
    }
    bool Prepare(RenderDevice& device,int index,const VXGIWorld& world,const VXGISDF& sdf)
    {
        if(index<0 || size_t(index)>=m_frames.size()) return false;
        auto& frame=m_frames[index];
        const auto completed=device.GetFence(index)->GetCompletedValue();
        if(completed==UINT64_MAX || completed<device.GetFenceValue(index)) return false;
        for(auto& buffer:frame.buffers) buffer.staging.Reset();
        if(sdf.GetTriangles().empty()) { frame=Frame{}; return true; }
        const bool geometry=frame.meshBackend || frame.sourceWorld!=&world || frame.sourceSDF!=&sdf ||
            frame.geometryRevision!=sdf.GetGeometryRevision() || !frame.buffers[0].resource;
        const bool material=geometry || frame.materialRevision!=sdf.GetMaterialRevision();
        if(!geometry && !material) return true;
        std::vector<GpuTriangle> triangles;
        if(material)
        {
            triangles.resize(sdf.GetTriangles().size());
            for(size_t i=0;i<triangles.size();++i)
            {
                const auto& source=sdf.GetTriangles()[i]; auto& target=triangles[i];
                target.p0=source.positions[0];target.p1=source.positions[1];target.p2=source.positions[2];
                target.triangleId=static_cast<uint32_t>(i);target.flags=source.isCutout?1u:0u;
                target.geometricNormal=source.geometricNormal;target.uv=source.uv;
                target.shadingNormals=source.shadingNormals;target.tangents=source.tangents;target.bitangents=source.bitangents;
                target.materialId=source.materialId;target.instanceId=source.instanceId;
            }
        }
        std::array<Buffer,8> next;
        if(geometry && (!Upload(device.GetDevice(),sdf.GetNodes(),L"VXGI SDF nodes",next[0]) ||
            !Upload(device.GetDevice(),m_useAcceleration?sdf.GetGPUCellOffsets():sdf.GetCellOffsets(),L"VXGI SDF offsets",next[1]) ||
            !Upload(device.GetDevice(),m_useAcceleration?sdf.GetGPUTraceReferences():sdf.GetTriangleReferences(),L"VXGI triangle references",next[2]))) return false;
        if(material && !Upload(device.GetDevice(),triangles,L"VXGI world triangles",next[3])) return false;
        auto* cmd=device.GetCommandList();
        for(size_t i=0;i<next.size();++i)
        {
            auto& buffer=next[i]; if(!buffer.resource) continue;
            const auto copyBarrier=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1,&copyBarrier);
            cmd->CopyBufferRegion(buffer.resource.Get(),0,buffer.staging.Get(),0,buffer.bytes);
            const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState);
            cmd->ResourceBarrier(1,&barrier);frame.buffers[i]=std::move(buffer);
        }
        frame.geometryRevision=sdf.GetGeometryRevision();frame.materialRevision=sdf.GetMaterialRevision();

        frame.sourceWorld=&world;frame.sourceSDF=&sdf;
        frame.meshBackend=false;frame.sourceMeshScene=nullptr;frame.assetOwners.clear();
        frame.instanceMetadata.clear();frame.materialIds.clear();
        for(size_t i=4;i<frame.buffers.size();++i) frame.buffers[i]=Buffer{};
        frame.distanceNodeCount=static_cast<uint32_t>(sdf.GetNodes().size());
        frame.instanceBVHNodeCount=0;frame.instanceBVHRevision=UINT64_MAX;
        frame.grid=sdf.GetGrid();frame.triangleCount=static_cast<uint32_t>(sdf.GetTriangles().size());
        return true;
    }
    bool Prepare(RenderDevice& device,int index,const VXGIWorld& world,const VXGIMeshSDFScene& scene)
    {
        if(index<0 || size_t(index)>=m_frames.size()) return false;
        auto& frame=m_frames[index];
        const auto completed=device.GetFence(index)->GetCompletedValue();
        if(completed==UINT64_MAX || completed<device.GetFenceValue(index)) return false;
        for(auto& buffer:frame.buffers) buffer.staging.Reset();
        if(scene.GetInstances().empty())
        {
            // Empty queries still bind a valid BVH root address. Its explicit
            // zero count prevents dereferencing this one-record padding.
            Buffer dummy=std::move(frame.buffers[7]);
            if(!dummy.resource)
            {
                if(!Upload(device.GetDevice(),std::vector<GpuInstanceBVHNode>(1),L"VXGI empty instance BVH",dummy)) return false;
                RecordUpload(device.GetCommandList(),dummy);
            }
            frame=Frame{};frame.buffers[7]=std::move(dummy);return true;
        }
        try
        {
            auto assets=VXGIMeshSDFGPUData::CollectAssets(scene);
            const bool geometry=!frame.meshBackend || assets!=frame.assetOwners || !frame.buffers[0].resource;
            const bool metadata=geometry || frame.sourceMeshScene!=&scene || frame.meshRevision!=scene.GetRevision();
            const bool instanceBVH=!frame.meshBackend || frame.sourceMeshScene!=&scene ||
                frame.instanceBVHRevision!=scene.GetInstanceBVH().GetRevision() || !frame.buffers[7].resource;
            if(!geometry && !metadata && !instanceBVH) return true;
            VXGIMeshSDFGPUData::Geometry packed;
            VXGIMeshSDFGPUData::Instances instanceData;
            VXGIMeshSDFGPUData::InstanceAcceleration acceleration;
            if(geometry) packed=VXGIMeshSDFGPUData::BuildAssets(assets,m_useAcceleration);
            if(metadata) instanceData=VXGIMeshSDFGPUData::BuildInstances(scene,assets);
            if(instanceBVH) acceleration=VXGIMeshSDFGPUData::BuildInstanceAcceleration(scene);
            const bool instances=metadata && (geometry || !SameBytes(instanceData.instances,frame.instanceMetadata) || !frame.buffers[5].resource);
            const bool materials=metadata && (geometry || instanceData.materialIds!=frame.materialIds || !frame.buffers[6].resource);
            std::array<Buffer,8> next;
            if(geometry && (!Upload(device.GetDevice(),packed.nodes,L"VXGI mesh SDF nodes",next[0]) ||
                !Upload(device.GetDevice(),packed.cellOffsets,L"VXGI mesh SDF offsets",next[1]) ||
                !Upload(device.GetDevice(),packed.references,L"VXGI mesh triangle references",next[2]) ||
                !Upload(device.GetDevice(),packed.triangles,L"VXGI local asset triangles",next[3]) ||
                !Upload(device.GetDevice(),packed.assets,L"VXGI mesh SDF assets",next[4]))) return false;
            if(instances && !Upload(device.GetDevice(),instanceData.instances,L"VXGI mesh SDF instances",next[5])) return false;
            if(materials && !Upload(device.GetDevice(),instanceData.materialIds,L"VXGI mesh material IDs",next[6])) return false;
            if(instanceBVH && !Upload(device.GetDevice(),acceleration.nodes,L"VXGI world instance BVH",next[7])) return false;
            auto* cmd=device.GetCommandList();
            for(size_t i=0;i<next.size();++i)
            {
                auto& buffer=next[i];if(!buffer.resource) continue;
                RecordUpload(cmd,buffer);frame.buffers[i]=std::move(buffer);
            }
            if(geometry)
            {
                ++frame.meshUploads.assetUploads;
                for(const size_t slot:std::array<size_t,5>{0,1,2,3,4}) frame.meshUploads.assetBytes+=frame.buffers[slot].bytes;
                frame.triangleCount=static_cast<uint32_t>(packed.triangles.size());
                frame.distanceNodeCount=static_cast<uint32_t>(packed.nodes.size());
                frame.assetOwners=std::move(assets);
            }
            if(instances) { ++frame.meshUploads.instanceUploads;frame.meshUploads.instanceBytes+=frame.buffers[5].bytes; }
            if(materials) { ++frame.meshUploads.materialMapUploads;frame.meshUploads.materialMapBytes+=frame.buffers[6].bytes; }
            if(instanceBVH)
            {
                ++frame.meshUploads.instanceBVHUploads;frame.meshUploads.instanceBVHBytes+=frame.buffers[7].bytes;
                frame.instanceBVHNodeCount=acceleration.nodeCount;frame.instanceBVHRevision=acceleration.revision;
            }
            if(metadata)
            {
                frame.instanceMetadata=std::move(instanceData.instances);frame.materialIds=std::move(instanceData.materialIds);
            }
            if(metadata) frame.grid=VXGIMeshSDFGPUData::MakeEnvelope(scene,world.GetVoxelSize());
            frame.meshBackend=true;frame.sourceWorld=&world;frame.sourceSDF=nullptr;frame.sourceMeshScene=&scene;
            frame.meshRevision=scene.GetRevision();
            frame.geometryRevision=UINT64_MAX;frame.materialRevision=UINT64_MAX;
            return true;
        }
        catch(const std::exception&) { return false; }
    }
    ID3D12Resource* Get(int index,UINT buffer) const { return m_frames.at(index).buffers.at(buffer).resource.Get(); }
    bool IsMeshBackend(int index) const { return m_frames.at(index).meshBackend; }
    uint32_t GetInstanceCount(int index) const { return static_cast<uint32_t>(m_frames.at(index).instanceMetadata.size()); }
    uint32_t GetAssetCount(int index) const { return static_cast<uint32_t>(m_frames.at(index).assetOwners.size()); }
    uint32_t GetDistanceNodeCount(int index) const { return m_frames.at(index).distanceNodeCount; }
    uint32_t GetInstanceBVHNodeCount(int index) const { return m_frames.at(index).instanceBVHNodeCount; }
    uint64_t GetInstanceBVHRevision(int index) const { return m_frames.at(index).instanceBVHRevision; }
    uint32_t GetTriangleCount(int index) const { return m_frames.at(index).triangleCount; }
    uint64_t GetMeshRevision(int index) const { return m_frames.at(index).meshRevision; }
    const VXGISDF::Grid& GetGrid(int index) const { return m_frames.at(index).grid; }
    const MeshUploadStats& GetMeshUploadStats(int index) const { return m_frames.at(index).meshUploads; }
    uint64_t GetGPUMemoryBytes() const
    {
        uint64_t bytes=0;for(const auto& frame:m_frames) for(const auto& buffer:frame.buffers)
            bytes+=buffer.bytes*(buffer.staging?2u:1u);return bytes;
    }
private:
    struct Buffer { Microsoft::WRL::ComPtr<ID3D12Resource> resource,staging;uint64_t bytes=0; };
    struct Frame { std::array<Buffer,8> buffers;uint64_t geometryRevision=UINT64_MAX,materialRevision=UINT64_MAX;
        const VXGIWorld* sourceWorld=nullptr;const VXGISDF* sourceSDF=nullptr;
        const VXGIMeshSDFScene* sourceMeshScene=nullptr;uint64_t meshRevision=UINT64_MAX,instanceBVHRevision=UINT64_MAX;
        bool meshBackend=false;uint32_t triangleCount=0,distanceNodeCount=0,instanceBVHNodeCount=0;VXGISDF::Grid grid;
        VXGIMeshSDFGPUData::AssetList assetOwners;
        std::vector<GpuInstance> instanceMetadata;std::vector<uint32_t> materialIds;MeshUploadStats meshUploads; };
    template<class T> static bool SameBytes(const std::vector<T>& a,const std::vector<T>& b)
    {
        return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0);
    }
    static void RecordUpload(ID3D12GraphicsCommandList* cmd,const Buffer& buffer)
    {
        const auto before=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1,&before);cmd->CopyBufferRegion(buffer.resource.Get(),0,buffer.staging.Get(),0,buffer.bytes);
        const auto after=CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ReadState);
        cmd->ResourceBarrier(1,&after);
    }
    template<class T> static bool Upload(ID3D12Device* device,const std::vector<T>& values,const wchar_t* name,Buffer& result)
    {
        if(values.empty()) return false;
        result.bytes=uint64_t(values.size())*sizeof(T);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(result.bytes);
        const auto gpu=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),cpu=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        if(FAILED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result.resource))) ||
            FAILED(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result.staging)))) return false;
        void* mapped=nullptr;const D3D12_RANGE noReads{0,0};
        if(FAILED(result.staging->Map(0,&noReads,&mapped))) return false;
        std::memcpy(mapped,values.data(),size_t(result.bytes));const D3D12_RANGE writes{0,SIZE_T(result.bytes)};
        result.staging->Unmap(0,&writes);result.resource->SetName(name);return true;
    }
    std::vector<Frame> m_frames;
    bool m_useAcceleration=true;
};
