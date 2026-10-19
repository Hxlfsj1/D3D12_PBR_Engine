// Actual configured-scene acceptance for the approximate nearest-two consumer.
// Reuse the existing hidden GPU harness so the exact production query is run.
#define main VXGIThresholdSyntheticFixtureMain
#include "VXGIMeshSDFThresholdGPUValidation.cpp"
#undef main
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "../ThirdParty/json.hpp"
#include <numeric>
#include <cfloat>

namespace ThresholdAcceptance
{
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
    struct Vertex
    {
        XMFLOAT3 Position, Normal = {0, 0, 1};
        XMFLOAT2 TexCoords = {};
        XMFLOAT3 Tangent = {1, 0, 0}, Bitangent = {0, 1, 0};
    };
    struct TextureResource { void* Get() const { return nullptr; } };
    struct Texture { uint32_t type = 0; TextureResource Resource; std::string path; };
    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<std::vector<uint32_t>> lodIndices = {{}};
        uint32_t materialID = 7;
        bool isUnlit = false;
        XMFLOAT4 baseColorFactor = {1, 1, 1, 1};
        XMFLOAT3 emissiveFactor = {};
        std::vector<Texture> textures;
    };
    struct Model { std::vector<Mesh> meshes; };
    struct SourceInstance
    {
        uint32_t editorId = 0;
        Model* pModel = nullptr;
        XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
        uint32_t customMaterialID = UINT32_MAX;
        bool isTransparent = false, isCutout = false;
    };
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
double Elapsed(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
XMFLOAT3 V3(const Json& item){return {item[0].get<float>(),item[1].get<float>(),item[2].get<float>()};}
DirectX::XMMATRIX Matrix(const aiMatrix4x4& m){return DirectX::XMMATRIX(m.a1,m.b1,m.c1,m.d1,m.a2,m.b2,m.c2,m.d2,m.a3,m.b3,m.c3,m.d3,m.a4,m.b4,m.c4,m.d4);}
void ImportNode(Model& output,const aiScene& scene,const aiNode& node,DirectX::FXMMATRIX parent,uint32_t& material)
{
    using namespace DirectX;
    const auto transform=Matrix(node.mTransformation)*parent;
    const auto normal=XMMatrixTranspose(XMMatrixInverse(nullptr,transform));
    for(uint32_t i=0;i<node.mNumMeshes;++i)
    {
        const auto& source=*scene.mMeshes[node.mMeshes[i]];
        Mesh mesh; mesh.materialID=material++;
        for(uint32_t j=0;j<source.mNumVertices;++j)
        {
            Vertex v{};
            XMStoreFloat3(&v.Position,XMVector4Transform(XMVectorSet(source.mVertices[j].x,source.mVertices[j].y,source.mVertices[j].z,1),transform));
            if(source.HasNormals())XMStoreFloat3(&v.Normal,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mNormals[j])),normal)));
            if(source.mTextureCoords[0])v.TexCoords={source.mTextureCoords[0][j].x,source.mTextureCoords[0][j].y};
            if(source.HasTangentsAndBitangents())
            {
                XMStoreFloat3(&v.Tangent,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mTangents[j])),normal)));
                XMStoreFloat3(&v.Bitangent,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&source.mBitangents[j])),normal)));
            }
            mesh.vertices.push_back(v);
        }
        for(uint32_t j=0;j<source.mNumFaces;++j)
        {
            Require(source.mFaces[j].mNumIndices==3,"Assimp triangulation failed");
            mesh.lodIndices[0].insert(mesh.lodIndices[0].end(),source.mFaces[j].mIndices,source.mFaces[j].mIndices+3);
        }
        output.meshes.push_back(std::move(mesh));
    }
    for(uint32_t i=0;i<node.mNumChildren;++i)ImportNode(output,scene,*node.mChildren[i],transform,material);
}
Model Load(const std::string& path,uint32_t& material)
{
    Assimp::Importer importer;
    const unsigned flags=aiProcess_Triangulate|aiProcess_GenSmoothNormals|aiProcess_CalcTangentSpace|aiProcess_JoinIdenticalVertices|aiProcess_MakeLeftHanded|aiProcess_FlipWindingOrder|aiProcess_FlipUVs;
    const auto* scene=importer.ReadFile(path,flags);
    Require(scene&&scene->mRootNode,"Cannot load configured model: "+path+": "+importer.GetErrorString());
    Model model;ImportNode(model,*scene,*scene->mRootNode,DirectX::XMMatrixIdentity(),material);return model;
}
struct D
{
    double x=0,y=0,z=0;
    D operator+(D q)const{return {x+q.x,y+q.y,z+q.z};}
    D operator-(D q)const{return {x-q.x,y-q.y,z-q.z};}
    D operator*(double q)const{return {x*q,y*q,z*q};}
    double operator[](int i)const{return i==0?x:i==1?y:z;}
};
D Double(XMFLOAT3 p){return {p.x,p.y,p.z};}
D Cross(D p,D q){return {p.y*q.z-p.z*q.y,p.z*q.x-p.x*q.z,p.x*q.y-p.y*q.x};}
double Dot(D p,D q){return p.x*q.x+p.y*q.y+p.z*q.z;}
struct Expected{bool hit=false;double distance=0;uint32_t triangle=UINT32_MAX;};
// Independent world-space median BVH. No asset-local bounds, inverse transforms,
// distance nodes, cell references, or production trace functions are consulted.
class Reference
{
    struct Bounds{D low={DBL_MAX,DBL_MAX,DBL_MAX},high={-DBL_MAX,-DBL_MAX,-DBL_MAX};};
    struct Node{Bounds bounds;uint32_t first=0,count=0,left=0,right=0;};
    const std::vector<VXGIWorld::Triangle>& triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    static void Expand(Bounds& b,D p){b.low={(std::min)(b.low.x,p.x),(std::min)(b.low.y,p.y),(std::min)(b.low.z,p.z)};b.high={(std::max)(b.high.x,p.x),(std::max)(b.high.y,p.y),(std::max)(b.high.z,p.z)};}
    D Center(uint32_t i)const{const auto& p=triangles[i].positions;return (Double(p[0])+Double(p[1])+Double(p[2]))*(1.0/3);}
    uint32_t Build(uint32_t first,uint32_t count)
    {
        const uint32_t index=uint32_t(nodes.size());nodes.push_back({});Bounds b,c;
        for(uint32_t i=first;i<first+count;++i){for(const auto& p:triangles[order[i]].positions)Expand(b,Double(p));Expand(c,Center(order[i]));}
        nodes[index].bounds=b;
        if(count<=8){nodes[index].first=first;nodes[index].count=count;return index;}
        const D e=c.high-c.low;const int axis=e.x>e.y?(e.x>e.z?0:2):(e.y>e.z?1:2);const uint32_t mid=first+count/2;
        std::nth_element(order.begin()+first,order.begin()+mid,order.begin()+first+count,[&](uint32_t a,uint32_t q){return Center(a)[axis]<Center(q)[axis];});
        const uint32_t left=Build(first,count/2),right=Build(mid,count-count/2);nodes[index].left=left;nodes[index].right=right;return index;
    }
    static bool Box(const Bounds& b,D o,D d,double low,double high)
    {
        for(int axis=0;axis<3;++axis){if(std::abs(d[axis])<1e-30){if(o[axis]<b.low[axis]||o[axis]>b.high[axis])return false;continue;}double a=(b.low[axis]-o[axis])/d[axis],z=(b.high[axis]-o[axis])/d[axis];if(a>z)std::swap(a,z);low=(std::max)(low,a);high=(std::min)(high,z);if(low>high)return false;}return true;
    }
public:
    explicit Reference(const VXGIWorld& world):triangles(world.GetTriangles()){order.resize(triangles.size());std::iota(order.begin(),order.end(),0u);Build(0,uint32_t(order.size()));}
    Expected Trace(const Ray& ray,bool alreadyNormalized=false)const
    {
        const D origin=Double(ray.origin),direction=alreadyNormalized?Double(ray.direction):Double(ray.direction)*(1/std::sqrt(Dot(Double(ray.direction),Double(ray.direction))));
        Expected best;double maximum=ray.maximum;std::vector<uint32_t> stack={0};
        while(!stack.empty())
        {
            const auto& node=nodes[stack.back()];stack.pop_back();if(!Box(node.bounds,origin,direction,ray.minimum,maximum))continue;
            if(!node.count){stack.push_back(node.left);stack.push_back(node.right);continue;}
            for(uint32_t j=0;j<node.count;++j)
            {
                const uint32_t index=order[node.first+j];const auto& q=triangles[index].positions;
                const D e1=Double(q[1])-Double(q[0]),e2=Double(q[2])-Double(q[0]),p=Cross(direction,e2);const double determinant=Dot(e1,p);
                if(std::abs(determinant)<1e-12)continue;
                const D s=origin-Double(q[0]),v=Cross(s,e1);const double u=Dot(s,p)/determinant,w=Dot(direction,v)/determinant,distance=Dot(e2,v)/determinant;
                constexpr double tolerance=2e-5;
                if(u < -tolerance||w < -tolerance||u+w>1+tolerance||distance<ray.minimum||distance>maximum)continue;
                best={true,distance,index};maximum=distance;
            }
        }
        return best;
    }
};

