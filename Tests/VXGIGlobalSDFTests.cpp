#include "Resources/VXGIGlobalSDF.h"
#include "ThirdParty/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using DirectX::XMFLOAT3;
using Global = VXGIGlobalSDF;
using Json = nlohmann::json;
struct Vertex { XMFLOAT3 Position; };
struct Mesh { std::vector<Vertex> vertices; std::vector<std::vector<uint32_t>> lodIndices = {{}}; uint32_t materialID = 0; };
struct Model { std::vector<Mesh> meshes; };
struct Instance
{
    uint32_t editorId = 0; Model* pModel = nullptr;
    XMFLOAT3 translation = {}, rotation = {}, scale = {1,1,1};
    uint32_t customMaterialID = UINT32_MAX; bool isTransparent = false, isCutout = false;
};
void Require(bool condition, const std::string& error) { if(!condition) throw std::runtime_error(error); }
Model Plane()
{
    Mesh mesh;
    mesh.vertices={{{-1,-1,0}},{{1,-1,0}},{{1,1,0}},{{-1,1,0}}};
    mesh.lodIndices[0]={0,1,2,0,2,3};
    return {{std::move(mesh)}};
}
XMFLOAT3 Transform(XMFLOAT3 p,const DirectX::XMFLOAT4X4& matrix)
{
    XMFLOAT3 output;
    DirectX::XMStoreFloat3(&output,DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&p),DirectX::XMLoadFloat4x4(&matrix)));
    return output;
}
float RectangleDistance(XMFLOAT3 p,const VXGIMeshSDFGPUData::Instance& instance)
{
    const auto local=Transform(p,instance.localFromWorld);
    const XMFLOAT3 closestLocal={(std::max)(-1.f,(std::min)(1.f,local.x)),(std::max)(-1.f,(std::min)(1.f,local.y)),0};
    const auto closest=Transform(closestLocal,instance.worldFromLocal);
    return std::hypot(p.x-closest.x,p.y-closest.y,p.z-closest.z);
}
int main(int argc,char**argv)
{
    try
    {
        Json report;
        Global::Layout layout; std::string error;
        Require(Global::BuildLayout({.24f,.49f,-.01f},{},0,0,0,layout,&error),error);
        Require(sizeof(Global::Constants)==176&&sizeof(Global::Clipmap)==48,"constant ABI");
        Require(layout.width==65&&layout.height==65&&layout.depth==195&&layout.totalNodeCount==823875,"default texture layout");
        Require(layout.constants.clipmaps[0].origin.x==-16&&layout.constants.clipmaps[0].origin.y==-16&&layout.constants.clipmaps[0].origin.z==-16.5f,"negative center must floor to grid");
        for(uint32_t layer=0;layer<3;++layer)
        {
            const auto& grid=layout.constants.clipmaps[layer];
            Require(grid.cellSize==std::ldexp(.5f,int(layer))&&grid.nodeOffsetZ==65*layer,"clipmap scale and Z offset");
            const auto maximum=Global::NodePosition(grid,64,64,64);
            Require(maximum.x==grid.boundsMaximum.x&&maximum.y==grid.boundsMaximum.y&&maximum.z==grid.boundsMaximum.z,"boundary nodes must match bounds");
            if(layer) Require(grid.origin.x<=layout.constants.clipmaps[layer-1].origin.x&&grid.boundsMaximum.x>=layout.constants.clipmaps[layer-1].boundsMaximum.x,"layers must nest");
        }
        report["default_layout"]={{"nodes",layout.totalNodeCount},{"bytes_r32_float",layout.totalNodeCount*4},{"resolution",65},{"depth",195},{"maximum_distance",layout.maximumDistance}};
        Global::Settings compactSettings;
        compactSettings.baseCellSize=.25f;compactSettings.layerCellsPerAxis={4,8,8};
        Require(Global::BuildLayout({-.01f,.49f,-.26f},compactSettings,0,0,0,layout,&error),error);
        Require(layout.width==9&&layout.height==9&&layout.depth==23&&layout.totalNodeCount==1863,
            "mixed layer resolutions must use maximum XY resolution and packed Z slices");
        Require(layout.constants.clipmapResolutions==std::array<uint32_t,3>{5,9,9},"mixed node resolutions");
        std::vector<bool> addressed(size_t(layout.totalNodeCount),false);uint64_t activeNodes=0;
        const uint32_t expectedOffsets[]={0,5,14};const float expectedSpans[]={1,4,8};
        for(uint32_t layer=0;layer<3;++layer)
        {
            auto& grid=layout.constants.clipmaps[layer];const uint32_t n=Global::Resolution(layout.constants,layer);
            Require(grid.nodeOffsetZ==expectedOffsets[layer]&&grid.boundsMaximum.x-grid.origin.x==expectedSpans[layer],
                "requested layer spans and compact Z offsets");
            Require(grid.cellSize==std::ldexp(.25f,int(layer)),"requested layer spacing");
            const auto maximum=Global::NodePosition(grid,n-1,n-1,n-1);
            Require(maximum.x==grid.boundsMaximum.x&&maximum.y==grid.boundsMaximum.y&&maximum.z==grid.boundsMaximum.z,
                "each layer must own its boundary node");
            grid.scrollOffset={n-1,n-2,n-3};
            for(uint32_t z=0;z<n;++z)for(uint32_t y=0;y<n;++y)for(uint32_t x=0;x<n;++x)
            {
                const size_t index=Global::PhysicalNodeIndex(layout,layer,x,y,z);
                Require(index<addressed.size()&&!addressed[index],"mixed ring addresses must not overlap layers or padding");
                addressed[index]=true;++activeNodes;
            }
            Require(Global::PhysicalNodeIndex(layout,layer,1,2,3)==size_t(9)*9*grid.nodeOffsetZ,
                "ring modulo must use this layer's node resolution, not atlas width");
        }
        Require(activeNodes==1583,"mixed lattice active node count excludes XY padding");
        report["compact_1_4_8_layout"]={{"shape",{layout.width,layout.height,layout.depth}},
            {"layer_nodes",layout.constants.clipmapResolutions},{"spans",{1,4,8}},{"cell_sizes",{.25,.5,1}},
            {"active_nodes",activeNodes},{"allocated_nodes",layout.totalNodeCount}};
        auto badLayers=compactSettings;badLayers.layerCellsPerAxis[1]=3;
        Require(!Global::BuildLayout({},badLayers,0,0,0,layout,&error),"reject odd per-layer cell count");
        badLayers=compactSettings;badLayers.layerCellsPerAxis={16,4,8};
        Require(!Global::BuildLayout({},badLayers,0,0,0,layout,&error),"reject an outer layer smaller than its inner layer");
        Global::Settings bad; bad.cellsPerAxis=257;
        Require(!Global::BuildLayout({},bad,0,0,0,layout,&error)&&layout.width==0&&!error.empty(),"reject giant grid transactionally");
        bad={};bad.cellsPerAxis=1;Require(!Global::BuildLayout({},bad,0,0,0,layout,&error),"reject nonnesting one-cell grid");
        bad={};bad.cellsPerAxis=3;Require(!Global::BuildLayout({},bad,0,0,0,layout,&error),"reject odd-cell grid");
        bad={};bad.clipmapCount=4;Require(!Global::BuildLayout({},bad,0,0,0,layout,&error),"reject fourth layer");
        bad={};bad.baseCellSize=0;Require(!Global::BuildLayout({},bad,0,0,0,layout,&error),"reject zero size");
        Require(!Global::BuildLayout({std::numeric_limits<float>::infinity(),0,0},{},0,0,0,layout,&error),"reject nonfinite center");
        Require(!Global::BuildLayout({1e20f,0,0},{},0,0,0,layout,&error),"reject unrepresentable world spacing");
        Require(Global::BuildLayout({}, {},0,0,0,layout,&error),error);
        Require(Global::ValidateSources({}, {}, &error),error);
        Require(Global::ComposeReferenceNode({}, {}, {},layout.maximumDistance)==layout.maximumDistance,"empty scene must return finite far");

        Model model=Plane();VXGIMeshSDFCache cache;VXGIMeshSDFScene scene;
        Instance original; original.editorId=1;original.pModel=&model;
        original.translation={3,-2,5};original.rotation={.23f,-.41f,.15f};original.scale={-2,.5f,3};
        Require(scene.UpdateInstances(std::vector<Instance>{original},cache,{.125f,0}),scene.GetLastError());
        auto owners=VXGIMeshSDFGPUData::CollectAssets(scene);
        auto geometry=VXGIMeshSDFGPUData::BuildAssets(owners);
        auto instances=VXGIMeshSDFGPUData::BuildInstances(scene,owners);
        Require(Global::ValidateSources(geometry,instances,&error),error);
        uint32_t comparisons=0,positive=0;float maximumExcess=0,maximumGap=0;
        for(int z=-12;z<=12;++z) for(int y=-12;y<=12;++y) for(int x=-12;x<=12;++x)
        {
            const XMFLOAT3 local={x*.19f,y*.17f,z*.13f};
            const auto world=Transform(local,instances.instances[0].worldFromLocal);
            const float exact=RectangleDistance(world,instances.instances[0]);
            const float value=Global::ComposeReferenceNode(geometry,instances,world,layout.maximumDistance);
            Require(std::isfinite(value)&&value>=0,"composition must remain finite/nonnegative");
            const float excess=value-exact;
            maximumExcess=(std::max)(maximumExcess,excess);maximumGap=(std::max)(maximumGap,exact-value);
            Require(excess<1e-4f,"negative nonuniform rotated transform must remain a lower bound");
            if(value>0)++positive;++comparisons;
        }
        Require(positive>comparisons/2,"bound cannot collapse to zero away from geometry");
        report["rotated_negative_nonuniform_rectangle"]={{"comparisons",comparisons},{"positive_bounds",positive},{"maximum_excess",maximumExcess},{"maximum_gap",maximumGap},{"scale",{-2,.5,3}},{"local_cell_size",.125}};

        std::vector<Instance> triple(3);
        for(uint32_t i=0;i<3;++i){triple[i].pModel=&model;triple[i].editorId=i+1;triple[i].translation.z=float(2+2*i);}
        Require(scene.UpdateInstances(triple,cache,{.125f,0}),scene.GetLastError());
        owners=VXGIMeshSDFGPUData::CollectAssets(scene);geometry=VXGIMeshSDFGPUData::BuildAssets(owners);instances=VXGIMeshSDFGPUData::BuildInstances(scene,owners);
        Require(Global::ValidateSources(geometry,instances,&error),error);
        const float third=Global::ComposeReferenceNode(geometry,instances,{0,0,6},layout.maximumDistance);
        auto firstTwo=instances;firstTwo.instances.resize(2);
        const float omitted=Global::ComposeReferenceNode(geometry,firstTwo,{0,0,6},layout.maximumDistance);
        Require(third==0&&omitted>1,"composition must include third model");
        report["all_instances"]={{"distance_with_third",third},{"distance_without_third",omitted},{"instance_count",3}};
        auto malformed=geometry;malformed.assets[0].nodeOffset=UINT32_MAX;
        Require(!Global::ValidateSources(malformed,instances,&error),"reject out-of-buffer grid");
        malformed=geometry;malformed.nodes[0]=std::numeric_limits<float>::quiet_NaN();
        Require(!Global::ValidateSources(malformed,instances,&error),"reject nonfinite source");
        auto shear=instances;shear.instances[0].worldFromLocal._12=.5f;
        Require(!Global::ValidateSources(geometry,shear,&error),"minimum basis scale is conservative only for TRS");
        report["passed"]=true;
        const std::string output=argc>1?argv[1]:"cpu-result.json";
        std::ofstream(output)<<report.dump(2)<<'\n';
        std::cout<<report.dump(2)<<'\n';return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
