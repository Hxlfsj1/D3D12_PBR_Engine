// Executed in the real deferred pixel shader by the isolated validation runner.
// Inputs span resolutions, anisotropic footprints, winding, and degenerate data.
float4 TestShadowReceiverPlane(uint caseIndex)
{
    static const float scales[8] = {1, 1e-3f, 1e-6f, 1e-12f, 1e-20f, 1e10f, 1e20f, 1e30f};
    float3 dx = float3(1, 0, 0), dy = float3(0, 1, 0);
    if (caseIndex < 24)
    {
        uint group = caseIndex / 8;
        float2 slope = group == 0 ? float2(0.25f, -0.5f) :
            group == 1 ? float2(2, -3) : float2(100, 30);
        dx = float3(1, 0, slope.x) * scales[caseIndex % 8];
        dy = float3(0, 1, slope.y) * scales[(caseIndex + 3) % 8];
        if (group == 1) dx = -dx;
    }
    else if (caseIndex == 24) dx = 0;
    else if (caseIndex == 25) dy = 0;
    else if (caseIndex == 26) dy = dx;
    else if (caseIndex == 27) dx.x = asfloat(0x7fc00000u);
    else if (caseIndex == 28) dy.y = asfloat(0x7f800000u);
    else if (caseIndex == 29) { dx = float3(0, 1, 0); dy = float3(0, 0, 1); }
    else if (caseIndex == 31) { dx = asfloat(0x7fc00000u).xxx; dy = 0; }
    return float4(CalculateShadowReceiverDepthGradient(dx, dy), 1, 1);
}