struct Case
{
    Ray ray;
    uint32_t receiverInstance = 0, receiverTriangle = UINT32_MAX;
    uint32_t group = 0;
};

class SurfaceSampler
{
public:
    explicit SurfaceSampler(const VXGIWorld& world)
    {
        const auto& triangles = world.GetTriangles();
        for (uint32_t i = 0; i < triangles.size(); ++i)
        {
            const auto& triangle = triangles[i];
            const D e1 = Double(triangle.positions[1]) - Double(triangle.positions[0]);
            const D e2 = Double(triangle.positions[2]) - Double(triangle.positions[0]);
            const D cross = Cross(e1,e2);
            const double area = .5 * std::sqrt(Dot(cross,cross));
            if (!(area > 0)) continue;
            totalArea += area;
            cumulative.push_back(totalArea);
            indices.push_back(i);
            auto& bucket = instances[triangle.instanceId];
            bucket.area += area;
            bucket.cumulative.push_back(bucket.area);
            bucket.indices.push_back(i);
        }
        Require(!indices.empty(),"Actual scene has no non-degenerate triangles");
    }
    uint32_t Select(double unit, uint32_t instance = 0) const
    {
        const auto& values = instance ? instances.at(instance).cumulative : cumulative;
        const auto& order = instance ? instances.at(instance).indices : indices;
        const double area = instance ? instances.at(instance).area : totalArea;
        const size_t index = (std::min)(order.size()-1,size_t(std::lower_bound(values.begin(),values.end(),unit*area)-values.begin()));
        return order[index];
    }
    std::vector<uint32_t> InstanceIds() const
    {
        std::vector<uint32_t> result;
        for (const auto& item : instances) result.push_back(item.first);
        return result;
    }
    double Area() const { return totalArea; }
private:
    struct Bucket { double area = 0; std::vector<double> cumulative; std::vector<uint32_t> indices; };
    double totalArea = 0;
    std::vector<double> cumulative;
    std::vector<uint32_t> indices;
    std::map<uint32_t,Bucket> instances;
};

