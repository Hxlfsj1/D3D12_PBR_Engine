// Reuse the production screen queries and bindless textures. The legacy
// entry points are dead-stripped when compiling one of the probe entry points.
#define SSGI_PASS_TRACE 1
#include "Shaders_For_SSGI.hlsl"

// Screen-probe fields use explicit names in the shared screen-trace constants.
#define SP_POSITION probePositionIndex
#define SP_NORMAL probeNormalIndex
#define SP_RECEIVER probeReceiverIndex
#define SP_PREVIOUS_POSITION previousProbePositionIndex
#define SP_PREVIOUS_NORMAL previousProbeNormalIndex
#define SP_PREVIOUS_RADIANCE previousProbeRadianceIndex
#define SP_RADIANCE_UAV probeRadianceUavIndex
#define SP_RADIANCE_SRV probeRadianceIndex
#define SP_FILTERED_UAV probeFilteredUavIndex
#define SP_FILTERED_SRV probeFilteredIndex
#define SP_HISTORY_LIMIT probeHistoryLimit
#define SP_SCREEN_RAYS probeScreenRays
#define SP_POSITION_UAV probePositionUavIndex
#define SP_NORMAL_UAV probeNormalUavIndex
#define SP_RECEIVER_UAV probeReceiverUavIndex
#define SP_JITTER_WIDTH saturate(probeJitterWidth)
#define SP_ANCHOR_UAV probeAnchorUavIndex
#define SP_PREVIOUS_ANCHOR previousProbeAnchorIndex



#include "ScreenProbeWorldEmissive.hlsli"
#include "ScreenProbeScreenEmissive.hlsli"
bool ProbeSamplesEmission(){return SP_EMISSIVE_SAMPLES>0 && (worldLumen!=0?worldEmitterCount:ScreenEmissiveCount())>0;}

ScreenProbeQuery ProbeQuery()
{
    ScreenProbeQuery q;
    q.sceneSize = sceneSize; q.uniformSize = outputSize; q.atlasSize = probeAtlasSize;
    q.factor = SSGITraceFactor(traceDownsampleFactor);
    q.positionIndex = SP_POSITION; q.normalIndex = SP_NORMAL; q.receiverIndex = SP_RECEIVER;
    q.shIndex = probeSHIndex; q.capacity = adaptiveCapacity;
    q.countIndex = adaptiveCountIndex; q.headerIndex = adaptiveHeaderIndex; q.indicesIndex = adaptiveIndicesIndex;
    return q;
}

uint ProbeScheduledCount() { return outputSize.x * outputSize.y + SPAdaptiveCount(ProbeQuery()); }

uint2 ProbeDispatchCoord(uint3 group)
{
    return SPAtlasCoord(probeDispatchBase + group.y * probeAtlasSize.x + group.x, probeAtlasSize);
}

bool ProbeIsScheduled(uint2 probe)
{
    return all(probe < probeAtlasSize) && probe.y * probeAtlasSize.x + probe.x < ProbeScheduledCount();
}

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

