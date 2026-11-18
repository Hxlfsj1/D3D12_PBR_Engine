#include "VXGISDFCommon.hlsli"
#include "VXGIGlobalSDFCommon.hlsli"

cbuffer DebugConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float3 gridOrigin; float cellSize;
    uint3 cellDimensions; uint debugMode;
    float3 boundsMaximum; uint triangleCount;
    float3 sunDirectionTowardLight; float sliceCoordinateWorld;
    uint sliceAxis; float2 inverseOutputSize; float materialMipLevel;
    float rayMinDistance; float maxRayDistance; uint materialCount; uint globalSDFIndex;
    float meshTraceDistance; float globalTraceDistance; uint globalMaxSteps; uint cardCount;
    VXGIGlobalSDFConstants global;
};

StructuredBuffer<float> distanceNodes : register(t0);
StructuredBuffer<uint> cellOffsets : register(t1);
StructuredBuffer<uint> triangleReferences : register(t2);
StructuredBuffer<VXGISDFTriangle> triangles : register(t3);
StructuredBuffer<VXGIMaterialData> materials : register(t4);
SamplerState materialSampler : register(s0);

struct DebugCard
{
    float3 origin; uint instanceIndex;
    float3 axisX; float extentX;
    float3 axisY; float extentY;
    float3 axisZ; float extentZ;
    uint direction; uint cardIndex; float cellSize; float padding;
};
StructuredBuffer<DebugCard> debugCards : register(t5);

static const float3 Background = float3(0.018, 0.022, 0.03);
static const float3 InvalidTrace = float3(1, 0, 1);

VXGISDFGrid MeshGrid()
{
    VXGISDFGrid grid;
    grid.origin = gridOrigin;
    grid.cellSize = cellSize;
    grid.cellDimensions = cellDimensions;
    grid.boundsMaximum = boundsMaximum;
    return grid;
}

bool TraceMesh(float3 origin, float3 direction, float maximumDistance, out VXGISDFHit hit)
{
    return VXGISDFTrace(origin, direction, 0.0, maximumDistance, MeshGrid(), distanceNodes,
        cellOffsets, triangleReferences, triangles, materials, materialCount,
        materialSampler, materialMipLevel, hit);
}

// Distances are unsigned. At an exterior field hit their gradient points away
// from the surface; use the same clipmap for all six derivative samples.
float3 GlobalNormal(Texture3D<float> field, VXGIGlobalSDFTraceResult hit, float3 rayDirection)
{
    float h = global.clipmaps[hit.clipmapIndex].cellSize * 0.5;
    float3 dx = float3(h, 0, 0), dy = float3(0, h, 0), dz = float3(0, 0, h);
    float3 gradient = float3(
        VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position + dx).distance
            - VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position - dx).distance,
        VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position + dy).distance
            - VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position - dy).distance,
        VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position + dz).distance
            - VXGIGlobalSDFSampleClipmapUnchecked(field, global, hit.clipmapIndex, hit.position - dz).distance);
    return dot(gradient, gradient) > 1e-12 ? normalize(gradient) : -rayDirection;
}

float3 PositionColor(float3 position)
{
    // All backends share one display range for direct image comparisons.
    return saturate((position - gridOrigin) / (boundsMaximum - gridOrigin));
}

float3 SliceColor(float distance, float fieldCellSize)
{
    return lerp(float3(0.06, 0.13, 0.3), float3(1.0, 0.68, 0.08), exp(-distance / fieldCellSize));
}

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 CardColor(DebugCard card)
{
    float id = float(card.instanceIndex * 32u + card.cardIndex + 1u);
    return 0.2 + 0.75 * frac(id * float3(0.61803399, 0.38196601, 0.75487767));
}

float3 CardCoordinates(DebugCard card, float3 position)
{
    float3 p = position - card.origin;
    return float3(dot(p, card.axisX), dot(p, card.axisY), dot(p, card.axisZ));
}

// Geometry reference for coverage before a material/depth atlas exists. Cast
// from this Card's actual near plane, against its owning instance only.
bool CardSeesHit(DebugCard card, VXGISDFHit hit)
{
    if (card.instanceIndex != hit.instanceIndex) return false;
    VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[hit.instanceIndex];
    float3 localPosition = mul(float4(hit.position, 1), instance.localFromWorld).xyz;
    float3 p = CardCoordinates(card, localPosition);
    if (any(abs(p) > float3(card.extentX, card.extentY, card.extentZ) + 1e-5)) return false;
    if (dot(triangles[hit.triangleIndex].geometricNormal, card.axisZ) < 0.25) return false;
    float3 localOrigin = localPosition + card.axisZ * (card.extentZ - p.z);
    float3 origin = mul(float4(localOrigin, 1), instance.worldFromLocal).xyz;
    float3 segment = hit.position - origin;
    float distanceToHit = length(segment);
    VXGISDFHit first;
    if (!VXGISDFTraceInstance(hit.instanceIndex, origin, normalize(segment), 0, distanceToHit + 0.001,
        MeshGrid(), distanceNodes, cellOffsets, triangleReferences, triangles, first)) return false;
    return length(first.position - hit.position) < 0.001;
}