XMFLOAT3 Float(D value) { return {float(value.x),float(value.y),float(value.z)}; }
D Unit(D value) { return value * (1/std::sqrt(Dot(value,value))); }

std::vector<Case> MakeCases(const VXGIWorld& world,uint32_t count,float radius,float bias)
{
    std::mt19937 random(0x541cce71u);
    std::uniform_real_distribution<double> uniform(0,1);
    const SurfaceSampler sampler(world);
    const auto ids = sampler.InstanceIds();
    std::vector<Case> cases;
    cases.reserve(size_t(count)*4);
    for (uint32_t group = 0; group < 4; ++group)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            // Groups 0 and 2 use actual surface area; group 1 balances model
            // instances so the large floor cannot hide the car's local detail.
            const uint32_t index = sampler.Select(uniform(random),group==1 ? ids[i%ids.size()] : 0);
            const auto& triangle = world.GetTriangles()[index];
            const double root = std::sqrt(uniform(random)), v = uniform(random);
            const D center = Double(triangle.positions[0])*(1-root) +
                Double(triangle.positions[1])*(root*(1-v)) + Double(triangle.positions[2])*(root*v);
            const D normal = Unit(Double(triangle.geometricNormal));
            const D tangent = Unit(Cross(std::abs(normal.y)<.95 ? D{0,1,0} : D{1,0,0},normal));
            const D bitangent = Cross(normal,tangent);
            const double azimuth = uniform(random)*6.283185307179586;
            const double r = std::sqrt(uniform(random));
            D direction = tangent*(std::cos(azimuth)*r) + bitangent*(std::sin(azimuth)*r) + normal*std::sqrt(1-r*r);
            Case item;item.group=group;item.receiverInstance=triangle.instanceId;item.receiverTriangle=index;
            item.ray.minimum=0;item.ray.maximum=radius;
            D origin = center + normal*bias;
            if (group==2) direction=direction*-1;
            if (group==3)
            {
                // Finite .5 rays around the configured world bounds, including
                // proven AABB misses and partial intervals outside all assets.
                origin = {uniform(random)*20-10,uniform(random)*8-2,uniform(random)*20-10};
                direction = center-origin;
                item.receiverInstance=0;item.receiverTriangle=UINT32_MAX;
            }
            item.ray.origin=Float(origin);
            Require(Threshold::NormalizeDirection(Float(direction),item.ray.direction),"Ray normalization failed");
            cases.push_back(item);
        }
    }
    return cases;
}

