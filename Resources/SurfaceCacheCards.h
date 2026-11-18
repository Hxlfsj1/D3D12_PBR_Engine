#pragma once

#include "VXGISDF.h"
#include <climits>
#include <numeric>

// Six-direction surface parameterization, built once per immutable mesh asset.
// Algorithm reference: UE 5.8 MeshCardRepresentationUtilities (surfel visibility,
// capture-near-plane clustering, weighted coverage budget). Units here are meters.
// No material capture, lighting, virtual pages or streaming live in this module.
class SurfaceCacheCards
{
public:
    using Vec = DirectX::XMFLOAT3;
    struct Settings
    {
        float cellSize = 0.1f; // UE's 10 cm target detail, independent of Mesh SDF voxels.
        uint32_t maxCards = 12;
        bool operator==(const Settings&) const = default;
    };
    struct Basis
    {
        Vec origin{}, x{}, y{}, forward{};
        std::array<int, 3> size{};
        float cellSize = 0;
    };
    struct Surfel
    {
        std::array<int, 3> coord{};
        int minRayZ = 0, cardIndex = -1;
        Vec position{}, normal{};
        float coverage = 0, visibility = 0, weightedCoverage = 0;
    };
    struct Card
    {
        Vec origin{}, extent{}, axisX{}, axisY{}, axisZ{};
        uint32_t direction = 0, surfelCount = 0;
        int nearPlane = 0;
        float coverage = 0, weightedCoverage = 0;
    };
    struct Data
    {
        Settings settings;
        std::array<Basis, 6> bases;
        std::array<std::vector<Surfel>, 6> surfels;
        std::vector<Card> cards;
        std::array<uint32_t, 6> lookup{};
        uint32_t candidates = 0, surfelCount = 0, coveredSurfels = 0;
        float totalWeight = 0, coveredWeight = 0, cellSize = 0;
        double buildMilliseconds = 0;
    };
    // Debug-only per-instance view of the asset-local Card geometry.
    struct DebugCard
    {
        Vec origin; uint32_t instanceIndex;
        Vec axisX; float extentX;
        Vec axisY; float extentY;
        Vec axisZ; float extentZ;
        uint32_t direction, cardIndex; float cellSize, padding = 0;
    };
    static_assert(sizeof(DebugCard) == 80);

