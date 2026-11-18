#pragma once

#include "../Resources/RenderPassView.h"
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

// Optional bounded runs use the same application render/capture path. These
// arguments select a result; they never enable a pipeline feature.
struct RenderPassRunOptions
{
    RenderPassView view;
    uint64_t frameLimit = 0;
    uint64_t captureFrame = 0;
    uint64_t captureEvery = 0; // Optional bounded continuous validation sequence.
    bool captureCompact = false;
    uint32_t captureROI[4] = {}; // Internal scene pixels: x,y,width,height; zero width disables.
    bool hidden = false;
    uint32_t sdfView = 0; // Geometry diagnostics, independent of the lighting pass selector.
    uint32_t cardQueryChannel = 0;
    uint32_t sdfSliceAxis = 2;
    float sdfSlicePosition = 0;
    std::string cameraPreset; // Optional saved pose for repeatable captures.
    float cameraStep[3] = {}; // Bounded-run world translation per rendered frame.

    bool RequestsCapture(uint64_t frame) const noexcept
    {
        return captureFrame != 0u && frame >= captureFrame &&
            (frame == captureFrame || (captureEvery != 0u && (frame - captureFrame) % captureEvery == 0u));
    }
};

inline constexpr std::array<const char*,10> CardViewNames={
    "placement","coverage","albedo","normal","emissive","depth","metallic","roughness","lighting","visibility"};
inline constexpr std::array<uint32_t,10> CardViewModes={18,19,20,21,22,23,24,25,29,30};
inline constexpr std::array<const char*,3> CardQueryModes={"mesh","point","global"};
inline constexpr std::array<const char*,6> CardQueryChannels={"albedo","normal","emissive","card-id","lighting","visibility"};
inline const char* CardViewName(uint32_t view)
{
    if(view>=26 && view<=28)return CardQueryModes[view-26];
    for(uint32_t i=0;i<CardViewModes.size();++i)if(CardViewModes[i]==view)return CardViewNames[i];
    return "none";
}

