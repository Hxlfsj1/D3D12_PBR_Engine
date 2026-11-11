// Reuse the production screen/SDF queries and their root bindings. The legacy
// entry points are dead-stripped when compiling one of the probe entry points.
#define SSGI_PASS_TRACE 1
#define VXGI_SSGI_THRESHOLD 1
#define VXGI_SSGI_GLOBAL 1
#define SSGI_MODE_FROM_SRV 1
#include "Shaders_For_SSGI.hlsl"

// Per-stage SRV/UAV views use the trace ABI's reserved channels. Depth pyramid
// descriptors remain in every levels[i].x, and worldMode remains at byte 604.
#define SP_POSITION levels[0].y
#define SP_NORMAL levels[0].z
#define SP_RECEIVER levels[0].w
#define SP_PREVIOUS_POSITION levels[1].y
#define SP_PREVIOUS_NORMAL levels[1].z
#define SP_PREVIOUS_RADIANCE levels[1].w
#define SP_RADIANCE_UAV levels[2].y
#define SP_RADIANCE_SRV levels[2].w
#define SP_FILTERED_UAV levels[3].y
#define SP_FILTERED_SRV levels[3].z
#define SP_HISTORY_LIMIT levels[3].w
#define SP_RAYS levels[4].y
#define SP_SCREEN_RAYS levels[4].z
#define SP_POSITION_UAV levels[5].z
#define SP_NORMAL_UAV levels[5].w
#define SP_RECEIVER_UAV levels[6].y
#define SP_JITTER_WIDTH saturate(asfloat(levels[6].z))
#define SP_ANCHOR_UAV levels[6].w
#define SP_PREVIOUS_ANCHOR levels[2].z



#include "ScreenProbeEmissive.hlsli"

uint2 ProbePixel(uint packed)
{
    uint index = packed - 1u;
    return uint2(index % sceneSize.x, index / sceneSize.x);
}

float3 ProbeDirection(float2 xi, float3 n)
{
    // Equal-solid-angle hemisphere cells: mu=cos(theta) is uniform. Radiance
    // radiance is stored per bin before SH solid-angle projection and diffuse convolution.
    n = normalize(n);
    float mu = xi.x, phi = 6.28318530718 * xi.y;
    float3 tangent = normalize(cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    return sqrt(max(0, 1 - mu * mu)) * (cos(phi) * tangent + sin(phi) * cross(n, tangent)) + mu * n;
}



float ProbeFootprint(uint2 pixel, float3 p)
{
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    // A held probe retains the actual ray origin, not the current candidate.
    // Compute pixel spacing from the candidate itself to avoid inflating the
    // footprint by its accumulated displacement from that held origin.
    float3 center = ReconstructPosition((pixel + 0.5) / sceneSize, z, invViewProjMat);
    return max(SameDepthPixelFootprint((pixel + 0.5) / sceneSize,
        z, center, sceneSize, invViewProjMat), 1e-5);
}

bool ProbeSurfaceCompatible(float3 p, float3 n, float3 q, float3 m, float footprint)
{
    return all(isfinite(q)) && all(isfinite(m)) && dot(n, m) >= 0.99 &&
        dot(ProbeDirection(float2(.5, 0), n), ProbeDirection(float2(.5, 0), m)) >= .98 &&
        SymmetricPlaneDistance(q - p, n, m) <= max(footprint * .75, .001);
}

bool ProbeHistorySampleVisible(float3 p, float3 n, float footprint)
{
    float3 viewPosition = mul(float4(p, 1), viewMat).xyz;
    if (viewPosition.z <= 0 || !all(isfinite(viewPosition))) return false;
    float3 projected = Project(viewPosition);
    if (!all(isfinite(projected)) || any(projected.xy < 0) || any(projected.xy >= 1)) return false;
    uint2 pixel = min(uint2(projected.xy * sceneSize), sceneSize - 1u);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    if (!isfinite(z) || z < 0 || z >= 1 || orm.Load(int3(pixel, 0)).a < .5) return false;
    float3 q = ReconstructPosition((pixel + .5) / sceneSize, z, invViewProjMat);
    float3 m = SSGIReceiverGeometricNormal(pixel, normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz)));
    return ProbeSurfaceCompatible(p, n, q, m, footprint);
}

