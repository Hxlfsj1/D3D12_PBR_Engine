#include "Resources/VXGIGlobalSDFState.h"
#include "ThirdParty/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using Global = VXGIGlobalSDF;
using State = VXGIGlobalSDFState;
using DirectX::XMFLOAT3;
using Json = nlohmann::json;
struct Vertex { XMFLOAT3 Position; };
struct Mesh { std::vector<Vertex> vertices; std::vector<std::vector<uint32_t>> lodIndices = {{}}; uint32_t materialID = 0; };
struct Model { std::vector<Mesh> meshes; };
struct Instance
{
    uint32_t editorId = 0; Model* pModel = nullptr;
    XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
    uint32_t customMaterialID = UINT32_MAX; bool isTransparent = false, isCutout = false;
};
void Require(bool condition, const std::string& error) { if (!condition) throw std::runtime_error(error); }
Model Plane()
{
    Mesh mesh;
    mesh.vertices = {{{-.35f, -.35f, 0}}, {{.35f, -.35f, 0}}, {{.35f, .35f, 0}}, {{-.35f, .35f, 0}}};
    mesh.lodIndices[0] = {0, 1, 2, 0, 2, 3};
    return {{std::move(mesh)}};
}
struct Fixture
{
    State state;
    Global::Settings settings{16, 3, .5f};
    VXGIMeshSDFCache cache;
    VXGIMeshSDFScene scene;
    VXGIMeshSDFGPUData::Geometry geometry;
    VXGIMeshSDFGPUData::Instances packed;
    std::vector<float> texture, expected;
    std::string error;
    uint64_t compared = 0, plans = 0, writes = 0;
    Json cases = Json::array();

