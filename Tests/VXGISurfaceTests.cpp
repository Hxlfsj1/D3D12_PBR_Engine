#include "../Resources/VXGISDF.h"
#include "../Resources/VXGISurface.h"
#include <iostream>
#include <span>
#include <stdexcept>

namespace
{
    struct Resource { void* pointer=nullptr; void* Get() const { return pointer; } };
    struct Texture { uint32_t type=0; Resource Resource; std::string path="material.png"; };
    struct Vertex
    {
        DirectX::XMFLOAT3 Position,Normal={0,0,1};
        DirectX::XMFLOAT2 TexCoords={};
        DirectX::XMFLOAT3 Tangent={1,0,0},Bitangent={0,1,0};
    };
    struct Mesh
    {
        std::vector<Vertex> vertices={{{0.1f,0.1f,0.1f}},{{0.9f,0.1f,0.1f}},{{0.1f,0.9f,0.1f}}};
        std::vector<std::vector<unsigned>> lodIndices={{0,1,2},{},{}};
        std::vector<Texture> textures={{}};
        uint32_t materialID=0;
        bool isUnlit=false;
        DirectX::XMFLOAT4 baseColorFactor={1,1,1,1};
        DirectX::XMFLOAT3 emissiveFactor={};
    };
    struct Model { std::vector<Mesh> meshes={{}}; };
    struct Instance
    {
        uint32_t editorId=11;
        Model* pModel=nullptr;
        DirectX::XMFLOAT3 translation={},rotation={},scale={1,1,1};
        uint32_t customMaterialID=UINT32_MAX;
        bool isTransparent=false,isCutout=false,isVisible=true,isDirty=false;
        int currentLodLevel=0;
    };
    void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    struct Fixture
    {
        Model model;
        std::vector<Instance> instances={{}};
        VXGIWorld world;
        VXGISDF sdf;
        VXGISurface surface;
        std::vector<VXGISurface::MaterialRecord> materials=std::vector<VXGISurface::MaterialRecord>(2);
        VXGISurface::Parameters parameters;
        Fixture()
        {
            instances[0].pModel=&model;
            materials[0].albedoIdx=3; materials[0].normalIdx=4; materials[0].ormIdx=5; materials[0].emissiveIdx=6;
            materials[0].emissiveFactor={2,3,4};
            Check(Update(),"initial world/SDF/surface update failed");
        }
        bool Update(float voxelSize=0.5f)
        {
            return world.Update(instances,voxelSize) && sdf.Update(world,0.25f,128) &&
                surface.Update(std::span<const VXGISurface::MaterialRecord>(materials),world.GetGeometryRevision(),
                    sdf.GetGeometryRevision(),world.GetMaterialRevision(),parameters);
        }
    };
    void TestCameraAndLightSamplingState()
    {
        Fixture fixture;
        const auto world=fixture.world.GetRevision(),geometry=fixture.sdf.GetGeometryRevision(),surface=fixture.surface.GetRevision();
        const auto* nodes=fixture.sdf.GetNodes().data();
        const auto* materials=fixture.surface.GetMaterials().data();
        fixture.instances[0].isVisible=false; fixture.instances[0].currentLodLevel=2; fixture.instances[0].isDirty=true;
        Check(fixture.Update() && fixture.world.GetRevision()==world && fixture.surface.GetRevision()==surface,
            "camera visibility/LOD/dirty flag changed surface state");
        Check(fixture.sdf.GetGeometryRevision()==geometry && fixture.sdf.GetNodes().data()==nodes &&
            fixture.surface.GetMaterials().data()==materials,"camera reallocated geometry or materials");
        const auto lighting=fixture.surface.GetLightingRevision(),sampling=fixture.surface.GetSamplingRevision();
        fixture.parameters.lightDirection={1,-1,0};
        Check(fixture.Update() && fixture.surface.GetLightingRevision()==lighting+1 && fixture.surface.GetRevision()==surface+1,
            "sun direction did not invalidate lighting");
        fixture.parameters.lightColor={2,1,0.5f};
        Check(fixture.Update() && fixture.surface.GetLightingRevision()==lighting+2,"sun color change missed");
        Check(fixture.sdf.GetGeometryRevision()==geometry && fixture.sdf.GetNodes().data()==nodes,"sun edit rebaked geometry");
        fixture.parameters.materialMipLevel=0.75f;
        Check(fixture.Update() && fixture.surface.GetSamplingRevision()==sampling+1 &&
            fixture.surface.GetParameters().materialMipLevel==0.75f,"explicit fractional SampleLevel LOD not tracked");
        fixture.parameters.maxRayDistance=16; fixture.parameters.rayMinDistance=0.0005f;
        Check(fixture.Update() && fixture.surface.GetSamplingRevision()==sampling+2,"ray range/self-hit interval edit missed");
        Check(fixture.sdf.GetNodes().data()==nodes,"surface sampling parameter rebaked geometry");
    }
    void TestMaterialAndIdentityReuse()
    {
        Fixture fixture;
        const auto worldGeometry=fixture.world.GetGeometryRevision(),sdfGeometry=fixture.sdf.GetGeometryRevision();
        const auto allocation=fixture.world.GetAllocationRevision();
        const auto geometryFingerprint=fixture.world.GetStats().geometryFingerprint;
        const auto* nodes=fixture.sdf.GetNodes().data();
        const auto* offsets=fixture.sdf.GetCellOffsets().data();
        const auto* references=fixture.sdf.GetTriangleReferences().data();
        const auto* cells=fixture.world.GetCells().data();
        const auto* triangles=fixture.sdf.GetTriangles().data();
        auto verifyReuse=[&]()
        {
            Check(fixture.world.GetGeometryRevision()==worldGeometry && fixture.world.GetAllocationRevision()==allocation &&
                fixture.world.GetCells().data()==cells && fixture.world.GetStats().geometryFingerprint==geometryFingerprint,
                "material edit rebuilt sparse allocation");
            Check(fixture.sdf.GetGeometryRevision()==sdfGeometry && fixture.sdf.GetNodes().data()==nodes &&
                fixture.sdf.GetCellOffsets().data()==offsets && fixture.sdf.GetTriangleReferences().data()==references &&
                fixture.sdf.GetTriangles().data()==triangles,"material/identity edit rebuilt SDF/BVH/CSR buffers");
            Check(!fixture.sdf.GetStats().geometryRebuilt && fixture.sdf.GetStats().distanceBakeMilliseconds==0,
                "identity refresh claimed distance rebaking");
        };
        const auto materialRevision=fixture.world.GetMaterialRevision(),surfaceMaterial=fixture.surface.GetMaterialRevision();
        fixture.model.meshes[0].baseColorFactor.x=0.25f; fixture.materials[0].baseColorFactor.x=0.25f;
        Check(fixture.Update() && fixture.world.GetMaterialRevision()==materialRevision+1 &&
            fixture.surface.GetMaterialRevision()==surfaceMaterial+1,"material factor update missed"); verifyReuse();
        fixture.model.meshes[0].isUnlit=true; fixture.materials[0].isUnlit=1;
        Check(fixture.Update(),"unlit material refresh failed"); verifyReuse();
        int textureResource=0; fixture.model.meshes[0].textures[0].Resource.pointer=&textureResource;
        fixture.model.meshes[0].textures[0].path="replacement.png"; fixture.materials[0].albedoIdx=10;
        Check(fixture.Update(),"texture/resource identity refresh failed"); verifyReuse();
        fixture.model.meshes[0].emissiveFactor={1,2,3}; fixture.materials[0].emissiveFactor={1,2,3};
        Check(fixture.Update(),"emissive factor refresh failed"); verifyReuse();
        fixture.instances[0].customMaterialID=1;
        Check(fixture.Update() && fixture.world.GetTriangles()[0].materialId==1 && fixture.sdf.GetTriangles()[0].materialId==1,
            "custom material identity did not propagate"); verifyReuse();
        const auto hit=fixture.sdf.Trace({0.2f,0.2f,-1},{0,0,1});
        Check(hit.hit && hit.materialId==1,"reference/grid trace retained stale material identity");
        fixture.instances[0].isCutout=true;
        Check(fixture.Update() && fixture.sdf.GetTriangles()[0].isCutout && fixture.world.GetStats().cutoutInstances==1,
            "cutout proxy flag refresh missed"); verifyReuse();
        const auto materialState=fixture.surface.GetMaterialRevision();
        fixture.materials[1].normalIdx=12;
        Check(fixture.Update() && fixture.surface.GetMaterialRevision()==materialState+1,
            "material records changed without a world material revision were ignored");
        Check(std::memcmp(&fixture.materials[0],&fixture.surface.GetMaterials()[0],48)==0,
            "48-byte CPU material cache diverged from upload record");
    }
    void TestGeometryAllocationAndSourceReorder()
    {
        Fixture fixture;
        auto worldGeometry=fixture.world.GetGeometryRevision(),sdfGeometry=fixture.sdf.GetGeometryRevision();
        auto materialRevision=fixture.world.GetMaterialRevision();
        const auto* nodes=fixture.sdf.GetNodes().data();
        fixture.instances[0].translation.x=1;
        Check(fixture.Update() && fixture.world.GetGeometryRevision()==worldGeometry+1 && fixture.sdf.GetGeometryRevision()==sdfGeometry+1 &&
            fixture.sdf.GetStats().geometryRebuilt && fixture.world.GetMaterialRevision()==materialRevision,"transform failed separate geometry invalidation");
        worldGeometry=fixture.world.GetGeometryRevision(); sdfGeometry=fixture.sdf.GetGeometryRevision(); nodes=fixture.sdf.GetNodes().data();
        const auto surfaceRevision=fixture.surface.GetRevision(),allocation=fixture.world.GetAllocationRevision();
        Check(fixture.Update(0.25f) && fixture.world.GetGeometryRevision()==worldGeometry && fixture.world.GetAllocationRevision()==allocation+1 &&
            fixture.sdf.GetGeometryRevision()==sdfGeometry && fixture.sdf.GetNodes().data()==nodes &&
            fixture.surface.GetRevision()==surfaceRevision,"probe allocation size rebaked independent SDF/surface state");
        fixture.instances.push_back(fixture.instances[0]); fixture.instances.back().editorId=22; fixture.instances.back().translation.x=-1;
        Check(fixture.Update(),"multi-instance geometry build failed");
        const auto reorderGeometry=fixture.world.GetGeometryRevision(),reorderSDF=fixture.sdf.GetGeometryRevision();
        nodes=fixture.sdf.GetNodes().data();
        std::swap(fixture.instances[0],fixture.instances[1]);
        Check(fixture.Update() && fixture.world.GetGeometryRevision()==reorderGeometry && fixture.sdf.GetGeometryRevision()==reorderSDF &&
            fixture.sdf.GetNodes().data()==nodes && fixture.world.GetTriangles()[0].sourceInstanceIndex==1 &&
            fixture.sdf.GetTriangles()[0].sourceInstanceIndex==1,"source-order edit changed geometry or left stale source identity");
        fixture.model.meshes[0].vertices[0].Position.x+=0.1f; fixture.world.Invalidate();
        Check(fixture.Update() && fixture.world.GetGeometryRevision()==reorderGeometry+1 && fixture.sdf.GetGeometryRevision()==reorderSDF+1,
            "explicit in-place vertex invalidation failed to rebake");
    }
    void TestValidationPreservesState()
    {
        Fixture fixture;
        const auto revision=fixture.surface.GetRevision(); const auto* records=fixture.surface.GetMaterials().data();
        fixture.parameters.lightDirection={};
        Check(!fixture.Update() && fixture.surface.GetRevision()==revision && fixture.surface.GetMaterials().data()==records,
            "invalid light direction changed committed state");
        fixture.parameters.lightDirection={0,-1,0}; fixture.parameters.rayMinDistance=fixture.parameters.maxRayDistance;
        Check(!fixture.Update() && fixture.surface.GetRevision()==revision,"invalid ray interval changed committed state");
        fixture.parameters.rayMinDistance=0.0001f; fixture.materials[0].baseColorFactor.x=std::numeric_limits<float>::quiet_NaN();
        Check(!fixture.Update() && fixture.surface.GetRevision()==revision,"non-finite material cache was committed");
        fixture.materials[0].baseColorFactor.x=1;
        Check(fixture.Update() && fixture.surface.GetLastError().empty() && fixture.surface.GetRevision()==revision,"state did not recover cleanly");
    }
}
int main()
{
    try
    {
        TestCameraAndLightSamplingState(); TestMaterialAndIdentityReuse(); TestGeometryAllocationAndSourceReorder(); TestValidationPreservesState();
        std::cout<<"VXGISurface: all 4 CPU test groups passed (independent geometry/material/light/sampling revisions).\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr<<"VXGISurface test failed: "<<exception.what()<<'\n'; return 1;
    }
}