uint PreviousProbe(float3 p, float3 n, float footprint)
{
    if (historyValid == 0u) return 0u;
    float4 clip = mul(float4(p, 1), prevJitteredViewProj);
    if (clip.w <= 0) return 0u;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0) || any(uv >= 1) || ndc.z < 0 || ndc.z >= 1) return 0u;
    float2 grid = uv * sceneSize / float(SSGITraceFactor(traceDownsampleFactor)) - 0.5;
    int2 base = int2(floor(grid));
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_PREVIOUS_ANCHOR];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_PREVIOUS_NORMAL];
    float best = 1e20;
    uint result = 0;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        int2 tap = base + int2(i & 1u, i >> 1u);
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        float4 q = positions.Load(int3(tap, 0));
        float4 m = normals.Load(int3(tap, 0));
        if (q.a <= 0 || m.a < 1 || !all(isfinite(q)) || !all(isfinite(m))) continue;
        float d = distance(q.xyz, p);
        float tolerance = max(footprint * 0.75, 0.001);
        // A nearly identical normal can still cross the tangent-frame pole.
        // Reject that basis discontinuity rather than mixing unrelated bins.
        if (dot(n, m.xyz) < 0.99 || dot(ProbeDirection(float2(0.5, 0), n),
            ProbeDirection(float2(0.5, 0), m.xyz)) < 0.98 || SymmetricPlaneDistance(q.xyz - p, n, m.xyz) > tolerance ||
            d > footprint * float(SSGITraceFactor(traceDownsampleFactor)) * 2.0 || d >= best) continue;
        best = d;
        result = uint(tap.y) * outputSize.x + uint(tap.x) + 1u;
    }
    return result;
}

groupshared float probeScores[64];
groupshared uint probePixels[64];

