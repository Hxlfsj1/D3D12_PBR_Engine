#define NOMINMAX
#include <windows.h>
#include "../Resources/Settings_Manager.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void WritePipeline(const std::filesystem::path& path, const nlohmann::json& document)
    {
        std::ofstream stream(path);
        Check(bool(stream), "Could not write temporary config fixture.");
        stream << document.dump(2);
    }
}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2, "Expected a distinct temporary fixture path.");
        const std::filesystem::path fixtureRoot = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(fixtureRoot / "Settings");
        const auto path = fixtureRoot / "Settings" / "Pipeline.json";
        WritePipeline(fixtureRoot / "Settings" / "Window.json", nlohmann::json::object());
        WritePipeline(fixtureRoot / "Settings" / "Lighting.json", nlohmann::json::object());
        struct WorkingDirectory
        {
            std::filesystem::path previous = std::filesystem::current_path();
            ~WorkingDirectory() { std::error_code ignored; std::filesystem::current_path(previous, ignored); }
        } directory;
        std::filesystem::current_path(fixtureRoot);
        SettingsManager settings;
        Check(settings.ssgi.traceDownsampleFactor == 2, "Legacy SSGI default changed.");
        WritePipeline(path, { {"ssgi", {{"enabled", true}, {"quality", 1}, {"radius", 2}}} });
        settings.LoadAllSettingsFromJson();
        Check(settings.ssgi.traceDownsampleFactor == 2, "Missing factor did not preserve the legacy default.");
        for (const int factor : {2, 4, 8})
        {
            WritePipeline(path, { {"ssgi", {{"trace_downsample_factor", factor}}} });
            settings.LoadAllSettingsFromJson();
            Check(settings.ssgi.traceDownsampleFactor == factor, "Valid factor did not load.");
        }
        uint32_t rejected = 0;
        const std::vector<nlohmann::json> invalid = {-1, 0, 1, 3, 5, 16, 4.0, "4", true, nullptr, UINT64_MAX};
        for (const auto& factor : invalid)
        {
            WritePipeline(path, { {"ssgi", {{"trace_downsample_factor", factor}}} });
            try { settings.LoadAllSettingsFromJson(); }
            catch (const std::exception&) { ++rejected; }
            Check(settings.ssgi.traceDownsampleFactor == 8, "Invalid factor mutated the last valid setting.");
        }
        Check(rejected == invalid.size(), "Malformed factor was accepted.");
        Check(SSGIConfig::IsValidTraceDownsampleFactor(2) && SSGIConfig::IsValidTraceDownsampleFactor(4) &&
            SSGIConfig::IsValidTraceDownsampleFactor(8) && !SSGIConfig::IsValidTraceDownsampleFactor(0) &&
            !SSGIConfig::IsValidTraceDownsampleFactor(3), "Runtime factor validation disagrees with the config loader.");
        std::cout << "SSGIResolutionConfig: legacy/default and 2/4/8 factors passed; " << rejected << " invalid JSON values rejected.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "SSGIResolutionConfig failure: " << exception.what() << '\n';
        return 1;
    }
}