Json Distribution(const std::vector<double>& values)
{
    if (values.empty()) return {{"count",0}};
    auto sorted=values;std::sort(sorted.begin(),sorted.end());
    double sum=0;for (double value:values) sum+=value;
    return {{"count",values.size()},{"mean",sum/double(values.size())},
        {"p50",sorted[(sorted.size()-1)/2]},{"p95",sorted[size_t(std::ceil(.95*double(sorted.size())))-1]},
        {"maximum",sorted.back()}};
}

std::vector<Scene> SingleModelScenes(const std::vector<SourceInstance>& sources,VXGIMeshSDFCache& cache,
    const VXGIMeshSDFCache::BuildSettings& settings)
{
    std::vector<Scene> result;
    for (const auto& source:sources)
    {
        if (source.isTransparent || !source.pModel) continue;
        Scene scene;Require(scene.UpdateInstances(std::vector<SourceInstance>{source},cache,settings),scene.GetLastError());
        result.push_back(std::move(scene));
    }
    return result;
}

Query::Result AllModels(const std::vector<Scene>& scenes,const Ray& ray)
{
    Query::Result result;result.status=Threshold::Status::Miss;
    for (const auto& scene:scenes)
    {
        auto next=Query::Trace(scene,ray.origin,ray.direction,ray.minimum,ray.maximum,ray.parameters);
        if (uint32_t(next.status)>1) return next;
        if (next.status!=Threshold::Status::Hit) continue;
        if (result.status!=Threshold::Status::Hit || next.distance<result.distance ||
            (next.distance==result.distance && next.instanceId<result.instanceId)) result=next;
    }
    return result;
}

