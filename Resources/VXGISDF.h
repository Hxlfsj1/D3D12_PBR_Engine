#pragma once

#include "VXGIWorld.h"
#include "VXGITraceAcceleration.h"
#include <future>
#include <numeric>
#include <sstream>
#include <thread>

// Static, unsigned node distances. Open surfaces deliberately have no interior
// sign. The dense distance grid and sparse triangle CSR share one x-fast lattice.
class VXGISDF
{
public:
    struct Grid
    {
        DirectX::XMFLOAT3 origin = {};
        DirectX::XMFLOAT3 boundsMaximum = {};
        float cellSize = 0;
        std::array<uint32_t, 3> nodeDimensions = {};
        std::array<uint32_t, 3> cellDimensions = {};
        bool valid = false;
    };
    struct Hit
    {
        bool hit = false;
        float distance = 0;
        DirectX::XMFLOAT3 position = {};
        DirectX::XMFLOAT3 geometricNormal = {};
        DirectX::XMFLOAT3 barycentrics = {}; // Weights of vertices 0, 1, 2.
        uint32_t triangleIndex = UINT32_MAX;
        uint32_t materialId = UINT32_MAX;
        uint32_t instanceId = 0;
        bool backFace = false;
    };
    struct Stats
    {
        uint64_t triangles = 0;
        uint64_t bvhNodes = 0;
        uint64_t nodes = 0;
        uint64_t cells = 0;
        uint64_t occupiedCells = 0;
        uint64_t triangleReferences = 0;
        uint64_t candidateBoxTests = 0;
        uint64_t memoryBytes = 0;
        uint64_t worldRevision = 0;
        uint64_t worldGeometryRevision = 0;
        uint64_t worldMaterialRevision = 0;
        double buildMilliseconds = 0;
        double distanceBakeMilliseconds = 0;
        double identityRefreshMilliseconds = 0;
        bool geometryRebuilt = false; // Last committed update, not a per-frame flag.
        double maximumNodePositionMargin = 0;
    };

    bool Update(const VXGIWorld& world, float cellSize, uint32_t maxAxisResolution = 128)
    {
        if (!ValidateBuildSettings(cellSize, maxAxisResolution)) return false;
        const bool reusableGeometry = m_initialized && m_sourceWorld == &world &&
            m_data.stats.worldGeometryRevision == world.GetGeometryRevision() &&
            cellSize == m_data.grid.cellSize && maxAxisResolution == m_maxAxisResolution &&
            m_data.triangles.size() == world.GetTriangles().size();
        if (reusableGeometry && m_data.stats.worldRevision == world.GetRevision())
        {
            m_error.clear();
            return true;
        }
        const auto start = std::chrono::steady_clock::now();
        try
        {
            if (reusableGeometry)
            {
                // Surface identity/material edits cannot change distance data or
                // BVH/CSR. Retain all their buffers and update only hit metadata.
                for(size_t i=0;i<m_data.triangles.size();++i)
                {
                    const auto& source=world.GetTriangles()[i];
                    auto& destination=m_data.triangles[i];
                    destination.materialId=source.materialId;
                    destination.sourceInstanceIndex=source.sourceInstanceIndex;
                    destination.isCutout=source.isCutout;
                }
                if(m_data.stats.worldMaterialRevision!=world.GetMaterialRevision()) ++m_materialRevision;
                m_data.stats.worldRevision=world.GetRevision();
                m_data.stats.worldMaterialRevision=world.GetMaterialRevision();
                m_data.stats.geometryRebuilt=false;
                m_data.stats.distanceBakeMilliseconds=0;
                m_data.stats.buildMilliseconds=m_data.stats.identityRefreshMilliseconds=Milliseconds(start);
                ++m_revision;
                m_error.clear();
                return true;
            }
            Storage next;
            next.stats.worldRevision = world.GetRevision();
            next.stats.worldGeometryRevision = world.GetGeometryRevision();
            next.stats.worldMaterialRevision = world.GetMaterialRevision();
            next.stats.geometryRebuilt = true;
            next.triangles = world.GetTriangles();
            if (!BuildGeometry(next, cellSize, maxAxisResolution)) return false;
            next.stats.buildMilliseconds = Milliseconds(start);
            next.stats.memoryBytes = EstimateBytes(next);
            if(!m_initialized || m_data.stats.worldMaterialRevision!=world.GetMaterialRevision()) ++m_materialRevision;
            m_data = std::move(next);
            m_maxAxisResolution = maxAxisResolution;
            m_sourceWorld = &world;
            m_initialized = true;
            ++m_geometryRevision;
            ++m_revision;
            m_error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            m_error = std::string("SDF build failed: ") + exception.what();
            return false;
        }
    }