[numthreads(64, 1, 1)]
void CSMain_ProbePlace(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = group.xy;
    if (any(probe >= outputSize)) return;
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    uint2 blockMin = probe * factor;
    uint2 blockSize = min(uint2(factor, factor), sceneSize - blockMin);
    float2 center = float2(blockMin) + (float2(blockSize) - 1.0) * 0.5;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    float best = 1e30;
    uint packed = 0;
    // Exactly one primary receiver per downsample cell. Scan actual pixels so
    // a thin visible surface does not disappear just because the center is sky.
    for (uint i = lane; i < blockSize.x * blockSize.y; i += 64u)
    {
        uint2 pixel = blockMin + uint2(i % blockSize.x, i / blockSize.x);
        float z = depth.Load(int3(pixel, 0));
        if (!isfinite(z) || z < 0 || z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) continue;
        float2 delta = float2(pixel) - center;
        float score = dot(delta, delta);
        uint candidate = pixel.y * sceneSize.x + pixel.x + 1u;
        if (score < best || (score == best && candidate < packed)) { best = score; packed = candidate; }
    }
    probeScores[lane] = best;
    probePixels[lane] = packed;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 32u; step > 0u; step >>= 1u)
    {
        if (lane < step && (probeScores[lane + step] < probeScores[lane] ||
            (probeScores[lane + step] == probeScores[lane] && probePixels[lane + step] < probePixels[lane])))
        {
            probeScores[lane] = probeScores[lane + step];
            probePixels[lane] = probePixels[lane + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u)
    {
        RWTexture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION_UAV];
        RWTexture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL_UAV];
        RWTexture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER_UAV];
        RWTexture2D<float4> anchors = ResourceDescriptorHeap[SP_ANCHOR_UAV];
        uint receiver = probePixels[0];
        float4 position = 0, normal = 0, anchor = 0;
        if (receiver != 0u)
        {
            uint2 pixel = ProbePixel(receiver);
            Texture2D<float4> gbNormals = ResourceDescriptorHeap[normalTextureIdx];
            float3 n = normalize(DecodeGBufferNormal(gbNormals.Load(int3(pixel, 0)).xyz));
            n = SSGIReceiverGeometricNormal(pixel, n);
            float3 p = ReconstructPosition((pixel + 0.5) / sceneSize, depth.Load(int3(pixel, 0)), invViewProjMat);
            if (all(isfinite(n)) && all(isfinite(p)))
            {
                anchor = float4(p, 1);
                position = anchor; normal = float4(n, 0);
                float width = float(factor) * SP_JITTER_WIDTH;
                if (width > 0)
                {
                    // Total width, not radius. Quantize within the bounded region;
                    // if it contains no pixel centers, retain the stable receiver.
                    int2 lower = max(int2(blockMin), int2(ceil(center - width * .5)));
                    int2 upper = min(int2(blockMin + blockSize - 1u), int2(floor(center + width * .5)));
                    if (all(lower <= upper))
                    {
                        // Eight centered Hammersley phases. Intended checkerboard
                        // hold frames share the phase of the preceding fresh batch.
                        uint phase = (((frameIndex + ((probe.x + probe.y) & 1u)) >> 1u) + probe.x * 3u + probe.y * 5u) & 7u;
                        uint reverse = ((phase & 1u) << 2u) | (phase & 2u) | ((phase & 4u) >> 2u);
                        float2 sequence = (float2(phase, reverse) + .5) / 8.0;
                        uint2 tap = uint2(clamp(int2(floor(center + (sequence - .5) * width + .5)), lower, upper));
                        float tapDepth = depth.Load(int3(tap, 0));
                        if (isfinite(tapDepth) && tapDepth >= 0 && tapDepth < 1 && orm.Load(int3(tap, 0)).a >= .5)
                        {
                            float3 q = ReconstructPosition((tap + .5) / sceneSize, tapDepth, invViewProjMat);
                            float3 m = SSGIReceiverGeometricNormal(tap, normalize(DecodeGBufferNormal(gbNormals.Load(int3(tap, 0)).xyz)));
                            if (ProbeSurfaceCompatible(p, n, q, m, ProbeFootprint(pixel, p)))
                            {
                                position = float4(q, 1); normal = float4(m, 0);
                                receiver = tap.y * sceneSize.x + tap.x + 1u;
                            }
                        }
                    }
                }
            }
            else receiver = 0u;
        }
        positions[probe] = position;
        normals[probe] = normal;
        receivers[probe] = receiver;
        anchors[probe] = anchor;
    }
}

float4 ProbeScreenRay(uint2 receiver, float3 p, float3 n, float3 direction, out bool hit)
{
    hit = false;
    float3 viewOrigin = mul(float4(p + n * normalBias, 1), viewMat).xyz;
    float3 viewDirection = normalize(mul(float4(direction, 0), viewMat).xyz);
    SSGIScreenRayResult screen = TraceScreenRay(receiver, viewOrigin, viewDirection);
    if (screen.status == SSGI_SCREEN_HIT)
    {
        hit = true;
        Texture2D<float4> source = ResourceDescriptorHeap[sourceTextureIdx];
        float3 radiance = source.Load(int3(screen.hitPixel, 0)).rgb;
        // Emission is supplied by explicit screen-emitter sampling once.
        if (ScreenEmissiveCount() != 0u)
            radiance = max(radiance - ScreenEmissiveRadiance(screen.hitPixel), 0);
        return float4(all(isfinite(radiance)) ? clamp(radiance, 0, 60000) : 0, max(screen.distance, 0.001));
    }
    return float4(0, 0, 0, hybrid.worldMaxTraceDistance);
}