void ConsiderPreviousProbe(uint id, float3 p, float3 n, float footprint, uint2 receiver,
    bool adaptive, inout float best, inout uint result)
{
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_PREVIOUS_ANCHOR];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_PREVIOUS_NORMAL];
    uint2 coord = SPAtlasCoord(id, probeAtlasSize);
    float4 q = positions.Load(int3(coord, 0)), m = normals.Load(int3(coord, 0));
    if (q.a <= 0 || m.a < 1 || !all(isfinite(q)) || !all(isfinite(m))) return;
    float d = distance(q.xyz, p);
    if (!ProbeSurfaceCompatible(p, n, q.xyz, m.xyz, footprint)) return;
    if (adaptive)
    {
        // A grazing surface spans much more world distance than a same-depth
        // pixel footprint. Retain a visible old anchor in this actual screen
        // tile instead of moving it to each frame's random candidate.
        Texture2D<float4> oldPositions = ResourceDescriptorHeap[SP_PREVIOUS_POSITION];
        float3 old = oldPositions.Load(int3(coord, 0)).xyz;
        float3 viewPosition = mul(float4(old, 1), viewMat).xyz;
        if (viewPosition.z <= 0 || !all(isfinite(viewPosition))) return;
        float3 projected = Project(viewPosition);
        if (!all(isfinite(projected)) || any(projected.xy < 0) || any(projected.xy >= 1)) return;
        uint2 oldPixel = min(uint2(projected.xy * sceneSize), sceneSize - 1u);
        uint factor = SSGITraceFactor(traceDownsampleFactor);
        if (any(oldPixel / factor != receiver / factor) ||
            !ProbeSurfaceCompatible(p, n, old, m.xyz, footprint) ||
            !ProbeHistorySampleVisible(old, m.xyz, footprint)) return;
        d = distance(float2(oldPixel), float2(receiver));
    }
    else if (d > footprint * float(SSGITraceFactor(traceDownsampleFactor)) * 2) return;
    if (d >= best) return;
    best = d; result = id + 1u;
}

uint PreviousProbe(float3 p, float3 n, float footprint, uint2 receiver, bool adaptive)
{
    if (historyValid == 0u) return 0u;
    float4 clip = mul(float4(p, 1), prevJitteredViewProj);
    if (clip.w <= 0) return 0u;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0) || any(uv >= 1) || ndc.z < 0 || ndc.z >= 1) return 0u;
    float2 grid = uv * sceneSize / float(SSGITraceFactor(traceDownsampleFactor)) - 0.5;
    int2 base = int2(floor(grid));
    ScreenProbeQuery previous = ProbeQuery();
    previous.countIndex = previousAdaptiveCountIndex;
    previous.headerIndex = previousAdaptiveHeaderIndex;
    previous.indicesIndex = previousAdaptiveIndicesIndex;
    float best = 1e20;
    uint result = 0;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        int2 tap = base + int2(i & 1u, i >> 1u);
        if (any(tap < 0) || any(tap >= int2(outputSize))) continue;
        ConsiderPreviousProbe(uint(tap.y) * outputSize.x + uint(tap.x), p, n, footprint, receiver, adaptive, best, result);
        uint count = SPTileCount(previous, tap);
        [loop] for (uint slot = 0u; slot < count; ++slot)
        {
            uint id = SPTileProbe(previous, tap, slot);
            if (id != SP_INVALID_PROBE) ConsiderPreviousProbe(id, p, n, footprint, receiver, adaptive, best, result);
        }
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

float4 ProbeSourceRay(uint2 receiver, float3 p, float3 n, float3 direction)
{
    return SSGITraceCached(receiver,p+n*normalBias,direction,ProbeSamplesEmission());
}




groupshared uint previousProbe, heldProbe, rejectedHistory;
groupshared float previousCount, originDisplacement;
groupshared float minimumDistances[64];

