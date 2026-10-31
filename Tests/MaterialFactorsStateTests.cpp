#include "Passes/RenderStructs.h"
#include "Resources/VXGISurface.h"
#include "ThirdParty/json.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

int main(int argc, char** argv)
{
    nlohmann::json report;
    unsigned checks = 0;
    const auto require = [&checks](bool passed, const char* message) {
        ++checks;
        if (!passed) throw std::runtime_error(message);
    };
    try
    {
        static_assert(sizeof(MaterialData) == 80 && sizeof(VXGISurface::MaterialRecord) == 80);
        std::vector<MaterialData> materials(1);
        materials[0].baseColorFactor = {1, 1, 1, 1};
        require(materials[0].metallicFactor == 1 && materials[0].roughnessFactor == 1,
            "Default material factors must be neutral multipliers");
        require(materials[0].occlusionStrength == 1 && materials[0].alphaCutoff == .5f,
            "Default AO strength or alpha cutoff differs from glTF defaults");
        VXGISurface surface;
        const auto update = [&] { return surface.Update(materials, 11, 12, 13, {}, 14); };
        require(update(), "Initial production material state failed");
        const auto revisions = [&] {
            return std::array<uint64_t, 6>{surface.GetRevision(), surface.GetMaterialRevision(),
                surface.GetGeometryRevision(), surface.GetCoverageRevision(),
                surface.GetLightingRevision(), surface.GetSamplingRevision()};
        };
        for (unsigned factor = 0; factor < 4; ++factor)
        {
            const auto before = revisions();
            if (factor == 0) materials[0].metallicFactor = 0.25f;
            else if (factor == 1) materials[0].roughnessFactor = 0.8f;
            else if (factor == 2) materials[0].occlusionStrength = .5f;
            else materials[0].occlusionIdx = 17;
            require(update(), "Scalar factor edit was rejected");
            const auto& copied = surface.GetMaterials().at(0);
            require(copied.metallicFactor == materials[0].metallicFactor &&
                copied.roughnessFactor == materials[0].roughnessFactor &&
                copied.occlusionIdx == materials[0].occlusionIdx &&
                copied.occlusionStrength == materials[0].occlusionStrength &&
                copied.alphaCutoff == materials[0].alphaCutoff, "Copied material differs from production MaterialData");
            const auto after = revisions();
            require(after[0] == before[0] + 1 && after[1] == before[1] + 1,
                "Scalar edit did not advance material/source revision");
            require(std::equal(after.begin() + 2, after.end(), before.begin() + 2),
                "Scalar edit changed geometry, coverage, lighting or sampling revision");
            require(!surface.GetStats().geometryChanged && !surface.GetStats().coverageChanged &&
                surface.GetStats().materialChanged, "Scalar edit was classified incorrectly");
            require(update() && revisions() == after, "Unchanged material state did not stay idle");
        }
        const auto beforeCutoff = revisions();
        materials[0].alphaCutoff = .2f;
        require(update() && surface.GetMaterials()[0].alphaCutoff == .2f,
            "Authored alpha cutoff was not copied");
        const auto afterCutoff = revisions();
        require(afterCutoff[0] == beforeCutoff[0] + 1 && afterCutoff[1] == beforeCutoff[1] + 1 &&
            afterCutoff[3] == beforeCutoff[3] + 1, "Cutoff edit did not invalidate material and coverage state");
        require(afterCutoff[2] == beforeCutoff[2] && afterCutoff[4] == beforeCutoff[4] &&
            afterCutoff[5] == beforeCutoff[5], "Cutoff edit changed geometry, lighting or sampling revision");
        require(surface.GetStats().materialChanged && surface.GetStats().coverageChanged &&
            !surface.GetStats().geometryChanged, "Cutoff edit was classified incorrectly");
        require(update() && revisions() == afterCutoff, "Unchanged cutoff did not stay idle");
        // glTF imposes a lower bound of zero, but no upper bound. A cutoff
        // above one is valid and deliberately rejects every normalized alpha.
        materials[0].alphaCutoff = 1.25f;
        require(update() && surface.GetMaterials()[0].alphaCutoff == 1.25f,
            "Valid cutoff above one was rejected or clamped");
        const auto aboveOne = revisions();
        require(aboveOne[0] == afterCutoff[0] + 1 && aboveOne[1] == afterCutoff[1] + 1 &&
            aboveOne[3] == afterCutoff[3] + 1 && aboveOne[2] == afterCutoff[2] &&
            aboveOne[4] == afterCutoff[4] && aboveOne[5] == afterCutoff[5],
            "Cutoff above one did not invalidate only material and coverage");
        const auto validMaterial = materials[0];
        const auto committed = surface.GetMaterials().at(0);
        const auto committedRevisions = revisions();
        const float invalid[] = {-0.01f, 1.01f, std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
        for (unsigned factor = 0; factor < 4; ++factor) for (float value : invalid)
        {
            if (factor == 3 && value == 1.01f) continue;
            materials[0] = validMaterial;
            if (factor == 0) materials[0].metallicFactor = value;
            else if (factor == 1) materials[0].roughnessFactor = value;
            else if (factor == 2) materials[0].occlusionStrength = value;
            else materials[0].alphaCutoff = value;
            require(!update() && !surface.GetLastError().empty(), "Invalid scalar factor was accepted");
            require(revisions() == committedRevisions &&
                std::memcmp(&committed, surface.GetMaterials().data(), sizeof(committed)) == 0,
                "Rejected factor edit changed the last committed state");
        }
        materials[0] = validMaterial;
        require(update() && surface.GetLastError().empty() && revisions() == committedRevisions,
            "Valid state did not recover after rejected edits");
        report = {{"passed", true}, {"checks", checks}, {"material_stride", sizeof(MaterialData)},
            {"scalar_edits", 5}, {"texture_edits", 1}, {"invalid_scalar_cases", 19}, {"gpu_used", false}};
    }
    catch (const std::exception& error) { report = {{"passed", false}, {"checks", checks}, {"error", error.what()}}; }
    const std::string result = report.dump(2);
    std::ofstream output(argc > 1 ? argv[1] : "material-factors-state-report.json");
    output << result << '\n';
    std::cout << result << '\n';
    return report.value("passed", false) && output.good() ? 0 : 1;
}
