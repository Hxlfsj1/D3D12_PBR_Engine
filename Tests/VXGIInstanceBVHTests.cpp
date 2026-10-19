#include "../Resources/VXGIInstanceBVH.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <stdexcept>

namespace
{
    std::atomic<uint64_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
    using BVH = VXGIInstanceBVH;
    using Float3 = DirectX::XMFLOAT3;
    struct Instance
    {
        uint32_t instanceId = 0;
        Float3 boundsMinimum = {}, boundsMaximum = {};
        uint32_t materialId = 0;
    };
    uint64_t referenceComparisons = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    bool Near(double a, double b)
    {
        return std::abs(a - b) <= 2e-9 * (1.0 + (std::max)(std::abs(a), std::abs(b)));
    }
    Instance Box(uint32_t id, Float3 minimum, Float3 maximum)
    {
        return { id, minimum, maximum, 0 };
    }
    std::vector<BVH::Candidate> Reference(const std::vector<Instance>& instances,
        Float3 origin, Float3 direction, float rayMin, float rayMax)
    {
        std::vector<BVH::Candidate> candidates;
        const long double length = std::sqrt(static_cast<long double>(direction.x) * direction.x +
            static_cast<long double>(direction.y) * direction.y + static_cast<long double>(direction.z) * direction.z);
        const long double origins[3] = { origin.x, origin.y, origin.z };
        const long double directions[3] = { direction.x / length, direction.y / length, direction.z / length };
        for (size_t index = 0; index < instances.size(); ++index)
        {
            const auto& instance = instances[index];
            const float minimum[3] = { instance.boundsMinimum.x, instance.boundsMinimum.y, instance.boundsMinimum.z };
            const float maximum[3] = { instance.boundsMaximum.x, instance.boundsMaximum.y, instance.boundsMaximum.z };
            long double entry = rayMin, exit = rayMax;
            bool intersects = true;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                if (directions[axis] == 0)
                {
                    if (origins[axis] < minimum[axis] || origins[axis] > maximum[axis]) intersects = false;
                    continue;
                }
                const long double a = (minimum[axis] - origins[axis]) / directions[axis];
                const long double b = (maximum[axis] - origins[axis]) / directions[axis];
                entry = (std::max)(entry, (std::min)(a, b));
                exit = (std::min)(exit, (std::max)(a, b));
                if (entry > exit) intersects = false;
            }
            if (intersects)
                candidates.push_back({ static_cast<uint32_t>(index), instance.instanceId,
                    static_cast<double>(entry), static_cast<double>(exit) });
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
            {
                if (a.entryDistance != b.entryDistance) return a.entryDistance < b.entryDistance;
                if (a.instanceId != b.instanceId) return a.instanceId < b.instanceId;
                return a.instanceIndex < b.instanceIndex;
            });
        if (candidates.size() > 2) candidates.resize(2);
        return candidates;
    }
    void Compare(const BVH& bvh, const std::vector<Instance>& instances,
        Float3 origin, Float3 direction, float minimum, float maximum)
    {
        const auto expected = Reference(instances, origin, direction, minimum, maximum);
        const auto actual = bvh.QueryClosestTwo(origin, direction, minimum, maximum);
        Check(actual.valid && actual.count == expected.size(), "BVH/brute-force AABB candidate count mismatch");
        for (uint32_t index = 0; index < actual.count; ++index)
        {
            const auto& a = actual.candidates[index];
            const auto& e = expected[index];
            Check(a.instanceId == e.instanceId && a.instanceIndex == e.instanceIndex,
                "BVH/brute-force nearest AABB identity mismatch");
            Check(Near(a.entryDistance, e.entryDistance) && Near(a.exitDistance, e.exitDistance),
                "BVH/brute-force clipped finite interval mismatch");
            Check(a.entryDistance >= minimum && a.exitDistance <= maximum && a.entryDistance <= a.exitDistance,
                "candidate changed the original finite world ray interval");
        }
        ++referenceComparisons;
    }
    bool EqualNodes(const std::vector<BVH::Node>& a, const std::vector<BVH::Node>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].instanceIndex != b[i].instanceIndex || a[i].escapeIndex != b[i].escapeIndex ||
                a[i].boundsMinimum.x != b[i].boundsMinimum.x || a[i].boundsMinimum.y != b[i].boundsMinimum.y ||
                a[i].boundsMinimum.z != b[i].boundsMinimum.z || a[i].boundsMaximum.x != b[i].boundsMaximum.x ||
                a[i].boundsMaximum.y != b[i].boundsMaximum.y || a[i].boundsMaximum.z != b[i].boundsMaximum.z) return false;
        return true;
    }
    void CheckTree(const BVH& bvh, const std::vector<Instance>& instances)
    {
        const auto& nodes = bvh.GetNodes();
        Check(nodes.size() == (instances.empty() ? 0 : instances.size() * 2 - 1), "binary BVH has incorrect node count");
        std::vector<bool> seen(instances.size(), false);
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            const auto& node = nodes[index];
            Check(node.escapeIndex > index && node.escapeIndex <= nodes.size(), "invalid preorder escape index");
            if (node.instanceIndex != UINT32_MAX)
            {
                Check(node.instanceIndex < instances.size() && !seen[node.instanceIndex] && node.escapeIndex == index + 1,
                    "leaf did not contain one unique compact instance");
                seen[node.instanceIndex] = true;
                const auto& source = instances[node.instanceIndex];
                Check(node.boundsMinimum.x <= source.boundsMinimum.x && node.boundsMinimum.y <= source.boundsMinimum.y &&
                    node.boundsMinimum.z <= source.boundsMinimum.z && node.boundsMaximum.x >= source.boundsMaximum.x &&
                    node.boundsMaximum.y >= source.boundsMaximum.y && node.boundsMaximum.z >= source.boundsMaximum.z,
                    "leaf bounds truncated the source AABB");
            }
            else
            {
                const size_t left = index + 1;
                Check(left < nodes.size(), "internal node missing its left child");
                const size_t right = nodes[left].escapeIndex;
                Check(right < nodes.size() && nodes[right].escapeIndex == node.escapeIndex,
                    "preorder internal node does not contain exactly two subtrees");
                for (const auto child : { left, right })
                    Check(node.boundsMinimum.x <= nodes[child].boundsMinimum.x && node.boundsMinimum.y <= nodes[child].boundsMinimum.y &&
                        node.boundsMinimum.z <= nodes[child].boundsMinimum.z && node.boundsMaximum.x >= nodes[child].boundsMaximum.x &&
                        node.boundsMaximum.y >= nodes[child].boundsMaximum.y && node.boundsMaximum.z >= nodes[child].boundsMaximum.z,
                        "internal node did not enclose child bounds");
            }
        }
        for (bool found : seen) Check(found, "source instance missing from BVH leaves");
        if (!nodes.empty()) Check(nodes[0].escapeIndex == nodes.size(), "root escape does not end at the node count");
    }
    void TestEmptyAndSmallTrees()
    {
        static_assert(sizeof(BVH::Node) == 32);
        BVH bvh;
        std::vector<Instance> instances;
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        Check(bvh.GetRevision() == 1 && bvh.GetStats().builds == 1 && bvh.GetStats().lastUpdateRebuilt,
            "first empty update did not establish a valid revision");
        CheckTree(bvh, instances);
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 10);
        Check(bvh.Update(instances) && bvh.GetRevision() == 1 && bvh.GetStats().noops == 1,
            "unchanged empty update rebuilt the tree");
        instances.push_back(Box(19, { -1, -1, 2 }, { 1, 1, 3 }));
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        CheckTree(bvh, instances);
        Compare(bvh, instances, {}, { 0, 0, 2 }, 0, 10);
        instances.push_back(Box(9, { -1, -1, 4 }, { 1, 1, 5 }));
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        CheckTree(bvh, instances);
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 10);
    }
    void TestEntryOrderingAndTopTwo()
    {
        BVH bvh;
        // Box 90 has a much farther center, but the first entry. No geometry or
        // empty-space test can replace either selected AABB with the third one.
        std::vector<Instance> instances = { Box(90, { -1, -1, 1 }, { 1, 1, 1000 }),
            Box(30, { -1, -1, 2 }, { 1, 1, 3 }), Box(1, { -1, -1, 4 }, { 1, 1, 5 }) };
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        auto result = bvh.QueryClosestTwo({}, { 0, 0, 1 }, 0, 10);
        Check(result.valid && result.count == 2 && result.candidates[0].instanceId == 90 && result.candidates[1].instanceId == 30,
            "top two must use AABB entry, not center or a surface hit");
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 10);
        instances = { Box(99, { -1, -1, 1 }, { 1, 1, 2 }), Box(7, { -1, -1, 1 }, { 1, 1, 3 }),
            Box(7, { -1, -1, 1 }, { 1, 1, 4 }), Box(5, { -1, -1, 2 }, { 1, 1, 3 }) };
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 10);
        result = bvh.QueryClosestTwo({}, { 0, 0, 1 }, 0, 10);
        Check(result.candidates[0].instanceIndex == 1 && result.candidates[1].instanceIndex == 2,
            "equal entry ties must use stable instance ID, then compact index");
    }
    void TestFiniteIntervalsAndParallelRays()
    {
        BVH bvh;
        std::vector<Instance> instances = { Box(30, { -1, -1, -1 }, { 1, 1, 1 }),
            Box(20, { -1, -1, 2 }, { 1, 1, 3 }), Box(10, { -1, -1, 4 }, { 1, 1, 5 }),
            Box(5, { 1, -1, 3 }, { 1, 1, 3 }) };
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0.25f, 2.5f);
        Compare(bvh, instances, {}, { 0, 0, 8 }, 3, 4);
        Compare(bvh, instances, {}, { 0, 0, 1 }, 2, 2);
        Compare(bvh, instances, {}, { 0, 0, 1 }, 1.5f, 1.5f);
        Compare(bvh, instances, { 1, 0, 0 }, { -0.0f, 0, 1 }, 0, 3);
        Compare(bvh, instances, { 1.0001f, 0, 0 }, { 0, 0, 1 }, 0, 10);
        Compare(bvh, instances, { 0, 0, 6 }, { 0, 0, -1 }, 0.5f, 4);
        Compare(bvh, instances, {}, { 1, 0, 0 }, 0, 0);
        Compare(bvh, instances, { -10000, 0, 3 }, { 1, 0, 0 }, 9999, 10001);
    }
    void TestUpdatesAndTransactionality()
    {
        BVH bvh;
        std::vector<Instance> instances = { Box(2, { -1, -1, 2 }, { 1, 1, 3 }),
            Box(5, { -1, -1, 5 }, { 1, 1, 6 }), Box(9, { -1, -1, 8 }, { 1, 1, 9 }) };
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        const auto original = bvh.GetNodes();
        auto revision = bvh.GetRevision();
        instances[1].materialId = 999;
        Check(bvh.Update(instances) && bvh.GetRevision() == revision && bvh.GetStats().noops == 1 &&
            !bvh.GetStats().lastUpdateRebuilt && !bvh.GetStats().lastUpdateRefit && EqualNodes(original, bvh.GetNodes()),
            "material-only update changed intersection acceleration");
        instances[1].boundsMinimum.z = -3;
        instances[1].boundsMaximum.z = -2;
        Check(bvh.Update(instances) && bvh.GetRevision() == ++revision && bvh.GetStats().refits == 1 &&
            bvh.GetStats().builds == 1 && bvh.GetStats().lastUpdateRefit && !bvh.GetStats().lastUpdateRebuilt,
            "bounds-only movement did not refit the existing hierarchy");
        CheckTree(bvh, instances);
        const auto& refitted = bvh.GetNodes();
        Check(original.size() == refitted.size(), "refit changed hierarchy node count");
        for (size_t index = 0; index < original.size(); ++index)
            Check(original[index].instanceIndex == refitted[index].instanceIndex && original[index].escapeIndex == refitted[index].escapeIndex,
                "refit changed leaf ordering or subtree topology");
        Compare(bvh, instances, { 0, 0, -5 }, { 0, 0, 1 }, 0, 20);
        std::swap(instances[0], instances[2]);
        Check(bvh.Update(instances) && bvh.GetRevision() == ++revision && bvh.GetStats().builds == 2 && bvh.GetStats().lastUpdateRebuilt,
            "compact instance reordering did not rebuild index bindings");
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 20);
        instances[0].instanceId = 99;
        Check(bvh.Update(instances) && bvh.GetRevision() == ++revision && bvh.GetStats().builds == 3,
            "instance replacement did not rebuild identity bindings");
        instances.push_back(Box(7, { -2, -2, 11 }, { 2, 2, 12 }));
        Check(bvh.Update(instances) && bvh.GetRevision() == ++revision && bvh.GetStats().builds == 4,
            "instance addition did not rebuild hierarchy");
        instances.erase(instances.begin() + 1);
        Check(bvh.Update(instances) && bvh.GetRevision() == ++revision && bvh.GetStats().builds == 5,
            "instance removal did not rebuild hierarchy");
        CheckTree(bvh, instances);
        const auto retainedNodes = bvh.GetNodes();
        const auto retainedStats = bvh.GetStats();
        for (uint32_t failure = 0; failure < 3; ++failure)
        {
            auto invalid = instances;
            if (failure == 0) invalid[1].boundsMinimum.x = std::numeric_limits<float>::quiet_NaN();
            if (failure == 1) invalid[1].boundsMaximum.z = std::numeric_limits<float>::infinity();
            if (failure == 2) invalid[1].boundsMaximum.y = invalid[1].boundsMinimum.y - 1;
            Check(!bvh.Update(invalid) && !bvh.GetLastError().empty() && bvh.GetRevision() == revision &&
                EqualNodes(retainedNodes, bvh.GetNodes()), "invalid bounds replaced the previously valid BVH");
            const auto& stats = bvh.GetStats();
            Check(stats.instanceCount == retainedStats.instanceCount && stats.nodeCount == retainedStats.nodeCount &&
                stats.builds == retainedStats.builds && stats.refits == retainedStats.refits && stats.noops == retainedStats.noops &&
                stats.lastUpdateRebuilt == retainedStats.lastUpdateRebuilt && stats.lastUpdateRefit == retainedStats.lastUpdateRefit,
                "invalid update changed successful-update counters");
            Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 20);
        }
        Check(bvh.Update(instances) && bvh.GetLastError().empty() && bvh.GetRevision() == revision,
            "valid unchanged input did not recover after an invalid update");
    }
    void TestInvalidRaysAndAllocationFreeQueries()
    {
        BVH bvh;
        std::vector<Instance> instances = { Box(2, { -1, -1, 2 }, { 1, 1, 3 }) };
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        const auto inf = std::numeric_limits<float>::infinity();
        const auto Invalid = [&](Float3 origin, Float3 direction, float minimum, float maximum)
        {
            BVH::QueryStats stats;
            const auto result = bvh.QueryClosestTwo(origin, direction, minimum, maximum, &stats);
            Check(!result.valid && result.count == 0 && stats.nodeTests == 0 && stats.leafTests == 0,
                "invalid ray entered the BVH or returned candidates");
        };
        Invalid({}, {}, 0, 10);
        Invalid({ nan, 0, 0 }, { 0, 0, 1 }, 0, 10);
        Invalid({}, { 0, inf, 1 }, 0, 10);
        Invalid({}, { 0, 0, 1 }, -1, 10);
        Invalid({}, { 0, 0, 1 }, 4, 3);
        Invalid({}, { 0, 0, 1 }, 0, inf);
        Invalid({}, { 0, 0, 1 }, nan, 10);
        const auto revision = bvh.GetRevision();
        const auto allocations = allocationCount.load(std::memory_order_relaxed);
        uint64_t hits = 0;
        for (uint32_t index = 0; index < 4096; ++index)
        {
            const auto result = bvh.QueryClosestTwo({ 0, 0, -static_cast<float>(index % 7) }, { 0, 0, 2 }, 0, 10);
            hits += result.count;
        }
        Check(hits == 4096 && allocationCount.load(std::memory_order_relaxed) == allocations && bvh.GetRevision() == revision,
            "per-ray query allocated memory or modified the BVH");
    }
    void TestLargeTreePruning()
    {
        BVH bvh;
        std::vector<Instance> instances;
        for (uint32_t index = 0; index < 1024; ++index)
        {
            const float z = 2.0f + 3.0f * static_cast<float>(index);
            instances.push_back(Box(1024 - index, { -1, -1, z }, { 1, 1, z + 1 }));
        }
        Check(bvh.Update(instances), bvh.GetLastError().c_str());
        CheckTree(bvh, instances);
        Compare(bvh, instances, {}, { 0, 0, 1 }, 0, 4000);
        BVH::QueryStats stats;
        const auto result = bvh.QueryClosestTwo({}, { 0, 0, 1 }, 0, 4000, &stats);
        Check(result.count == 2 && stats.prunedNodes > 0 && stats.leafTests < instances.size() / 8 &&
            stats.nodeTests < bvh.GetNodes().size() / 4 && stats.intersectedLeaves >= 2,
            "nearest-two bound did not prune distant instance subtrees");
        BVH::QueryStats missStats;
        const auto miss = bvh.QueryClosestTwo({ 100, 100, 0 }, { 0, 0, 1 }, 0, 4000, &missStats);
        Check(miss.valid && miss.count == 0 && missStats.nodeTests == 1 && missStats.leafTests == 0,
            "root miss descended into model leaves");
    }
    Instance RandomTransformedBox(uint32_t id, std::mt19937& random)
    {
        std::uniform_real_distribution<float> position(-45, 45), extent(0.05f, 3.0f), angle(-3.14f, 3.14f), scale(0.2f, 4);
        const Float3 center = { position(random), position(random), position(random) };
        const Float3 half = { extent(random), extent(random), extent(random) };
        const Float3 scales = { scale(random) * (id & 1 ? -1.0f : 1.0f), scale(random), scale(random) };
        const double rx = angle(random), ry = angle(random), rz = angle(random);
        const double cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
        Instance instance = Box(id, { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX });
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            const double x = half.x * scales.x * (corner & 1 ? 1 : -1);
            const double y = half.y * scales.y * (corner & 2 ? 1 : -1);
            const double z = half.z * scales.z * (corner & 4 ? 1 : -1);
            const double y1 = cx * y - sx * z, z1 = sx * y + cx * z;
            const double x2 = cy * x + sy * z1, z2 = -sy * x + cy * z1;
            const Float3 world = { center.x + static_cast<float>(cz * x2 - sz * y1),
                center.y + static_cast<float>(sz * x2 + cz * y1), center.z + static_cast<float>(z2) };
            instance.boundsMinimum = { (std::min)(instance.boundsMinimum.x, world.x), (std::min)(instance.boundsMinimum.y, world.y),
                (std::min)(instance.boundsMinimum.z, world.z) };
            instance.boundsMaximum = { (std::max)(instance.boundsMaximum.x, world.x), (std::max)(instance.boundsMaximum.y, world.y),
                (std::max)(instance.boundsMaximum.z, world.z) };
        }
        return instance;
    }
    void TestRandomWorldAABBReference()
    {
        std::mt19937 random(0xB7AABB21u);
        std::uniform_real_distribution<float> coordinate(-75, 75), component(-1, 1), minimum(0, 25), length(0, 180);
        BVH bvh;
        for (uint32_t scene = 0; scene < 24; ++scene)
        {
            std::vector<Instance> instances;
            const uint32_t count = scene == 0 ? 0 : (scene == 1 ? 1 : 8 + scene * 7);
            for (uint32_t index = 0; index < count; ++index) instances.push_back(RandomTransformedBox(index * 13 + 3, random));
            std::shuffle(instances.begin(), instances.end(), random);
            Check(bvh.Update(instances), bvh.GetLastError().c_str());
            CheckTree(bvh, instances);
            for (uint32_t ray = 0; ray < 64; ++ray)
            {
                const Float3 origin = { coordinate(random), coordinate(random), coordinate(random) };
                Float3 direction = { component(random), component(random), component(random) };
                if (!instances.empty() && ray % 2 == 0)
                {
                    const auto& target = instances[ray % instances.size()];
                    direction = { (target.boundsMinimum.x + target.boundsMaximum.x) * 0.5f - origin.x,
                        (target.boundsMinimum.y + target.boundsMaximum.y) * 0.5f - origin.y,
                        (target.boundsMinimum.z + target.boundsMaximum.z) * 0.5f - origin.z };
                }
                const float first = minimum(random), last = first + length(random);
                Compare(bvh, instances, origin, direction, first, last);
            }
            if (!instances.empty())
            {
                for (auto& instance : instances)
                {
                    const float delta = component(random) * 10;
                    instance.boundsMinimum.z += delta;
                    instance.boundsMaximum.z += delta;
                }
                const auto builds = bvh.GetStats().builds;
                Check(bvh.Update(instances) && bvh.GetStats().lastUpdateRefit && bvh.GetStats().builds == builds,
                    "random moved bounds unexpectedly rebuilt the tree");
                CheckTree(bvh, instances);
                for (uint32_t ray = 0; ray < 16; ++ray)
                    Compare(bvh, instances, { coordinate(random), coordinate(random), coordinate(random) },
                        { component(random), component(random), component(random) }, 1, 160);
            }
        }
    }
}

int main()
{
    try
    {
        TestEmptyAndSmallTrees();
        TestEntryOrderingAndTopTwo();
        TestFiniteIntervalsAndParallelRays();
        TestUpdatesAndTransactionality();
        TestInvalidRaysAndAllocationFreeQueries();
        TestLargeTreePruning();
        TestRandomWorldAABBReference();
        std::cout << "VXGIInstanceBVH: all 7 CPU test groups passed (" << referenceComparisons
            << " independent finite world-AABB comparisons; 4096 allocation-free queries).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VXGIInstanceBVH: " << error.what() << '\n';
        return 1;
    }
}