[numthreads(64, 1, 1)]
void CSMain_ProbePrepareHistory(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = ProbeDispatchCoord(group), cell = uint2(lane & 7u, lane >> 3u);
    if (!ProbeIsScheduled(probe)) return;
    RWTexture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION_UAV];
    RWTexture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL_UAV];
    RWTexture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER_UAV];
    RWTexture2D<uint4> links = ResourceDescriptorHeap[probeHistoryLinkUavIndex];
    RWTexture2D<uint2> seeds = ResourceDescriptorHeap[probeSeedUavIndex];
    RWTexture2D<float4> anchors = ResourceDescriptorHeap[SP_ANCHOR_UAV];
    float4 position = positions[probe], normal = normals[probe];
    float4 anchor = anchors[probe];
    uint packed = receivers[probe];
    if (position.a <= 0 || packed == 0u)
    { if (lane == 0u) { links[probe] = 0; seeds[probe] = 0; } return; }
    uint2 receiver = ProbePixel(packed);
    float footprint = ProbeFootprint(receiver, position.xyz);
    bool adaptive = probe.y * probeAtlasSize.x + probe.x >= outputSize.x * outputSize.y;
    Texture2D<float4> previous = ResourceDescriptorHeap[SP_PREVIOUS_RADIANCE];
    Texture2D<float4> oldPositions = ResourceDescriptorHeap[SP_PREVIOUS_POSITION];
    Texture2D<float4> oldNormals = ResourceDescriptorHeap[SP_PREVIOUS_NORMAL];
    Texture2D<float4> oldAnchors = ResourceDescriptorHeap[SP_PREVIOUS_ANCHOR];
    if (lane == 0u)
    {
        previousProbe = PreviousProbe(anchor.xyz, normal.xyz, footprint, receiver, adaptive);
        previousCount = 0; originDisplacement = 0; rejectedHistory = 0;
        if (previousProbe != 0u)
        {
            uint2 q = SPAtlasCoord(previousProbe - 1u, probeAtlasSize);
            float4 previousPosition = oldPositions.Load(int3(q, 0));
            float4 previousNormal = oldNormals.Load(int3(q, 0));
            // Anchor stability is not permission to reuse a point now hidden
            // behind a wall. Check the actual old receiver against this GBuffer.
            if (((SP_JITTER_WIDTH > 0 || adaptive) &&
                 !ProbeHistorySampleVisible(previousPosition.xyz, previousNormal.xyz, footprint)) ||
                (adaptive && !ProbeSurfaceCompatible(anchor.xyz, normal.xyz,
                    previousPosition.xyz, previousNormal.xyz, footprint)))
                previousProbe = 0;
            else
            {
                previousCount = previousNormal.a;
                originDisplacement = distance(oldAnchors.Load(int3(q, 0)).xyz, anchor.xyz);
                if (adaptive)
                {
                    position.xyz = previousPosition.xyz;
                    normal = previousNormal;
                    anchor = oldAnchors.Load(int3(q, 0));
                    float3 projected = Project(mul(float4(position.xyz, 1), viewMat).xyz);
                    uint2 oldPixel = min(uint2(projected.xy * sceneSize), sceneSize - 1u);
                    packed = oldPixel.y * sceneSize.x + oldPixel.x + 1u;
                    originDisplacement = 0; // The ray origin itself was retained, not translated.
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    float4 oldValue = 0;
    if (previousProbe != 0u)
    {
        uint2 q = SPAtlasCoord(previousProbe - 1u, probeAtlasSize);
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
    {
        uint2 seed = probe;
        if (adaptive)
        {
            seed = uint2(ProbeRandomHash(packed), ProbeRandomHash(packed ^ 0x68bc21ebu));
            if (previousProbe != 0u)
            {
                Texture2D<uint2> oldSeeds = ResourceDescriptorHeap[previousProbeSeedIndex];
                seed = oldSeeds.Load(int3(SPAtlasCoord(previousProbe - 1u, probeAtlasSize), 0));
            }
        }
        seeds[probe] = seed;
        heldProbe = previousProbe != 0u && ((seed.x + seed.y + frameIndex) & 1u) != 0u &&
            originDisplacement <= min(footprint * float(SSGITraceFactor(traceDownsampleFactor)) * 0.5,
                minimumDistances[0] * 0.05) ? 1u : 0u;
        links[probe] = uint4(previousProbe, asuint(previousCount), asuint(originDisplacement), heldProbe);
        SPCounter(probeDiagnosticsIndexPlusOne, 8u, previousProbe != 0u ? 1u : 0u);
        SPCounter(probeDiagnosticsIndexPlusOne, 9u, heldProbe);
        if (adaptive)
        {
            SPCounter(probeDiagnosticsIndexPlusOne, 6u, previousProbe != 0u ? 1u : 0u);
            SPCounter(probeDiagnosticsIndexPlusOne, 7u, heldProbe);
        }
        // Publish UAV changes after every lane has finished reading the original
        // candidate. A different wave must not observe half of an adopted anchor.
        if (adaptive && previousProbe != 0u) receivers[probe] = packed;
        if (heldProbe != 0u)
        {
            uint2 q = SPAtlasCoord(previousProbe - 1u, probeAtlasSize);
            normals[probe] = oldNormals.Load(int3(q, 0));
            positions[probe] = float4(oldPositions.Load(int3(q, 0)).xyz, 1);
            anchors[probe] = oldAnchors.Load(int3(q, 0));
        }
        else
        {
            positions[probe] = float4(position.xyz, 2);
            if (adaptive && previousProbe != 0u) { normals[probe] = normal; anchors[probe] = anchor; }
        }
    }
}

[numthreads(64, 1, 1)]
void CSMain_ProbeTrace(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = ProbeDispatchCoord(group), cell = uint2(lane & 7u, lane >> 3u);
    if (!ProbeIsScheduled(probe)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    RWTexture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL_UAV];
    Texture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER];
    Texture2D<uint4> links = ResourceDescriptorHeap[probeHistoryLinkIndex];
    Texture2D<uint2> seeds = ResourceDescriptorHeap[probeSeedIndex];
    Texture2D<float4> previous = ResourceDescriptorHeap[SP_PREVIOUS_RADIANCE];
    RWTexture2D<float4> output = ResourceDescriptorHeap[SP_RADIANCE_UAV];
    float4 position = positions.Load(int3(probe, 0)), normal = normals[probe];
    uint packed = receivers.Load(int3(probe, 0));
    if (position.a <= 0 || packed == 0u) { output[probe * 8u + cell] = 0; return; }
    uint2 receiver = ProbePixel(packed), seed = seeds.Load(int3(probe, 0));
    Texture2D<float4> gbNormals = ResourceDescriptorHeap[normalTextureIdx];
    float3 launchNormal = normalize(DecodeGBufferNormal(gbNormals.Load(int3(receiver, 0)).xyz));
    if (lane == 0u)
    {
        uint4 link = links.Load(int3(probe, 0));
        sharedScreenEmissiveCount=0u;
        [branch] if(worldLumen==0)sharedScreenEmissiveCount=LoadScreenEmissiveCount();
        previousProbe = link.x; previousCount = asfloat(link.y);
        originDisplacement = asfloat(link.z); heldProbe = link.w; rejectedHistory = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    float4 oldValue = 0;
    if (previousProbe != 0u)
        oldValue = previous.Load(int3(SPAtlasCoord(previousProbe - 1u, probeAtlasSize) * 8u + cell, 0));
    if (ProbeSamplesEmission() && heldProbe == 0u)
    {
        ProbeEmissiveSample light;
        if(worldLumen!=0)light=SampleWorldEmissive(seed,lane,position.xyz,launchNormal,normal.xyz);
        else light=SampleScreenEmissive(seed,receiver,lane,position.xyz,launchNormal,normal.xyz);
        sharedEmissiveRadiance[lane] = light.radiance;
        sharedEmissiveCell[lane] = light.cell;
        sharedEmissiveDistance[lane] = light.distance;
        GroupMemoryBarrierWithGroupSync();
    }
    float4 value = oldValue;
    if (heldProbe == 0u)
    {
        Texture2DArray<uint2> noise = ResourceDescriptorHeap[blueNoiseTextureIdx];
        uint samplesPerDirection = SP_SCREEN_RAYS / 64u;
        float3 radiance = 0;
        float hitDistance = worldTraceDistance;
        [loop] for (uint sample = 0; sample < samplesPerDirection; ++sample)
        {
            float2 xi = (float2(cell) + LoadSTBN2D(noise, seed, frameIndex,
                SSGIRaySampleIndex(16u, lane * samplesPerDirection + sample))) / 8.0;
            float3 direction = ProbeDirection(xi, normal.xyz);
            float4 ray=ProbeSourceRay(receiver,position.xyz,launchNormal,direction);
            radiance+=ray.rgb;
            hitDistance = min(hitDistance, ray.a);
        }
        radiance /= float(samplesPerDirection);
        if (ProbeSamplesEmission())
        {
            [loop] for (uint i = 0; i < SP_EMISSIVE_SAMPLES; ++i)
                if (sharedEmissiveCell[i] == lane)
                {
                    // Already normalized by the independent emitter sample count.
                    radiance += sharedEmissiveRadiance[i];
                    hitDistance = min(hitDistance, sharedEmissiveDistance[i]);
                }
        }
        value = float4(radiance, hitDistance);
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
        if (heldProbe == 0u)
        {
            normals[probe] = float4(normal.xyz,
                rejectedHistory != 0u ? 1.0 : min(previousCount + 1.0, float(SP_HISTORY_LIMIT)));
        }
    }

}

// Geometry is shared by all 64 directions. Zero weight marks rejected neighbors;
// their frames are never read. The center frame occupies slot 12.
groupshared float4 probeFilterPositionWeight[25];
groupshared float3 probeFilterNormal[25];
groupshared float3 probeFilterTangent[25];
groupshared float3 probeFilterBitangent[25];
groupshared uint2 probeFilterCoord[25];

void StoreProbeFilterFrame(uint slot, float3 n)
{
    // Keep the construction identical to ProbeDirection; only this pass caches it.
    n = normalize(n);
    float3 tangent = normalize(cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    probeFilterNormal[slot] = n;
    probeFilterTangent[slot] = tangent;
    probeFilterBitangent[slot] = cross(n, tangent);
}

void PrepareProbeFilterNeighbor(uint slot, uint2 probe, float3 p, float4 normalTolerance)
{
    probeFilterPositionWeight[slot] = 0;
    float3 n = normalTolerance.xyz;
    if (slot == 12u) { StoreProbeFilterFrame(slot, n); return; }
    int2 offset = int2(int(slot % 5u) - 2, int(slot / 5u) - 2);
    Texture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER];
    uint2 tile = ProbePixel(receivers.Load(int3(probe, 0))) / SSGITraceFactor(traceDownsampleFactor);
    int2 tap = int2(tile) + offset * 2;
    if (any(tap < 0) || any(tap >= int2(outputSize))) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    float kernel = (offset.x == 0 ? 6.0 : (abs(offset.x) == 1 ? 4.0 : 1.0)) *
        (offset.y == 0 ? 6.0 : (abs(offset.y) == 1 ? 4.0 : 1.0));
    ScreenProbeQuery query = ProbeQuery();
    uint count = SPTileCount(query, tap);
    float best = 0;
    [loop] for (uint candidate = 0u; candidate <= count; ++candidate)
    {
        uint id = candidate == 0u ? uint(tap.y) * outputSize.x + uint(tap.x) : SPTileProbe(query, tap, candidate - 1u);
        if (id == SP_INVALID_PROBE) continue;
        uint2 coord = SPAtlasCoord(id, probeAtlasSize);
        float4 q = positions.Load(int3(coord, 0));
        if (q.a <= 0) continue;
        float3 m = normals.Load(int3(coord, 0)).xyz;
        float agreement = dot(n, m);
        float planeError = SymmetricPlaneDistance(q.xyz - p, n, m) / normalTolerance.w;
        if (agreement < .95 || planeError >= 3) continue;
        float weight = kernel * pow(saturate(agreement), 32) * exp2(-4 * planeError * planeError);
        if (weight > best)
        {
            best = weight; probeFilterCoord[slot] = coord;
            probeFilterPositionWeight[slot] = float4(q.xyz, weight);
            StoreProbeFilterFrame(slot, m);
        }
    }
}

float3 CachedProbeFilterDirection(uint slot, float mu, float radial, float cosine, float sine)
{
    return radial * (cosine * probeFilterTangent[slot] + sine * probeFilterBitangent[slot]) +
        mu * probeFilterNormal[slot];
}

[numthreads(64, 1, 1)]
void CSMain_ProbeFilter(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = ProbeDispatchCoord(group), cell = uint2(lane & 7u, lane >> 3u);
    if (!ProbeIsScheduled(probe)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    Texture2D<float4> input = ResourceDescriptorHeap[SP_RADIANCE_SRV];
    Texture2D<uint> receivers = ResourceDescriptorHeap[SP_RECEIVER];
    RWTexture2D<float4> output = ResourceDescriptorHeap[SP_FILTERED_UAV];
    float4 p = positions.Load(int3(probe, 0));
    if (p.a <= 0) { output[probe * 8u + cell] = 0; return; }
    // Extent and validity checks are group-uniform. Only neighbor preparation
    // is masked; all valid lanes reach the publication barrier below.
    if (lane < 25u)
    {
        float4 normalTolerance = 0;
        if (WaveIsFirstLane())
        {
            float3 n = normals.Load(int3(probe, 0)).xyz;
            float tolerance = max(ProbeFootprint(ProbePixel(receivers.Load(int3(probe, 0))), p.xyz) *
                float(SSGITraceFactor(traceDownsampleFactor)) * 0.5, 0.001);
            normalTolerance = float4(n, tolerance);
        }
        // Each active wave supplies its own center data. No wave-width or
        // SV_GroupIndex-to-wave mapping assumption is required.
        normalTolerance = WaveReadLaneFirst(normalTolerance);
        PrepareProbeFilterNeighbor(lane, probe, p.xyz, normalTolerance);
    }
    GroupMemoryBarrierWithGroupSync();
    float2 xi = (float2(cell) + 0.5) / 8.0;
    float mu = xi.x, phi = 6.28318530718 * xi.y;
    float radial = sqrt(max(0, 1 - mu * mu)), cosine = cos(phi), sine = sin(phi);
    float3 direction = CachedProbeFilterDirection(12u, mu, radial, cosine, sine);
    float4 center = input.Load(int3(probe * 8u + cell, 0));
    float3 sum = center.rgb * 36.0;
    float weightSum = 36.0;
    [loop] for (int y = -2; y <= 2; ++y)
    [loop] for (int x = -2; x <= 2; ++x)
    {
        if (x == 0 && y == 0) continue;
        uint slot = uint((y + 2) * 5 + x + 2);
        float4 q = probeFilterPositionWeight[slot];
        if (q.w <= 0) continue;
        uint2 tap = probeFilterCoord[slot];
        float3 neighborDirection = CachedProbeFilterDirection(slot, mu, radial, cosine, sine);
        if (dot(direction, neighborDirection) < 0.985) continue;
        float4 value = input.Load(int3(uint2(tap) * 8u + cell, 0));
        // Hit distance controls parallax and prevents far neighbors from
        // washing over a close occluder/contact shadow in the same plane.
        float3 toHit = q.xyz + neighborDirection * max(min(center.a, value.a), 0.001) - p.xyz;
        float angle = acos(clamp(dot(direction, normalize(toHit)), -1, 1));
        float w = q.w * pow(saturate(1.0 - angle / 0.2), 2);
        sum += value.rgb * w;
        weightSum += w;
    }
    output[probe * 8u + cell] = float4(sum / weightSum, center.a);
}

#define SP_SH_UAV probeSHUavIndex
#define SP_SH_SRV probeSHIndex
// A 64-thread group contains at most 16 waves (HLSL minimum wave width is 4).
// Each wave leader claims a slot; do not assume SV_GroupIndex / wave width
// identifies the wave. Only wave sums cross group-shared memory.
// Wave-major layout keeps the nine coefficient readers contiguous at merge time.
groupshared float3 irradianceWaveCoefficients[16][9];
groupshared uint irradianceWaveCount;

[numthreads(64, 1, 1)]
void CSMain_ProbeIntegrate(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 probe = ProbeDispatchCoord(group), cell = uint2(lane & 7u, lane >> 3u);
    if (!ProbeIsScheduled(probe)) return;
    Texture2D<float4> positions = ResourceDescriptorHeap[SP_POSITION];
    Texture2D<float4> normals = ResourceDescriptorHeap[SP_NORMAL];
    Texture2D<float4> directions = ResourceDescriptorHeap[SP_FILTERED_SRV];
    RWTexture2D<float4> sh = ResourceDescriptorHeap[SP_SH_UAV];
    float4 p = positions.Load(int3(probe, 0));
    // Every lane reads the same probe validity: the exit is group-uniform and
    // precedes all synchronization. Clear all coefficients for deterministic output.
    if (!(p.a > 0))
    {
        if (lane < 9u)
            sh[probe * 3u + uint2(lane % 3u, lane / 3u)] = 0;
        return;
    }
    if (lane == 0u) irradianceWaveCount = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint waveSlot = 0u;
    if (WaveIsFirstLane()) InterlockedAdd(irradianceWaveCount, 1u, waveSlot);
    float3 n = normals.Load(int3(probe, 0)).xyz;
    float3 radiance = directions.Load(int3(probe * 8u + cell, 0)).rgb;
    float3 direction = ProbeDirection((float2(cell) + 0.5) / 8.0, n);
    float basis[9];
    SPBasis(direction, basis);
    [unroll] for (uint c = 0; c < 9u; ++c)
    {
        float convolution = c == 0u ? 1.0 : (c < 4u ? 2.0 / 3.0 : 0.25);
        float3 waveSum = WaveActiveSum(radiance * basis[c] * (6.28318530718 / 64.0) * convolution);
        if (WaveIsFirstLane()) irradianceWaveCoefficients[waveSlot][c] = waveSum;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 9u)
    {
        float3 sum = 0;
        [loop] for (uint wave = 0; wave < irradianceWaveCount; ++wave)
            sum += irradianceWaveCoefficients[wave][lane];
        sh[probe * 3u + uint2(lane % 3u, lane / 3u)] = float4(sum, 0);
    }
}

[numthreads(8, 8, 1)]
void CSMain_ProbeResolve(uint3 id : SV_DispatchThreadID)
{
    uint factor = max(denoiseDownsampleFactor, 1u);
    uint2 signalSize = (sceneSize + factor - 1u) / factor;
    if (any(id.xy >= signalSize)) return;
    // Fixed receiver convention matches temporal reprojection, geometry guides
    // and the final bilateral upsample, including odd scene dimensions.
    uint2 pixel = id.xy * factor;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    RWTexture2D<float4> reprojected = ResourceDescriptorHeap[reprojectedGITextureIdx];
    RWTexture2D<float4> reprojectedFast = ResourceDescriptorHeap[reprojectedFastTextureIdx];
    output[id.xy] = 0; reprojected[id.xy] = 0; reprojectedFast[id.xy] = 0;
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
    float3 lighting; bool anyFresh;
    bool supported = SPResolveSH(ProbeQuery(), pixel, p, geometric, shading, footprint, lighting, anyFresh);
    if (!supported) SPCounter(probeDiagnosticsIndexPlusOne, 10u, 1u);
    float4 history = 0, fast = 0;
    if (historyValid != 0u)
        SSGIReprojectHistory(pixel, sceneSize, signalSize, factor, z, shading,
            invViewProjMat, prevJitteredViewProj, prevJitteredInvViewProj,
            previousGITextureIdx, previousDepthTextureIdx, previousNormalTextureIdx,
            previousFastTextureIdx, history, fast);
    reprojected[id.xy] = history;
    reprojectedFast[id.xy] = fast;
    float marker = !supported ? -2.0 : (anyFresh || history.a <= 0 ? 1.0 : -1.0);
    output[id.xy] = float4(lighting, marker);
}

#include "ScreenProbeAdaptive.hlsl"
