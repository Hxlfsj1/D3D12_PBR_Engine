// Stages retain independent b0 layouts; select one with SSGI_PASS_<STAGE>.

#if defined(SSGI_PASS_PYRAMID)

#include "SSGICommon.hlsli"

cbuffer SSGIPyramidConstants : register(b0)
{
    float4x4 invProjMat;
    uint2 sourceSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint outputDepthTextureIdx;
    uint firstLevel;
    uint padding;
};

// Bounds are positive view Z: (nearest, furthest). Empty cells use (1e20, 0).
// Each level is a separate resource so RDG needs no subresource state tracking.

[numthreads(8, 8, 1)]
void CSMain_Pyramid(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize))
    {
        return;
    }
    Texture2D<float2> depths = ResourceDescriptorHeap[depthTextureIdx];
    RWTexture2D<float2> outputDepth = ResourceDescriptorHeap[outputDepthTextureIdx];
    float2 bounds = float2(1e20, 0);

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        uint2 p = id.xy * 2 + uint2(i & 1, i >> 1);
        float2 b = float2(1e20, 0);
        if (all(p < sourceSize))
        {
            b = depths.Load(int3(p, 0));
            if (firstLevel)
            {
                float z = ReconstructPosition((p + 0.5) / sourceSize, b.x, invProjMat).z;
                b = b.x < 1.0 ? z.xx : float2(1e20, 0);
            }
        }
        bounds = float2(min(bounds.x, b.x), max(bounds.y, b.y));
    }
    outputDepth[id.xy] = bounds;
}

#elif defined(SSGI_PASS_TRACE)

#include "SSGICommon.hlsli"
#include "ScreenProbeCommon.hlsli"
#include "BlueNoise.hlsli"
#include "SSGIScreenTraceCommon.hlsli"

cbuffer SSGITraceConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 projMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 outputSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint emissiveSourceIndex;
    uint outputTextureIdx;
    uint levelCount;
    uint frameIndex;
    uint blueNoiseTextureIdx;
    float radius;
    float thickness;
    float normalBias;
    uint maxIterations;
    uint traceDownsampleFactor;
    uint denoiseDownsampleFactor;
    uint probeScreenRays;
    uint emissiveSamples;
    uint4 levels[7]; // Keep in sync with SSGIMaxLevels.
    float4x4 invViewMat;
    float4x4 invViewProjMat;
    float4x4 prevJitteredViewProj;
    float4x4 prevJitteredInvViewProj;
    uint previousGITextureIdx;
    uint previousDepthTextureIdx;
    uint previousNormalTextureIdx;
    uint historyValid;
    uint previousFastTextureIdx;
    uint reprojectedGITextureIdx;
    uint reprojectedFastTextureIdx;
    uint probePositionIndex;
    uint probeNormalIndex;
    uint probeReceiverIndex;
    uint previousProbePositionIndex;
    uint previousProbeNormalIndex;
    uint previousProbeRadianceIndex;
    uint probeRadianceUavIndex;
    uint probeRadianceIndex;
    uint probeFilteredUavIndex;
    uint probeFilteredIndex;
    uint probeHistoryLimit;
    uint probeSHUavIndex;
    uint probeSHIndex;
    uint probePositionUavIndex;
    uint probeNormalUavIndex;
    uint probeReceiverUavIndex;
    float probeJitterWidth;
    uint probeAnchorUavIndex;
    uint previousProbeAnchorIndex;
    uint probeDispatchBase;
    uint emissiveListIndex;
    uint2 probeAtlasSize;
    uint adaptiveCapacity;
    uint adaptiveCountIndex;
    uint adaptiveCountUavIndex;
    uint adaptiveHeaderIndex;
    uint adaptiveHeaderUavIndex;
    uint adaptiveIndicesIndex;
    uint adaptiveIndicesUavIndex;
    uint adaptiveMaskIndex;
    uint adaptiveMaskUavIndex;
    uint adaptiveArgsUavIndex;
    uint previousAdaptiveHeaderIndex;
    uint previousAdaptiveIndicesIndex;
    uint previousAdaptiveCountIndex;
    uint probeHistoryLinkIndex;
    uint probeHistoryLinkUavIndex;
    uint probeSeedIndex;
    uint probeSeedUavIndex;
    uint previousProbeSeedIndex;
    float probeCameraFadeEnd;
    uint probeCameraFadeEnabled;
    uint probeDiagnosticsIndexPlusOne;
    uint emissiveCountIndex;
    uint emissiveListUavIndex,emissiveCountUavIndex;
    uint2 screenEmissivePadding;
};

