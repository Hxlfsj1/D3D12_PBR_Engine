#include "VXGISurfaceCommon.hlsli"
#include "VXGIProbeQueryCommon.hlsli"

cbuffer DebugConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint debugMode;
    float3 boundsMaximum; uint triangleCount;
    float3 sunDirectionTowardLight; float sliceCoordinateWorld;
    uint sliceAxis; float2 inverseOutputSize; float unusedPadding;
    float3 sunRadiance; float materialMipLevel;
    float rayMinDistance; float maxRayDistance; uint materialCount; uint surfacePadding;
    float voxelSize; uint probeCount; uint2 queryPadding;
};

StructuredBuffer<float> distanceNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
StructuredBuffer<float3> probeCenters : register(t5);
StructuredBuffer<VXGIProbeCellEntry> probeLookup : register(t6);
StructuredBuffer<VXGIProbeRecord> rawProbes : register(t7);
SamplerState materialSampler : register(s0);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pixel : SV_Position) : SV_Target0
{
    const float3 background = float3(0.018, 0.022, 0.03);
    float2 ndc = pixel.xy * inverseOutputSize * float2(2, -2) + float2(-1, 1);
    float4 nearH = mul(float4(ndc, 0, 1), inverseViewProjection);
    float4 farH = mul(float4(ndc, 1, 1), inverseViewProjection);
    float3 rayOrigin = nearH.xyz / nearH.w;
    float3 rayDirection = normalize(farH.xyz / farH.w - rayOrigin);
    VXGISDFGrid grid;
    grid.origin = gridOrigin;
    grid.cellSize = cellSize;
    grid.cellDimensions = cellDimensions;
    grid.boundsMaximum = boundsMaximum;

    if (debugMode == 4)
    {
        if (abs(rayDirection[sliceAxis]) < 1e-8) return float4(background, 1);
        float planeDistance = (sliceCoordinateWorld - rayOrigin[sliceAxis]) / rayDirection[sliceAxis];
        float3 position = rayOrigin + rayDirection * planeDistance;
        if (planeDistance < 0 || any(position < gridOrigin) || any(position > boundsMaximum))
            return float4(background, 1);
#ifdef VXGI_MESH_SDF
        float d = VXGIMeshSDFDisplayDistance(position, distanceNodes);
#else
        float d = VXGISDFDisplayDistance(position, grid, distanceNodes);
#endif
        float nearSurface = exp(-d / max(cellSize, 1e-6));
        return float4(lerp(float3(0.06, 0.13, 0.3), float3(1.0, 0.68, 0.08), nearSurface), 1);
    }

    VXGISDFHit hit;
    if (!VXGISDFTrace(rayOrigin, rayDirection, 0.0, 1e30, grid, distanceNodes,
        cellOffsets, triangleReferences, triangles, materials, materialCount, materialSampler, materialMipLevel, hit))
        return float4(hit.traceStatus >= 2 ? float3(1, 0, 1) : background, 1);
    if (debugMode == 5)
        return float4(saturate((hit.position - gridOrigin) / max(boundsMaximum - gridOrigin, 1e-6)), 1);
    if (debugMode == 6)
        return float4(hit.geometricNormal * 0.5 + 0.5, 1);
    if (debugMode == 7)
    {
        VXGISDFHit blocker;
        float bias = max(cellSize * 1e-4, 1e-5);
        bool occluded = VXGISDFTrace(hit.position, normalize(sunDirectionTowardLight), bias, 1e30,
            grid, distanceNodes, cellOffsets, triangleReferences, triangles,
            materials, materialCount, materialSampler, materialMipLevel, blocker);
        if (blocker.traceStatus >= 2) return float4(1, 0, 1, 1);
        return float4(occluded ? float3(0.24, 0.035, 0.025) : float3(0.3, 0.9, 0.48), 1);
    }
    if (debugMode >= 8)
    {
        if (VXGISDFHitMaterialId(hit,triangles) >= materialCount) return float4(1, 0, 1, 1);
        if (debugMode == 12)
        {
            VXGISurfaceSource source = VXGIEvaluateSurfaceSource(hit, grid, distanceNodes, cellOffsets,
                triangleReferences, triangles, materials, materialCount, materialSampler, materialMipLevel,
                sunDirectionTowardLight, sunRadiance, rayMinDistance, maxRayDistance);
            if (source.valid == 0) return float4(1, 0, 1, 1);
            // Display only. Source queries keep linear HDR radiance.
            return float4(pow(max(source.lo0, 0.0) / (1.0 + max(source.lo0, 0.0)), 1.0 / 2.2), 1);
        }
        VXGISurfaceMaterial surface = VXGISampleSurfaceMaterial(hit, triangles, materials, materialSampler, materialMipLevel);
        if (debugMode >= 18)
        {
            VXGIProbeQueryParameters parameters = (VXGIProbeQueryParameters)0;
            parameters.gridOrigin=gridOrigin;parameters.cellSize=cellSize;parameters.cellDimensions=cellDimensions;
            parameters.boundsMaximum=boundsMaximum;parameters.triangleCount=triangleCount;
            parameters.voxelSize=voxelSize;parameters.probeCount=probeCount;
            parameters.materialCount=materialCount;parameters.materialMipLevel=materialMipLevel;
            VXGIProbeQueryResult query=VXGIProbeQuerySurface(hit,surface.shadingNormal,parameters,probeLookup,
                probeCenters,rawProbes,distanceNodes,cellOffsets,triangleReferences,triangles,materials,materialSampler);
            // A valid black sample is distinguishable from missing support.
            if(query.hasSupport==0) return float4(query.traceFailures?float3(1,0,1):float3(.8,.04,.02),1);
            if(debugMode==18) return float4(pow(query.irradianceOverPi/(1+query.irradianceOverPi),1.0/2.2),1);
            if(debugMode==19) return float4(lerp(float3(.03,.1,.4),float3(.1,1,.2),saturate(query.supportWeight)),1);
            return float4(query.negativeMask?float3(1,.1,1):float3(.04,.8,.25),1);
        }
        if (debugMode == 8) return float4(pow(max(surface.albedo, 0.0), 1.0 / 2.2), 1);
        if (debugMode == 9) return float4(surface.shadingNormal * 0.5 + 0.5, 1);
        if (debugMode == 10) return float4(surface.metallic.xxx, 1);
        if (debugMode == 11)
            return float4(pow(max(surface.uncappedEmissive, 0.0) / (1.0 + max(surface.uncappedEmissive, 0.0)), 1.0 / 2.2), 1);
    }
    return float4(background, 1);
}
