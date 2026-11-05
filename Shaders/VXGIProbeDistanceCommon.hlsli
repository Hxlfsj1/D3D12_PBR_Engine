#ifndef VXGI_PROBE_DISTANCE_COMMON_HLSLI
#define VXGI_PROBE_DISTANCE_COMMON_HLSLI

// 8x8 octahedral bins, uint2 per bin. x: FP16 normalized radial distance
// and plane offset; y: octahedral 16+16-bit normal pointing away from probe.
// The nearest sampled hit supplies a local blocker plane; verified misses
// within two probe spacings carry a zero normal. An all-zero bin is unknown.
static const uint VXGI_PROBE_DISTANCE_BINS = 64;
float2 VXGIProbeOctSign(float2 p) { return float2(p.x >= 0 ? 1 : -1, p.y >= 0 ? 1 : -1); }
float2 VXGIProbeOctEncode(float3 direction)
{
    float3 n = direction / max(dot(abs(direction), 1.0), 1e-20);
    return n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * VXGIProbeOctSign(n.xy);
}
uint VXGIProbeDistanceBin(float3 direction)
{
    uint2 texel = min(uint2(saturate(VXGIProbeOctEncode(direction) * 0.5 + 0.5) * 8.0), 7u);
    return texel.x + texel.y * 8;
}
uint VXGIProbeDistanceNormalPack(float3 normal)
{
    uint2 p=uint2(round(saturate(VXGIProbeOctEncode(normal)*0.5+0.5)*65535.0));
    return max(p.x|(p.y<<16),1u); // Reserve zero for a verified miss.
}
float3 VXGIProbeDistanceNormalUnpack(uint packed)
{
    float2 p=float2(packed&65535u,packed>>16)/65535.0*2.0-1.0;
    float3 n=float3(p,1.0-abs(p.x)-abs(p.y));
    if(n.z<0) n.xy=(1.0-abs(n.yx))*VXGIProbeOctSign(n.xy);
    return normalize(n);
}
float VXGIProbeDistanceVisibility(uint descriptor, uint probeIndex, float3 offset, float spacing, float3 worldRayDirection)
{
    if(length(offset) < 1e-6 * spacing) return 1.0;
    StructuredBuffer<uint2> cache = ResourceDescriptorHeap[descriptor];
    uint2 bin = cache[probeIndex * VXGI_PROBE_DISTANCE_BINS + VXGIProbeDistanceBin(offset)];
    if(bin.x==0) return 0.0;
    if(bin.y==0) return 1.0;
    float planeDistance=f16tof32(bin.x>>16);
    float3 normal=VXGIProbeDistanceNormalUnpack(bin.y);
    float pointDistance=dot(normal,offset)/(2.0*spacing);
    // FP16 plane quantization is bounded by 0.00049 in this normalized range.
    float delta=pointDistance-planeDistance;
    if(abs(delta)<=0.0005) return dot(normal,worldRayDirection)>0.0?1.0:0.0;
    return delta<0.0?1.0:0.0;
}
#endif