float3 Project(float3 p)
{
    float4 clip = mul(float4(p, 1), projMat);
    return float3(clip.xy / clip.w * float2(0.5, -0.5) + 0.5, clip.z / clip.w);
}

float3 CosineDirection(float2 xi, float3 n)
{
    float phi = 6.28318530718 * xi.y;
    float3 tangent = normalize(cross(abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), n));
    return tangent * (sqrt(xi.x) * cos(phi)) + cross(n, tangent) * (sqrt(xi.x) * sin(phi)) + n * sqrt(1 - xi.x);
}

// The hierarchy stores positive view-Z bounds, while traversal uses projected device Z.
float DeviceDepth(float viewZ)
{
    float4 clip = mul(float4(0, 0, viewZ, 1), projMat);
    return clip.z / clip.w;
}

// Select the half-open cell containing t, including the forward side of a crossed
// boundary. Compare boundary times instead of nudging pixel coordinates: a fixed
// epsilon can disappear at large coordinates or skip a nearby fine-cell hit.
int2 RayCell(float2 startPixels, float2 deltaPixels, float2 inverseDelta, float t, uint mip)
{
    float cellSize = float(1u << mip);
    int2 cell = int2(floor((startPixels + deltaPixels * t) / cellSize));
    [unroll] for (uint axis = 0; axis < 2; ++axis)
    {
        float delta = deltaPixels[axis];
        // Match CellExit's stationary-axis convention and never divide by zero.
        if (abs(delta) > 1e-8)
        {
            int step = delta > 0 ? 1 : -1;
            float entry = (float(cell[axis]) + float(delta < 0)) * cellSize;
            float exit = (float(cell[axis]) + float(delta > 0)) * cellSize;
            float entryT = (entry - startPixels[axis]) * inverseDelta[axis];
            float exitT = (exit - startPixels[axis]) * inverseDelta[axis];
            // Reconstructing start + delta*t can round to either side of the
            // boundary. Preserve cells still ahead; discard cells already left.
            if (entryT > t) cell[axis] -= step;
            else if (exitT <= t) cell[axis] += step;
        }
    }
    return cell;
}

// Compute the reciprocal once per ray for both Hi-Z query types.
float CellExit(float2 startPixels, float2 deltaPixels, float2 inverseDelta,
    int2 cell, uint mip, float endT)
{
    float2 boundary = (float2(cell) + float2(deltaPixels.x > 0, deltaPixels.y > 0)) * float(1u << mip);
    float2 times = (boundary - startPixels) * inverseDelta;
    float tx = abs(deltaPixels.x) > 1e-8 ? times.x : endT;
    float ty = abs(deltaPixels.y) > 1e-8 ? times.y : endT;
    return min(endT, min(tx, ty));
}

// Intersect the WHOLE segment inside a cell with its conservative depth slab.
// This also handles rays travelling towards the camera and rays parallel to the depth plane.
bool DepthInterval(float startZ, float deltaZ, float2 bounds, float t, float exitT, out float entryT)
{
    float nearDepth = DeviceDepth(bounds.x);
    float farDepth = DeviceDepth(bounds.y);
    entryT = t;
    if (abs(deltaZ) < 1e-8)
    {
        return startZ >= nearDepth && startZ <= farDepth;
    }
    float a = (nearDepth - startZ) / deltaZ;
    float b = (farDepth - startZ) / deltaZ;
    entryT = max(t, min(a, b));
    return entryT <= min(exitT, max(a, b));
}