    const Grid& GetGrid() const { return m_data.grid; }
    const std::vector<float>& GetNodes() const { return m_data.distances; }
    const std::vector<float>& GetDistances() const { return m_data.distances; }
    const std::vector<uint32_t>& GetCellOffsets() const { return m_data.cellOffsets; }
    const std::vector<uint32_t>& GetTriangleReferences() const { return m_data.triangleReferences; }
    // CPU tracing remains an independent CSR/BVH reference. Production GPU
    // consumers use optional per-cell stackless bounds in the same two SRVs.
    const std::vector<uint32_t>& GetGPUCellOffsets() const
    {
        return m_data.traceAcceleration.offsets.empty() ? m_data.cellOffsets : m_data.traceAcceleration.offsets;
    }
    const std::vector<uint32_t>& GetGPUTraceReferences() const
    {
        return m_data.traceAcceleration.references.empty() ? m_data.triangleReferences : m_data.traceAcceleration.references;
    }
    const VXGITraceAcceleration::Stats& GetTraceAccelerationStats() const { return m_data.traceAcceleration.stats; }
    const std::vector<VXGIWorld::Triangle>& GetTriangles() const { return m_data.triangles; }
    const Stats& GetStats() const { return m_data.stats; }
    uint64_t GetRevision() const { return m_revision; }
    uint64_t GetGeometryRevision() const { return m_geometryRevision; }
    uint64_t GetMaterialRevision() const { return m_materialRevision; }
    uint64_t Revision() const { return m_revision; }
    const std::string& GetLastError() const { return m_error; }

    double NearestDistance(const DirectX::XMFLOAT3& position) const
    {
        if (!Finite(position) || m_data.bvh.empty()) return std::numeric_limits<double>::infinity();
        return std::sqrt(NearestSquared(m_data, Vec(position)));
    }

    // Independent BVH triangle tracing is the correctness reference for the grid.
    Hit TraceReference(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
        float maxDistance = 1000.0f) const
    {
        Vec rayOrigin, rayDirection;
        if (!PrepareRay(origin, direction, maxDistance, rayOrigin, rayDirection) || m_data.bvh.empty()) return {};
        Hit best;
        double bestDistance = maxDistance;
        std::array<uint32_t, 128> stack;
        size_t count = 1; stack[0] = 0;
        while (count)
        {
            const auto& node = m_data.bvh[stack[--count]];
            double enter, exit;
            if (!RayBox(rayOrigin, rayDirection, node.bounds, 0, bestDistance, enter, exit)) continue;
            if (node.count)
            {
                for (uint32_t i = 0; i < node.count; ++i)
                    ConsiderHit(m_data, m_data.order[node.first+i], rayOrigin, rayDirection, 0, maxDistance, bestDistance, best);
            }
            else
            {
                // Balanced median BVH has depth < 32 for our bounded triangle set.
                stack[count++] = node.left; stack[count++] = node.right;
            }
        }
        return best;
    }

    // GPU golden contract: nearest node D - |p-node| is a conservative lower
    // bound, unlike ordinary trilinear interpolation of unsigned distances.
    // All candidates of a closed cell receive an exact two-sided test over that
    // cell's ray interval. A zero lower bound advances to the exact cell exit;
    // it never becomes an arbitrary positive minimum sphere-tracing step.
    Hit Trace(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
        float maxDistance = 1000.0f) const
    {
        Vec rayOrigin, rayDirection;
        if (!m_data.grid.valid || !PrepareRay(origin, direction, maxDistance, rayOrigin, rayDirection)) return {};
        const Bounds bounds = GridBounds(m_data.grid);
        double enter, exit;
        if (!RayBox(rayOrigin, rayDirection, bounds, 0, maxDistance, enter, exit)) return {};
        double current = enter;
        const double epsilon = double(m_data.grid.cellSize) * 1e-4;
        const uint32_t iterationLimit = 4 * (m_data.grid.cellDimensions[0] + m_data.grid.cellDimensions[1] +
            m_data.grid.cellDimensions[2]) + 16;
        for (uint32_t iteration = 0; iteration < iterationLimit && current <= exit; ++iteration)
        {
            const Vec position = Add(rayOrigin, Scale(rayDirection, current));
            std::array<uint32_t, 3> cell;
            if (!LocateCell(m_data.grid, position, rayDirection, cell)) break;
            const double cellExit = (std::min)(exit, CellExit(m_data.grid, cell, rayOrigin, rayDirection));
            const uint32_t flat = CellIndex(m_data.grid, cell[0], cell[1], cell[2]);
            Hit best;
            double bestDistance = (std::min)(double(maxDistance), cellExit + epsilon);
            for (uint32_t i = m_data.cellOffsets[flat]; i < m_data.cellOffsets[flat+1]; ++i)
                ConsiderHit(m_data, m_data.triangleReferences[i], rayOrigin, rayDirection,
                    (std::max)(0.0, current-epsilon), (std::min)(double(maxDistance), cellExit+epsilon), bestDistance, best);
            if (best.hit) return best;
            if (cellExit >= exit) break;
            const double lowerBound = NodeLowerBound(m_data, position, epsilon);
            const double sphereStep = 0.9 * lowerBound;
            // A valid distance lower bound can skip subsequent empty space. If
            // it does not clear this cell, exact interval testing already proved
            // this entire segment clear; advance all tied DDA axes together.
            const double next = sphereStep > cellExit-current ? (std::min)(exit, current+sphereStep) : cellExit;
            if (next <= current) break;
            current = next;
        }
        return {};
    }

private:
    friend class VXGIMeshSDFCache;
    friend class VXGIMeshSDFScene;

