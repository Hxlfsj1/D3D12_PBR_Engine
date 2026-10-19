// Test-only deterministic replay of the production trace. No GI output writes.
// The optional hooks in TraceHybridRay preprocess away in normal builds.
#define SSGI_PASS_TRACE 1
#define VXGI_SSGI_THRESHOLD 1
#define SSGI_MODE_FROM_SRV 1
#define SSGI_RAY_ROUTING_DIAGNOSTICS 1
void SSGIRoutingScreenStatus(uint status);
void SSGIRoutingDisabled();
void SSGIRoutingFallback();
void SSGIRoutingInvalidSegment();
#include "../Shaders/Shaders_For_SSGI.hlsl"

RWStructuredBuffer<uint> routingCounts : register(u0, space2);
// Private per shader invocation, never groupshared. Group 0 is all traced
// receivers; group 1 approximates the car by unbiased receiver world Y > .03.
static bool routingAboveGround = false;

void RoutingAdd(uint index)
{
    InterlockedAdd(routingCounts[index], 1);
    if (routingAboveGround) InterlockedAdd(routingCounts[32 + index], 1);
}
void SSGIRoutingScreenStatus(uint status) { if (status <= 5) RoutingAdd(5 + status); }
void SSGIRoutingDisabled() { RoutingAdd(17); }
void SSGIRoutingFallback() { RoutingAdd(16); }
void SSGIRoutingInvalidSegment() { RoutingAdd(18); }

// Each group contains 32 counters. CPU field names and partition checks are
// kept in VXGIRayRoutingCapture.inl; slots 5..10 are the six screen statuses,
// slots 19..23 are the five world threshold statuses. Slot 24 is unknown status.
[numthreads(8, 8, 1)]
void CSMain_Count(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize)) return;
    routingAboveGround = false;
    RoutingAdd(0);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    uint2 pixel = id.xy * max(traceDownsampleFactor, 2u);
    float z = depth.Load(int3(pixel, 0));
    if (z >= 1) { RoutingAdd(1); return; }
    float3 position = ReconstructPosition((pixel + 0.5) / sceneSize, z, invProjMat);
    float4 receiverWorld = mul(float4(position, 1), invViewMat);
    routingAboveGround = all(isfinite(receiverWorld)) && receiverWorld.w != 0
        && receiverWorld.y / receiverWorld.w > 0.03;
    if (routingAboveGround) InterlockedAdd(routingCounts[32], 1);
    if (orm.Load(int3(pixel, 0)).a < 0.5) { RoutingAdd(2); return; }
    RoutingAdd(3);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(pixel, 0)).xyz, viewMat);
    Texture2DArray<uint2> blueNoise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    [loop] for (uint ray = 0; ray < rayCount; ++ray)
    {
        RoutingAdd(4);
        float2 xi = LoadSTBN2D(blueNoise, id.xy, frameIndex, ray);
        float3 viewDirection = CosineDirection(xi, n);
        SSGIHybridRayResult result = TraceHybridRay(pixel, position + n * normalBias, viewDirection);
        bool positive = any(result.radiance > 0.0);
        if (result.backend == SSGI_BACKEND_SCREEN)
        {
            if (result.sourceValid != 0) RoutingAdd(11);
            RoutingAdd(positive ? 12 : 13);
        }
        else if (result.backend == SSGI_BACKEND_WORLD)
        {
            RoutingAdd(result.worldTraceStatus <= 4 ? 19 + result.worldTraceStatus : 24);
            if (result.hit != 0)
            {
                RoutingAdd(result.probeHasSupport != 0 ? 25 : 28);
                if (result.sourceValid != 0) RoutingAdd(26);
                RoutingAdd(positive ? 27 : 29);
            }
        }
    }
}