    static Vec Add(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    static Vec Sub(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    static Vec Mul(Vec v, float s) { return {v.x*s,v.y*s,v.z*s}; }
    static float Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    static Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    static float& Axis(Vec& v, int i) { return i==0?v.x:i==1?v.y:v.z; }
    static float Axis(const Vec& v, int i) { return i==0?v.x:i==1?v.y:v.z; }
    static Vec Point(const Basis& b, float x, float y, float z)
    {
        return Add(b.origin, Mul(Add(Add(Mul(b.x,x),Mul(b.y,y)),Mul(b.forward,z)),b.cellSize));
    }

    static Data Build(const VXGISDF& geometry, const Settings& settings)
    {
        const auto start = std::chrono::steady_clock::now();
        Data data; data.settings = settings;
        if (geometry.GetTriangles().empty()) return data;
        Vec minimum = geometry.GetTriangles()[0].positions[0], maximum = minimum;
        for (const auto& triangle : geometry.GetTriangles())
            for (Vec p : triangle.positions)
                for (int a=0;a<3;++a)
                {
                    Axis(minimum,a) = (std::min)(Axis(minimum,a),Axis(p,a));
                    Axis(maximum,a) = (std::max)(Axis(maximum,a),Axis(p,a));
                }
        const Vec center = Mul(Add(minimum,maximum),0.5f);
        // A one-centimeter geometry margin also gives flat meshes a capture slab.
        Vec size = Add(Sub(maximum,minimum),{0.02f,0.02f,0.02f});
        constexpr int maxAxisCells = 64;
        const float h = (std::max)(settings.cellSize,(std::max)({size.x,size.y,size.z})/maxAxisCells);
        std::array<int,3> dims;
        for (int a=0;a<3;++a) dims[a]=std::clamp(int(std::round(Axis(size,a)/h)),1,maxAxisCells);
        const Vec gridMin = Sub(center,{dims[0]*h*0.5f,dims[1]*h*0.5f,dims[2]*h*0.5f});
        const Vec gridMax = Add(center,{dims[0]*h*0.5f,dims[1]*h*0.5f,dims[2]*h*0.5f});
        data.cellSize = h;
        std::array<std::vector<Cluster>,6> clusters;
        const float averageFaceArea = float(dims[0]*dims[1]+dims[0]*dims[2]+dims[1]*dims[2])/3;
        const float minOuterCoverage = (std::min)(15.0f,averageFaceArea*0.5f);
        for (uint32_t direction=0;direction<6;++direction)
        {
            auto& b=data.bases[direction]; b.cellSize=h;
            const int axis=int(direction/2), u=axis==0?1:0, v=axis==2?1:2;
            Axis(b.x,u)=1; Axis(b.y,v)=1;
            Axis(b.forward,axis)=(direction&1)?-1.0f:1.0f;
            b.origin=gridMin; if(direction&1) Axis(b.origin,axis)=Axis(gridMax,axis);
            b.size={dims[u],dims[v],dims[axis]};
            GenerateSurfels(geometry,b,data.surfels[direction]);
            auto& surfels=data.surfels[direction];
            std::vector<bool> assigned(surfels.size(),false);
            auto outer=MakeCluster(0,surfels,assigned);
            if (Valid(outer,minOuterCoverage)) Commit(std::move(outer),assigned,clusters[direction]);
            for (;;)
            {
                Cluster best;
                for (int nearPlane=1;nearPlane<b.size[2];++nearPlane)
                {
                    auto candidate=MakeCluster(nearPlane,surfels,assigned);
                    if (Valid(candidate,15.0f) && candidate.weight>best.weight) best=std::move(candidate);
                }
                if (best.indices.empty()) break;
                Commit(std::move(best),assigned,clusters[direction]);
            }
            data.candidates+=uint32_t(clusters[direction].size());
            std::stable_sort(clusters[direction].begin(),clusters[direction].end(),
                [](const Cluster& a,const Cluster& b){return a.weight>b.weight;});
        }
        uint32_t count=data.candidates;
        while(count>settings.maxCards)
        {
            int worst=-1;
            for(int d=0;d<6;++d)
                if(!clusters[d].empty() && (worst<0 || clusters[d].back().weight<clusters[worst].back().weight)) worst=d;
            clusters[worst].pop_back(); --count;
        }
        for(uint32_t d=0;d<6;++d)
        {
            auto& surfels=data.surfels[d]; const auto& b=data.bases[d];
            data.surfelCount+=uint32_t(surfels.size());
            for(const auto& s:surfels) data.totalWeight+=s.weightedCoverage;
            for(const auto& cluster:clusters[d])
            {
                const uint32_t index=uint32_t(data.cards.size());
                Vec lo={float(cluster.lo[0]),float(cluster.lo[1]),float(cluster.lo[2])-0.5f};
                Vec hi={float(cluster.hi[0]+1),float(cluster.hi[1]+1),float(cluster.hi[2])+1.5f};
                // Clamp XY to mesh bounds; Z retains one detail-cell capture margin.
                for(int a=0;a<2;++a)
                {
                    const Vec basisAxis=a==0?b.x:b.y;
                    float meshLo=std::numeric_limits<float>::infinity(),meshHi=-meshLo;
                    for(int corner=0;corner<8;++corner)
                    {
                        Vec p={corner&1?maximum.x:minimum.x,corner&2?maximum.y:minimum.y,corner&4?maximum.z:minimum.z};
                        const float coordinate=Dot(Sub(p,b.origin),basisAxis)/h;
                        meshLo=(std::min)(meshLo,coordinate);meshHi=(std::max)(meshHi,coordinate);
                    }
                    Axis(lo,a)=(std::max)(Axis(lo,a),meshLo-0.01f/h);
                    Axis(hi,a)=(std::min)(Axis(hi,a),meshHi+0.01f/h);
                }
                Card card;
                card.origin=Point(b,(lo.x+hi.x)*0.5f,(lo.y+hi.y)*0.5f,(lo.z+hi.z)*0.5f);
                card.extent=Mul(Sub(hi,lo),h*0.5f);
                card.axisX=b.x;card.axisY=b.y;card.axisZ=Mul(b.forward,-1);
                card.direction=d;card.nearPlane=cluster.nearPlane;
                card.surfelCount=uint32_t(cluster.indices.size());
                card.coverage=cluster.coverage;card.weightedCoverage=cluster.weight;
                for(uint32_t s:cluster.indices) surfels[s].cardIndex=int(index);
                data.coveredSurfels+=card.surfelCount;data.coveredWeight+=cluster.weight;
                data.lookup[d]|=1u<<index;data.cards.push_back(card);
            }
        }
        data.buildMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        return data;
    }

private:
    struct Sample { Vec position,normal; int z,minRayZ; };
    struct Cluster
    {
        std::vector<uint32_t> indices;
        std::array<int,3> lo={INT_MAX,INT_MAX,INT_MAX},hi={INT_MIN,INT_MIN,INT_MIN};
        int nearPlane=0;float coverage=0,weight=0;
    };
    static bool Valid(const Cluster& c,float minimum)
    {
        return !c.indices.empty() && c.weight>=minimum &&
            c.coverage/float((c.hi[0]-c.lo[0]+1)*(c.hi[1]-c.lo[1]+1))>0.2f/3.0f;
    }
    static Cluster MakeCluster(int nearPlane,const std::vector<Surfel>& surfels,const std::vector<bool>& assigned)
    {
        Cluster c;c.nearPlane=nearPlane;
        for(uint32_t i=0;i<surfels.size();++i)
        {
            const auto& s=surfels[i];
            if(assigned[i] || s.coord[2]<nearPlane || s.minRayZ>nearPlane) continue;
            c.indices.push_back(i);c.coverage+=s.coverage;c.weight+=s.weightedCoverage;
            for(int a=0;a<3;++a){c.lo[a]=(std::min)(c.lo[a],s.coord[a]);c.hi[a]=(std::max)(c.hi[a],s.coord[a]);}
        }
        return c;
    }
    static void Commit(Cluster c,std::vector<bool>& assigned,std::vector<Cluster>& clusters)
    {
        for(uint32_t i:c.indices) assigned[i]=true;
        clusters.push_back(std::move(c));
    }
    static uint32_t ReverseBits(uint32_t v)
    {
        v=(v>>16)|(v<<16);v=((v&0xff00ff00)>>8)|((v&0x00ff00ff)<<8);
        v=((v&0xf0f0f0f0)>>4)|((v&0x0f0f0f0f)<<4);v=((v&0xcccccccc)>>2)|((v&0x33333333)<<2);
        return ((v&0xaaaaaaaa)>>1)|((v&0x55555555)<<1);
    }
    static float RadicalInverse(uint32_t v) { return float(double(ReverseBits(v))/4294967296.0); }
    static std::pair<float,bool> Visibility(const VXGISDF& geometry,const std::vector<Sample>& samples,size_t begin,size_t end,float maxDistance)
    {
        uint32_t hits=0,backFaces=0;float visible=0;
        for(uint32_t i=0;i<32;++i)
        {
            const auto& s=samples[begin+i%(end-begin)];
            const Vec n=s.normal;
            // Orthonormal tangent frame; 32 deterministic uniform-hemisphere samples.
            Vec t=std::abs(n.z)<0.999f?Cross({0,0,1},n):Cross({0,1,0},n);
            t=Mul(t,1/std::sqrt(Dot(t,t)));const Vec b=Cross(n,t);
            const float z=(i+0.5f)/32,r=std::sqrt(1-z*z),phi=6.28318530718f*RadicalInverse(i);
            const Vec ray=Add(Add(Mul(t,r*std::cos(phi)),Mul(b,r*std::sin(phi))),Mul(n,z));
            const auto hit=geometry.TraceReference(s.position,ray,0.001f,maxDistance);
            if(hit.hit){++hits;if(hit.backFace)++backFaces;}else visible+=1;
        }
        return {visible/32,!(hits>0.8f*32 && backFaces>0.2f*32)};
    }
    static void GenerateSurfels(const VXGISDF& geometry,const Basis& b,std::vector<Surfel>& surfels)
    {
        const float h=b.cellSize,pullback=2*h;
        const auto& grid=geometry.GetGrid();
        const Vec diagonal=Sub(grid.boundsMaximum,grid.origin);
        const float rayMaximum=std::sqrt(Dot(diagonal,diagonal))+4*h;
        std::vector<Sample> samples;
        for(int y=0;y<b.size[1];++y) for(int x=0;x<b.size[0];++x)
        {
            samples.clear();
            for(uint32_t i=0;i<32;++i)
            {
                const Vec origin=Sub(Point(b,x+(i+0.5f)/32,y+RadicalInverse(i),0),Mul(b.forward,pullback));
                int lastZ=-2;float minimum=0;
                while(lastZ+1<b.size[2])
                {
                    const auto hit=geometry.TraceReference(origin,b.forward,minimum,rayMaximum);
                    if(!hit.hit) break;
                    const int z=std::clamp(int((hit.distance-pullback)/h),0,b.size[2]-1);
                    if(Dot(Mul(b.forward,-1),hit.geometricNormal)>=0.25f && z>lastZ+1)
                        samples.push_back({hit.position,hit.geometricNormal,z,(std::max)(0,lastZ+1)});
                    lastZ=z;
                    minimum=std::nextafter((std::max)(pullback+(z+1)*h,hit.distance),std::numeric_limits<float>::infinity());
                }
            }
            std::sort(samples.begin(),samples.end(),[](const Sample& a,const Sample& c)
            {
                if(a.z!=c.z)return a.z<c.z;
                if(a.minRayZ!=c.minRayZ)return a.minRayZ>c.minRayZ;
                if(a.position.x!=c.position.x)return a.position.x<c.position.x;
                if(a.position.y!=c.position.y)return a.position.y<c.position.y;
                return a.position.z<c.position.z;
            });
            for(size_t begin=0;begin<samples.size();)
            {
                size_t end=begin+1;
                while(end<samples.size() && samples[end].z==samples[begin].z && samples[end].minRayZ==samples[begin].minRayZ)++end;
                const auto [visibility,valid]=Visibility(geometry,samples,begin,end,rayMaximum);
                if(valid)
                {
                    const auto& sample=samples[begin];
                    Surfel s;s.coord={x,y,sample.z};s.minRayZ=sample.minRayZ;s.position=sample.position;s.normal=sample.normal;
                    s.coverage=float(end-begin)/32;s.visibility=visibility;s.weightedCoverage=s.coverage*(1+visibility);
                    surfels.push_back(s);
                }
                begin=end;
            }
        }
    }
};
