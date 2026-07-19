#define OCCLUSION_CANDIDATE_FORCE_VISIBLE 1u
#define MAX_HIZ_LEVELS 16

struct OcclusionCandidateData
{
    float3 boundsCenter;
    uint occlusionId;
    float3 boundsExtents;
    uint flags;
};

cbuffer OcclusionCullConstants : register(b0)
{
    float4x4 viewProj;
    uint candidateCount;
    uint visibilityCount;
    uint visibilityBufferIdx;
    uint hizLevelCount;
    uint screenWidth;
    uint screenHeight;
    uint hizTextureIdx[MAX_HIZ_LEVELS];
};

StructuredBuffer<OcclusionCandidateData> gCandidates : register(t0);

// Default every object to visible. The cull pass only writes definite occlusion.
[numthreads(64, 1, 1)]
void CSClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint visibilityIndex = dispatchThreadId.x;
    if (visibilityIndex >= visibilityCount)
    {
        return;
    }

    RWStructuredBuffer<uint> visibility = ResourceDescriptorHeap[visibilityBufferIdx];
    visibility[visibilityIndex] = 1;
}

float ComputeMipLevel(float rectWidth, float rectHeight)
{
    float maxExtent = max(max(rectWidth, rectHeight), 1.0f);
    return floor(log2(maxExtent));
}

uint GetMipScale(uint mipLevel)
{
    return 1u << mipLevel;
}

uint2 GetMipSize(uint mipLevel)
{
    uint mipScale = GetMipScale(mipLevel);
    return uint2(
        max(1u, (screenWidth + mipScale - 1u) >> mipLevel),
        max(1u, (screenHeight + mipScale - 1u) >> mipLevel));
}

bool CanMipProveOccluded(
    uint mipLevel,
    float minX,
    float minY,
    float maxX,
    float maxY,
    float objectNearDepth)
{
    const uint maxTexelsPerMip = 64u;
    const float depthBias = 0.005f;

    if (objectNearDepth <= depthBias)
    {
        return false;
    }

    uint mipScale = GetMipScale(mipLevel);
    uint2 mipSize = GetMipSize(mipLevel);
    uint2 maxCoord = mipSize - 1u;

    uint2 rectMinCoord = min(uint2((uint)(minX / (float)mipScale), (uint)(minY / (float)mipScale)), maxCoord);
    uint2 rectMaxCoord = min(uint2((uint)(maxX / (float)mipScale), (uint)(maxY / (float)mipScale)), maxCoord);
    uint2 rectSize = rectMaxCoord - rectMinCoord + 1u;
    uint texelCount = rectSize.x * rectSize.y;

    if (texelCount == 0u || texelCount > maxTexelsPerMip)
    {
        return false;
    }

    Texture2D<float> hizTexture = ResourceDescriptorHeap[hizTextureIdx[mipLevel]];
    float depthThreshold = objectNearDepth - depthBias;

    for (uint y = rectMinCoord.y; y <= rectMaxCoord.y; ++y)
    {
        for (uint x = rectMinCoord.x; x <= rectMaxCoord.x; ++x)
        {
            float hizMaxDepth = hizTexture.Load(int3(uint2(x, y), 0));
            if (hizMaxDepth >= depthThreshold)
            {
                return false;
            }
        }
    }

    return true;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= candidateCount)
    {
        return;
    }

    OcclusionCandidateData candidate = gCandidates[candidateIndex];
    if (candidate.occlusionId >= visibilityCount)
    {
        return;
    }

    RWStructuredBuffer<uint> visibility = ResourceDescriptorHeap[visibilityBufferIdx];

    if ((candidate.flags & OCCLUSION_CANDIDATE_FORCE_VISIBLE) != 0 ||
        hizLevelCount == 0 ||
        screenWidth == 0 ||
        screenHeight == 0)
    {
        visibility[candidate.occlusionId] = 1;
        return;
    }

    float3 center = candidate.boundsCenter;
    float3 extent = candidate.boundsExtents;

    float minX = 1.0e20f;
    float minY = 1.0e20f;
    float maxX = -1.0e20f;
    float maxY = -1.0e20f;
    float minDepth = 1.0f;

    [unroll]
    for (uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        float3 cornerOffset = float3(
            (cornerIndex & 1) != 0 ? extent.x : -extent.x,
            (cornerIndex & 2) != 0 ? extent.y : -extent.y,
            (cornerIndex & 4) != 0 ? extent.z : -extent.z);

        float4 clipPos = mul(float4(center + cornerOffset, 1.0f), viewProj);
        if (clipPos.w <= 0.0001f)
        {
            visibility[candidate.occlusionId] = 1;
            return;
        }

        float3 ndc = clipPos.xyz / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
        {
            visibility[candidate.occlusionId] = 1;
            return;
        }

        float screenX = (ndc.x * 0.5f + 0.5f) * (float)screenWidth;
        float screenY = (0.5f - ndc.y * 0.5f) * (float)screenHeight;

        minX = min(minX, screenX);
        minY = min(minY, screenY);
        maxX = max(maxX, screenX);
        maxY = max(maxY, screenY);
        minDepth = min(minDepth, ndc.z);
    }

    if (maxX < 0.0f || maxY < 0.0f || minX > (float)screenWidth || minY > (float)screenHeight)
    {
        visibility[candidate.occlusionId] = 1;
        return;
    }

    minX = clamp(minX, 0.0f, (float)screenWidth);
    minY = clamp(minY, 0.0f, (float)screenHeight);
    maxX = clamp(maxX, 0.0f, (float)screenWidth);
    maxY = clamp(maxY, 0.0f, (float)screenHeight);

    float rectWidth = max(maxX - minX, 1.0f);
    float rectHeight = max(maxY - minY, 1.0f);
    if (minDepth < 0.02f || rectWidth < 2.0f || rectHeight < 2.0f)
    {
        visibility[candidate.occlusionId] = 1;
        return;
    }

    uint mipLevel = min((uint)ComputeMipLevel(rectWidth, rectHeight), hizLevelCount - 1);
    bool occluded = false;

    for (uint mipIteration = mipLevel + 1u; mipIteration > 0u; --mipIteration)
    {
        uint currentMip = mipIteration - 1u;
        if (CanMipProveOccluded(currentMip, minX, minY, maxX, maxY, minDepth))
        {
            occluded = true;
            break;
        }
    }

    visibility[candidate.occlusionId] = occluded ? 0 : 1;
}
