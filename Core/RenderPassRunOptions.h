#pragma once

#include "../Resources/RenderPassView.h"
#include <charconv>
#include <cctype>
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
    bool hidden = false;
};

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
            else if (value == "mesh-sdf") parsed.view.giSource = RenderPassGISource::SSGIAndMeshSDF;
            else return fail("--pass-gi-source requires default, ssgi or mesh-sdf.");
        }
        else if (token == "--frames" || token == "--capture-frame" || token == "--pass-view" ||
            token == "--pass-result" || token == "--pass-channel" || token == "--pass-gi-source")
            return fail("Pass-view options require --name=value syntax.");
        // Other application arguments, including --ui-scale, belong to their
        // existing parser and do not affect these options.
    }
    if (parsed.frameLimit && parsed.captureFrame > parsed.frameLimit)
        return fail("--capture-frame must not exceed --frames.");
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