    void Scene(const std::vector<Instance>& instances)
    {
        Require(scene.UpdateInstances(instances, cache, {.125f, 0}), scene.GetLastError());
        const auto assets = VXGIMeshSDFGPUData::CollectAssets(scene);
        geometry = VXGIMeshSDFGPUData::BuildAssets(assets);
        packed = VXGIMeshSDFGPUData::BuildInstances(scene, assets);
    }
    const State::Plan& Prepare(XMFLOAT3 center, uint64_t frame)
    {
        Require(state.Prepare(center, settings, scene, frame, uint32_t(geometry.assets.size()),
            uint32_t(geometry.nodes.size()), &error), error);
        return state.GetPendingPlan();
    }
    void Submit(const std::string& label)
    {
        const auto& plan = state.GetPendingPlan();
        const auto& layout = plan.layout;
        if (plan.recreate)
        {
            texture.assign(size_t(layout.totalNodeCount), -12345);
            expected.assign(size_t(layout.totalNodeCount), -98765);
        }
        uint64_t countedWrites = 0;
        for (const auto& region : plan.regions)
        {
            Require(region.clipmapIndex < layout.constants.clipmapCount, "region layer");
            const uint32_t n = Global::Resolution(layout.constants, region.clipmapIndex);
            for (uint32_t axis = 0; axis < 3; ++axis)
                Require(region.size[axis] && region.minimum[axis] + region.size[axis] <= n, "region lattice bounds");
            const auto& clipmap = layout.constants.clipmaps[region.clipmapIndex];
            for (uint32_t z = region.minimum[2]; z < region.minimum[2] + region.size[2]; ++z)
                for (uint32_t y = region.minimum[1]; y < region.minimum[1] + region.size[1]; ++y)
                    for (uint32_t x = region.minimum[0]; x < region.minimum[0] + region.size[0]; ++x)
                    {
                        texture[Global::PhysicalNodeIndex(layout, region.clipmapIndex, x, y, z)] =
                            Global::ComposeReferenceNode(geometry, packed, Global::NodePosition(clipmap, x, y, z), clipmap.distanceLimit);
                        ++countedWrites;
                    }
        }
        Require(countedWrites == plan.updatedNodes, "dispatch write accounting");
        for (uint32_t layer = 0; layer < layout.constants.clipmapCount; ++layer)
        {
            const auto& clipmap = layout.constants.clipmaps[layer];
            const uint32_t n = Global::Resolution(layout.constants, layer);
            Require(clipmap.distanceLimit == 4 * clipmap.cellSize, "narrow-band range");
            for (uint32_t z = 0; z < n; ++z) for (uint32_t y = 0; y < n; ++y) for (uint32_t x = 0; x < n; ++x)
            {
                const auto physical = Global::PhysicalNodeIndex(layout, layer, x, y, z);
                if (plan.updated[layer])
                    expected[physical] = Global::ComposeReferenceNode(geometry, packed,
                        Global::NodePosition(clipmap, x, y, z), clipmap.distanceLimit);
                Require(std::abs(texture[physical] - expected[physical]) < 1e-5f,
                    label + ": incremental lattice differs from complete recomposition at layer " + std::to_string(layer));
                ++compared;
            }
            if (layer)
            {
                const auto& inner = layout.constants.clipmaps[layer - 1];
                Require(clipmap.origin.x <= inner.origin.x && clipmap.origin.y <= inner.origin.y && clipmap.origin.z <= inner.origin.z &&
                    clipmap.boundsMaximum.x >= inner.boundsMaximum.x && clipmap.boundsMaximum.y >= inner.boundsMaximum.y &&
                    clipmap.boundsMaximum.z >= inner.boundsMaximum.z, "deferred clipmaps must remain nested");
            }
        }
        cases.push_back({{"name", label}, {"frame", plan.frameId}, {"regions", plan.regions.size()},
            {"updated_nodes", plan.updatedNodes}, {"full_update", plan.fullUpdate}, {"updated", plan.updated}, {"recreate", plan.recreate}});
        writes += countedWrites; ++plans;
        Require(state.Commit(), "submit commit");
        Require(!state.HasPendingPlan(), "commit clears pending");
    }
};
int main(int argc, char** argv)
{
    try
    {
        Fixture fixture;
        Model model = Plane(), changedAsset = Plane();
        Instance a; a.editorId = 7; a.pModel = &model;
        std::vector<Instance> instances{a};
        fixture.Scene(instances);
        auto plan = fixture.Prepare({}, 1);
        Require(plan.recreate && plan.regions.size() == 3 && plan.updatedNodes == plan.layout.totalNodeCount,
            "initial frame must immediately populate every layer");
        fixture.Submit("initial");

        plan = fixture.Prepare({}, 2);
        Require(plan.regions.empty(), "stationary scene must issue no dispatches");
        fixture.Submit("stationary");
        plan = fixture.Prepare({.1f, .1f, .1f}, 3);
        Require(plan.regions.empty(), "subcell camera motion must issue no dispatches");
        fixture.Submit("subcell");
        plan = fixture.Prepare({.5f, 0, 0}, 3);
        const auto n = plan.layout.constants.resolution;
        Require(plan.updatedNodes == uint64_t(n) * n && plan.regions.size() == 1, "one-cell movement must update one plane");
        Require(plan.layout.constants.clipmaps[0].scrollOffset[0] == 1 && !plan.updated[1] && !plan.updated[2], "positive wrap and distant cadence");
        fixture.Submit("one_cell_positive");
        plan = fixture.Prepare({-1.1f, 1.2f, -.6f}, 4);
        Require(plan.layout.constants.clipmaps[0].scrollOffset[0] == n - 3 &&
            plan.layout.constants.clipmaps[0].scrollOffset[2] == n - 2, "negative modulo wrap");
        fixture.Submit("negative_xyz");
        for (uint64_t frame = 5; frame <= 24; ++frame)
        {
            fixture.Prepare({float(frame - 5) * .7f - 1, std::sin(float(frame)) * .9f, -float(frame - 5) * .6f}, frame);
            fixture.Submit("multi_axis_rolling_" + std::to_string(frame));
        }

        // Return the camera without a large jump, then change the same object
        // repeatedly while outer layers retain different committed versions.
        fixture.Prepare({}, 28); fixture.Submit("camera_return");
        instances[0].translation = {-1, 0, .5f}; fixture.Scene(instances);
        plan = fixture.Prepare({}, 29);
        Require(!plan.updated[1] && !plan.updated[2], "outer layers must defer first dirty change");
        fixture.Submit("dirty_first_deferred");
        instances[0].translation = {1, .5f, -.5f}; fixture.Scene(instances);
        plan = fixture.Prepare({}, 30);
        Require(plan.updated[1] && !plan.updated[2], "middle layer must apply accumulated changes on its turn");
        fixture.Submit("dirty_second_change");
        instances[0].translation = {0, -1, 1}; fixture.Scene(instances);
        fixture.Prepare({}, 31); fixture.Submit("dirty_third_change");
        fixture.Prepare({}, 32); fixture.Submit("dirty_far_catches_up");

        const auto committed = fixture.state.GetLayout();
        instances[0].translation = {.5f, .5f, .5f}; fixture.Scene(instances);
        fixture.Prepare({.5f, .5f, .5f}, 33);
        fixture.state.Cancel();
        Require(!fixture.state.HasPendingPlan() && std::memcmp(&committed.constants,
            &fixture.state.GetLayout().constants, sizeof(committed.constants)) == 0, "cancel must preserve committed origin and scene");
        plan = fixture.Prepare({.5f, .5f, .5f}, 33);
        Require(plan.updatedNodes > 0, "cancel retry must reproduce pending updates");
        fixture.Submit("cancel_retry");
        // Cancel a second transform, then replace it before committing. Only
        // the old committed and final positions need clearing.
        instances[0].translation = {-2, -1, -1}; fixture.Scene(instances);
        fixture.Prepare({}, 34); fixture.state.Cancel();
        instances[0].translation = {2, 1, 1}; fixture.Scene(instances);
        fixture.Prepare({}, 34); fixture.Submit("cancel_replace_scene");
        fixture.Prepare({}, 36); fixture.Submit("cancel_replace_far_flush");

        instances[0].customMaterialID = 101; instances[0].isCutout = true; fixture.Scene(instances);
        plan = fixture.Prepare({}, 37);
        Require(plan.regions.empty(), "material and cutout metadata must not dirty geometric fields");
        fixture.Submit("material_only_idle");

        Instance b = instances[0]; b.editorId = 8; b.translation = {-2, 0, 0};
        instances.push_back(b); fixture.Scene(instances);
        fixture.Prepare({}, 38); fixture.Submit("add_existing_asset");
        instances.erase(instances.begin()); fixture.Scene(instances);
        fixture.Prepare({}, 39); fixture.Submit("remove_instance");
        fixture.Prepare({}, 40); fixture.Submit("remove_far_flush");
        instances[0].scale = {-2, .5f, 3}; instances[0].rotation = {.3f, -.4f, .1f}; fixture.Scene(instances);
        fixture.Prepare({}, 41); fixture.Submit("negative_nonuniform_scale");
        fixture.Prepare({}, 44); fixture.Submit("scale_far_flush");
        instances[0].pModel = &changedAsset; fixture.Scene(instances);
        plan = fixture.Prepare({}, 45);
        Require(plan.fullUpdate[0] && plan.fullUpdate[1] && plan.fullUpdate[2], "immutable asset replacement must rebuild all layers");
        fixture.Submit("asset_replacement");

        // Force an outer update ahead of its cadence while remaining below a
        // full-layer camera jump; the next step propagates to the farthest box.
        plan = fixture.Prepare({5.5f, 0, 0}, 47);
        Require(plan.updated[0] && plan.updated[1] && !plan.updated[2], "middle box must update early to contain the near box");
        fixture.Submit("early_middle_for_nesting");
        plan = fixture.Prepare({10.5f, 0, 0}, 49);
        Require(plan.updated[2], "nesting must propagate an early update to the outer box");
        fixture.Submit("early_far_for_nesting");
        plan = fixture.Prepare({100, 0, 0}, 51);
        Require(plan.fullUpdate[0] && plan.fullUpdate[1] && plan.fullUpdate[2], "large jump must bypass cadence and rebuild every layer");
        fixture.Submit("large_jump");
        fixture.settings.cellsPerAxis = 18;
        plan = fixture.Prepare({}, 53);
        Require(plan.recreate && plan.fullUpdate[2], "resizing must recreate and fully populate the texture");
        fixture.Submit("resize");
        fixture.settings.baseCellSize = .75f;
        plan = fixture.Prepare({}, 55);
        Require(!plan.recreate && plan.fullUpdate[2], "cell-size changes rebuild without resizing the atlas");
        fixture.Submit("cell_size_change");
        fixture.Scene({});
        plan = fixture.Prepare({}, 57);
        Require(plan.fullUpdate[2], "empty packed geometry change must clear every layer immediately");
        fixture.Submit("empty_scene");
        fixture.Prepare({-.75f, 0, 0}, 58); fixture.Submit("empty_scroll");

        // Invalid preparation is also transactional; no stale pending plan may
        // accidentally be committed after an invalid frame.
        const auto saved = fixture.state.GetLayout();
        Require(!fixture.state.Prepare({std::numeric_limits<float>::infinity(), 0, 0}, fixture.settings,
            fixture.scene, 59, 0, 0, &fixture.error), "reject invalid center");
        Require(!fixture.state.HasPendingPlan() && !fixture.state.Commit() &&
            std::memcmp(&saved.constants, &fixture.state.GetLayout().constants, sizeof(saved.constants)) == 0,
            "invalid prepare must preserve committed state");
        fixture.state.Reset();
        Require(!fixture.state.IsInitialized() && !fixture.state.HasPendingPlan(), "reset state");

        Fixture defaults;
        defaults.settings = {};
        instances = {a}; defaults.Scene(instances);
        defaults.Prepare({}, 0); defaults.Submit("default_65_initial");
        plan = defaults.Prepare({.5f, 0, 0}, 1);
        Require(plan.updatedNodes == 65 * 65 && plan.updatedNodes * 195 == plan.layout.totalNodeCount,
            "default one-cell move must dispatch exactly 1/195 of the three-layer atlas");
        defaults.Submit("default_65_one_plane");
        defaults.Prepare({-.5f, 0, 0}, 2); defaults.Submit("default_65_negative_wrap");
        defaults.Prepare({-.5f, 0, 0}, 4); defaults.Submit("default_65_far_camera_catches_up");
        Instance remote = a; remote.editorId = 99; remote.translation = {1000, 1000, 1000};
        instances.push_back(remote); defaults.Scene(instances);
        plan = defaults.Prepare({-.5f, 0, 0}, 5);
        Require(plan.regions.empty(), "adding an object beyond every narrow band must not dirty the atlas");
        defaults.Submit("remote_add_outside_narrow_band");
        instances[1].translation = {1001, 1001, 1001}; defaults.Scene(instances);
        plan = defaults.Prepare({-.5f, 0, 0}, 6);
        Require(plan.regions.empty(), "moving a distant object must not change cached nodes");
        defaults.Submit("remote_move_outside_narrow_band");
        instances[1].translation = {2, 0, 1}; defaults.Scene(instances);
        defaults.Prepare({-.5f, 0, 0}, 7); defaults.Submit("remote_enters_local_band");
        defaults.Prepare({-.5f, 0, 0}, 8); defaults.Submit("remote_enters_far_catches_up");

        Fixture compact;
        compact.settings.baseCellSize = .25f; compact.settings.layerCellsPerAxis = {4, 8, 8};
        instances = {a}; compact.Scene(instances);
        plan = compact.Prepare({}, 0);
        Require(plan.recreate && plan.updatedNodes == 1583 && plan.layout.totalNodeCount == 1863,
            "compact initialization must populate active nodes without dispatching XY padding");
        compact.Submit("compact_initial_1_4_8");
        plan = compact.Prepare({.25f, 0, 0}, 1);
        Require(plan.updatedNodes == 25 && plan.layout.constants.clipmaps[0].scrollOffset[0] == 1 &&
            !plan.updated[1] && !plan.updated[2], "compact near one-cell movement must update one 5x5 plane");
        compact.Submit("compact_positive_near_plane");
        plan = compact.Prepare({-.25f, 0, 0}, 2);
        Require(plan.layout.constants.clipmaps[0].scrollOffset[0] == 4 &&
            plan.layout.constants.clipmaps[1].scrollOffset[0] == 8 && !plan.updated[2],
            "compact negative rolling must wrap near modulo five and middle modulo nine");
        compact.Submit("compact_negative_distinct_moduli");
        plan = compact.Prepare({-.25f, 0, 0}, 3);
        Require(plan.regions.empty(), "compact distant rolling must wait for its cadence when nesting permits");
        compact.Submit("compact_far_deferred");
        plan = compact.Prepare({-.25f, 0, 0}, 4);
        Require(plan.updated[2] && plan.layout.constants.clipmaps[2].scrollOffset[0] == 8,
            "compact outer rolling must use its nine-node lattice on the fourth frame");
        compact.Submit("compact_far_catches_up");
        instances[0].translation = {.125f, 0, .125f}; compact.Scene(instances);
        plan = compact.Prepare({-.25f, 0, 0}, 5);
        Require(plan.updated[0] && !plan.updated[1] && !plan.updated[2], "compact edit must retain layer cadence");
        compact.Submit("compact_dirty_near");
        compact.Prepare({-.25f, 0, 0}, 6); compact.Submit("compact_dirty_middle");
        compact.Prepare({-.25f, 0, 0}, 8); compact.Submit("compact_dirty_far");
        // Changing per-layer dimensions can preserve the atlas allocation but
        // moves the packed Z boundaries and changes every toroidal modulus.
        compact.settings.layerCellsPerAxis = {6, 6, 8};
        plan = compact.Prepare({}, 9);
        Require(!plan.recreate && plan.fullUpdate[0] && plan.fullUpdate[1] && plan.fullUpdate[2] &&
            plan.updatedNodes == 1415, "same-atlas layer reshaping must fully repopulate all active nodes");
        compact.Submit("compact_same_atlas_reshape");

        Json report = {{"passed", true}, {"plans", fixture.plans + defaults.plans + compact.plans},
            {"node_comparisons", fixture.compared + defaults.compared + compact.compared},
            {"dispatched_node_writes", fixture.writes + defaults.writes + compact.writes},
            {"cases", fixture.cases}, {"default_resolution_cases", defaults.cases}, {"compact_resolution_cases", compact.cases}};
        if (argc > 1) { std::ofstream file(argv[1]); file << report.dump(2); }
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
