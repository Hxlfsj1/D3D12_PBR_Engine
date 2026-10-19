#include <windows.h> // Exercise public headers with Windows min/max macros active.
#include "../Resources/VXGIProbeTopology.h"
#include "../Resources/VXGISDF.h"
#include "../Resources/VXGIProbeState.h"
#include <iostream>
#include <stdexcept>

namespace
{
    using Vec=DirectX::XMFLOAT3;
    using Record=VXGIProbeState::GPURecord;
    struct Resource { void* pointer=nullptr; void* Get() const { return pointer; } };
    struct Texture { uint32_t type=0; Resource Resource; std::string path="fixture.png"; };
    struct Vertex { Vec Position,Normal={0,0,1}; DirectX::XMFLOAT2 TexCoords={}; Vec Tangent={1,0,0},Bitangent={0,1,0}; };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<unsigned>> lodIndices={{},{},{}};
        std::vector<Texture> textures={{}};
        uint32_t materialID=0;
        bool isUnlit=false;
        DirectX::XMFLOAT4 baseColorFactor={1,1,1,1};
        Vec emissiveFactor={};
    };
    struct Model { std::vector<Mesh> meshes={{}}; };
    struct Instance
    {
        uint32_t editorId=13;
        Model* pModel=nullptr;
        Vec translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=false;
        int currentLodLevel=0;
    };
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    void Near(float actual,float expected,const char* message) { Check(std::abs(actual-expected)<0.00003f,message); }
    Vec Sub(Vec a,Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    double Length(Vec value) { return std::sqrt(double(value.x)*value.x+double(value.y)*value.y+double(value.z)*value.z); }
    double Dot(Vec a,Vec b) { return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z; }
    bool Finite(Vec value) { return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z); }
    float MaxAbs(Vec value) { return (std::max)({std::abs(value.x),std::abs(value.y),std::abs(value.z)}); }
    void Triangle(Model& model,Vec a,Vec b,Vec c)
    {
        auto& mesh=model.meshes[0];
        const auto first=static_cast<unsigned>(mesh.vertices.size());
        mesh.vertices.push_back({a}); mesh.vertices.push_back({b}); mesh.vertices.push_back({c});
        mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),{first,first+1,first+2});
    }
    void Marker(Model& model,VXGIWorld::Coordinate coordinate,float size=1)
    {
        const float x=(float(coordinate.x)+0.05f)*size,y=(float(coordinate.y)+0.05f)*size,z=(float(coordinate.z)+0.15f)*size;
        Triangle(model,{x,y,z},{x+0.1f*size,y,z},{x,y+0.1f*size,z});
    }
    struct Fixture
    {
        Model model;
        std::vector<Instance> instances={{}};
        VXGIWorld world;
        VXGISDF sdf;
        VXGIProbeTopology topology;
        std::vector<Record> records;
        void Build(float voxelSize=1)
        {
            instances[0].pModel=&model;
            Check(world.Update(instances,voxelSize),world.GetLastError().c_str());
            Check(sdf.Update(world,0.25f,128),sdf.GetLastError().c_str());
            Check(topology.Update(world),topology.GetLastError().c_str());
            records.resize(world.GetProbes().size());
        }
    };
    Record Constant(Vec radiance)
    {
        Record result;
        result.state=VXGIProbeState::Valid; result.updateCount=3; result.sampleCount=192; result.lastUpdateFrame=7;
        result.classificationRays=64; result.backfaceRatio=0.125f; result.minHitDistance=0.1f;
        constexpr float y00=0.28209479177387814f;
        result.sh[0]={radiance.x/y00,radiance.y/y00,radiance.z/y00,0};
        return result;
    }
    bool Usable(const Record& record)
    {
        if(record.state!=VXGIProbeState::Valid || record.updateCount==0) return false;
        for(const auto& coefficient:record.sh)
            if(!std::isfinite(coefficient.x)||!std::isfinite(coefficient.y)||!std::isfinite(coefficient.z)||!std::isfinite(coefficient.w)) return false;
        return true;
    }
    // Reference tests trace exact BVH triangles rather than duplicate the GPU's
    // distance-grid traversal. Canonical undirected edges have closed endpoints.
    bool EdgeVisible(const Fixture& fixture,uint32_t a,uint32_t b)
    {
        if(a>=fixture.records.size() || b>=fixture.records.size() || a==b) return false;
        if(a>b) std::swap(a,b);
        const auto& centers=fixture.world.GetOccupiedCenters();
        const auto delta=Sub(centers[b],centers[a]);
        const double distance=Length(delta);
        if(!Finite(delta)||!std::isfinite(distance)||distance<=0) return false;
        return !fixture.sdf.TraceReference(centers[a],delta,float(distance)).hit;
    }
    float EndpointTolerance(const Fixture& fixture,Vec probe,Vec target,float distance)
    {
        return (std::max)({1e-6f,fixture.sdf.GetGrid().cellSize*1e-5f,
            8*FLT_EPSILON*(std::max)({MaxAbs(probe),MaxAbs(target),distance})});
    }
    bool SurfaceVisible(const Fixture& fixture,uint32_t probe,const VXGISDF::Hit& surface)
    {
        if(probe>=fixture.records.size() || !surface.hit || !Finite(surface.position)) return false;
        const auto center=fixture.world.GetOccupiedCenters()[probe];
        const auto delta=Sub(surface.position,center);
        const float distance=float(Length(delta));
        if(!Finite(delta)||!std::isfinite(distance)||distance<=0) return false;
        const float tolerance=EndpointTolerance(fixture,center,surface.position,distance);
        const auto first=fixture.sdf.TraceReference(center,delta,distance+tolerance);
        return first.hit && first.triangleIndex==surface.triangleIndex && std::abs(first.distance-distance)<=tolerance &&
            Length(Sub(first.position,surface.position))<=tolerance;
    }
    std::vector<Record> Filter(const Fixture& fixture,float neighborWeight=0.25f)
    {
        Check(std::isfinite(neighborWeight)&&neighborWeight>=0&&neighborWeight<=1,"invalid reference filter weight");
        auto filtered=fixture.records;
        for(size_t index=0;index<filtered.size();++index)
        {
            auto& output=filtered[index];
            if(!Usable(fixture.records[index])) { output={}; continue; }
            float sum=1;
            for(const auto neighbor:fixture.topology.GetNeighbors()[index].indices)
            {
                if(neighbor==VXGIProbeTopology::Missing || neighborWeight==0 || !Usable(fixture.records[neighbor]) ||
                    !EdgeVisible(fixture,static_cast<uint32_t>(index),neighbor)) continue;
                for(size_t band=0;band<4;++band)
                {
                    output.sh[band].x+=fixture.records[neighbor].sh[band].x*neighborWeight;
                    output.sh[band].y+=fixture.records[neighbor].sh[band].y*neighborWeight;
                    output.sh[band].z+=fixture.records[neighbor].sh[band].z*neighborWeight;
                }
                sum+=neighborWeight;
            }
            for(auto& coefficient:output.sh) { coefficient.x/=sum; coefficient.y/=sum; coefficient.z/=sum; coefficient.w=0; }
        }
        return filtered;
    }
    struct QueryResult { bool supported=false; Vec irradiance={}; float weight=0; uint32_t contributors=0; };
    QueryResult Query(const Fixture& fixture,const std::vector<Record>& raw,const VXGISDF::Hit& surface,Vec mappedNormal)
    {
        QueryResult result;
        if(!surface.hit || !Finite(mappedNormal) || Length(mappedNormal)==0) return result;
        std::array<VXGIProbeTopology::QueryCandidate,8> candidates;
        if(!fixture.topology.GetQueryCandidates(surface.position,candidates)) return result;
        Vec face=surface.geometricNormal;
        if(surface.backFace) face={-face.x,-face.y,-face.z};
        Record estimate;
        for(const auto& candidate:candidates)
        {
            if(candidate.weight<=0 || candidate.probeIndex>=raw.size() || !Usable(raw[candidate.probeIndex]) ||
                Dot(face,Sub(fixture.world.GetOccupiedCenters()[candidate.probeIndex],surface.position))<=0 ||
                !SurfaceVisible(fixture,candidate.probeIndex,surface)) continue;
            for(size_t coefficient=0;coefficient<4;++coefficient)
            {
                estimate.sh[coefficient].x+=raw[candidate.probeIndex].sh[coefficient].x*candidate.weight;
                estimate.sh[coefficient].y+=raw[candidate.probeIndex].sh[coefficient].y*candidate.weight;
                estimate.sh[coefficient].z+=raw[candidate.probeIndex].sh[coefficient].z*candidate.weight;
            }
            result.weight+=candidate.weight; ++result.contributors;
        }
        if(result.weight>0)
        {
            result.supported=true;
            for(auto& coefficient:estimate.sh)
            {
                coefficient.x/=result.weight; coefficient.y/=result.weight; coefficient.z/=result.weight;
            }
            // Signed SH must interpolate before reconstruction is clamped.
            result.irradiance=VXGIProbeState::EvaluateIrradianceOverPi(estimate,mappedNormal);
        }
        return result;
    }
    void TestTopologyAndRevisions()
    {
        Fixture fixture;
        for(int x=-1;x<=1;++x) for(int y=-1;y<=1;++y) for(int z=-1;z<=1;++z) Marker(fixture.model,{x,y,z});
        fixture.Build();
        Check(fixture.topology.GetLookup().size()==27,"marker topology contains unexpected occupancy");
        Check(fixture.topology.GetStats().neighborLinks==108 && fixture.topology.GetStats().missingNeighborLinks==54 &&
            fixture.topology.GetStats().lookupBytes==27*16 && fixture.topology.GetStats().neighborBytes==27*24,
            "topology link or packed memory statistics incorrect");
        const auto center=fixture.topology.FindProbe({0,0,0});
        const std::array<VXGIWorld::Coordinate,6> coordinates={{{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}}};
        for(size_t direction=0;direction<6;++direction)
            Check(fixture.topology.GetNeighbors()[center].indices[direction]==fixture.topology.FindProbe(coordinates[direction]),
                "six-neighbor order or lookup disagrees with World");
        for(const auto& entry:fixture.topology.GetLookup())
            Check(fixture.world.FindCell(entry.coordinate)==static_cast<int32_t>(entry.probeIndex),"binary lookup disagrees with actual World hash");
        Check(fixture.topology.FindProbe({2,0,0})==VXGIProbeTopology::Missing,"missing sparse cell returned a probe");
        std::array<VXGIProbeTopology::QueryCandidate,8> candidates;
        Check(fixture.topology.GetQueryCandidates({0,0,0},candidates),"negative-cell center lattice query failed");
        for(uint32_t corner=0;corner<8;++corner)
        {
            const VXGIWorld::Coordinate coordinate={-1+int32_t(corner&1),-1+int32_t((corner>>1)&1),-1+int32_t((corner>>2)&1)};
            Check(candidates[corner].probeIndex==fixture.topology.FindProbe(coordinate),"eight-corner ordering/global coordinates wrong");
            Near(candidates[corner].weight,0.125f,"trilinear weights do not sum to one at midpoint");
        }
        Check(fixture.topology.GetQueryCandidates({10,10,10},candidates),"missing support should still have valid geometric weights");
        for(const auto candidate:candidates) Check(candidate.probeIndex==VXGIProbeTopology::Missing,"query broadened support with a fallback");
        Check(!fixture.topology.GetQueryCandidates({std::numeric_limits<float>::infinity(),0,0},candidates),"nonfinite query accepted");
        Check(!fixture.topology.GetQueryCandidates({float(INT32_MAX),0,0},candidates),"unsafe corner integer range accepted");
        VXGIWorld::Coordinate overflow;
        Check(!VXGIProbeTopology::NeighborCoordinate({INT32_MIN,0,0},0,overflow) &&
            !VXGIProbeTopology::NeighborCoordinate({INT32_MAX,0,0},1,overflow),"neighbor coordinate overflow wrapped");
        const auto revision=fixture.topology.GetRevision();
        const auto* lookup=fixture.topology.GetLookup().data(); const auto* neighbors=fixture.topology.GetNeighbors().data();
        const auto sdfGeometry=fixture.sdf.GetGeometryRevision();
        fixture.model.meshes[0].baseColorFactor.x=0.3f;
        fixture.instances[0].customMaterialID=8; fixture.instances[0].isVisible=false; fixture.instances[0].currentLodLevel=2;
        fixture.Build();
        Check(fixture.topology.GetRevision()==revision && fixture.topology.GetLookup().data()==lookup &&
            fixture.topology.GetNeighbors().data()==neighbors && fixture.sdf.GetGeometryRevision()==sdfGeometry,
            "material/camera edits rebuilt allocation-only topology or SDF");
        VXGIWorld unbuilt;
        Check(!fixture.topology.Update(unbuilt) && fixture.topology.GetRevision()==revision &&
            fixture.topology.GetLookup().data()==lookup,"failed topology update discarded its committed buffers");
        fixture.instances[0].translation.x=0.17f;
        fixture.Build();
        Check(fixture.topology.GetRevision()==revision+1 && fixture.topology.GetStats().worldAllocationRevision==fixture.world.GetAllocationRevision(),
            "movement allocation revision did not refresh topology");
        std::cout<<"Topology: 27 occupied marker cells, ordered 6/8 lookup, missing/range guards, reuse and transactional failure passed.\n";
    }
    void TestConstantSignedFilterAndUnsupported()
    {
        Fixture fixture;
        for(int x=-1;x<=1;++x) for(int y=-1;y<=1;++y) for(int z=-1;z<=1;++z) Marker(fixture.model,{x,y,z});
        Triangle(fixture.model,{-2,-2,0.3f},{2,-2,0.3f},{2,2,0.3f});
        Triangle(fixture.model,{-2,-2,0.3f},{2,2,0.3f},{-2,2,0.3f});
        fixture.Build();
        for(auto& record:fixture.records) record=Constant({2,3,4});
        const auto filtered=Filter(fixture);
        for(size_t index=0;index<filtered.size();++index)
        {
            Near(filtered[index].sh[0].x,fixture.records[index].sh[0].x,"neighbor filtering failed constant SH preservation");
            Check(filtered[index].updateCount==3 && filtered[index].lastUpdateFrame==7 && filtered[index].backfaceRatio==0.125f,
                "filter changed valid self metadata");
        }
        const auto surface=fixture.sdf.TraceReference({-0.13f,0.19f,1},{0,0,-1},2);
        Check(surface.hit && surface.position.z==0.3f,"reference receiver surface not found");
        auto query=Query(fixture,fixture.records,surface,{0,0,1});
        Check(query.supported && query.contributors>0,"constant surface query lacks support");
        Near(query.irradiance.x,2,"constant red E/pi query wrong"); Near(query.irradiance.y,3,"constant green E/pi query wrong");
        Near(query.irradiance.z,4,"constant blue E/pi query wrong");
        for(size_t index=0;index<fixture.records.size();++index)
            fixture.records[index]=Constant({fixture.world.GetOccupiedCenters()[index].x<surface.position.x?-2.0f:1.0f,0,0});
        query=Query(fixture,fixture.records,surface,{0,0,1});
        Check(query.supported && query.contributors>=2,"signed SH interpolation lacks mixed support");
        Near(query.irradiance.x,0,"query clamped each probe before signed SH interpolation");
        for(auto& record:fixture.records) record=Constant({0,0,0});
        query=Query(fixture,fixture.records,surface,{0,0,1});
        Check(query.supported && query.irradiance.x==0 && query.irradiance.y==0 && query.irradiance.z==0,"valid black SH confused with absent support");
        for(auto& record:fixture.records) record.updateCount=0;
        Check(!Query(fixture,fixture.records,surface,{0,0,1}).supported,"unupdated valid probes consumed");
        for(auto& record:fixture.records) { record=Constant({1,1,1}); record.state=VXGIProbeState::Disabled; }
        Check(!Query(fixture,fixture.records,surface,{0,0,1}).supported,"disabled probes consumed");
        for(auto& record:fixture.records) { record=Constant({1,1,1}); record.sh[2].x=std::numeric_limits<float>::quiet_NaN(); }
        Check(!Query(fixture,fixture.records,surface,{0,0,1}).supported,"nonfinite raw SH consumed");
        const auto invalidSources=Filter(fixture);
        Check(!Query(fixture,invalidSources,surface,{0,0,1}).supported,
            "nonfinite valid raw source became a filtered valid black probe");
        auto wrong=surface; wrong.triangleIndex=UINT32_MAX;
        for(auto& record:fixture.records) record=Constant({1,1,1});
        Check(!Query(fixture,fixture.records,wrong,{0,0,1}).supported,"endpoint position without triangle identity accepted");
        Fixture signedFixture;
        Marker(signedFixture.model,{0,0,0}); Marker(signedFixture.model,{1,0,0}); signedFixture.Build();
        signedFixture.records[0]=Constant({0,0,0}); signedFixture.records[1]=Constant({0,0,0});
        signedFixture.records[0].sh[1].x=-2; signedFixture.records[1].sh[1].x=6;
        const auto signedFilter=Filter(signedFixture);
        Near(signedFilter[0].sh[1].x,-0.4f,"signed SH was clamped during filtering");
        const auto selfOnly=Filter(signedFixture,0); Near(selfOnly[0].sh[1].x,-2,"zero neighbor weight changed self SH");
        signedFixture.records[0].state=VXGIProbeState::Disabled;
        const auto invalidFilter=Filter(signedFixture);
        Check(invalidFilter[0].state==VXGIProbeState::Unclassified && invalidFilter[0].updateCount==0,
            "invalid filter source retained a usable state or update history");
        for(const auto coefficient:invalidFilter[0].sh)
            Check(coefficient.x==0 && coefficient.y==0 && coefficient.z==0 && coefficient.w==0,"invalid self did not zero filtered SH");
        Fixture endpoint;
        Triangle(endpoint.model,{0.5f,0.4f,0.4f},{0.5f,0.6f,0.4f},{0.5f,0.4f,0.6f});
        Marker(endpoint.model,{1,0,0}); endpoint.Build();
        endpoint.records[0]=Constant({1,0,0}); endpoint.records[1]=Constant({0,0,1});
        Check(!EdgeVisible(endpoint,0,1) && !EdgeVisible(endpoint,1,0),
            "canonical closed center edge exempted an endpoint surface hit");
        Near(Filter(endpoint)[0].sh[0].z,0,"endpoint-blocked neighbor leaked color");
        std::cout<<"Reference filter/query: constant and signed SH, metadata, black/support distinction and invalid/unupdated/identity rejection passed.\n";
    }
    void TestThinWallAndThreePointLeak()
    {
        Fixture wall;
        Triangle(wall.model,{0,-2,-2},{0,-2,2},{0,2,2});
        Triangle(wall.model,{0,-2,-2},{0,2,2},{0,2,-2});
        wall.Build();
        for(size_t index=0;index<wall.records.size();++index)
            wall.records[index]=Constant(wall.world.GetOccupiedCenters()[index].x<0 ? Vec{1,0,0} : Vec{0,0,1});
        const auto filtered=Filter(wall);
        for(size_t index=0;index<filtered.size();++index)
        {
            const auto center=wall.world.GetOccupiedCenters()[index];
            if(std::abs(center.y)>=2 || std::abs(center.z)>=2) continue; // Edge rays outside the finite wall are correctly visible.
            const auto value=VXGIProbeState::EvaluateIrradianceOverPi(filtered[index],{1,0,0});
            if(wall.world.GetOccupiedCenters()[index].x<0) { Near(value.x,1,"thin wall dimmed red side"); Near(value.z,0,"neighbor filter leaked blue through thin wall"); }
            else { Near(value.x,0,"neighbor filter leaked red through thin wall"); Near(value.z,1,"thin wall dimmed blue side"); }
        }
        auto surface=wall.sdf.TraceReference({-1,0.13f,0.21f},{1,0,0},2);
        Check(surface.hit && !surface.backFace,"front-facing thin wall target not found");
        auto query=Query(wall,wall.records,surface,{0,1,0}); // Mapped normal cannot change geometric side eligibility.
        Check(query.supported,"thin wall front side lacks support"); Near(query.irradiance.x,1,"thin wall raw query lost red"); Near(query.irradiance.z,0,"thin wall query leaked blue");
        surface.backFace=true;
        query=Query(wall,wall.records,surface,{0,1,0});
        Check(query.supported,"ray-facing back surface lacks support"); Near(query.irradiance.x,0,"back-face side query leaked red"); Near(query.irradiance.z,1,"back-face side query lost blue");

        Fixture corner;
        Triangle(corner.model,{0.5f,0.3f,0.1f},{0.5f,0.6f,0.1f},{0.5f,0.3f,0.4f});
        Triangle(corner.model,{0.5f,0.6f,0.1f},{0.5f,0.6f,0.4f},{0.5f,0.3f,0.4f});
        Triangle(corner.model,{0.2f,0.39f,0.15f},{0.2f,0.39f,0.35f},{0.5f,0.39f,0.35f});
        corner.Build(0.5f);
        const auto a=corner.topology.FindProbe({0,1,0}),b=corner.topology.FindProbe({1,1,0});
        Check(a!=UINT32_MAX && b!=UINT32_MAX,"three-point diagnostic probes not allocated");
        corner.records[a]=Constant({1,0,0}); corner.records[b]=Constant({0,0,1});
        const auto target=corner.sdf.TraceReference({0.31f,0.6f,0.25f},{0,-1,0},1);
        Check(target.hit && target.triangleIndex==2,"three-point receiver target not found");
        Check(EdgeVisible(corner,a,b) && EdgeVisible(corner,b,a) && SurfaceVisible(corner,a,target) && !SurfaceVisible(corner,b,target),
            "P-to-A/A-to-B clear and P-to-B blocked counterexample was not reproduced");
        const auto diagnostic=Filter(corner);
        Check(diagnostic[a].sh[0].z>0,"diagnostic A filter did not contain visible blue neighbor B");
        const auto raw=Query(corner,corner.records,target,{0,1,0});
        Check(raw.supported && raw.contributors==1,"raw query did not retain exactly the visible contributor");
        Near(raw.irradiance.x,1,"raw three-point query lost visible red"); Near(raw.irradiance.z,0,"raw three-point query leaked occluded blue");
        const auto unsafe=Query(corner,diagnostic,target,{0,1,0});
        Check(unsafe.supported && unsafe.irradiance.z>0.1f,"three-point fixture failed to expose transitive filtered-cache leakage");
        std::cout<<"Visibility: thin wall color isolation; P->A and A->B clear but P->B blocked; raw query stays red while diagnostic filtered A contains blue.\n";
    }
}

int main()
{
    try { TestTopologyAndRevisions(); TestConstantSignedFilterAndUnsupported(); TestThinWallAndThreePointLeak();
        std::cout<<"VXGIProbeQueryTests: 3 groups passed.\n"; return 0; }
    catch(const std::exception& exception) { std::cerr<<"VXGIProbeQueryTests failed: "<<exception.what()<<'\n'; return 1; }
}
