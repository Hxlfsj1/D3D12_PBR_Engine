#ifndef VXGI_SURFACE_COMMON_HLSLI
#define VXGI_SURFACE_COMMON_HLSLI

#include "VXGISDFCommon.hlsli"
#include "GISourceCommon.hlsli"

struct VXGISurfaceMaterial
{
    float2 uv;
    float3 albedo;
    float alpha;
    float3 orm;
    float occlusion;
    float perceptualRoughness;
    float metallic;
    float3 geometricNormal;
    float3 shadingNormal;
    float3 tangent;
    float3 bitangent;
    float3 uncappedEmissive;
    uint materialId;
    uint instanceId;
    uint isUnlit;
    uint backFace;
    uint isCutout;
    uint valid;
};

struct VXGISurfaceSource
{
    VXGISurfaceMaterial surface;
    float3 directDiffuse;
    float3 limitedEmissive;
    float3 lo0;
    float sunVisibility;
    uint shadowTraceStatus; // Same status convention as VXGISDFHit.traceStatus.
    uint valid;
};

// Mirrors GBuffer LOD0 material semantics. Every texture read has an explicit,
// camera-independent SampleLevel footprint; no screen derivative/mip bias is used.
// Cutout coverage was already accepted by the shared material-aware trace.
VXGISurfaceMaterial VXGISampleSurfaceMaterial(VXGISDFHit hit,
    StructuredBuffer<VXGISDFTriangle> triangles, StructuredBuffer<VXGIMaterialData> materials,
    SamplerState materialSampler, float materialMipLevel)
{
    VXGISurfaceMaterial result = (VXGISurfaceMaterial)0;
    if (hit.valid == 0) return result;
    VXGISDFSurface surface = VXGISDFResolveSurface(hit, triangles);
    VXGIMaterialData material = materials[surface.materialId];
    float mipLevel = max(materialMipLevel, 0.0);
    Texture2D<float4> albedoTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoIdx)];
    float4 albedoSample = albedoTexture.SampleLevel(materialSampler, surface.uv, mipLevel);
    result.uv = surface.uv;
    result.albedo = DecodeSRGBColor(albedoSample.rgb) * material.baseColorFactor.rgb;
    result.alpha = albedoSample.a * material.baseColorFactor.a;

    Texture2D<float4> ormTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.ormIdx)];
    result.orm = ormTexture.SampleLevel(materialSampler, surface.uv, mipLevel).rgb;
    result.occlusion = max(result.orm.r, 0.01);
    result.perceptualRoughness = ClampPerceptualRoughness(result.orm.g);
    result.metallic = result.orm.b;

    Texture2D<float4> normalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.normalIdx)];
    float3 tangentNormal = normalTexture.SampleLevel(materialSampler, surface.uv, mipLevel).xyz * 2.0 - 1.0;
    tangentNormal.y = -tangentNormal.y;
    float3 mappedNormal = mul(tangentNormal, float3x3(surface.tangent, surface.bitangent, surface.shadingNormal));
    mappedNormal = dot(mappedNormal, mappedNormal) > 1e-20 ? normalize(mappedNormal) : surface.shadingNormal;
    result.shadingNormal = FaceNormalByFrontFace(mappedNormal, hit.backFace == 0);
    result.geometricNormal = hit.geometricNormal;
    result.tangent = surface.tangent;
    result.bitangent = surface.bitangent;

    // GBuffer suppresses emissive for unlit materials; preserve that convention.
    if (material.isUnlit == 0)
    {
        Texture2D<float4> emissiveTexture = ResourceDescriptorHeap[NonUniformResourceIndex(material.emissiveIdx)];
        result.uncappedEmissive = DecodeSRGBColor(emissiveTexture.SampleLevel(materialSampler, surface.uv, mipLevel).rgb)
            * material.emissiveFactor;
    }
    result.materialId = surface.materialId;
    result.instanceId = surface.instanceId;
    result.isUnlit = material.isUnlit;
    result.backFace = hit.backFace;
    result.isCutout = VXGISDFHitIsCutout(hit, triangles);
    result.valid = 1;
    return result;
}

// Known source radiance Lo0 = direct diffuse + limited emission. This is a
// hit-UV query, not one uniform light value per triangle and not probe feedback.
VXGISurfaceSource VXGIEvaluateSurfaceSource(VXGISDFHit hit, VXGISDFGrid grid,
    StructuredBuffer<float> nodes, StructuredBuffer<uint> cellOffsets,
    StructuredBuffer<uint> triangleReferences, StructuredBuffer<VXGISDFTriangle> triangles,
    StructuredBuffer<VXGIMaterialData> materials, uint materialCount, SamplerState materialSampler,
    float materialMipLevel, float3 sunDirectionTowardLight, float3 sunRadiance,
    float rayMinDistance, float maxRayDistance)
{
    VXGISurfaceSource source = (VXGISurfaceSource)0;
    if (hit.valid == 0 || VXGISDFHitMaterialId(hit, triangles) >= materialCount) return source;
    source.surface = VXGISampleSurfaceMaterial(hit, triangles, materials, materialSampler, materialMipLevel);
    if (source.surface.valid == 0) return source;

    VXGISDFHit blocker;
    bool occluded = VXGISDFTrace(hit.position, sunDirectionTowardLight, rayMinDistance, maxRayDistance,
        grid, nodes, cellOffsets, triangleReferences, triangles,
        materials, materialCount, materialSampler, materialMipLevel, blocker);
    source.shadowTraceStatus = blocker.traceStatus;
    source.sunVisibility = !occluded && blocker.traceStatus == 0 ? 1.0 : 0.0;
    float NdotL = 0.0;
    if (blocker.traceStatus < 2)
        NdotL = max(dot(source.surface.shadingNormal, normalize(sunDirectionTowardLight)), 0.0);
    source.directDiffuse = source.surface.isUnlit == 0
        ? GIComputeSourceDirectDiffuse(source.surface.albedo, source.surface.metallic, sunRadiance, NdotL, source.sunVisibility)
        : float3(0, 0, 0);
    source.limitedEmissive = source.surface.isUnlit == 0
        ? GICapSourceEmissive(source.surface.uncappedEmissive) : float3(0, 0, 0);
    source.lo0 = GIComputeSourceRadiance(source.surface.albedo, source.surface.metallic,
        sunRadiance, NdotL, source.sunVisibility, source.surface.uncappedEmissive, source.surface.isUnlit != 0);
    source.valid = blocker.traceStatus < 2 ? 1 : 0;
    return source;
}

#endif