struct Group
{
    uint32_t rays=0, geometryHit=0, approximateHit=0, trueHit=0, trueMiss=0, falsePositive=0, falseNegative=0;
    uint32_t wrongInstance=0, nearStart=0, nearStartSameReceiver=0, nearStartGeometryMiss=0;
    uint32_t zeroStart=0,zeroStartSameReceiver=0;
    uint32_t invalid=0, intervalFailures=0, finiteFailures=0, budgetFailures=0;
    uint32_t cpuGPUStatus=0,cpuGPUIdentity=0,cpuGPUDistance=0, allModelOmissions=0, allModelEarlier=0;
    uint64_t samples=0,candidates=0;
    double maximumCPUGPUDistanceError=0,maximumCPUGPUPositionError=0,maximumCPUGPUFieldError=0;
    std::vector<double> distanceError,signedError,hitDistances;
    Json examples=Json::array(),byReceiver=Json::object();
    Json Serialize() const
    {
        return {{"rays",rays},{"geometry_hits",geometryHit},{"approximate_hits",approximateHit},
            {"both_hit",trueHit},{"both_miss",trueMiss},{"false_positive_vs_triangles",falsePositive},
            {"false_negative_vs_triangles",falseNegative},{"different_nearest_instance",wrongInstance},
            {"near_start_hits_at_most_normal_bias_0_02",nearStart},{"near_start_same_receiver_instance",nearStartSameReceiver},
            {"zero_start_hits_at_most_1e_minus_5",zeroStart},{"zero_start_same_receiver_instance",zeroStartSameReceiver},
            {"near_start_geometry_miss",nearStartGeometryMiss},{"invalid_exhausted_stalled",invalid},
            {"finite_interval_failures",intervalFailures},{"nonfinite_failures",finiteFailures},{"two_candidate_budget_failures",budgetFailures},
            {"cpu_gpu_status_mismatches",cpuGPUStatus},{"cpu_gpu_instance_mismatches",cpuGPUIdentity},{"cpu_gpu_distance_mismatches_over_1e_minus_3",cpuGPUDistance},
            {"maximum_cpu_gpu_distance_error_world",maximumCPUGPUDistanceError},{"maximum_cpu_gpu_position_component_error_world",maximumCPUGPUPositionError},
            {"maximum_cpu_gpu_local_field_distance_error",maximumCPUGPUFieldError},
            {"top_two_omitted_all_model_hit",allModelOmissions},{"all_model_hit_earlier_than_top_two",allModelEarlier},
            {"average_node_samples",rays?double(samples)/rays:0},{"average_aabb_candidates",rays?double(candidates)/rays:0},
            {"absolute_distance_error_world",Distribution(distanceError)},{"signed_approx_minus_geometry_distance_world",Distribution(signedError)},
            {"approximate_hit_distance_world",Distribution(hitDistances)},{"by_originating_instance_and_cutout_flag",byReceiver},{"examples",examples}};
    }
    bool Sound() const
    {
        return invalid==0 && intervalFailures==0 && finiteFailures==0 && budgetFailures==0 &&
            cpuGPUStatus==0 && cpuGPUIdentity==0 && cpuGPUDistance==0;
    }
};

