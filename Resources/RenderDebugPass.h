#pragma once

#include <cstdint>

// Temporary viewport presentation selector. Quality and feature settings stay
// in Pipeline.json; this selection lives only in the running editor.
enum class RenderDebugPass
{
    Default,
    SSGIOnly,
    GIOnly,
    VXGIOff
};

inline bool IsGIOnlyDebugPass(RenderDebugPass pass)
{
    return pass == RenderDebugPass::SSGIOnly || pass == RenderDebugPass::GIOnly;
}

inline uint32_t RenderDebugWorldMode(RenderDebugPass pass)
{
    return pass == RenderDebugPass::SSGIOnly || pass == RenderDebugPass::VXGIOff ? 0u : 2u;
}