float4 ProbeWorldRay(uint2 receiver, float3 p, float3 n, float3 direction, inout float4 cachedOffset)
{
    if (SSGIWorldMode() == 0u) return float4(0, 0, 0, hybrid.worldMaxTraceDistance);
    float3 origin = p + n * normalBias;
    float limit = hybrid.worldMeshMaxTraceDistance;
    if (!SSGIApplyWorldNormalBias(receiver, origin, limit, cachedOffset)) return float4(0, 0, 0, 0.001);
    float spent = max(cachedOffset.w, 0.0);
    VXGIMeshSDFThresholdCounts counts;
    counts.nodeCount = hybrid.nodeCount; counts.assetCount = hybrid.assetCount;
    counts.instanceCount = hybrid.instanceCount; counts.bvhNodeCount = hybrid.bvhNodeCount;
    VXGIThresholdParameters parameters;
    parameters.hitThresholdCells = SSGI_WORLD_HIT_THRESHOLD_CELLS;
    parameters.stepScale = 1.0; parameters.maxSteps = 128; parameters.padding = 0;
    VXGIMeshSDFThresholdResult hit = (VXGIMeshSDFThresholdResult)0;
    if (limit > 0) hit = SSGITraceMeshNear(origin, direction, 0, limit, counts, parameters, hybridNodes, hybrid);
    if (hit.status == VXGI_THRESHOLD_HIT)
    {
        float3 radiance = 0;
        if (hit.distance + spent <= hybrid.worldMeshMaxTraceDistance)
        {
            VXGIProbeRadianceQueryResult query = VXGIProbeQueryRadiance(hit.position, direction,
                hybrid.voxelSize, hybrid.probeCount, hybridLookup, hybridRawRecords,
                probeNeighborsIndex, asuint(hybrid.reservedLighting0.x));
            if (query.hasSupport != 0u && all(isfinite(query.radiance))) radiance = clamp(query.radiance, 0, 60000);
        }
        return float4(radiance, max(hit.distance + spent, 0.001));
    }
    // A failed/unfinished query must not see through an unresolved near surface.
    if (hit.status != VXGI_THRESHOLD_MISS || global.enabled == 0u) return float4(0, 0, 0, hybrid.worldMaxTraceDistance);
    SSGIGlobalParameters farParameters = global;
    farParameters.maxTraceDistance = max(0.0, hybrid.worldMaxTraceDistance - spent);
    SSGIHybridRayResult farHit = SSGIGlobalRaySource(origin, direction, limit, hybrid,
        hybridNodes, hybridLookup, hybridRawRecords, farParameters, probeNeighborsIndex);
    float distance = farHit.distance + spent;
    if (farHit.hit != 0u && distance > hybrid.worldMeshMaxTraceDistance && distance <= hybrid.worldMaxTraceDistance)
        return float4(all(isfinite(farHit.radiance)) ? clamp(farHit.radiance, 0, 60000) : 0, max(distance, 0.001));
    return float4(0, 0, 0, hybrid.worldMaxTraceDistance);
}


groupshared uint previousProbe, heldProbe, rejectedHistory;
groupshared float previousCount, originDisplacement;
groupshared float minimumDistances[64];
groupshared float4 sharedWorldOffset;

