// Run from a VS Developer shell. Optional --benchmark builds a 129792-triangle,
// 128^3-node fixture to measure the upper end of the initial bake budget.
#include "../Resources/VXGISDF.h"
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
    struct Resource { void* Get() const { return nullptr; } };
    struct Texture { uint32_t type=0; Resource Resource; std::string path; };
    struct Vertex
    {
        DirectX::XMFLOAT3 Position, Normal={0,1,0};
        DirectX::XMFLOAT2 TexCoords={};
        DirectX::XMFLOAT3 Tangent={1,0,0},Bitangent={0,0,1};
    };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices={{}};
        std::vector<Texture> textures;
        uint32_t materialID=7;
        bool isUnlit=false;
        DirectX::XMFLOAT4 baseColorFactor={0,0,0,1}; // Black still terminates rays.
        DirectX::XMFLOAT3 emissiveFactor={};
    };
    struct Model { std::vector<Mesh> meshes; };
    struct Instance
    {
        uint32_t editorId=41;
        Model* pModel=nullptr;
        DirectX::XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=false;
        int currentLodLevel=0;
    };
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    bool Near(double a,double b,double tolerance=1e-5) { return std::abs(a-b)<=tolerance; }
    void Quad(Mesh& mesh,DirectX::XMFLOAT3 a,DirectX::XMFLOAT3 b,DirectX::XMFLOAT3 c,DirectX::XMFLOAT3 d)
    {
        const auto first=static_cast<unsigned>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{a},{b},{c},{d}});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2,first,first+2,first+3});
    }
    Model Box(float minimum=-1,float maximum=1)
    {
        Mesh mesh; const float a=minimum,b=maximum;
        Quad(mesh,{a,a,b},{b,a,b},{b,b,b},{a,b,b});
        Quad(mesh,{a,a,a},{a,b,a},{b,b,a},{b,a,a});
        Quad(mesh,{b,a,a},{b,b,a},{b,b,b},{b,a,b});
        Quad(mesh,{a,a,a},{a,a,b},{a,b,b},{a,b,a});
        Quad(mesh,{a,b,a},{a,b,b},{b,b,b},{b,b,a});
        Quad(mesh,{a,a,a},{b,a,a},{b,a,b},{a,a,b});
        return {{std::move(mesh)}};
    }
    void Build(Model& model,VXGIWorld& world,VXGISDF& sdf,float size=0.5f)
    {
        std::vector<Instance> instances={{}}; instances[0].pModel=&model;
        Check(world.Update(instances,0.5f),world.GetLastError().c_str());
        Check(sdf.Update(world,size,128),sdf.GetLastError().c_str());
    }
    void Compare(const VXGISDF& sdf,DirectX::XMFLOAT3 origin,DirectX::XMFLOAT3 direction,float maxDistance=20)
    {
        const auto reference=sdf.TraceReference(origin,direction,maxDistance), actual=sdf.Trace(origin,direction,maxDistance);
        Check(actual.hit==reference.hit,"grid/reference hit mismatch");
        if(reference.hit)
        {
            Check(Near(actual.distance,reference.distance,1e-4),"grid/reference distance mismatch");
            Check(actual.materialId==reference.materialId && actual.instanceId==reference.instanceId,"surface identity mismatch");
            Check(Near(actual.barycentrics.x+actual.barycentrics.y+actual.barycentrics.z,1),"barycentrics do not sum to one");
            Check(actual.backFace==reference.backFace,"raw geometric normal back-face mismatch");
        }
    }
    void TestBoxDistancesAndTrace()
    {
        auto model=Box(); VXGIWorld world; VXGISDF sdf; Build(model,world,sdf);
        Check(Near(sdf.NearestDistance({0,0,0}),1),"inside unsigned box distance incorrect");
        Check(Near(sdf.NearestDistance({2,2,2}),std::sqrt(3.0)),"outside box distance incorrect");
        Check(Near(sdf.NearestDistance({0,0,-1}),0),"surface distance nonzero");
        const auto& grid=sdf.GetGrid(); const auto& dimensions=grid.nodeDimensions;
        for(uint32_t z=0;z<dimensions[2];++z) for(uint32_t y=0;y<dimensions[1];++y) for(uint32_t x=0;x<dimensions[0];++x)
        {
            const double px=grid.origin.x+double(x)*grid.cellSize,py=grid.origin.y+double(y)*grid.cellSize,pz=grid.origin.z+double(z)*grid.cellSize;
            const double dx=(std::max)(std::abs(px)-1,0.0),dy=(std::max)(std::abs(py)-1,0.0),dz=(std::max)(std::abs(pz)-1,0.0);
            const double analytical=dx+dy+dz>0 ? std::sqrt(dx*dx+dy*dy+dz*dz) : 1-(std::max)({std::abs(px),std::abs(py),std::abs(pz)});
            const float stored=sdf.GetNodes()[x+dimensions[0]*(y+dimensions[1]*z)];
            Check(stored<=analytical+1e-12 && Near(stored,analytical,sdf.GetStats().maximumNodePositionMargin+2e-6),
                "distance node overestimated or differs beyond conservative arithmetic margin");
        }
        const auto hit=sdf.Trace({0,0,-3},{0,0,1},20);
        Check(hit.hit && Near(hit.distance,2) && hit.materialId==7 && hit.instanceId==41,"black box hit identity incorrect");
        Check(Near(hit.geometricNormal.z,-1) && !hit.backFace,"front-face geometric normal flipped");
        const auto inside=sdf.Trace({0,0,0},{0,1,0},20);
        Check(inside.hit && Near(inside.distance,1) && inside.backFace && Near(inside.geometricNormal.y,1),"inside start unsigned trace failed");
        const auto surface=sdf.Trace({0,0,-1},{0,0,-1},20);
        Check(surface.hit && Near(surface.distance,0),"surface-origin trace did not terminate at zero");
        Compare(sdf,{0,0,-3},{0,0,1}); Compare(sdf,{-3,-3,-3},{1,1,1}); Compare(sdf,{3,3,3},{-1,-1,-1});
        Compare(sdf,{0,0,-1},{0,0,-1},0); Compare(sdf,{0,0,-3},{0,0,1},1.5f);
        Check(!sdf.Trace({3,3,3},{1,0,0},20).hit,"miss returned a hit");
        Check(!sdf.Trace({0,0,0},{0,0,0},20).hit && !sdf.TraceReference({0,0,0},{0,0,0},20).hit,"zero direction accepted");
        Check(sdf.GetCellOffsets().size()==sdf.GetStats().cells+1 && sdf.GetCellOffsets().back()==sdf.GetTriangleReferences().size(),"CSR size mismatch");
    }
    void TestOpenPlaneThinWallsAndCSR()
    {
        Mesh plane; Quad(plane,{-2,0,-2},{-2,0,2},{2,0,2},{2,0,-2});
        Model model={{plane}}; VXGIWorld world; VXGISDF sdf; Build(model,world,sdf,0.5f);
        Check(Near(sdf.NearestDistance({0,1,0}),1) && Near(sdf.NearestDistance({0,-1,0}),1),"open plane acquired an interior sign");
        Compare(sdf,{0,1,0},{0,-1,0}); Compare(sdf,{0,-1,0},{0,1,0}); Compare(sdf,{0,0,0},{0,1,0});
        const auto& grid=sdf.GetGrid();
        // All cells touching y=0 need both face triangles when their closed box
        // intersects the diagonal; nearest-triangle IDs alone cannot supply CSR.
        bool multiCandidate=false;
        for(size_t i=0;i+1<sdf.GetCellOffsets().size();++i)
            multiCandidate |= sdf.GetCellOffsets()[i+1]-sdf.GetCellOffsets()[i]>=2;
        Check(multiCandidate,"CSR omitted same-cell candidate triangles");
        for(uint32_t z=0;z<grid.cellDimensions[2];++z) for(uint32_t y=0;y<grid.cellDimensions[1];++y) for(uint32_t x=0;x<grid.cellDimensions[0];++x)
        {
            const DirectX::XMFLOAT3 center={grid.origin.x+(x+0.5f)*grid.cellSize,grid.origin.y+(y+0.5f)*grid.cellSize,grid.origin.z+(z+0.5f)*grid.cellSize};
            const float h=grid.cellSize*0.5f;
            const auto cell=x+grid.cellDimensions[0]*(y+grid.cellDimensions[1]*z);
            for(uint32_t triangle=0;triangle<sdf.GetTriangles().size();++triangle)
                if(VXGIWorld::TriangleIntersectsBox(sdf.GetTriangles()[triangle].positions,center,{h,h,h}))
                    Check(std::find(sdf.GetTriangleReferences().begin()+sdf.GetCellOffsets()[cell],
                        sdf.GetTriangleReferences().begin()+sdf.GetCellOffsets()[cell+1],triangle)!=sdf.GetTriangleReferences().begin()+sdf.GetCellOffsets()[cell+1],
                        "closed-cell CSR candidate omitted");
        }
        Mesh walls;
        Quad(walls,{0.03f,-1,-1},{0.03f,1,-1},{0.03f,1,1},{0.03f,-1,1});
        Quad(walls,{0.036f,-1,-1},{0.036f,-1,1},{0.036f,1,1},{0.036f,1,-1});
        model.meshes={walls}; Build(model,world,sdf,0.25f);
        Compare(sdf,{-2,0.13f,0.17f},{1,0,0}); Compare(sdf,{2,0.13f,0.17f},{-1,0,0});
        Check(Near(sdf.Trace({-2,0,0},{1,0,0},20).position.x,0.03f),"thin wall leaked through node distance grid");
        Compare(sdf,{0.033f,0,0},{1,0,0}); Compare(sdf,{0.03f,0,0},{-1,0,0});
    }
    void TestTransformsRandomReferenceAndReuse()
    {
        auto model=Box(); std::vector<Instance> instances={{}}; instances[0].pModel=&model;
        instances[0].scale={-2,0.5f,1.5f}; instances[0].rotation={0.1f,0.2f,0.3f}; instances[0].translation={0.25f,-0.13f,0.17f};
        instances[0].customMaterialID=99;
        VXGIWorld world; VXGISDF sdf;
        Check(world.Update(instances,0.5f) && sdf.Update(world,0.25f,128),"nonuniform mirror build failed");
        std::mt19937 generator(19); std::uniform_real_distribution<float> position(-4,4),direction(-1,1);
        for(size_t i=0;i<600;++i) Compare(sdf,{position(generator),position(generator),position(generator)},
            {direction(generator),direction(generator),direction(generator)},20);
        const auto revision=sdf.GetRevision(); const auto* nodes=sdf.GetNodes().data();
        Check(sdf.Update(world,0.25f,128) && sdf.GetRevision()==revision && sdf.GetNodes().data()==nodes,"unchanged world rebuilt SDF");
        Check(sdf.Update(world,0.3f,128) && sdf.GetRevision()==revision+1,"independent SDF resolution change missed");
        Check(Near(world.GetVoxelSize(),0.5f),"SDF resolution altered probe resolution");
        const auto successful=sdf.GetRevision();
        Check(!sdf.Update(world,0.01f,16) && sdf.GetRevision()==successful && !sdf.GetLastError().empty(),"resolution overflow mutated cache");
        instances[0].translation.x+=1; Check(world.Update(instances,0.5f),"world edit failed");
        Check(sdf.Update(world,0.25f,128) && sdf.GetRevision()==successful+1 && sdf.GetStats().worldRevision==world.GetRevision(),"world revision rebuild missed");
        for(size_t i=0;i<100;++i) Compare(sdf,{position(generator),position(generator),position(generator)},
            {direction(generator),direction(generator),direction(generator)},20);
        VXGIWorld empty; std::vector<Instance> noInstances; Check(empty.Update(noInstances,0.5f),"empty world build failed");
        VXGISDF emptySDF; Check(emptySDF.Update(empty,0.5f,128) && emptySDF.GetNodes().empty() &&
            !emptySDF.Trace({0,0,0},{0,1,0}).hit && !emptySDF.TraceReference({0,0,0},{0,1,0}).hit,"empty SDF failed");
    }
    void TestFarFractionalBoundariesAndDenseCSR()
    {
        auto model=Box(-0.45f,0.45f); std::vector<Instance> instances={{}}; instances[0].pModel=&model;
        instances[0].translation={10000.0f,-10000.0f,10000.0f};
        VXGIWorld world; VXGISDF sdf;
        Check(world.Update(instances,0.5f) && sdf.Update(world,0.03f,128),"far-origin fractional grid build failed");
        const auto& grid=sdf.GetGrid();
        for(uint32_t z=0;z<grid.nodeDimensions[2];z+=3) for(uint32_t y=0;y<grid.nodeDimensions[1];y+=3)
            for(uint32_t x=0;x<grid.nodeDimensions[0];x+=3)
            {
                // Reproduce non-fused shader float reconstruction. Stored values
                // must bound distances there, not only at ideal double positions.
                const DirectX::XMFLOAT3 position={grid.origin.x+float(x)*grid.cellSize,grid.origin.y+float(y)*grid.cellSize,
                    grid.origin.z+float(z)*grid.cellSize};
                const float stored=sdf.GetNodes()[x+grid.nodeDimensions[0]*(y+grid.nodeDimensions[1]*z)];
                Check(stored<=sdf.NearestDistance(position)+1e-12,"stored node distance unsafe at shader float node position");
            }
        Compare(sdf,{10002,-10000,10000},{-1,0,0}); Compare(sdf,{10000,-9998,10000},{0,-1,0});
        Compare(sdf,{10000,-10000,10002},{0,0,-1});
        const auto revision=sdf.GetRevision();
        Check(!sdf.Update(world,0.0003f,256) && sdf.GetRevision()==revision,"unsupported far-origin sub-ULP grid accepted");
        auto tinyFar=Box(-0.001f,0.001f); instances[0].pModel=&tinyFar;
        Check(world.Update(instances,0.5f),"tiny far-origin world fixture failed");
        Check(!sdf.Update(world,0.0003f,256) && sdf.GetLastError().find("precision budget")!=std::string::npos,
            "sub-ULP grid did not report its precision constraint independently of resolution budget");
        // Coordinate boundaries are exact binary numbers and traversed in both
        // signs, including simultaneous x/y/z exits and a cell's negative face.
        model=Box(); instances[0].pModel=&model; instances[0].translation={};
        world.Invalidate(); Check(world.Update(instances,0.5f) && sdf.Update(world,0.5f,128),"boundary fixture build failed");
        Compare(sdf,{1.5f,0.5f,0.5f},{-1,0,0}); Compare(sdf,{0.5f,1.5f,0.5f},{0,-1,0});
        Compare(sdf,{0.5f,0.5f,1.5f},{0,0,-1}); Compare(sdf,{1.5f,1.5f,1.5f},{-1,-1,-1});
        Compare(sdf,{-1.5f,-1.5f,-1.5f},{1,1,1}); Compare(sdf,{1,1,1},{-1,-1,-1});
        // Many distinct surfaces share every interior distance cell. CSR must
        // retain all intersecting triangles, not saturate or select one nearest.
        Mesh dense;
        for(uint32_t i=0;i<32;++i)
        {
            const float x=0.01f+float(i)*0.006f;
            Quad(dense,{x,-0.4f,-0.4f},{x,0.4f,-0.4f},{x,0.4f,0.4f},{x,-0.4f,0.4f});
        }
        model.meshes={dense}; world.Invalidate(); Check(world.Update(instances,0.5f) && sdf.Update(world,0.25f,128),"dense CSR build failed");
        uint32_t maximumCandidates=0;
        for(size_t i=0;i+1<sdf.GetCellOffsets().size();++i)
            maximumCandidates=(std::max)(maximumCandidates,sdf.GetCellOffsets()[i+1]-sdf.GetCellOffsets()[i]);
        Check(maximumCandidates==64,"dense cell candidate list saturated or discarded overlapping surfaces");
        Compare(sdf,{-1,0.1f,0.13f},{1,0,0}); Compare(sdf,{1,0.1f,0.13f},{-1,0,0});
        Check(Near(sdf.Trace({-1,0.1f,0.13f},{1,0,0}).position.x,0.01f),"dense CSR missed closest true surface");
    }
    Model SubdividedBox(uint32_t subdivisions)
    {
        Mesh mesh; constexpr float extent=7.8125f;
        for(uint32_t face=0;face<6;++face) for(uint32_t y=0;y<subdivisions;++y) for(uint32_t x=0;x<subdivisions;++x)
        {
            const float a=extent*x/subdivisions,b=extent*(x+1)/subdivisions,c=extent*y/subdivisions,d=extent*(y+1)/subdivisions;
            if(face==0) Quad(mesh,{a,c,0},{a,d,0},{b,d,0},{b,c,0});
            else if(face==1) Quad(mesh,{a,c,extent},{b,c,extent},{b,d,extent},{a,d,extent});
            else if(face==2) Quad(mesh,{0,a,c},{0,a,d},{0,b,d},{0,b,c});
            else if(face==3) Quad(mesh,{extent,a,c},{extent,b,c},{extent,b,d},{extent,a,d});
            else if(face==4) Quad(mesh,{a,0,c},{b,0,c},{b,0,d},{a,0,d});
            else Quad(mesh,{a,extent,c},{a,extent,d},{b,extent,d},{b,extent,c});
        }
        return {{std::move(mesh)}};
    }
    void Benchmark()
    {
        auto model=SubdividedBox(104); VXGIWorld world; VXGISDF sdf; Build(model,world,sdf,0.0625f);
        const auto& stats=sdf.GetStats();
        std::cout<<"SDF benchmark: "<<stats.triangles<<" triangles, "<<stats.nodes<<" nodes, "<<stats.occupiedCells<<" occupied cells, "
            <<stats.triangleReferences<<" references, "<<stats.buildMilliseconds<<" ms build ("<<stats.distanceBakeMilliseconds
            <<" ms distance bake), "<<stats.memoryBytes/(1024.0*1024.0)<<" MiB retained CPU.\n";
        Check(stats.nodes==128ull*128*128,"upper-budget fixture dimensions wrong");
        Compare(sdf,{-1,3.1f,2.2f},{1,0,0}); Compare(sdf,{3.1f,3.2f,3.3f},{1,0.3f,-0.4f});
    }
}
int main(int argc,char** argv)
{
    try
    {
        TestBoxDistancesAndTrace(); TestOpenPlaneThinWallsAndCSR(); TestTransformsRandomReferenceAndReuse(); TestFarFractionalBoundariesAndDenseCSR();
        std::cout<<"VXGISDF: all 4 CPU test groups passed (700 randomized BVH/grid comparisons).\n";
        if(argc>1 && std::string(argv[1])=="--benchmark") Benchmark();
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr<<"VXGISDF test failed: "<<exception.what()<<'\n'; return 1;
    }
}