// This is the finite segment already used by screen tracing. Viewport clipping
// only limits the screen traversal; it never changes this segment's 3D extent.
float SSGIViewRayLength(float3 origin, float3 direction)
{
    float rayLength = radius;
    float nearZ = ReconstructPosition(0.5.xx, 0, invProjMat).z;
    if (direction.z < 0)
    {
        rayLength = min(rayLength, max(0, (origin.z - nearZ * 1.01) / -direction.z));
    }
    return rayLength;
}

float3 SSGIReceiverGeometricNormal(uint2 receiver, float3 shadingNormal)
{
    return SPGeometricNormal(receiver, shadingNormal, depthTextureIdx, sceneSize, invViewProjMat);
}


#include "SSGIHiZTrace.hlsli"
#include "SSGISurfaceCache.hlsli"

uint SSGIRaySampleIndex(uint baseIndex, uint ray)
{
    // Preserve the existing STBN pattern sequence used by screen probes.
    // This is a procedural pattern ID, not a resource address. Unsigned wrap
    // is allowed; LoadSTBN2D bounds the final texture lookup.
    return baseIndex + (ray & 15u) + 48u * (ray >> 4u);
}

[numthreads(8, 8, 1)]
void CSMain_Trace(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= outputSize)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputTextureIdx];
    RWTexture2D<float4> reproj = ResourceDescriptorHeap[reprojectedGITextureIdx];
    RWTexture2D<float4> fast = ResourceDescriptorHeap[reprojectedFastTextureIdx];
    output[id.xy] = 0;
    reproj[id.xy] = 0;
    fast[id.xy] = 0;

    uint2 pixel = id.xy * SSGITraceFactor(traceDownsampleFactor);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normals = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    if (z >= 1 || orm.Load(int3(pixel, 0)).a < .5) return;
    float3 normal = normalize(DecodeGBufferNormal(normals.Load(int3(pixel, 0)).xyz));
    float4 history = 0, fastHistory = 0;
    if (historyValid != 0u)
        SSGIReprojectHistory(pixel, sceneSize, outputSize, SSGITraceFactor(traceDownsampleFactor), z, normal,
            invViewProjMat, prevJitteredViewProj, prevJitteredInvViewProj, previousGITextureIdx,
            previousDepthTextureIdx, previousNormalTextureIdx, previousFastTextureIdx, history, fastHistory);
    reproj[id.xy] = history;
    fast[id.xy] = fastHistory;
    if (history.a > 0 && ((id.x + id.y + frameIndex) & 1u) != 0u)
    {
        output[id.xy] = float4(history.rgb, -1);
        return;
    }

    float3 origin = ReconstructPosition((pixel + .5) / sceneSize, z, invProjMat);
    float3 n = SSGIDecodeViewNormal(normals.Load(int3(pixel, 0)).xyz, viewMat);
    Texture2DArray<uint2> noise = ResourceDescriptorHeap[blueNoiseTextureIdx];
    float3 sum = 0;
    [unroll]
    for (uint i = 0; i < 4u; ++i)
    {
        float3 direction = CosineDirection(LoadSTBN2D(noise, id.xy, frameIndex, SSGIRaySampleIndex(16u, i)), n);
        float3 worldOrigin=mul(float4(origin+n*normalBias,1),invViewMat).xyz;
        float3 worldDirection=normalize(mul(float4(direction,0),invViewMat).xyz);
        sum+=SSGITraceCached(pixel,worldOrigin,worldDirection,false).rgb;
    }
    output[id.xy] = float4(sum * .25, 1);
}

#elif defined(SSGI_PASS_COMPOSITE)

#include "SSGICommon.hlsli"
#include "ScreenProbeCommon.hlsli"
#include "MaterialCommon.hlsli"
#include "FullscreenTriangle.hlsli"