bool CardWire(DebugCard card, float3 worldOrigin, float3 worldDirection)
{
    VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[card.instanceIndex];
    float3 localOrigin = mul(float4(worldOrigin, 1), instance.localFromWorld).xyz;
    float3 localDirection = mul(float4(worldDirection, 0), instance.localFromWorld).xyz;
    float3 o = CardCoordinates(card, localOrigin);
    float3 d = float3(dot(localDirection,card.axisX),dot(localDirection,card.axisY),dot(localDirection,card.axisZ));
    float3 extent = float3(card.extentX,card.extentY,card.extentZ);
    float entry = -1e30, exit = 1e30;
    [unroll] for (uint axis=0;axis<3;++axis)
    {
        if (abs(d[axis]) < 1e-8) { if(abs(o[axis]) > extent[axis]) return false; }
        else
        {
            float a=(-extent[axis]-o[axis])/d[axis],b=(extent[axis]-o[axis])/d[axis];
            entry=max(entry,min(a,b));exit=min(exit,max(a,b));
        }
    }
    if(exit<max(entry,0)) return false;
    float width = max(0.002,min(card.extentX,card.extentY)*0.012);
    float3 nearEdge = extent-abs(o+d*max(entry,0));
    float3 farEdge = extent-abs(o+d*exit);
    return dot(float3(nearEdge<width),1.0)>=2 || dot(float3(farEdge<width),1.0)>=2;
}

float4 CardView(float3 rayOrigin,float3 rayDirection)
{
    VXGISDFHit hit;
    bool valid = TraceMesh(rayOrigin,rayDirection,maxRayDistance,hit);
    float3 color = valid ? (0.12+0.22*abs(dot(hit.geometricNormal,normalize(float3(1,2,-3))))) : Background;
    if(debugMode==19 && valid)
    {
        color=float3(1,0,1); // Missing geometric capture coverage.
        [loop] for(uint i=0;i<cardCount;++i)
            if(CardSeesHit(debugCards[i],hit)) {color=CardColor(debugCards[i]);break;}
    }
    if(debugMode==18)
    {
        // X-ray OBB edges expose internal capture slabs as well as outer Cards.
        [loop] for(uint i=0;i<cardCount;++i)
            if(CardWire(debugCards[i],rayOrigin,rayDirection))color=CardColor(debugCards[i]);
    }
    return float4(color,1);
}

float4 PSMain(float4 pixel : SV_Position) : SV_Target0
{
    float2 ndc = pixel.xy * inverseOutputSize * float2(2, -2) + float2(-1, 1);
    float4 nearH = mul(float4(ndc, 0, 1), inverseViewProjection);
    float4 farH = mul(float4(ndc, 1, 1), inverseViewProjection);
    float3 rayOrigin = nearH.xyz / nearH.w;
    float3 rayDirection = normalize(farH.xyz / farH.w - rayOrigin);

    if(debugMode==18 || debugMode==19) return CardView(rayOrigin,rayDirection);

    if (debugMode == 4 || debugMode == 13)
    {
        if (abs(rayDirection[sliceAxis]) < 1e-8) return float4(Background, 1);
        float planeDistance = (sliceCoordinateWorld - rayOrigin[sliceAxis]) / rayDirection[sliceAxis];
        if (planeDistance < 0) return float4(Background, 1);
        float3 position = rayOrigin + rayDirection * planeDistance;
        if (debugMode == 4)
        {
            if (any(position < gridOrigin) || any(position > boundsMaximum)) return float4(Background, 1);
            float distance = VXGIMeshSDFDisplayDistance(position, distanceNodes);
            return float4(SliceColor(distance, cellSize), 1);
        }
        Texture3D<float> field = ResourceDescriptorHeap[globalSDFIndex];
        VXGIGlobalSDFSample sample = VXGIGlobalSDFSampleDistance(field, global, position);
        return float4(sample.valid ? SliceColor(sample.distance, sample.cellSize) : Background, 1);
    }

    if (debugMode <= 7 || debugMode == 17)
    {
        VXGISDFHit hit;
        bool meshHit = TraceMesh(rayOrigin, rayDirection,
            debugMode == 17 ? meshTraceDistance : maxRayDistance, hit);
        if (meshHit)
        {
            if (debugMode == 5 || debugMode == 17) return float4(PositionColor(hit.position), 1);
            if (debugMode == 6) return float4(hit.geometricNormal * 0.5 + 0.5, 1);
            VXGISDFHit blocker;
            // Use a normal-offset launch for this visibility diagnostic so a
            // neighboring coplanar triangle is not mistaken for a blocker.
            bool occluded = VXGISDFTrace(hit.position + hit.geometricNormal * rayMinDistance,
                sunDirectionTowardLight, 0.0, maxRayDistance, MeshGrid(),
                distanceNodes, cellOffsets, triangleReferences,
                triangles, materials, materialCount, materialSampler, materialMipLevel, blocker);
            if (blocker.traceStatus >= 2) return float4(InvalidTrace, 1);
            return float4(occluded ? float3(0.24, 0.035, 0.025) : float3(0.3, 0.9, 0.48), 1);
        }
        if (hit.traceStatus >= 2) return float4(InvalidTrace, 1);
        if (debugMode != 17) return float4(Background, 1);
    }

    Texture3D<float> field = ResourceDescriptorHeap[globalSDFIndex];
    VXGIGlobalSDFTraceResult globalHit = VXGIGlobalSDFTrace(field, global, rayOrigin, rayDirection,
        debugMode == 17 ? meshTraceDistance : 0.0, globalTraceDistance, globalMaxSteps);
    if (globalHit.status != VXGI_GLOBAL_SDF_HIT)
        return float4(globalHit.status >= VXGI_GLOBAL_SDF_INVALID ? InvalidTrace : Background, 1);
    if (debugMode == 14 || debugMode == 17) return float4(PositionColor(globalHit.position), 1);
    float3 normal = GlobalNormal(field, globalHit, rayDirection);
    if (debugMode == 15) return float4(normal * 0.5 + 0.5, 1);
    return float4(Background, 1);
}
