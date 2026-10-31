// Exercise the real ShadowPass::PrepareFrame, including the culling bounds and
// transposed GPU matrix. Test a world-anchored lattice, not matching source code.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION
#include "Passes/ShadowPass.h"
#include "ThirdParty/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <filesystem>

using namespace DirectX;
static size_t checks = 0;
static void Require(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static ShadowPass::FrameData Frame(Camera& camera, XMFLOAT3 light)
{
    return ShadowPass::PrepareFrame({&camera, light, 1.6f, 4096.0f});
}
static XMFLOAT2 TexelPosition(const ShadowPass::FrameData& frame, UINT cascade, XMVECTOR point)
{
    const auto matrix = XMMatrixTranspose(XMLoadFloat4x4(&frame.lightViewProj[cascade]));
    const auto clip = XMVector3TransformCoord(point, matrix);
    return {(XMVectorGetX(clip) * .5f + .5f) * 4096,
            (-XMVectorGetY(clip) * .5f + .5f) * 4096};
}
int main(int argc, char** argv)
{
    nlohmann::json report;
    try
    {
        float maxResidual = 0;
        nlohmann::json cases = nlohmann::json::array();
        const XMFLOAT3 origins[] = {{-.1158f,3.2589f,7.0816f}, {-32,8,-64}};
        const XMFLOAT3 lights[] = {{.5f,-.5f,1}, {0,-1,0}};
        for (auto origin : origins) for (auto light : lights)
        {
            Camera camera(origin, {0,1,0}, 50.25f, 40);
            camera.Zoom = 45;
            const auto base = Frame(camera, light);
            const auto view = XMLoadFloat4x4(&base.lightView);
            const auto inverseView = XMMatrixInverse(nullptr, view);
            const auto* widths = &base.cascadeOrthoWidths.x;
            for (UINT cascade = 0; cascade < NUM_CASCADES; ++cascade)
            {
                const float texelSize = widths[cascade] / 4096;
                const auto point = XMVectorAdd(XMLoadFloat3(&origin), XMVectorSet(2, -1, 3, 0));
                const auto originalTexel = TexelPosition(base, cascade, point);
                for (UINT axis = 0; axis < 2; ++axis)
                {
                    const auto basis = XMVector3TransformNormal(
                        axis == 0 ? XMVectorSet(1,0,0,0) : XMVectorSet(0,1,0,0), inverseView);
                    for (float step : {-.75f, -.25f, .25f, .75f, 1.25f, -1.25f})
                    {
                        Camera moved = camera;
                        XMStoreFloat3(&moved.Position, XMVectorAdd(XMLoadFloat3(&origin),
                            XMVectorScale(basis, step * texelSize)));
                        const auto actual = Frame(moved, light);
                        const auto sample = TexelPosition(actual, cascade, point);
                        const float dx = sample.x - originalTexel.x, dy = sample.y - originalTexel.y;
                        const float residual = (std::max)(std::abs(dx - std::round(dx)),
                            std::abs(dy - std::round(dy)));
                        maxResidual = (std::max)(maxResidual, residual);
                        Require(std::isfinite(residual) && residual < .02f,
                            "World point drifts by a fractional shadow texel");
                        Require(std::abs((&actual.cascadeOrthoWidths.x)[cascade] - widths[cascade]) < 1e-5f,
                            "Camera translation changes the cascade scale");
                    }
                }
                // Caster culling and GPU clip space must describe the same box.
                XMFLOAT3 corners[8];
                base.cascadeShadowAreas[cascade].GetCorners(corners);
                const auto matrix = XMMatrixTranspose(XMLoadFloat4x4(&base.lightViewProj[cascade]));
                for (const auto& corner : corners)
                {
                    const auto world = XMVector3TransformCoord(XMLoadFloat3(&corner), inverseView);
                    const auto clip = XMVector3TransformCoord(world, matrix);
                    Require(std::abs(std::abs(XMVectorGetX(clip)) - 1) < 1e-4f &&
                        std::abs(std::abs(XMVectorGetY(clip)) - 1) < 1e-4f,
                        "Caster bounds disagree with the sampling/drawing matrix in XY");
                    const float z = XMVectorGetZ(clip);
                    Require(std::isfinite(z) && (std::abs(z) < 1e-4f || std::abs(z - 1) < 1e-4f),
                        "Caster bounds disagree with the sampling/drawing matrix in Z");
                }
            }
            for (float yaw : {-130.f, 0.f, 110.f})
            {
                Camera rotated(origin, {0,1,0}, yaw, -10);
                rotated.Zoom = 45;
                const auto frame = Frame(rotated, light);
                for (UINT cascade = 0; cascade < NUM_CASCADES; ++cascade)
                    Require(std::abs((&frame.cascadeOrthoWidths.x)[cascade] - widths[cascade]) < 1e-5f,
                        "Camera rotation changes the stable cascade scale");
            }
            cases.push_back({{"origin",{origin.x,origin.y,origin.z}}, {"light",{light.x,light.y,light.z}},
                {"passed",true}});
        }
        // Transition corners must be valid in both maps, including the widest
        // possible PCSS offset and the comparison sampler's bilinear footprint.
        Camera overlapCamera(origins[0], {0,1,0}, 50.25f, 40);
        overlapCamera.Zoom = 45;
        const float sun = std::tan(XMConvertToRadians(.266f));
        for (float fraction : {0.f, .1f, .3f})
        {
            const auto frame = ShadowPass::PrepareFrame({&overlapCamera,lights[0],1.6f,4096,fraction,sun});
            for (UINT i = 0; i < NUM_CASCADES; ++i)
            {
                const float split = ShadowPass::kCascadeSplits[i+1];
                const float end = XMVectorGetByIndex(XMLoadFloat4(&frame.cascadeBlendEnds), i);
                Require(std::abs(end - (split + (i<3 ? fraction*(split-ShadowPass::kCascadeSplits[i]) : 0))) < 1e-5f,
                    "Transition end disagrees with logical split interval");
                if (i == 3 || fraction == 0) continue;
                XMFLOAT3 corners[8];
                overlapCamera.GetWorldSpaceFrustum(1.6f,split,end).GetCorners(corners);
                for (UINT layer : {i,i+1})
                {
                    const float width = XMVectorGetByIndex(XMLoadFloat4(&frame.cascadeOrthoWidths), layer);
                    const float range = XMVectorGetByIndex(XMLoadFloat4(&frame.cascadeDepthRanges), layer);
                    const float margin = range * sun * 1.25f / width + .5f/4096;
                    for (auto corner : corners)
                    {
                        const auto point = XMLoadFloat3(&corner);
                        const auto uv = TexelPosition(frame,layer,point);
                        Require(uv.x/4096 >= margin && uv.x/4096 <= 1-margin &&
                            uv.y/4096 >= margin && uv.y/4096 <= 1-margin,
                            "Transition/filter footprint leaves a neighboring shadow map");
                        const auto clip = XMVector3TransformCoord(point,
                            XMMatrixTranspose(XMLoadFloat4x4(&frame.lightViewProj[layer])));
                        Require(XMVectorGetZ(clip) >= -1e-5f && XMVectorGetZ(clip) <= 1.00001f,
                            "Transition leaves the neighboring cascade's depth range");
                    }
                }
            }
        }
        SettingsManager settings;
        const auto workingDirectory = std::filesystem::current_path();
        struct DirectoryRestore { std::filesystem::path path; ~DirectoryRestore() { std::filesystem::current_path(path); } } restore{workingDirectory};
        const auto settingsRoot = std::filesystem::absolute(std::filesystem::path(argc>1?argv[1]:"report.json")).parent_path()/"settings-fixture";
        std::filesystem::create_directories(settingsRoot/"Settings");
        const auto configPath = settingsRoot/"Settings/Pipeline.json";
        { std::ofstream f(settingsRoot/"Settings/Window.json"); f << "{}"; }
        { std::ofstream f(settingsRoot/"Settings/Lighting.json"); f << "{}"; }
        std::filesystem::current_path(settingsRoot);
        for (float fraction : {0.f,.1f,.3f})
        {
            { std::ofstream f(configPath); f << nlohmann::json{{"shadows",{{"cascades",{{"transition_fraction",fraction}}}}}}; }
            settings.LoadAllSettingsFromJson();
            Require(settings.pipeline.shadowCascadeTransitionFraction == fraction, "Explicit transition fraction lost");
        }
        { std::ofstream f(configPath); f << "{}"; }
        settings.LoadAllSettingsFromJson();
        Require(settings.pipeline.shadowCascadeTransitionFraction == .1f, "Missing transition must default to 0.1");
        for (const nlohmann::json fraction : {nlohmann::json(-.1),nlohmann::json(.31),nlohmann::json("invalid"),nlohmann::json(nullptr)})
        {
            { std::ofstream f(configPath); f << nlohmann::json{{"shadows",{{"cascades",{{"transition_fraction",fraction}}}}}}; }
            bool rejected = false;
            try { settings.LoadAllSettingsFromJson(); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid transition configuration accepted");
        }
        report = {{"passed",true},{"checks",checks},{"cases",cases},
            {"max_fractional_texel_residual",maxResidual},{"gpu_used",false}};
    }
    catch (const std::exception& error) { report = {{"passed",false},{"checks",checks},{"error",error.what()}}; }
    const char* path = argc > 1 ? argv[1] : "report.json";
    std::ofstream file(path); file << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return file && report.value("passed",false) ? 0 : 1;
}
