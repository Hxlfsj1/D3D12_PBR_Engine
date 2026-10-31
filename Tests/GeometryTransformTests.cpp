// CPU regression for the production world-triangle attribute transform. It uses
// deliberately oblique tangents so inverse-transpose cannot accidentally pass.
#include "Resources/VXGIWorld.h"
#include "ThirdParty/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
struct Vertex { XMFLOAT3 Position, Normal; XMFLOAT2 TexCoords; XMFLOAT3 Tangent, Bitangent; };
struct Resource { const void* Get() const { return nullptr; } };
struct Texture { Resource Resource; unsigned type = 0; std::string path; };
struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<std::vector<unsigned>> lodIndices = {{0, 1, 2}};
    unsigned materialID = 0;
    bool isUnlit = false;
    XMFLOAT4 baseColorFactor = {1, 1, 1, 1};
    XMFLOAT3 emissiveFactor = {};
    float metallicFactor = 0, roughnessFactor = 1;
    float occlusionStrength = 1, alphaCutoff = .5f;
    std::vector<Texture> textures;
};
struct Model { std::vector<Mesh> meshes; };
struct Instance
{
    unsigned editorId = 1;
    Model* pModel = nullptr;
    XMFLOAT3 translation = {}, rotation = {}, scale = {1, 1, 1};
    unsigned customMaterialID = UINT32_MAX;
    bool isTransparent = false, isCutout = false;
};
static float Dot(XMFLOAT3 a, XMFLOAT3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static XMFLOAT3 Unit(XMFLOAT3 v)
{
    const float inverse = 1 / std::sqrt(Dot(v, v));
    return {v.x*inverse, v.y*inverse, v.z*inverse};
}

int main(int argc, char** argv)
{
    nlohmann::json report;
    unsigned checks = 0;
    const auto require = [&checks](bool passed, const char* error) {
        ++checks;
        if (!passed) throw std::runtime_error(error);
    };
    try
    {
        const XMFLOAT3 n = { .577350269f, .577350269f, .577350269f };
        const XMFLOAT3 t = { .707106781f, -.707106781f, 0 };
        const XMFLOAT3 b = { .408248290f, .408248290f, -.816496581f };
        Mesh mesh;
        mesh.vertices = {{{}, n, {0, 0}, t, b}, {t, n, {1, 0}, t, b}, {b, n, {0, 1}, t, b}};
        Model model{{mesh}};
        struct Test { const char* name; XMFLOAT3 scale, rotation, normal, tangent, bitangent; };
        const Test cases[] = {
            {"identity", {1, 1, 1}, {}, n, t, b},
            {"nonuniform", {2, 1, 1}, {}, {.288675135f, .577350269f, .577350269f},
                {1.414213562f, -.707106781f, 0}, {.816496581f, .408248290f, -.816496581f}},
            {"nonuniform_rotated_y", {2, 1, 1}, {0, DirectX::XM_PIDIV2, 0},
                {.577350269f, .577350269f, -.288675135f},
                {0, -.707106781f, -1.414213562f}, {-.816496581f, .408248290f, -.816496581f}}
        };
        nlohmann::json results = nlohmann::json::array();
        for (const auto& test : cases)
        {
            VXGIWorld world;
            Instance instance; instance.pModel = &model; instance.scale = test.scale; instance.rotation = test.rotation;
            require(world.Update(std::vector<Instance>{instance}, .5f), world.GetLastError().c_str());
            require(world.GetTriangles().size() == 1, "Production world lost the triangle");
            const auto& triangle = world.GetTriangles().front();
            for (unsigned vertex = 0; vertex < 3; ++vertex)
            {
                const auto actualN = Unit(triangle.shadingNormals[vertex]);
                const auto actualT = Unit(triangle.tangents[vertex]);
                const auto actualB = Unit(triangle.bitangents[vertex]);
                require(Dot(actualN, Unit(test.normal)) > .99999f, "Normal no longer uses inverse-transpose");
                require(Dot(actualT, Unit(test.tangent)) > .99999f, "Tangent does not follow the linear transform");
                require(Dot(actualB, Unit(test.bitangent)) > .99999f, "Bitangent does not follow the linear transform");
                require(std::abs(Dot(actualN, actualT)) < 1e-5f && std::abs(Dot(actualN, actualB)) < 1e-5f,
                    "Transformed surface directions leave the tangent plane");
                require(Dot(Unit(triangle.geometricNormal), actualN) > .99999f, "Geometry and shading normal disagree");
                require(triangle.sourceVertexIndices[vertex] == vertex, "Surface identity changed during attribute transform");
            }
            results.push_back({{"name", test.name}, {"passed", true}});
        }
        report = {{"passed", true}, {"checks", checks}, {"cases", results}, {"gpu_used", false}};
    }
    catch (const std::exception& error) { report = {{"passed", false}, {"checks", checks}, {"error", error.what()}}; }
    std::ofstream output(argc > 1 ? argv[1] : "geometry-transform-report.json");
    output << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return report.value("passed", false) && output.good() ? 0 : 1;
}