    struct Vec
    {
        double x = 0, y = 0, z = 0;
        Vec() = default;
        Vec(double px, double py, double pz) : x(px), y(py), z(pz) {}
        explicit Vec(DirectX::XMFLOAT3 value) : x(value.x), y(value.y), z(value.z) {}
        double operator[](size_t axis) const { return axis == 0 ? x : axis == 1 ? y : z; }
    };
    struct Bounds { Vec minimum, maximum; };
    struct Primitive { Bounds bounds; Vec centroid; };
    struct BVHNode
    {
        Bounds bounds;
        uint32_t left = 0, right = 0, first = 0, count = 0;
    };
    struct Storage
    {
        Grid grid;
        Stats stats;
        std::vector<VXGIWorld::Triangle> triangles;
        std::vector<Primitive> primitives;
        std::vector<uint32_t> order;
        std::vector<BVHNode> bvh;
        std::vector<float> distances;
        std::vector<uint32_t> cellOffsets;
        std::vector<uint32_t> triangleReferences;
        VXGITraceAcceleration::Packed traceAcceleration;
    };
    Storage m_data;
    uint64_t m_revision = 0;
    uint64_t m_geometryRevision = 0;
    uint64_t m_materialRevision = 0;
    const VXGIWorld* m_sourceWorld = nullptr;
    uint32_t m_maxAxisResolution = 0;
    bool m_initialized = false;
    std::string m_error;

    bool ValidateBuildSettings(float cellSize, uint32_t maxAxisResolution)
    {
        if (!std::isfinite(cellSize) || cellSize <= 0 ||
            (maxAxisResolution != 0 && (maxAxisResolution < 3 || maxAxisResolution > 256)))
        {
            m_error = "SDF cellSize must be positive; maxAxisResolution must be 0 (automatic) or 3..256 nodes.";
            return false;
        }
        return true;
    }

    // Asset construction only. The mesh cache exposes the result as const and
    // deliberately has no geometry-edit, invalidation or rebuild operation.
    bool InitializeStaticMesh(std::vector<VXGIWorld::Triangle> triangles, float cellSize,
        uint32_t maxAxisResolution = 0)
    {
        if (m_initialized)
        {
            m_error = "Static mesh SDF is already initialized.";
            return false;
        }
        if (!ValidateBuildSettings(cellSize, maxAxisResolution)) return false;
        const auto start = std::chrono::steady_clock::now();
        try
        {
            Storage next;
            next.triangles = std::move(triangles);
            next.stats.geometryRebuilt = true;
            if (!BuildGeometry(next, cellSize, maxAxisResolution)) return false;
            next.stats.buildMilliseconds = Milliseconds(start);
            next.stats.memoryBytes = EstimateBytes(next);
            m_data = std::move(next);
            m_maxAxisResolution = maxAxisResolution;
            m_initialized = true;
            ++m_geometryRevision;
            ++m_revision;
            m_error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            m_error = std::string("Static mesh SDF build failed: ") + exception.what();
            return false;
        }
    }

    bool BuildGeometry(Storage& data, float cellSize, uint32_t maxAxisResolution)
    {
        data.grid.cellSize = cellSize;
        data.stats.triangles = data.triangles.size();
        if (data.triangles.empty()) return true;
        BuildBVH(data);
        if (!BuildGrid(data, cellSize, maxAxisResolution) || !BuildCandidates(data)) return false;
        data.traceAcceleration = VXGITraceAcceleration::Build(data.cellOffsets,
            data.triangleReferences, data.triangles, cellSize);
        const auto bakeStart = std::chrono::steady_clock::now();
        BakeDistances(data);
        data.stats.distanceBakeMilliseconds = Milliseconds(bakeStart);
        return true;
    }

