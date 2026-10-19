#include "../Resources/VXGIMeshSDFGPUData.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
    struct Vertex
    {
        DirectX::XMFLOAT3 Position={},Normal={0,0,1};DirectX::XMFLOAT2 TexCoords={};
        DirectX::XMFLOAT3 Tangent={1,0,0},Bitangent={0,1,0};
    };
    struct Mesh
    {
        std::vector<Vertex> vertices;std::vector<std::vector<unsigned>> lodIndices={{}};
        uint32_t materialID=7;bool isUnlit=false;
    };
    struct Model { std::vector<Mesh> meshes; };
    struct Instance
    {
        Model* pModel=nullptr;uint32_t editorId=0,customMaterialID=UINT32_MAX;
        DirectX::XMFLOAT3 translation={},rotation={},scale={1,1,1};
        bool isTransparent=false,isCutout=false;
    };
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    Model Plane(float z,uint32_t material)
    {
        Mesh mesh;mesh.materialID=material;
        mesh.vertices={{{-1,-1,z}},{{1,-1,z}},{{1,1,z}},{{-1,1,z}}};
        mesh.lodIndices[0]={0,1,2,0,2,3};return {{std::move(mesh)}};
    }
    template<class T> bool BytesEqual(const std::vector<T>& a,const std::vector<T>& b)
    {
        return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0);
    }
    template<class T> void CheckSlice(const std::vector<T>& packed,size_t first,const std::vector<T>& local)
    {
        Check(first+local.size()<=packed.size(),"Packed slice extends beyond its buffer.");
        Check(std::equal(local.begin(),local.end(),packed.begin()+first),"Asset-local buffer values were rebased or changed.");
    }
}