[numthreads(64, 1, 1)]
void CSMain_ProbeTrace(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = group.xy, cell = uint2(lane & 7u, lane >> 3u);
    if (any(probe >= outputSize)) return;
    RWTexture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION_UAV];
    RWTexture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL_UAV];
    Texture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER];
    RWTexture2D<float4> output = ResourceDescriptorHeap[SP_RADIANCE_UAV];
    RWTexture2D<float4> anchors = ResourceDescriptorHeap[SP_ANCHOR_UAV];
    float4 position = positions[probe], normal = normals[probe];
    float4 anchor = anchors[probe];
    uint packed = receivers.Load(int3(probe, 0));
    if (position.a <= 0 || packed == 0u)
    { output[probe * 8u + cell] = 0; return; }
    uint2 receiver = ProbePixel(packed);
    float footprint = ProbeFootprint(receiver, position.xyz);
    Texture2D<float4> gbNormals = ResourceDescriptorHeap[normalTextureIdx];
    float3 launchNormal = normalize(DecodeGBufferNormal(gbNormals.Load(int3(receiver, 0)).xyz));
    Texture2D<float4> previous = ResourceDescriptorHeap[SP_PREVIOUS_RADIANCE];
    Texture2D<float4> oldPositions = ResourceDescriptorHeap[SP_PREVIOUS_POSITION];
    Texture2D<float4> oldNormals = ResourceDescriptorHeap[SP_PREVIOUS_NORMAL];
    Texture2D<float4> oldAnchors = ResourceDescriptorHeap[SP_PREVIOUS_ANCHOR];
    if (lane == 0u)
    {
        previousProbe = PreviousProbe(anchor.xyz, normal.xyz, footprint);
        previousCount = 0; originDisplacement = 0; rejectedHistory = 0;
        if (previousProbe != 0u)
        {
            uint2 q = uint2((previousProbe - 1u) % outputSize.x, (previousProbe - 1u) / outputSize.x);
            float4 previousPosition = oldPositions.Load(int3(q, 0));
            float4 previousNormal = oldNormals.Load(int3(q, 0));
            // Anchor stability is not permission to reuse a point now hidden
            // behind a wall. Check the actual old receiver against this GBuffer.
            if (SP_JITTER_WIDTH > 0 && !ProbeHistorySampleVisible(previousPosition.xyz, previousNormal.xyz, footprint))
                previousProbe = 0;
            else
            {
                previousCount = previousNormal.a;
                originDisplacement = distance(oldAnchors.Load(int3(q, 0)).xyz, anchor.xyz);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    float4 oldValue = 0;
    if (previousProbe != 0u)
    {
        uint2 q = uint2((previousProbe - 1u) % outputSize.x, (previousProbe - 1u) / outputSize.x);
        oldValue = previous.Load(int3(q * 8u + cell, 0));
    }
    minimumDistances[lane] = oldValue.a;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 32u; step > 0u; step >>= 1u)
    {
        if (lane < step) minimumDistances[lane] = min(minimumDistances[lane], minimumDistances[lane + step]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u)
        heldProbe = previousProbe != 0u && ((probe.x + probe.y + frameIndex) & 1u) != 0u &&
            originDisplacement <= min(footprint * float(SSGITraceFactor(traceDownsampleFactor)) * 0.5,
                minimumDistances[0] * 0.05) ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        sharedScreenEmissiveCount = LoadScreenEmissiveCount();
        sharedWorldOffset = float4(0, 0, 0, -1);
        if (heldProbe == 0u && SSGIWorldMode() != 0u)
        {
            float3 launch = position.xyz + launchNormal * normalBias;
            float limit = hybrid.worldMeshMaxTraceDistance;
            SSGIApplyWorldNormalBias(receiver, launch, limit, sharedWorldOffset);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (sharedScreenEmissiveCount > 0u && heldProbe == 0u)
    {
        ScreenEmissiveSample light = SampleScreenEmissive(probe, receiver, lane, position.xyz, launchNormal, normal.xyz);
        sharedEmissiveRadiance[lane] = light.radiance;
        sharedEmissiveCell[lane] = light.cell;
        sharedEmissiveDistance[lane] = light.distance;
        GroupMemoryBarrierWithGroupSync();
    }
    float4 value = oldValue;
    if (heldProbe == 0u)
    {
        Texture2DArray<uint2> noise = ResourceDescriptorHeap[blueNoiseTextureIdx];
        uint samples = SP_RAYS / 64u;
        float3 radiance;
        float hitDistance;
        uint screenReuse = SP_RAYS / SP_SCREEN_RAYS;
        bool screenHit = false;
        float4 screenValue = 0;
        radiance = 0;
        hitDistance = hybrid.worldMaxTraceDistance;
        float4 cachedOffset = sharedWorldOffset;
        [loop] for (uint sample = 0; sample < samples; ++sample)
        {
            float2 xi = (float2(cell) + LoadSTBN2D(noise, probe, frameIndex,
                SSGIRaySampleIndex(16u, lane * samples + sample))) / 8.0;
            float3 direction = ProbeDirection(xi, normal.xyz);
            // Reuse the representative screen observation in this direction cell.
            // On a miss, each world sample still traces its own direction.
            if (sample % screenReuse == 0u)
                screenValue = ProbeScreenRay(receiver, position.xyz, launchNormal, direction, screenHit);
            float4 ray;
            if (screenHit) ray = screenValue;
            else ray = ProbeWorldRay(receiver, position.xyz, launchNormal, direction, cachedOffset);
            radiance += ray.rgb;
            hitDistance = min(hitDistance, ray.a);
        }
        if (sharedScreenEmissiveCount > 0u)
        {
            [loop] for (uint i = 0; i < SP_EMISSIVE_SAMPLES; ++i)
                if (sharedEmissiveCell[i] == lane)
                {
                    radiance += sharedEmissiveRadiance[i] * float(samples);
                    hitDistance = min(hitDistance, sharedEmissiveDistance[i]);
                }
        }
        value = float4(radiance / float(samples), hitDistance);
        bool reuse = previousProbe != 0u && all(isfinite(oldValue)) && oldValue.a > 0 &&
            originDisplacement <= min(oldValue.a, hitDistance) * 0.1;
        if (reuse)
        {
            value.rgb = lerp(oldValue.rgb, value.rgb, rcp(min(previousCount + 1.0, float(SP_HISTORY_LIMIT))));
        }
        else InterlockedOr(rejectedHistory, 1u);
    }
    output[probe * 8u + cell] = value;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        if (heldProbe != 0u)
        {
            uint2 q = uint2((previousProbe - 1u) % outputSize.x, (previousProbe - 1u) / outputSize.x);
            normals[probe] = oldNormals.Load(int3(q, 0));
            positions[probe] = float4(oldPositions.Load(int3(q, 0)).xyz, 1);
            anchors[probe] = oldAnchors.Load(int3(q, 0));
        }
        else
        {
            normals[probe] = float4(normal.xyz,
                rejectedHistory != 0u ? 1.0 : min(previousCount + 1.0, float(SP_HISTORY_LIMIT)));
            positions[probe] = float4(position.xyz, 2);
        }
    }

}

[numthreads(64, 1, 1)]
void CSMain_ProbeFilter(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = group.xy, cell = uint2(lane & 7u, lane >> 3u);
    if (any(probe >= outputSize)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    Texture2D<float4> input = ResourceDescriptorHeap[SP_RADIANCE_SRV];
    Texture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER];
    RWTexture2D<float4> output = ResourceDescriptorHeap[SP_FILTERED_UAV];
    float4 p = positions.Load(int3(probe, 0));
    if (p.a <= 0) { output[probe * 8u + cell] = 0; return; }
    float3 n = normals.Load(int3(probe, 0)).xyz;
    float3 direction = ProbeDirection((float2(cell) + 0.5) / 8.0, n);
    float4 center = input.Load(int3(probe * 8u + cell, 0));
    float tolerance = max(ProbeFootprint(ProbePixel(receivers.Load(int3(probe, 0))), p.xyz) *
        float(SSGITraceFactor(traceDownsampleFactor)) * 0.5, 0.001);
    float3 sum = center.rgb * 36.0;
    float weightSum = 36.0;
    [loop] for (int y = -2; y <= 2; ++y)
    [loop] for (int x = -2; x <= 2; ++x)
    {
        if (x == 0 && y == 0) continue;
        int2 tap = int2(probe) + int2(x, y) * 2;
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        float4 q = positions.Load(int3(tap, 0));
        if (q.a <= 0) continue;
        float3 m = normals.Load(int3(tap, 0)).xyz;
        float agreement = dot(n, m);
        float planeError = SymmetricPlaneDistance(q.xyz - p.xyz, n, m) / tolerance;
        if (agreement < 0.95 || planeError >= 3) continue;
        float3 neighborDirection = ProbeDirection((float2(cell) + 0.5) / 8.0, m);
        if (dot(direction, neighborDirection) < 0.985) continue;
        float4 value = input.Load(int3(uint2(tap) * 8u + cell, 0));
        // Hit distance controls parallax and prevents far neighbors from
        // washing over a close occluder/contact shadow in the same plane.
        float3 toHit = q.xyz + neighborDirection * max(min(center.a, value.a), 0.001) - p.xyz;
        float angle = acos(clamp(dot(direction, normalize(toHit)), -1, 1));
        float kernel = (x == 0 ? 6.0 : (abs(x) == 1 ? 4.0 : 1.0)) *
            (y == 0 ? 6.0 : (abs(y) == 1 ? 4.0 : 1.0));
        float w = kernel *
            pow(saturate(agreement), 32) * exp2(-4 * planeError * planeError) *
            pow(saturate(1.0 - angle / 0.2), 2);
        sum += value.rgb * w;
        weightSum += w;
    }
    output[probe * 8u + cell] = float4(sum / weightSum, center.a);
}

// Orthonormal real SH, projected in world space after directional filtering.
void ProbeSH(float3 w, out float basis[9])
{
    basis[0] = 0.2820947918;
    basis[1] = 0.4886025119 * w.y;
    basis[2] = 0.4886025119 * w.z;
    basis[3] = 0.4886025119 * w.x;
    basis[4] = 1.0925484306 * w.x * w.y;
    basis[5] = 1.0925484306 * w.y * w.z;
    basis[6] = 0.3153915653 * (3.0 * w.z * w.z - 1.0);
    basis[7] = 1.0925484306 * w.x * w.z;
    basis[8] = 0.5462742153 * (w.x * w.x - w.y * w.y);
}

#define SP_SH_UAV levels[4].w
#define SP_SH_SRV levels[5].y
groupshared float3 irradianceCoefficients[9][64];

[numthreads(64, 1, 1)]
void CSMain_ProbeIntegrate(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = group.xy, cell = uint2(lane & 7u, lane >> 3u);
    if (any(probe >= outputSize)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    Texture2D<float4> directions = ResourceDescriptorHeap[SP_FILTERED_SRV];
    RWTexture2D<float4> sh = ResourceDescriptorHeap[SP_SH_UAV];
    float4 p = positions.Load(int3(probe, 0));
    float3 n = normals.Load(int3(probe, 0)).xyz;
    float3 radiance = p.a > 0 ? directions.Load(int3(probe * 8u + cell, 0)).rgb : 0;
    float3 direction = p.a > 0 ? ProbeDirection((float2(cell) + 0.5) / 8.0, n) : float3(0, 0, 1);
    float basis[9];
    ProbeSH(direction, basis);
    [unroll] for (uint c = 0; c < 9u; ++c)
    {
        float convolution = c == 0u ? 1.0 : (c < 4u ? 2.0 / 3.0 : 0.25);
        irradianceCoefficients[c][lane] = radiance * basis[c] * (6.28318530718 / 64.0) * convolution;
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 32u; step > 0u; step >>= 1u)
    {
        if (lane < step)
            [unroll] for (uint c = 0; c < 9u; ++c)
                irradianceCoefficients[c][lane] += irradianceCoefficients[c][lane + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u)
        [unroll] for (uint c = 0; c < 9u; ++c)
            sh[probe * 3u + uint2(c % 3u, c / 3u)] = float4(irradianceCoefficients[c][0], 0);
}

bool GatherProbeSH(int2 tap, float3 p, float3 geometric, float footprint, float basis[9],
    out float3 lighting, out float geometryWeight, out bool fresh)
{
    lighting = 0; geometryWeight = 0; fresh = false;
    if (any(tap < 0) || any(tap >= int2(outputSize))) return false;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    Texture2D<float4> sh = ResourceDescriptorHeap[SP_SH_SRV];
    float4 q = positions.Load(int3(tap, 0));
    if (q.a <= 0) return false;
    float3 n = normalize(normals.Load(int3(tap, 0)).xyz);
    float agreement = saturate(dot(geometric, n));
    if (agreement < 0.8) return false;
    float3 delta = q.xyz - p;
    float curvature = 0.75 * length(delta) * sqrt(saturate(1 - agreement * agreement));
    float error = SymmetricPlaneDistance(delta, geometric, n) / (max(footprint * 0.75, 0.001) + curvature);
    if (error >= 3) return false;
    [unroll] for (uint c = 0; c < 9u; ++c)
        lighting += sh.Load(int3(uint2(tap) * 3u + uint2(c % 3u, c / 3u), 0)).rgb * basis[c];
    lighting = max(lighting, 0); // Truncated irradiance SH can ring below zero.
    geometryWeight = pow(agreement, 32) * exp2(-4 * error * error);
    fresh = q.a > 1.5;
    return true;
}

[numthreads(8, 8, 1)]
void CSMain_ProbeResolve(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (any(pixel >= sceneSize)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    RWTexture2D<float4> reprojected = ResourceDescriptorHeap[reprojectedGITextureIdx];
    RWTexture2D<float4> reprojectedFast = ResourceDescriptorHeap[reprojectedFastTextureIdx];
    output[pixel] = 0; reprojected[pixel] = 0; reprojectedFast[pixel] = 0;
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    if (!isfinite(z) || z < 0 || z >= 1 || orm.Load(int3(pixel, 0)).a < 0.5) return;
    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invViewProjMat);
    float3 shading = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float3 geometric = SSGIReceiverGeometricNormal(pixel, shading);
    if (dot(geometric, shading) < 0.5) geometric = shading;
    float footprint = ProbeFootprint(pixel, p);
    float basis[9];
    ProbeSH(shading, basis); // Each full-resolution pixel supplies its own normal.
    float2 grid = (pixel + 0.5) / float(SSGITraceFactor(traceDownsampleFactor)) - 0.5;
    int2 base = int2(floor(grid));
    float2 fraction = frac(grid);
    float3 sum = 0;
    float weights = 0;
    bool anyFresh = false;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        int2 offset = int2(i & 1u, i >> 1u);
        float3 lighting; float geometryWeight; bool fresh;
        if (!GatherProbeSH(base + offset, p, geometric, footprint, basis, lighting, geometryWeight, fresh)) continue;
        float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
        float w = bilinear.x * bilinear.y * geometryWeight;
        sum += lighting * w; weights += w; anyFresh = anyFresh || (fresh && w > 0);
    }
    if (weights <= 1e-5)
    {
        // Explicit edge repair: search nearby compatible surface probes, rather
        // than accepting an unrelated wall or inventing a black observation.
        int2 nearest = int2(floor(grid + 0.5));
        [loop] for (int y = -1; y <= 1; ++y)
        [loop] for (int x = -1; x <= 1; ++x)
        {
            int2 tap = nearest + int2(x, y);
            float3 lighting; float geometryWeight; bool fresh;
            if (!GatherProbeSH(tap, p, geometric, footprint, basis, lighting, geometryWeight, fresh)) continue;
            float2 delta = float2(tap) - grid;
            float w = exp2(-dot(delta, delta)) * geometryWeight;
            sum += lighting * w; weights += w; anyFresh = anyFresh || (fresh && w > 0);
        }
    }
    float4 history = 0, fast = 0;
    if (historyValid != 0u)
        SSGIReprojectHistory(pixel, sceneSize, sceneSize, 1u, z, shading,
            invViewProjMat, prevJitteredViewProj, prevJitteredInvViewProj,
            previousGITextureIdx, previousDepthTextureIdx, previousNormalTextureIdx,
            previousFastTextureIdx, history, fast);
    reprojected[pixel] = history;
    reprojectedFast[pixel] = fast;
    float marker = weights <= 1e-5 ? -2.0 : (anyFresh || history.a <= 0 ? 1.0 : -1.0);
    output[pixel] = float4(weights > 1e-5 ? sum / weights : 0, marker);
}
