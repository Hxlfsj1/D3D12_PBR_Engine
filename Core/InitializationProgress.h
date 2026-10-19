#pragma once

#include <algorithm>
#include <functional>
#include <string>

// Optional, synchronous startup reporting. Resource builders remain independent
// of the loading screen; callers may map each operation's local 0..1 range.
using InitializationProgress = std::function<void(float, const std::string&)>;

inline void ReportInitializationProgress(const InitializationProgress& progress,
    float fraction, const std::string& status)
{
    if (progress)
        progress(std::clamp(fraction, 0.0f, 1.0f), status);
}
