#pragma once

#include <cstdint>
#include "RenderPassView.h"

// Temporary viewport presentation selector. Quality and feature settings stay
// in Pipeline.json; this selection lives only in the running editor.
enum class RenderDebugPass
{
    Default,
    SSGIOnly,
    GIOnly,
    VXGIOff
};

inline bool IsRenderDebugPassAvailable(RenderDebugPass pass, const RenderPassAvailability& available)
{
    switch (pass)
    {
    case RenderDebugPass::Default: return true;
    case RenderDebugPass::SSGIOnly:
    case RenderDebugPass::GIOnly: return IsScreenGIAvailable(available);
    case RenderDebugPass::VXGIOff: return IsWorldGIAvailable(available);
    }
    return false;
}

inline void NormalizeRenderDebugPass(RenderDebugPass& pass, const RenderPassAvailability& available)
{
    if (!IsRenderDebugPassAvailable(pass, available)) pass = RenderDebugPass::Default;
}

inline bool IsGIOnlyDebugPass(RenderDebugPass pass)
{
    return pass == RenderDebugPass::SSGIOnly || pass == RenderDebugPass::GIOnly;
}

inline uint32_t RenderDebugWorldMode(RenderDebugPass pass)
{
    return pass == RenderDebugPass::SSGIOnly || pass == RenderDebugPass::VXGIOff ? 0u : 2u;
}