Json Diagnose(GPU* gpu,const Scene& scene,const VXGIWorld& world,const std::vector<Scene>& singles,
    const std::vector<Case>& cases)
{
    std::vector<Ray> rays;for (const auto& item:cases) rays.push_back(item.ray);
    std::vector<Hit> actual;
    if (gpu) actual=gpu->Dispatch(scene,rays);
    const Reference reference(world);
    std::array<Group,4> groups;
    for (size_t i=0;i<cases.size();++i)
    {
        const auto& item=cases[i];const auto& ray=item.ray;auto& group=groups[item.group];++group.rays;
        Query::Stats stats;const auto cpu=Query::Trace(scene,ray.origin,ray.direction,ray.minimum,ray.maximum,ray.parameters,&stats);
        Hit hit={};
        if (gpu) hit=actual[i];
        else
        {
            hit.position=cpu.position;hit.distance=cpu.distance;hit.fieldDistance=cpu.fieldDistance;
            hit.status=uint32_t(cpu.status);hit.instanceId=cpu.instanceId;hit.instanceIndex=cpu.instanceIndex;
            hit.sampleCount=stats.samples;hit.candidateCount=stats.candidates;
        }
        Ray exactRay=ray;
        // Use the same robust FP32 direction normalization as the shader before
        // sending it to the independent double triangle oracle, with no change
        // to the original finite world interval.
        Require(Threshold::NormalizeDirection(ray.direction,exactRay.direction),"Exact comparison normalization failed");
        const auto exact=reference.Trace(exactRay,true);
        const auto all=AllModels(singles,ray);
        const bool found=hit.status==1,expected=exact.hit;
        if (expected) ++group.geometryHit;
        if (found) ++group.approximateHit;
        if (found && expected) ++group.trueHit;
        else if (!found && !expected) ++group.trueMiss;
        else if (found) ++group.falsePositive;
        else ++group.falseNegative;
        if (hit.status>1 || uint32_t(all.status)>1) ++group.invalid;
        if (hit.status!=uint32_t(cpu.status)) ++group.cpuGPUStatus;
        if (found && cpu.status==Threshold::Status::Hit)
        {
            if (hit.instanceId!=cpu.instanceId) ++group.cpuGPUIdentity;
            if (std::abs(hit.distance-cpu.distance)>1e-3f) ++group.cpuGPUDistance;
            maximumError=(std::max)(maximumError,std::abs(hit.distance-cpu.distance));
            group.maximumCPUGPUDistanceError=(std::max)(group.maximumCPUGPUDistanceError,double(std::abs(hit.distance-cpu.distance)));
            group.maximumCPUGPUPositionError=(std::max)(group.maximumCPUGPUPositionError,double(Difference(hit.position,cpu.position)));
            group.maximumCPUGPUFieldError=(std::max)(group.maximumCPUGPUFieldError,double(std::abs(hit.fieldDistance-cpu.fieldDistance)));
        }
        if (hit.candidateCount>2 || hit.sampleCount>2*ray.parameters.maxSteps) ++group.budgetFailures;
        group.samples+=hit.sampleCount;group.candidates+=hit.candidateCount;
        if (all.status==Threshold::Status::Hit)
        {
            if (!found) ++group.allModelOmissions;
            else if (all.distance+1e-3f<hit.distance) ++group.allModelEarlier;
        }
        if (found)
        {
            if (!std::isfinite(hit.distance)||!std::isfinite(hit.fieldDistance)||!std::isfinite(hit.position.x)||
                !std::isfinite(hit.position.y)||!std::isfinite(hit.position.z)) ++group.finiteFailures;
            if (hit.distance<ray.minimum || hit.distance>ray.maximum) ++group.intervalFailures;
            group.hitDistances.push_back(hit.distance);
            if (hit.distance<=1e-5f)
            {
                ++group.zeroStart;
                if(hit.instanceId==item.receiverInstance)++group.zeroStartSameReceiver;
            }
            if (hit.distance<=.02f)
            {
                ++group.nearStart;
                if (hit.instanceId==item.receiverInstance) ++group.nearStartSameReceiver;
                if (!expected) ++group.nearStartGeometryMiss;
            }
            if (expected)
            {
                const double error=double(hit.distance)-exact.distance;
                group.signedError.push_back(error);group.distanceError.push_back(std::abs(error));
                if (hit.instanceId!=world.GetTriangles()[exact.triangle].instanceId) ++group.wrongInstance;
            }
        }
        const bool originCutout=item.receiverTriangle!=UINT32_MAX && world.GetTriangles()[item.receiverTriangle].isCutout;
        const std::string bucket="instance_"+std::to_string(item.receiverInstance)+(originCutout?"_cutout":"_opaque");
        auto& grouped=group.byReceiver[bucket];
        if(grouped.is_null())grouped={{"rays",0},{"geometry_hits",0},{"approximate_hits",0},{"false_positive_vs_triangles",0},
            {"false_negative_vs_triangles",0},{"zero_start_hit",0},{"zero_start_same_receiver_instance",0},{"near_start_same_receiver_instance",0}};
        grouped["rays"]=grouped["rays"].get<uint32_t>()+1;
        if(expected)grouped["geometry_hits"]=grouped["geometry_hits"].get<uint32_t>()+1;
        if(found)grouped["approximate_hits"]=grouped["approximate_hits"].get<uint32_t>()+1;
        if(found&&!expected)grouped["false_positive_vs_triangles"]=grouped["false_positive_vs_triangles"].get<uint32_t>()+1;
        if(!found&&expected)grouped["false_negative_vs_triangles"]=grouped["false_negative_vs_triangles"].get<uint32_t>()+1;
        if(found&&hit.distance<=1e-5f)grouped["zero_start_hit"]=grouped["zero_start_hit"].get<uint32_t>()+1;
        if(found&&hit.instanceId==item.receiverInstance&&hit.distance<=1e-5f)
            grouped["zero_start_same_receiver_instance"]=grouped["zero_start_same_receiver_instance"].get<uint32_t>()+1;
        if(found&&hit.instanceId==item.receiverInstance&&hit.distance<=.02f)
            grouped["near_start_same_receiver_instance"]=grouped["near_start_same_receiver_instance"].get<uint32_t>()+1;
        if ((found!=expected || (found && expected && std::abs(double(hit.distance)-exact.distance)>.1)) && group.examples.size()<8)
            group.examples.push_back({{"ray",i},{"receiver_instance",item.receiverInstance},{"receiver_triangle",item.receiverTriangle},
                {"origin",{ray.origin.x,ray.origin.y,ray.origin.z}},{"direction",{ray.direction.x,ray.direction.y,ray.direction.z}},
                {"ray_maximum",ray.maximum},{"geometry_hit",expected},{"geometry_distance",exact.distance},
                {"approximate_hit",found},{"approximate_distance",hit.distance},{"approximate_instance",hit.instanceId},
                {"status",hit.status},{"candidate_count",hit.candidateCount},{"field_distance",hit.fieldDistance}});
    }
    Json result;bool sound=true;
    const char* names[]={"area_weighted_outward","instance_balanced_outward","area_weighted_inward","finite_world_rays"};
    for (uint32_t i=0;i<groups.size();++i) {result[names[i]]=groups[i].Serialize();sound=sound&&groups[i].Sound();}
    result["implementation_invariants_passed"]=sound;
    result["quality_acceptance_requires_review"]=true;
    return result;
}
}