cbuffer SSGICompositeConstants : register(b0)
{
    float4x4 viewMat;
    float4x4 invProjMat;
    uint2 sceneSize;
    uint2 halfSize;
    uint depthTextureIdx;
    uint normalTextureIdx;
    uint ormTextureIdx;
    uint albedoTextureIdx;
    uint giTextureIdx;
    float intensity;
    uint traceDownsampleFactor;
    uint padding;
    float cameraFadeStart;
    float cameraFadeEnd;
    uint cameraFadeEnabled;
    uint cameraFadePadding;
    float4x4 invViewProjMat;
    uint2 probeUniformSize;
    uint2 probeAtlasSize;
    uint probeFactor;
    uint probePositionIndex;
    uint probeNormalIndex;
    uint probeReceiverIndex;
    uint probeSHIndex;
    uint adaptiveCapacity;
    uint adaptiveCountIndex;
    uint adaptiveHeaderIndex;
    uint adaptiveIndicesIndex;
    uint validityIndexPlusOne;
    uint probeDiagnosticsIndexPlusOne;
    uint probeCompositePadding;
};

float4 VSMain(uint id : SV_VertexID) : SV_POSITION
{
    return GetFullscreenTrianglePosition(GetFullscreenTriangleTexCoord(id));
}

float4 PSMain(float4 position : SV_POSITION) : SV_Target0
{
    uint2 pixel = uint2(position.xy);
    Texture2D<float> depth = ResourceDescriptorHeap[depthTextureIdx];
    Texture2D<float4> normal = ResourceDescriptorHeap[normalTextureIdx];
    Texture2D<float4> orm = ResourceDescriptorHeap[ormTextureIdx];
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[albedoTextureIdx];
    Texture2D<float4> gi = ResourceDescriptorHeap[giTextureIdx];
    float z = depth.Load(int3(pixel, 0));
    float4 material = orm.Load(int3(pixel, 0));
    if (z >= 1 || material.a < 0.5)
    {
        return 0;
    }

    float2 uv = (pixel + 0.5) / sceneSize;
    float3 p = ReconstructPosition(uv, z, invProjMat);
    // Presentation weight only: accumulated lighting stays unfaded so moving
    // closer restores GI without retaining a previously darkened history.
    float cameraFade = cameraFadeEnabled != 0u
        ? 1.0 - smoothstep(cameraFadeStart, cameraFadeEnd, length(p)) : 1.0;
    if (cameraFade <= 0.0) return 0;
    float3 n = SSGIDecodeViewNormal(normal.Load(int3(pixel, 0)).xyz, viewMat);
    float footprint = SameDepthPixelFootprint(uv, z, p, sceneSize, invProjMat);
    float tolerance = max(min(footprint * 0.5, p.z * 0.01), 0.001);
    uint factor = SSGITraceFactor(traceDownsampleFactor);
    float inverseFactor = factor == 2u ? 0.5 : rcp(float(factor));
    // Signal texel i samples full-resolution pixel factor*i, including odd extents.
    float2 halfPixel = padding != 0u ? (float2(pixel) + 0.5) * inverseFactor - 0.5 : float2(pixel) * inverseFactor;
    int2 base = padding != 0u ? int2(floor(halfPixel + 0.5)) - 1 : int2(floor(halfPixel));
    float2 fraction = frac(halfPixel);
    bool fullResolution = factor == 1u && padding == 0u;
    float3 sum = fullResolution ? gi.Load(int3(pixel, 0)).rgb : 0;
    float weightSum = fullResolution ? 1.0 : 0.0;
    if (fullResolution && validityIndexPlusOne != 0u)
    {
        Texture2D<float4> validity = ResourceDescriptorHeap[validityIndexPlusOne - 1u];
        if (validity.Load(int3(pixel, 0)).a <= 0) { sum = 0; weightSum = 0; }
    }

    [loop]
    for (int i = 0; i < (fullResolution ? 0 : (padding != 0u ? 9 : 4)); ++i)
    {
        int2 offset = padding != 0u ? int2(i % 3, i / 3) : int2(i & 1, i >> 1);
        int2 tap = base + offset;
        if (any(tap < 0) || any(tap >= int2(halfSize)))
        {
            continue;
        }
        if (validityIndexPlusOne != 0u)
        {
            Texture2D<float4> validity = ResourceDescriptorHeap[validityIndexPlusOne - 1u];
            if (validity.Load(int3(tap, 0)).a <= 0) continue;
        }
        uint2 fullPixel = SSGIReceiverPixel(uint2(tap), factor, sceneSize, padding);
        if (any(fullPixel >= sceneSize)) continue;
        float tapZ = depth.Load(int3(fullPixel, 0));
        if (tapZ >= 1 || orm.Load(int3(fullPixel, 0)).a < 0.5)
        {
            continue;
        }

        float3 tapN = SSGIDecodeViewNormal(normal.Load(int3(fullPixel, 0)).xyz, viewMat);
        float agreement = saturate(dot(n, tapN));
        if (agreement < 0.8)
        {
            continue;
        }
        float3 q = ReconstructPosition((fullPixel + 0.5) / sceneSize, tapZ, invProjMat);
        float3 delta = q - p;
        float planeError = SymmetricPlaneDistance(delta, n, tapN) / tolerance;

        // Preserve coplanar surfaces using plane distance rather than point distance.
        if (planeError >= 3)
        {
            continue;
        }
        float2 bilinear = lerp(1 - fraction, fraction, float2(offset));
        float2 separation = float2(tap) - halfPixel;
        float spatialWeight = padding != 0u ? exp2(-2.0 * dot(separation, separation)) : bilinear.x * bilinear.y;
        float weight = SSGIApplyBilateralGeometryWeight(spatialWeight, agreement, planeError);

        sum += gi.Load(int3(tap, 0)).rgb * weight; // Spatial variance in alpha is never a lighting factor.
        weightSum += weight;
    }
    float3 indirect = weightSum > 1e-5 ? sum / weightSum : 0;
    if (weightSum <= 1e-5 && adaptiveCapacity > 0u)
    {
        // A one-pixel surface can have probes but no half-resolution receiver.
        // Evaluate already filtered/temporal probe SH only for this missing support.
        ScreenProbeQuery query;
        query.sceneSize = sceneSize; query.uniformSize = probeUniformSize; query.atlasSize = probeAtlasSize;
        query.factor = probeFactor; query.positionIndex = probePositionIndex; query.normalIndex = probeNormalIndex;
        query.receiverIndex = probeReceiverIndex; query.shIndex = probeSHIndex; query.capacity = adaptiveCapacity;
        query.countIndex = adaptiveCountIndex; query.headerIndex = adaptiveHeaderIndex; query.indicesIndex = adaptiveIndicesIndex;
        float3 world = ReconstructPosition(uv, z, invViewProjMat);
        float3 shading = normalize(DecodeGBufferNormal(normal.Load(int3(pixel, 0)).xyz));
        float3 geometric = SPGeometricNormal(pixel, shading, depthTextureIdx, sceneSize, invViewProjMat);
        if (dot(geometric, shading) < .5) geometric = shading;
        float worldFootprint = max(SameDepthPixelFootprint(uv, z, world, sceneSize, invViewProjMat), 1e-5);
        bool fresh;
        SPCounter(probeDiagnosticsIndexPlusOne, 11u, 1u);
        bool supported = SPResolveSH(query, pixel, world, geometric, shading, worldFootprint, indirect, fresh);
        SPCounter(probeDiagnosticsIndexPlusOne, supported ? 12u : 13u, 1u);
    }
    indirect *= cameraFade;

#ifdef SSGI_DISPLAY_IRRADIANCE
    // Display the completed GI signal (E/pi), without the receiver's material response.
    return float4(indirect * intensity, 0);
#else
    float3 albedo = albedoTexture.Load(int3(pixel, 0)).rgb;
    float3 F = fresnelSchlickRoughness(saturate(dot(n, normalize(-p))), ComputeMaterialF0(albedo, material.b),
                                       ClampPerceptualRoughness(material.g));
    // Input already stores E/pi. Apply receiver diffuse response once; additive blend preserves SceneColor alpha.
    float3 result = indirect * albedo * ComputeDiffuseEnergy(F, material.b) * intensity;

    return float4(result, 0);
#endif
}

#endif
