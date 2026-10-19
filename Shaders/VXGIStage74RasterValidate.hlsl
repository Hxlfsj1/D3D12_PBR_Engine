// Background acceptance only: query real production raster samples through the
// production depth anchor and strict raw-probe visibility helpers.
#include "SSGIHybridCommon.hlsli"
#include "GeometryCommon.hlsli"
cbuffer Capture : register(b0)
{
    float4x4 viewMatrix;
    float4x4 projectionMatrix;
    float4x4 inverseViewProjection;
    SSGIHybridParameters hybrid;
    uint width; uint height; uint depthIndex; uint normalIndex;
    uint ormIndex; uint padding0; uint padding1; uint padding2;
};
StructuredBuffer<float> nodes : register(t0);
StructuredBuffer<uint> offsets : register(t1);
StructuredBuffer<uint> references : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
StructuredBuffer<float3> centers : register(t5);
StructuredBuffer<VXGIProbeCellEntry> lookup : register(t6);
StructuredBuffer<VXGIProbeRecord> rawRecords : register(t7);
SamplerState materialSampler : register(s0);
struct RasterResult
{
    float3 irradiance; uint anchorStatus;
    float supportWeight; uint acceptedCount; uint traceFailures; uint triangleIndex;
    uint eligible; uint depthULPs; float2 uvDelta;
    uint instanceId; uint firstHitStatus; float deviceDepth; float projectedDepth;
    uint failureInvalidInput; uint failureMiss; uint failureInvalidTrace; uint failureExhausted;
    uint failureEndpointDistance; uint failureEndpointPosition; uint failureOther; uint failedCornerMask;
};
RWStructuredBuffer<RasterResult> result : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 dispatch : SV_DispatchThreadID)
{
    if (dispatch.x >= width || dispatch.y >= height) return;
    RasterResult r = (RasterResult)0;
    r.triangleIndex = 0xffffffff;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[depthIndex];
    Texture2D<float4> normalTexture = ResourceDescriptorHeap[normalIndex];
    Texture2D<float4> ormTexture = ResourceDescriptorHeap[ormIndex];
    float depth = depthTexture.Load(int3(dispatch.xy, 0));
    if (depth < 1.0 && isfinite(depth))
    {
        r.eligible = ormTexture.Load(int3(dispatch.xy,0)).a >= 0.5;
        r.deviceDepth = depth;
        float2 uv = (float2(dispatch.xy) + 0.5) / float2(width, height);
        float3 nearPosition = ReconstructPosition(uv, 0.0, inverseViewProjection);
        float3 targetPosition = ReconstructPosition(uv, depth, inverseViewProjection);
        VXGISDFHit hit;
        bool anchor = SSGIPrimarySurfaceMatchDepth(nearPosition, targetPosition, uv, depth,
            uint2(width,height), viewMatrix, projectionMatrix, hybrid,
            nodes, offsets, references, triangles, materials, materialSampler, hit, r.anchorStatus);
        r.firstHitStatus = hit.traceStatus;
        if (hit.valid != 0 && hit.triangleIndex < hybrid.triangleCount)
        {
            r.triangleIndex = hit.triangleIndex;
            r.instanceId = triangles[hit.triangleIndex].instanceId;
            float4 clip = mul(mul(float4(hit.position,1),viewMatrix),projectionMatrix);
            r.projectedDepth = clip.z / clip.w;
            r.depthULPs = max(asuint(depth),asuint(r.projectedDepth)) - min(asuint(depth),asuint(r.projectedDepth));
            r.uvDelta = abs(clip.xy/clip.w * float2(0.5,-0.5) + 0.5 - uv);
        }
        if (anchor)
        {
            r.triangleIndex = hit.triangleIndex;
            float3 mappedNormal = normalTexture.Load(int3(dispatch.xy,0)).xyz * 2.0 - 1.0;
            VXGIProbeQueryResult query = VXGIProbeQuerySurface(hit, mappedNormal,
                SSGIHybridMakeQueryParameters(hybrid), lookup, centers, rawRecords,
                nodes, offsets, references, triangles, materials, materialSampler);
            r.irradiance = query.irradianceOverPi;
            r.supportWeight = query.supportWeight;
            r.acceptedCount = query.acceptedCount;
            r.traceFailures = query.traceFailures;
            if (query.traceFailures != 0)
            {
                // Diagnose only rejected contributors, keeping the production
                // result authoritative. The replay has the same immutable
                // inputs and trace interval; no offsets or skipped triangles.
                float3 lattice = hit.position / hybrid.voxelSize - 0.5;
                float3 baseFloat = floor(lattice);
                float3 fraction = lattice - baseFloat;
                int3 base = int3(baseFloat);
                VXGISDFGrid grid = SSGIHybridMakeGrid(hybrid);
                if (!all(isfinite(mappedNormal)) || VXGIProbeMaximumCoordinate(mappedNormal) == 0.0
                    || !all(isfinite(lattice)) || any(baseFloat < -2147483648.0) || any(baseFloat > 2147483520.0))
                    r.failureInvalidInput = 1;
                else [unroll] for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
                {
                    int3 corner = int3(cornerIndex & 1u, (cornerIndex >> 1) & 1u, (cornerIndex >> 2) & 1u);
                    float3 axisWeight = lerp(1.0-fraction,fraction,float3(corner));
                    if (axisWeight.x*axisWeight.y*axisWeight.z <= 0.0) continue;
                    uint probeIndex = VXGIProbeFindIndex(base+corner,lookup,hybrid.probeCount);
                    if (probeIndex == VXGI_PROBE_MISSING_INDEX || !VXGIProbeQueryRecordValid(rawRecords[probeIndex])) continue;
                    bool failure;
                    VXGIProbeSurfaceVisibility(hit,centers[probeIndex],grid,hybrid.triangleCount,
                        nodes,offsets,references,triangles,materials,hybrid.materialCount,
                        materialSampler,hybrid.materialMipLevel,failure);
                    if (!failure) continue;
                    r.failedCornerMask |= 1u << cornerIndex;
                    float3 delta = centers[probeIndex]-hit.position;
                    float distance = length(delta);
                    float tolerance = VXGIProbeQueryEndpointTolerance(hit.position,centers[probeIndex],distance,grid.cellSize);
                    if (!VXGIProbeQueryGridValid(grid) || !all(isfinite(delta)) || !isfinite(distance) || distance <= 0
                        || !isfinite(tolerance) || !isfinite(distance+tolerance))
                    { ++r.failureInvalidInput; continue; }
                    VXGISDFHit reverseHit;
                    bool found = VXGISDFTrace(centers[probeIndex],-delta/distance,0.0,distance+tolerance,
                        grid,nodes,offsets,references,triangles,materials,hybrid.materialCount,
                        materialSampler,hybrid.materialMipLevel,reverseHit);
                    if (!found || reverseHit.traceStatus != 1)
                    {
                        if (reverseHit.traceStatus == 0) ++r.failureMiss;
                        else if (reverseHit.traceStatus == 2) ++r.failureInvalidTrace;
                        else if (reverseHit.traceStatus == 3) ++r.failureExhausted;
                        else ++r.failureOther;
                    }
                    else if (reverseHit.triangleIndex != hit.triangleIndex) ++r.failureOther;
                    else if (abs(reverseHit.distance-distance)>tolerance) ++r.failureEndpointDistance;
                    else if (length(reverseHit.position-hit.position)>tolerance) ++r.failureEndpointPosition;
                    else ++r.failureOther;
                }
            }
        }
    }
    result[dispatch.y * width + dispatch.x] = r;
}
