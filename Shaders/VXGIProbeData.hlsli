#ifndef VXGI_PROBE_DATA_HLSLI
#define VXGI_PROBE_DATA_HLSLI

static const uint VXGI_PROBE_UNCLASSIFIED = 0;
static const uint VXGI_PROBE_VALID = 1;
static const uint VXGI_PROBE_DISABLED = 2;
static const uint VXGI_PROBE_REASON_NONE = 0;
static const uint VXGI_PROBE_REASON_BACKFACE = 1;
static const uint VXGI_PROBE_REASON_UNSAFE_ORIGIN = 2;
static const uint VXGI_PROBE_REASON_CLASSIFY_TRACE_FAILURE = 3;
static const uint VXGI_PROBE_REASON_SAMPLE_TRACE_FAILURE = 4;

// Exactly 96 bytes. SH is unconvolved incident radiance in [Y00, X, Y, Z]
// order. Coefficient RGB may be negative; the unused w component stays zero.
struct VXGIProbeRecord
{
    float4 sh[4];
    uint state;
    uint updateCount;
    uint sampleCount;
    uint lastUpdateFrame;
    float backfaceRatio;
    float minHitDistance;
    uint classificationRays;
    uint reason;
};

float4 VXGIProbeSHBasis(float3 direction)
{
    return float4(0.28209479177387814, 0.4886025119029199 * direction);
}

#endif