int main()
{
    try
    {
        using Data=VXGIMeshSDFGPUData;
        auto first=Plane(0,17),second=Plane(2,31);
        // A second mesh checks the per-instance mesh-to-material map.
        first.meshes.push_back(Plane(1,19).meshes.front());
        std::vector<Instance> sources={{&first,1},{&first,2},{&second,3}};
        sources[1].translation={3,4,5};sources[1].scale={-2,3,0.5f};sources[1].isCutout=true;
        VXGIMeshSDFCache cache;VXGIMeshSDFScene scene;
        Check(scene.UpdateInstances(sources,cache,{0.5f,128}),"Initial scene construction failed.");
        const auto owners=Data::CollectAssets(scene);Check(owners.size()==2,"Shared instances duplicated an immutable asset.");
        const auto accelerated=Data::BuildAssets(owners),linear=Data::BuildAssets(owners,false);
        Check(accelerated.assets.size()==2 && accelerated.triangles.size()==6,"Unexpected packed asset/triangle counts.");
        for(size_t index=0;index<owners.size();++index)
        {
            const auto& sdf=owners[index]->GetSDF();const auto& asset=accelerated.assets[index];
            Check(asset.assetCount==owners.size(),"Root SRV asset count was not packed explicitly.");
            CheckSlice(accelerated.nodes,asset.nodeOffset,sdf.GetNodes());
            CheckSlice(accelerated.cellOffsets,asset.cellOffset,sdf.GetGPUCellOffsets());
            CheckSlice(accelerated.references,asset.referenceOffset,sdf.GetGPUTraceReferences());
            CheckSlice(linear.cellOffsets,linear.assets[index].cellOffset,sdf.GetCellOffsets());
            CheckSlice(linear.references,linear.assets[index].referenceOffset,sdf.GetTriangleReferences());
            for(size_t triangle=0;triangle<sdf.GetTriangles().size();++triangle)
            {
                const auto& target=accelerated.triangles[asset.triangleOffset+triangle];
                Check(target.triangleId==triangle && target.pad0==sdf.GetTriangles()[triangle].meshIndex,
                    "Local triangle identity or source mesh identity was lost.");
                Check(target.materialId==UINT32_MAX && target.flags==0,"Instance material/cutout state contaminated an asset.");
            }
        }
        const auto instances=Data::BuildInstances(scene,owners);
        Check(instances.instances.size()==3 && instances.materialIds==std::vector<uint32_t>({17,19,17,19,31}),
            "Instance material maps were not concatenated by source mesh.");
        Check(instances.instances[0].assetIndex==0 && instances.instances[1].assetIndex==0 && instances.instances[2].assetIndex==1,
            "Instance asset indices do not preserve sharing.");
        Check(instances.instances[1].materialOffset==2 && instances.instances[1].materialCount==2 && instances.instances[1].flags==1,
            "Material range or cutout instance metadata is incorrect.");
        for(const auto& instance:instances.instances)
            Check(instance.sceneInstanceCount==3,"Root SRV scene instance count was not packed explicitly.");
        const auto acceleration=Data::BuildInstanceAcceleration(scene);
        Check(acceleration.nodeCount==5 && acceleration.nodes.size()==acceleration.nodeCount &&
            acceleration.revision==scene.GetInstanceBVH().GetRevision() &&
            BytesEqual(acceleration.nodes,scene.GetInstanceBVH().GetNodes()),
            "Instance BVH packing changed node layout, leaf indices, escape indices, bounds or explicit count.");
        Check(scene.UpdateInstances(sources,cache,{0.5f,128}),"Static scene update failed.");
        const auto accelerationStatic=Data::BuildInstanceAcceleration(scene);
        Check(accelerationStatic.revision==acceleration.revision && BytesEqual(accelerationStatic.nodes,acceleration.nodes),
            "Static scene invalidated the independent BVH upload revision.");
        sources[0].customMaterialID=77;
        Check(scene.UpdateInstances(sources,cache,{0.5f,128}),"Material-only scene update failed.");
        const auto accelerationMaterial=Data::BuildInstanceAcceleration(scene);
        Check(accelerationMaterial.revision==acceleration.revision && BytesEqual(accelerationMaterial.nodes,acceleration.nodes),
            "Material-only edits invalidated the independent BVH upload revision.");
        const auto envelope=Data::MakeEnvelope(scene,1.0f);
        Check(envelope.valid && envelope.cellDimensions==std::array<uint32_t,3>{1,1,1},"Bounds envelope is invalid.");
        for(const auto& instance:scene.GetInstances())
            Check(envelope.origin.x<=instance.boundsMinimum.x && envelope.origin.y<=instance.boundsMinimum.y && envelope.origin.z<=instance.boundsMinimum.z &&
                envelope.boundsMaximum.x>=instance.boundsMaximum.x && envelope.boundsMaximum.y>=instance.boundsMaximum.y && envelope.boundsMaximum.z>=instance.boundsMaximum.z,
                "Bounds envelope excludes an instance.");
        sources[1].translation.x+=20;sources[0].customMaterialID=99;
        const auto bakes=cache.GetStats().builds;
        Check(scene.UpdateInstances(sources,cache,{0.5f,128}),"Metadata-only update failed.");
        const auto ownersAfter=Data::CollectAssets(scene);
        const auto packedAfter=Data::BuildAssets(ownersAfter);
        const auto instancesAfter=Data::BuildInstances(scene,ownersAfter);
        const auto accelerationAfter=Data::BuildInstanceAcceleration(scene);
        Check(cache.GetStats().builds==bakes && owners==ownersAfter,"Moving an instance rebaked/replaced an asset.");
        Check(BytesEqual(accelerated.nodes,packedAfter.nodes) && BytesEqual(accelerated.cellOffsets,packedAfter.cellOffsets) &&
            BytesEqual(accelerated.references,packedAfter.references) && BytesEqual(accelerated.triangles,packedAfter.triangles) &&
            BytesEqual(accelerated.assets,packedAfter.assets),"Moving an instance changed immutable GPU asset bytes.");
        Check(!BytesEqual(instances.instances,instancesAfter.instances) && instancesAfter.materialIds[0]==99 && instancesAfter.materialIds[1]==99,
            "Moving/material edits did not update instance metadata/material maps.");
        Check(accelerationAfter.revision==acceleration.revision+1 && accelerationAfter.nodeCount==acceleration.nodeCount &&
            !BytesEqual(accelerationAfter.nodes,acceleration.nodes) &&
            BytesEqual(accelerationAfter.nodes,scene.GetInstanceBVH().GetNodes()) && scene.GetInstanceBVH().GetStats().lastUpdateRefit,
            "TRS refit did not update the GPU BVH bounds and separate revision.");
        sources.pop_back();
        Check(scene.UpdateInstances(sources,cache,{0.5f,128}),"Instance removal failed.");
        const auto accelerationRemoved=Data::BuildInstanceAcceleration(scene);
        Check(accelerationRemoved.revision==accelerationAfter.revision+1 && accelerationRemoved.nodeCount==3 &&
            accelerationRemoved.nodes.size()==3 && scene.GetInstanceBVH().GetStats().lastUpdateRebuilt,
            "Instance removal did not rebuild the GPU BVH membership and count.");
        VXGIMeshSDFScene empty;
        const auto emptyAcceleration=Data::BuildInstanceAcceleration(empty);
        Check(Data::CollectAssets(empty).empty() && Data::BuildAssets({}).assets.empty() &&
            Data::BuildInstances(empty,{}).instances.empty() && !Data::MakeEnvelope(empty,1.0f).valid &&
            emptyAcceleration.nodeCount==0 && emptyAcceleration.nodes.size()==1 &&
            emptyAcceleration.revision==empty.GetInstanceBVH().GetRevision(),
            "Empty scene packing is not safe.");
        std::cout<<"Mesh SDF GPU data packing: sharing, local offsets, materials, transforms, bounds, independent BVH revisions/refit/rebuild, empty padding passed.\n";
        return 0;
    }
    catch(const std::exception& exception) { std::cerr<<exception.what()<<'\n';return 1; }
}