inline bool ParseRenderPassRunOptions(const char* args, RenderPassRunOptions& options, std::string& error)
{
    RenderPassRunOptions parsed;
    bool explicitResult = false;
    error.clear();
    std::string_view remaining = args ? args : "";
    const auto fail = [&error](const char* message) { error = message; return false; };
    const auto positiveInteger = [](std::string_view text, uint64_t& output) {
        if (text.empty()) return false;
        uint64_t number = 0;
        const auto converted = std::from_chars(text.data(), text.data() + text.size(), number);
        if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || number == 0)
            return false;
        output = number;
        return true;
    };
    while (!remaining.empty())
    {
        while (!remaining.empty() && std::isspace(static_cast<unsigned char>(remaining.front())))
            remaining.remove_prefix(1);
        if (remaining.empty()) break;
        size_t count = 0;
        while (count < remaining.size() && !std::isspace(static_cast<unsigned char>(remaining[count]))) ++count;
        const auto token = remaining.substr(0, count);
        remaining.remove_prefix(count);
        if (token == "--hidden") parsed.hidden = true;
        else if(token.starts_with("--card-query="))
        {
            const auto value=token.substr(13);bool found=false;
            for(uint32_t i=0;i<CardQueryModes.size();++i)
                if(value==CardQueryModes[i]){parsed.sdfView=26+i;found=true;break;}
            if(!found)return fail("--card-query requires mesh, point or global.");
        }
        else if(token.starts_with("--card-query-channel="))
        {
            const auto value=token.substr(21);bool found=false;
            for(uint32_t i=0;i<CardQueryChannels.size();++i)
                if(value==CardQueryChannels[i]){parsed.cardQueryChannel=i;found=true;break;}
            if(!found)return fail("--card-query-channel requires albedo, normal, emissive, card-id, lighting or visibility.");
        }
        else if (token.starts_with("--card-view="))
        {
            const auto value=token.substr(12);
            bool found=false;
            for(uint32_t i=0;i<CardViewNames.size();++i)
                if(value==CardViewNames[i]){parsed.sdfView=CardViewModes[i];found=true;break;}
            if(!found)return fail("Unknown --card-view result.");
        }
        else if (token.starts_with("--sdf-view="))
        {
            const auto value = token.substr(11);
            if (value == "mesh-slice") parsed.sdfView = 4;
            else if (value == "mesh-hit") parsed.sdfView = 5;
            else if (value == "mesh-normal") parsed.sdfView = 6;
            else if (value == "mesh-shadow") parsed.sdfView = 7;
            else if (value == "global-slice") parsed.sdfView = 13;
            else if (value == "global-hit") parsed.sdfView = 14;
            else if (value == "global-normal") parsed.sdfView = 15;
            else if (value == "hybrid-hit") parsed.sdfView = 17;
            else return fail("Unknown --sdf-view geometry diagnostic.");
        }
        else if (token.starts_with("--sdf-slice="))
        {
            const auto value = token.substr(12);
            if (value.size() < 3 || value[1] != ',' || (value[0] != 'x' && value[0] != 'y' && value[0] != 'z'))
                return fail("--sdf-slice requires x,position or y,position or z,position.");
            parsed.sdfSliceAxis = value[0] == 'x' ? 0 : value[0] == 'y' ? 1 : 2;
            const auto text = value.substr(2);
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed.sdfSlicePosition);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || !std::isfinite(parsed.sdfSlicePosition))
                return fail("--sdf-slice requires a finite world coordinate.");
        }
        else if (token.starts_with("--camera-preset="))
        {
            const auto value = token.substr(16);
            if (value.empty()) return fail("--camera-preset requires a saved camera JSON path.");
            parsed.cameraPreset = std::string(value);
        }
        else if (token.starts_with("--camera-step="))
        {
            auto values = token.substr(14);
            for (uint32_t component = 0u; component < 3u; ++component)
            {
                const size_t comma = values.find(',');
                const auto part = values.substr(0, comma);
                auto& value = parsed.cameraStep[component];
                const auto converted = std::from_chars(part.data(), part.data() + part.size(), value);
                if (part.empty() || converted.ec != std::errc{} || converted.ptr != part.data() + part.size() ||
                    !std::isfinite(value) || ((component < 2u) != (comma != std::string_view::npos)))
                    return fail("--camera-step requires three finite world translations per frame: x,y,z.");
                if (comma != std::string_view::npos) values.remove_prefix(comma + 1u);
            }
        }
        else if (token.starts_with("--frames="))
        {
            if (!positiveInteger(token.substr(9), parsed.frameLimit))
                return fail("--frames requires a positive uint64 value.");
        }
        else if (token.starts_with("--capture-frame="))
        {
            if (!positiveInteger(token.substr(16), parsed.captureFrame))
                return fail("--capture-frame requires a positive uint64 value.");
        }
        else if (token.starts_with("--capture-every="))
        {
            if (!positiveInteger(token.substr(16), parsed.captureEvery))
                return fail("--capture-every requires a positive uint64 interval.");
        }
        else if (token == "--capture-compact") parsed.captureCompact = true;
        else if (token.starts_with("--capture-roi="))
        {
            auto values = token.substr(14);
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                const size_t comma = values.find(',');
                const auto part = values.substr(0, comma);
                auto& value = parsed.captureROI[component];
                const auto converted = std::from_chars(part.data(), part.data() + part.size(), value);
                if (part.empty() || converted.ec != std::errc{} || converted.ptr != part.data() + part.size() ||
                    ((component < 3u) != (comma != std::string_view::npos)))
                    return fail("--capture-roi requires four uint32 values: x,y,width,height.");
                if (comma != std::string_view::npos) values.remove_prefix(comma + 1u);
            }
            if (!parsed.captureROI[2] || !parsed.captureROI[3] ||
                uint64_t(parsed.captureROI[0]) + parsed.captureROI[2] > UINT32_MAX ||
                uint64_t(parsed.captureROI[1]) + parsed.captureROI[3] > UINT32_MAX)
                return fail("--capture-roi requires positive extents without uint32 overflow.");
        }
        else if (token.starts_with("--pass-view="))
        {
            const auto value = token.substr(12);
            bool found = false;
            for (const auto& pass : RenderPassCatalog)
                if (value == pass.id) { parsed.view.pass = pass.pass; found = true; break; }
            if (!found) return fail("--pass-view requires a known logical pass ID.");
        }
        else if (token.starts_with("--pass-result="))
        {
            const auto value = token.substr(14);
            if (value == "output") parsed.view.result = RenderPassResult::PassOutput;
            else if (value == "accumulated") parsed.view.result = RenderPassResult::Accumulated;
            else return fail("--pass-result requires output or accumulated.");
            explicitResult = true;
        }
        else if (token.starts_with("--pass-channel="))
        {
            const auto value = token.substr(15);
            bool found = false;
            for (const auto& channel : RenderPassChannels)
                if (value == channel.id) { parsed.view.channel = channel.channel; found = true; break; }
            if (!found) return fail("--pass-channel requires a known GBuffer channel ID.");
        }
        else if (token.starts_with("--pass-gi-source="))
        {
            const auto value = token.substr(17);
            if (value == "default") parsed.view.giSource = RenderPassGISource::Default;
            else if (value == "ssgi") parsed.view.giSource = RenderPassGISource::SSGIOnly;
            else return fail("--pass-gi-source requires default or ssgi.");
        }
        else if (token == "--frames" || token == "--capture-frame" || token == "--pass-view" ||
            token == "--capture-every" || token == "--capture-roi" ||
            token == "--pass-result" || token == "--pass-channel" || token == "--pass-gi-source" ||
            token == "--camera-preset" || token == "--camera-step" || token == "--sdf-view" || token == "--sdf-slice" ||
            token == "--card-view" || token == "--card-query" || token == "--card-query-channel")
            return fail("Pass-view options require --name=value syntax.");
        // Other application arguments, including --ui-scale, belong to their
        // existing parser and do not affect these options.
    }
    if (parsed.frameLimit && parsed.captureFrame > parsed.frameLimit)
        return fail("--capture-frame must not exceed --frames.");
    if (parsed.captureEvery && (!parsed.captureFrame || !parsed.frameLimit))
        return fail("--capture-every requires --capture-frame as the start and a bounded --frames run.");
    if ((parsed.cameraStep[0] != 0 || parsed.cameraStep[1] != 0 || parsed.cameraStep[2] != 0) &&
        (!parsed.frameLimit || parsed.cameraPreset.empty()))
        return fail("--camera-step requires a bounded --frames run and an explicit --camera-preset.");
    if ((parsed.captureCompact || parsed.captureROI[2]) && parsed.view.pass != RenderPassId::GI)
        return fail("Compact/ROI capture currently requires --pass-view=gi.");
    if (!SupportsRenderPassResult(parsed.view.pass, parsed.view.result))
    {
        if (explicitResult) return fail("The selected pass does not support that result mode.");
        const auto* info = GetRenderPassInfo(parsed.view.pass);
        parsed.view.result = info->passOutput ? RenderPassResult::PassOutput : RenderPassResult::Accumulated;
    }
    if (parsed.view.giSource != RenderPassGISource::Default &&
        (parsed.view.pass != RenderPassId::GI || parsed.view.result != RenderPassResult::PassOutput))
        return fail("A diagnostic GI source requires --pass-view=gi --pass-result=output.");
    options = parsed;
    return true;
}