int main(int argc,char** argv)
{
    try
    {
        using namespace ThresholdAcceptance;
        uint32_t count=8192;bool cpuOnly=false;
        for (int i=1;i<argc;++i)
        {
            const std::string option=argv[i];
            if (option=="--rays" && i+1<argc) count=uint32_t(std::stoul(argv[++i]));
            else if (option=="--cpu-only") cpuOnly=true;
            else throw std::runtime_error("Unknown threshold acceptance argument "+option);
        }
        Json config,pipeline;std::ifstream("Settings/Scene.json")>>config;std::ifstream("Settings/Pipeline.json")>>pipeline;
        std::vector<ThresholdAcceptance::Model> models;std::vector<SourceInstance> sources;models.reserve(config["instances"].size());uint32_t material=0;
        for (const auto& item:config["instances"])
        {
            models.push_back(Load(item["model_path"],material));SourceInstance source;source.pModel=&models.back();
            source.editorId=uint32_t(sources.size()+1);source.translation=V3(item["pos"]);source.rotation=V3(item["rot"]);source.scale=V3(item["scale"]);
            source.isCutout=item.value("is_cutout",false);source.isTransparent=item.value("is_transparent",false);sources.push_back(source);
        }
        const VXGIMeshSDFCache::BuildSettings settings={pipeline["vxgi"]["sdf"]["cell_size"].get<float>()};
        VXGIMeshSDFCache cache;Scene scene;Require(scene.UpdateInstances(sources,cache,settings),scene.GetLastError());
        VXGIWorld world;Require(world.Update(sources,pipeline["vxgi"]["voxel_size"].get<float>()),world.GetLastError());
        const auto singles=SingleModelScenes(sources,cache,settings);
        const float radius=pipeline["ssgi"]["radius"].get<float>(),bias=.02f;
        const auto cases=MakeCases(world,count,radius,bias);
        Json report;
        report["scope"]="Configured LOD0 models, production Assimp flags/node transforms and TRS. Approximate nearest-two threshold query compared against independent world-space double triangle BVH. Outward/inward surface rays use geometric normals, not normal maps or raster-visible samples. Original finite interval [0, configured SSGI radius], fixed production normal bias .02. Alpha-tested textures intentionally excluded from both geometry references.";
        report["sampling"]="Four fixed-seed groups with requested rays per group. Surface points use triangle area weighting; cosine hemisphere directions. Instance-balanced group draws equally from each included model, area weighted within each. World rays cover [-10,10] x [-2,6] x [-10,10], point toward sampled surfaces but retain the finite radius.";
        report["quality_interpretation"]="False positives, false negatives and distance errors measure the deliberate approximation versus actual triangles; they are reported without being treated as CPU/GPU correctness failures. Near-start same-receiver hits expose coarse threshold-shell self-hit risk. All-model diagnostic runs every instance independently with the same threshold helper; top-two misses relative to this separate candidate-budget loss from field approximation.";
        report["scene"]={{"models",sources.size()},{"triangles",world.GetTriangles().size()},{"surface_area",SurfaceSampler(world).Area()},
            {"local_cell_size",settings.cellSize},{"radius_world",radius},{"normal_bias_world",bias},{"threshold_cells",.9f}};
        report["assets"]=Json::array();
        for (const auto& instance:scene.GetInstances())
        {
            const auto& grid=instance.asset->GetSDF().GetGrid();
            report["assets"].push_back({{"instance_id",instance.instanceId},{"local_cell_size",grid.cellSize},{"local_threshold",grid.cellSize*.9f},
                {"cell_dimensions",grid.cellDimensions},{"world_model_min",{instance.boundsMinimum.x,instance.boundsMinimum.y,instance.boundsMinimum.z}},
                {"world_model_max",{instance.boundsMaximum.x,instance.boundsMaximum.y,instance.boundsMaximum.z}}});
        }
        std::unique_ptr<GPU> gpu;if(!cpuOnly) gpu=std::make_unique<GPU>();
        const auto begin=Clock::now();report["quality"]=Diagnose(gpu.get(),scene,world,singles,cases);report["diagnostic_wall_ms"]=Elapsed(begin);
        report["cpu_only"]=cpuOnly;report["no_hwnd_no_present"]=true;report["gpu_based_validation"]=!cpuOnly;
        report["gpu_warnings"]=gpu?gpu->Warnings():0;report["implementation_invariants_passed"]=report["quality"]["implementation_invariants_passed"].get<bool>()&&report["gpu_warnings"]==0;
        if(gpu)report["adapter"]=gpu->adapterName;
        const std::string path=cpuOnly?"tmp/VXGIMeshSDFThresholdAcceptance-CPU-report.json":"tmp/VXGIMeshSDFThresholdAcceptance-GPU-report.json";
        std::ofstream(path)<<std::setw(2)<<report;
        std::cout<<std::setw(2)<<report<<'\n';Require(report["implementation_invariants_passed"].get<bool>(),"Threshold acceptance invariant failures; inspect report");return 0;
    }
    catch(const std::exception& error)
    {std::cerr<<"Actual-scene threshold acceptance FAILED: "<<error.what()<<'\n';return 1;}
}