    static double Milliseconds(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    }
    static bool Finite(DirectX::XMFLOAT3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    static Vec Add(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    static Vec Subtract(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    static Vec Scale(Vec a, double scale) { return {a.x*scale,a.y*scale,a.z*scale}; }
    static double Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    static Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
    static Vec Minimum(Vec a, Vec b) { return {(std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::min)(a.z,b.z)}; }
    static Vec Maximum(Vec a, Vec b) { return {(std::max)(a.x,b.x),(std::max)(a.y,b.y),(std::max)(a.z,b.z)}; }
    static DirectX::XMFLOAT3 Float3(Vec value) { return {float(value.x),float(value.y),float(value.z)}; }
    static Bounds Merge(Bounds a, Bounds b) { return {Minimum(a.minimum,b.minimum),Maximum(a.maximum,b.maximum)}; }
    static Bounds GridBounds(const Grid& grid)
    {
        return {Vec(grid.origin), Add(Vec(grid.origin), {double(grid.cellDimensions[0])*grid.cellSize,
            double(grid.cellDimensions[1])*grid.cellSize,double(grid.cellDimensions[2])*grid.cellSize})};
    }
    static void BuildBVH(Storage& data)
    {
        data.primitives.reserve(data.triangles.size());
        for (const auto& triangle : data.triangles)
        {
            const Vec a(triangle.positions[0]), b(triangle.positions[1]), c(triangle.positions[2]);
            data.primitives.push_back({{Minimum(Minimum(a,b),c),Maximum(Maximum(a,b),c)},Scale(Add(Add(a,b),c),1.0/3)});
        }
        data.order.resize(data.triangles.size());
        std::iota(data.order.begin(), data.order.end(), uint32_t(0));
        size_t maximumLeaves=1;
        while((data.triangles.size()+maximumLeaves-1)/maximumLeaves>8) maximumLeaves*=2;
        data.bvh.reserve(maximumLeaves*2-1);
        BuildNode(data, 0, static_cast<uint32_t>(data.order.size()));
        data.stats.bvhNodes = data.bvh.size();
    }
    static uint32_t BuildNode(Storage& data, uint32_t first, uint32_t count)
    {
        Bounds bounds = data.primitives[data.order[first]].bounds;
        Vec minimum = data.primitives[data.order[first]].centroid, maximum = minimum;
        for (uint32_t i = first+1; i < first+count; ++i)
        {
            const auto& primitive = data.primitives[data.order[i]];
            bounds = Merge(bounds, primitive.bounds);
            minimum = Minimum(minimum,primitive.centroid); maximum = Maximum(maximum,primitive.centroid);
        }
        const uint32_t index = static_cast<uint32_t>(data.bvh.size());
        data.bvh.push_back({bounds});
        if (count <= 8)
        {
            data.bvh[index].first = first; data.bvh[index].count = count;
            return index;
        }
        const Vec extent = Subtract(maximum,minimum);
        const size_t axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        const uint32_t middle = first+count/2;
        std::nth_element(data.order.begin()+first,data.order.begin()+middle,data.order.begin()+first+count,
            [&](uint32_t a,uint32_t b) {
                const double ca=data.primitives[a].centroid[axis],cb=data.primitives[b].centroid[axis];
                return ca==cb ? a<b : ca<cb;
            });
        const uint32_t left = BuildNode(data,first,middle-first);
        const uint32_t right = BuildNode(data,middle,first+count-middle);
        data.bvh[index].left=left; data.bvh[index].right=right;
        return index;
    }
    static double BoundsDistanceSquared(Vec position, Bounds bounds)
    {
        const Vec delta = Maximum(Maximum(Subtract(bounds.minimum,position),Subtract(position,bounds.maximum)),{});
        return Dot(delta,delta);
    }
    static double TriangleDistanceSquared(Vec position, const VXGIWorld::Triangle& triangle)
    {
        // Closest-point Voronoi regions, evaluated in double for thin triangles.
        const Vec a(triangle.positions[0]), b(triangle.positions[1]), c(triangle.positions[2]);
        const Vec ab=Subtract(b,a),ac=Subtract(c,a),ap=Subtract(position,a);
        const double d1=Dot(ab,ap),d2=Dot(ac,ap);
        if(d1<=0 && d2<=0) return Dot(ap,ap);
        const Vec bp=Subtract(position,b); const double d3=Dot(ab,bp),d4=Dot(ac,bp);
        if(d3>=0 && d4<=d3) return Dot(bp,bp);
        const double vc=d1*d4-d3*d2;
        if(vc<=0 && d1>=0 && d3<=0)
        {
            const Vec delta=Subtract(position,Add(a,Scale(ab,d1/(d1-d3)))); return Dot(delta,delta);
        }
        const Vec cp=Subtract(position,c); const double d5=Dot(ab,cp),d6=Dot(ac,cp);
        if(d6>=0 && d5<=d6) return Dot(cp,cp);
        const double vb=d5*d2-d1*d6;
        if(vb<=0 && d2>=0 && d6<=0)
        {
            const Vec delta=Subtract(position,Add(a,Scale(ac,d2/(d2-d6)))); return Dot(delta,delta);
        }
        const double va=d3*d6-d5*d4;
        if(va<=0 && d4-d3>=0 && d5-d6>=0)
        {
            const Vec delta=Subtract(position,Add(b,Scale(Subtract(c,b),(d4-d3)/((d4-d3)+(d5-d6))))); return Dot(delta,delta);
        }
        const double inverse=1/(va+vb+vc);
        const Vec delta=Subtract(position,Add(a,Add(Scale(ab,vb*inverse),Scale(ac,vc*inverse))));
        return Dot(delta,delta);
    }
    static double NearestSquared(const Storage& data, Vec position)
    {
        double best=std::numeric_limits<double>::infinity();
        std::array<uint32_t,128> stack; size_t count=1; stack[0]=0;
        while(count)
        {
            const auto& node=data.bvh[stack[--count]];
            if(BoundsDistanceSquared(position,node.bounds)>best) continue;
            if(node.count)
            {
                for(uint32_t i=0;i<node.count;++i)
                    best=(std::min)(best,TriangleDistanceSquared(position,data.triangles[data.order[node.first+i]]));
            }
            else
            {
                const double left=BoundsDistanceSquared(position,data.bvh[node.left].bounds);
                const double right=BoundsDistanceSquared(position,data.bvh[node.right].bounds);
                if(left<right)
                {
                    if(right<=best) stack[count++]=node.right;
                    if(left<=best) stack[count++]=node.left;
                }
                else
                {
                    if(left<=best) stack[count++]=node.left;
                    if(right<=best) stack[count++]=node.right;
                }
            }
        }
        return best;
    }
    bool BuildGrid(Storage& data,float size,uint32_t maxResolution)
    {
        const Bounds scene=data.bvh[0].bounds;
        const Vec lower={ (std::floor(scene.minimum.x/size)-1)*size,
            (std::floor(scene.minimum.y/size)-1)*size,(std::floor(scene.minimum.z/size)-1)*size };
        data.grid.origin=Float3(lower);
        std::array<double,3> requestedCells={},requestedNodes={};
        for(size_t axis=0;axis<3;++axis)
        {
            const double upper=(std::ceil(scene.maximum[axis]/size)+1)*size;
            requestedCells[axis]=std::ceil((upper-Vec(data.grid.origin)[axis])/size-1e-10);
            requestedNodes[axis]=requestedCells[axis]+1;
        }
        // Preserve the old largest 256^3-node allocation budget while allowing
        // long, thin assets to exceed 256 nodes along one axis. Size comes only
        // from geometry and spacing; a budget failure never coarsens the field.
        constexpr uint64_t maximumNodes=uint64_t(256)*256*256;
        const auto failGrid=[&](const std::string& reason)
        {
            std::ostringstream message;
            message << "SDF grid requires " << requestedNodes[0] << 'x' << requestedNodes[1]
                << 'x' << requestedNodes[2] << " nodes at cellSize=" << size
                << " (including one-cell padding); " << reason;
            m_error=message.str();
            return false;
        };
        uint64_t nodeCount=1;
        for(size_t axis=0;axis<3;++axis)
        {
            if(!std::isfinite(requestedCells[axis]) || requestedCells[axis]<2 ||
                requestedCells[axis]>double(UINT32_MAX-1))
                return failGrid("dimensions exceed finite 32-bit grid indexing; increase sdfCellSize.");
            if(maxResolution!=0 && requestedNodes[axis]>maxResolution)
            {
                return failGrid("dimensions exceed the internal per-axis limit of " +
                    std::to_string(maxResolution) + " nodes; increase sdfCellSize or the explicit resolution budget.");
            }
            const uint64_t axisNodes=static_cast<uint64_t>(requestedNodes[axis]);
            // Divide before multiplying: even rejected dimensions cannot wrap.
            if(axisNodes>maximumNodes/nodeCount)
                return failGrid("dimensions exceed the total grid budget of " +
                    std::to_string(maximumNodes) + " nodes; increase sdfCellSize.");
            nodeCount*=axisNodes;
            data.grid.cellDimensions[axis]=static_cast<uint32_t>(requestedCells[axis]);
            data.grid.nodeDimensions[axis]=static_cast<uint32_t>(axisNodes);
        }
        data.grid.boundsMaximum=Float3(GridBounds(data.grid).maximum);
        if(!Finite(data.grid.origin) || !Finite(data.grid.boundsMaximum))
        {
            m_error="SDF padded bounds exceed finite float world coordinates.";
            return false;
        }
        double maximumSpacing=0;
        for(size_t axis=0;axis<3;++axis)
        {
            for(float value:{float(Vec(data.grid.origin)[axis]),float(Vec(data.grid.boundsMaximum)[axis])})
            {
                maximumSpacing=(std::max)(maximumSpacing,std::abs(double(std::nextafter(value,std::numeric_limits<float>::infinity()))-value));
                maximumSpacing=(std::max)(maximumSpacing,std::abs(double(std::nextafter(value,-std::numeric_limits<float>::infinity()))-value));
            }
        }
        if(double(size)<8*maximumSpacing)
        {
            m_error="SDF cellSize is below the float world-position precision budget; increase sdfCellSize or move the scene nearer the origin.";
            return false;
        }
        const Vec maximumOffset={double(data.grid.cellDimensions[0])*size,double(data.grid.cellDimensions[1])*size,
            double(data.grid.cellDimensions[2])*size};
        const Vec marginScale={std::abs(data.grid.origin.x)+maximumOffset.x,std::abs(data.grid.origin.y)+maximumOffset.y,
            std::abs(data.grid.origin.z)+maximumOffset.z};
        data.stats.maximumNodePositionMargin=4*std::numeric_limits<float>::epsilon()*std::sqrt(Dot(marginScale,marginScale));
        data.grid.valid=true;
        data.stats.nodes=nodeCount;
        data.stats.cells=uint64_t(data.grid.cellDimensions[0])*data.grid.cellDimensions[1]*data.grid.cellDimensions[2];
        return true;
    }
    static uint32_t CellIndex(const Grid& grid,uint32_t x,uint32_t y,uint32_t z)
    {
        return x+grid.cellDimensions[0]*(y+grid.cellDimensions[1]*z);
    }
    static uint32_t NodeIndex(const Grid& grid,uint32_t x,uint32_t y,uint32_t z)
    {
        return x+grid.nodeDimensions[0]*(y+grid.nodeDimensions[1]*z);
    }
    static Vec NodePosition(const Grid& grid,uint32_t x,uint32_t y,uint32_t z)
    {
        return Add(Vec(grid.origin),{double(x)*grid.cellSize,double(y)*grid.cellSize,double(z)*grid.cellSize});
    }
    bool BuildCandidates(Storage& data)
    {
        std::unordered_map<uint32_t,std::vector<uint32_t>> occupied;
        for(uint32_t triangleIndex=0;triangleIndex<data.triangles.size();++triangleIndex)
        {
            const auto& triangle=data.triangles[triangleIndex];
            const auto& bounds=data.primitives[triangleIndex].bounds;
            std::array<uint32_t,3> minimum,maximum;
            for(size_t axis=0;axis<3;++axis)
            {
                const double origin=Vec(data.grid.origin)[axis];
                const double minCell=std::floor((bounds.minimum[axis]-origin)/data.grid.cellSize-1e-6);
                const double maxCell=std::floor((bounds.maximum[axis]-origin)/data.grid.cellSize+1e-6);
                minimum[axis]=static_cast<uint32_t>((std::max)(0.0,minCell));
                maximum[axis]=static_cast<uint32_t>((std::min)(double(data.grid.cellDimensions[axis]-1),maxCell));
            }
            const uint64_t count=uint64_t(maximum[0]-minimum[0]+1)*(maximum[1]-minimum[1]+1)*(maximum[2]-minimum[2]+1);
            if(count>100000000ull-data.stats.candidateBoxTests)
            {
                m_error="SDF triangle/cell candidate construction exceeds 100 million tests; increase sdfCellSize.";
                return false;
            }
            data.stats.candidateBoxTests+=count;
            for(uint32_t z=minimum[2];z<=maximum[2];++z)
                for(uint32_t y=minimum[1];y<=maximum[1];++y)
                    for(uint32_t x=minimum[0];x<=maximum[0];++x)
                    {
                        const Vec exactCenter=Add(NodePosition(data.grid,x,y,z),{data.grid.cellSize*0.5,data.grid.cellSize*0.5,data.grid.cellSize*0.5});
                        const auto center=Float3(exactCenter);
                        // Account for float API center conversion so this closed
                        // overlap predicate remains conservative at distant cells.
                        const double half=data.grid.cellSize*(0.5+1e-6);
                        const DirectX::XMFLOAT3 extent={float(half+std::abs(center.x-exactCenter.x)),
                            float(half+std::abs(center.y-exactCenter.y)),float(half+std::abs(center.z-exactCenter.z))};
                        if(VXGIWorld::TriangleIntersectsBox(triangle.positions,center,extent))
                            occupied[CellIndex(data.grid,x,y,z)].push_back(triangleIndex);
                    }
        }
        data.cellOffsets.resize(size_t(data.stats.cells)+1);
        uint64_t total=0;
        for(uint32_t cell=0;cell<data.stats.cells;++cell)
        {
            data.cellOffsets[cell]=static_cast<uint32_t>(total);
            const auto found=occupied.find(cell);
            if(found==occupied.end()) continue;
            ++data.stats.occupiedCells;
            total+=found->second.size();
            if(total>UINT32_MAX) { m_error="SDF candidate references exceed 32-bit CSR capacity."; return false; }
        }
        data.cellOffsets.back()=static_cast<uint32_t>(total);
        data.triangleReferences.resize(static_cast<size_t>(total));
        for(const auto& entry:occupied)
            std::copy(entry.second.begin(),entry.second.end(),data.triangleReferences.begin()+data.cellOffsets[entry.first]);
        data.stats.triangleReferences=total;
        return true;
    }
    static void BakeDistances(Storage& data)
    {
        data.distances.resize(static_cast<size_t>(data.stats.nodes));
        const uint32_t workers=data.stats.nodes<32768 ? 1 : (std::max)(1u,(std::min)(8u,std::thread::hardware_concurrency()));
        auto bake=[&](uint32_t worker)
        {
            const uint64_t first=data.stats.nodes*worker/workers,last=data.stats.nodes*(worker+1)/workers;
            const auto& dimensions=data.grid.nodeDimensions;
            for(uint64_t index=first;index<last;++index)
            {
                const uint32_t x=uint32_t(index%dimensions[0]);
                const uint32_t y=uint32_t((index/dimensions[0])%dimensions[1]);
                const uint32_t z=uint32_t(index/(uint64_t(dimensions[0])*dimensions[1]));
                const double distance=std::sqrt(NearestSquared(data,NodePosition(data.grid,x,y,z)));
                // The GPU reconstructs node positions with float arithmetic,
                // possibly fused. Lipschitz distance decreases by at most the
                // Euclidean position error. Leave its arithmetic error envelope
                // in the stored value before directed float rounding; otherwise
                // D(ideal node)-|p-float node| could overestimate safe distance.
                const Vec marginScale={std::abs(data.grid.origin.x)+double(x)*data.grid.cellSize,
                    std::abs(data.grid.origin.y)+double(y)*data.grid.cellSize,
                    std::abs(data.grid.origin.z)+double(z)*data.grid.cellSize};
                const double margin=4*std::numeric_limits<float>::epsilon()*std::sqrt(Dot(marginScale,marginScale));
                const double conservative=(std::max)(0.0,distance-margin);
                float stored=static_cast<float>(conservative);
                if(double(stored)>conservative) stored=std::nextafter(stored,0.0f);
                data.distances[static_cast<size_t>(index)]=stored;
            }
        };
        if(workers==1) bake(0);
        else
        {
            std::vector<std::future<void>> jobs;
            jobs.reserve(workers);
            for(uint32_t worker=0;worker<workers;++worker) jobs.push_back(std::async(std::launch::async,bake,worker));
            for(auto& job:jobs) job.get();
        }
    }
    static bool PrepareRay(DirectX::XMFLOAT3 origin,DirectX::XMFLOAT3 direction,float maxDistance,Vec& rayOrigin,Vec& rayDirection)
    {
        if(!Finite(origin) || !Finite(direction) || !std::isfinite(maxDistance) || maxDistance<0) return false;
        rayOrigin=Vec(origin); rayDirection=Vec(direction);
        const double length=std::sqrt(Dot(rayDirection,rayDirection));
        if(length==0) return false;
        rayDirection=Scale(rayDirection,1/length);
        return true;
    }
    static bool RayBox(Vec origin,Vec direction,Bounds bounds,double minimum,double maximum,double& enter,double& exit)
    {
        enter=minimum; exit=maximum;
        for(size_t axis=0;axis<3;++axis)
        {
            if(direction[axis]==0)
            {
                if(origin[axis]<bounds.minimum[axis] || origin[axis]>bounds.maximum[axis]) return false;
                continue;
            }
            double a=(bounds.minimum[axis]-origin[axis])/direction[axis],b=(bounds.maximum[axis]-origin[axis])/direction[axis];
            if(a>b) std::swap(a,b);
            enter=(std::max)(enter,a); exit=(std::min)(exit,b);
            if(enter>exit) return false;
        }
        return true;
    }
    static void ConsiderHit(const Storage& data,uint32_t triangleIndex,Vec origin,Vec direction,
        double minimum,double maximum,double& bestDistance,Hit& best)
    {
        const auto& triangle=data.triangles[triangleIndex];
        const Vec a(triangle.positions[0]),ab=Subtract(Vec(triangle.positions[1]),a),ac=Subtract(Vec(triangle.positions[2]),a);
        const Vec p=Cross(direction,ac);
        const double determinant=Dot(ab,p);
        const Vec normal=Cross(ab,ac);
        if(std::abs(determinant)<=1e-12*std::sqrt(Dot(normal,normal))) return;
        const double inverse=1/determinant;
        const Vec fromA=Subtract(origin,a);
        const double u=Dot(fromA,p)*inverse;
        const Vec q=Cross(fromA,ab);
        const double v=Dot(direction,q)*inverse;
        constexpr double barycentricTolerance=1e-10;
        if(u<-barycentricTolerance || v<-barycentricTolerance || u+v>1+barycentricTolerance) return;
        double distance=Dot(ac,q)*inverse;
        const double numericTolerance=1e-10*(1+std::abs(distance));
        if(distance<minimum-numericTolerance || distance>maximum+numericTolerance || distance>bestDistance+numericTolerance) return;
        if(best.hit && std::abs(distance-bestDistance)<=numericTolerance && triangleIndex>=best.triangleIndex) return;
        distance=(std::max)(0.0,distance);
        bestDistance=distance;
        best.hit=true; best.distance=float(distance); best.position=Float3(Add(origin,Scale(direction,distance)));
        best.geometricNormal=triangle.geometricNormal;
        best.barycentrics={float(1-u-v),float(u),float(v)};
        best.triangleIndex=triangleIndex; best.materialId=triangle.materialId; best.instanceId=triangle.instanceId;
        best.backFace=Dot(direction,Vec(triangle.geometricNormal))>0;
    }
    static bool LocateCell(const Grid& grid,Vec position,Vec direction,std::array<uint32_t,3>& cell)
    {
        for(size_t axis=0;axis<3;++axis)
        {
            double coordinate=(position[axis]-Vec(grid.origin)[axis])/grid.cellSize;
            const double rounded=std::round(coordinate);
            const bool boundary=std::abs(coordinate-rounded)<=1e-9;
            if(boundary) coordinate=rounded;
            double index=std::floor(coordinate);
            if(boundary && direction[axis]<0) --index;
            if(index<0 || index>=grid.cellDimensions[axis]) return false;
            cell[axis]=static_cast<uint32_t>(index);
        }
        return true;
    }
    static double CellExit(const Grid& grid,const std::array<uint32_t,3>& cell,Vec origin,Vec direction)
    {
        double exit=std::numeric_limits<double>::infinity();
        for(size_t axis=0;axis<3;++axis)
        {
            if(direction[axis]==0) continue;
            const double boundary=Vec(grid.origin)[axis]+(double(cell[axis])+(direction[axis]>0?1:0))*grid.cellSize;
            exit=(std::min)(exit,(boundary-origin[axis])/direction[axis]);
        }
        return exit;
    }
    static double NodeLowerBound(const Storage& data,Vec position,double epsilon)
    {
        std::array<uint32_t,3> node;
        for(size_t axis=0;axis<3;++axis)
        {
            const double coordinate=std::round((position[axis]-Vec(data.grid.origin)[axis])/data.grid.cellSize);
            node[axis]=static_cast<uint32_t>((std::max)(0.0,(std::min)(double(data.grid.nodeDimensions[axis]-1),coordinate)));
        }
        const double distance=data.distances[NodeIndex(data.grid,node[0],node[1],node[2])];
        const Vec offset=Subtract(position,NodePosition(data.grid,node[0],node[1],node[2]));
        return (std::max)(0.0,distance-std::sqrt(Dot(offset,offset))-epsilon);
    }
    static uint64_t EstimateBytes(const Storage& data)
    {
        return sizeof(VXGISDF)+data.triangles.capacity()*sizeof(VXGIWorld::Triangle)+data.primitives.capacity()*sizeof(Primitive)+
            data.order.capacity()*sizeof(uint32_t)+data.bvh.capacity()*sizeof(BVHNode)+data.distances.capacity()*sizeof(float)+
            data.cellOffsets.capacity()*sizeof(uint32_t)+data.triangleReferences.capacity()*sizeof(uint32_t)+
            data.traceAcceleration.CapacityBytes();
    }
};
