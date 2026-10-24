#include "VXGIGlobalSDFCommon.hlsli"
#include "VXGIMeshSDFData.hlsli"
#include "VXGISDFThresholdCommon.hlsli"

ConstantBuffer<VXGIGlobalSDFConstants> GlobalSDFConstants : register(b0);
ConstantBuffer<VXGIGlobalSDFUpdateRegion> GlobalSDFRegion : register(b1);
StructuredBuffer<float> GlobalSDFMeshNodes : register(t0);
RWTexture3D<float> GlobalSDFOutput : register(u0);

float VXGIGlobalSDFMeshDistanceLowerBound(float3 worldPosition, VXGIMeshSDFInstance instance,
    VXGIMeshSDFAsset asset, float distanceLimit)
{
    VXGIThresholdGrid grid;
    grid.origin = asset.origin;
    grid.cellSize = asset.cellSize;
    grid.cellDimensions = asset.cellDimensions;
    grid.nodeOffset = asset.nodeOffset;
    grid.boundsMaximum = asset.boundsMaximum;
    grid.padding = 0;
    if (!VXGIThresholdValidGrid(grid, GlobalSDFConstants.nodeCount)) return 0.0;
    if (!all(isfinite(instance.boundsMinimum)) || !all(isfinite(instance.boundsMaximum)) ||
        any(instance.boundsMinimum > instance.boundsMaximum)) return 0.0;
    float worldBoxDistance = length(worldPosition -
        clamp(worldPosition, instance.boundsMinimum, instance.boundsMaximum));
    if (!isfinite(worldBoxDistance)) return 0.0;
    // This independent world-space lower bound localizes each object's
    // influence even under strongly nonuniform scale. Beyond the narrow band
    // its contribution is exactly the finite saturation value.
    if (worldBoxDistance >= distanceLimit) return distanceLimit;
    float3 localPosition = mul(float4(worldPosition, 1.0), instance.localFromWorld).xyz;
    if (!all(isfinite(localPosition))) return 0.0;
    float3 clamped = clamp(localPosition, grid.origin, grid.boundsMaximum);
    VXGIThresholdSample sample = VXGIThresholdSampleUnchecked(GlobalSDFMeshNodes, grid, clamped);
    if (!sample.valid) return 0.0;
    float roundoff = max(asset.cellSize * 1e-4, abs(sample.distance) * 2e-6);
    float localLower = max(0.0, sample.distance -
        VXGI_GLOBAL_SDF_INTERPOLATION_ERROR * asset.cellSize - roundoff);
    // Distance to the grid box is a lower bound because it encloses the mesh.
    // Extending the sampled boundary value with its Lipschitz cone preserves
    // another lower bound; their maximum is substantially tighter far away.
    float outsideDistance = length(localPosition - clamped);
    if (!isfinite(outsideDistance)) return 0.0;
    localLower = max(outsideDistance, localLower - outsideDistance);
    // For the scene's TRS transforms these row lengths are abs(scale.xyz).
    // The minimum scale preserves a lower bound under nonuniform/negative scale.
    float scale = min(length(instance.worldFromLocal[0].xyz),
        min(length(instance.worldFromLocal[1].xyz), length(instance.worldFromLocal[2].xyz)));
    if (!isfinite(scale) || scale <= 0.0) return 0.0;
    float worldLower = max(worldBoxDistance, localLower * scale);
    return isfinite(worldLower) && worldLower >= 0.0 ? worldLower : 0.0;
}

[numthreads(4, 4, 4)]
void CSCompose(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint resolution = GlobalSDFConstants.resolution;
    if (!VXGIGlobalSDFValidConstants(GlobalSDFConstants) ||
        any(dispatchThreadId >= GlobalSDFRegion.size)) return;
    uint clipmapIndex;
    uint3 localNode;
    if (GlobalSDFRegion.clipmapIndex == 0xffffffffu)
    {
        // The full-build entry uses the same PSO as incremental boxes.
        uint3 atlasNode = dispatchThreadId + GlobalSDFRegion.minimum;
        uint outer = GlobalSDFConstants.clipmapCount - 1;
        uint depth = GlobalSDFConstants.clipmaps[outer].nodeOffsetZ +
            GlobalSDFConstants.clipmapResolutions[outer];
        if (atlasNode.x >= resolution || atlasNode.y >= resolution || atlasNode.z >= depth) return;
        clipmapIndex = outer;
        [unroll] for (uint index = 0; index < VXGI_GLOBAL_SDF_MAX_CLIPMAPS; ++index)
        {
            if (index >= GlobalSDFConstants.clipmapCount) break;
            if (atlasNode.z < GlobalSDFConstants.clipmaps[index].nodeOffsetZ +
                GlobalSDFConstants.clipmapResolutions[index])
            {
                clipmapIndex = index;
                break;
            }
        }
        localNode = uint3(atlasNode.xy, atlasNode.z - GlobalSDFConstants.clipmaps[clipmapIndex].nodeOffsetZ);
        // Smaller layers leave unused atlas rows/columns. Define their values
        // for readback tools; sampling always stays inside each layer's lattice.
        if (any(localNode >= GlobalSDFConstants.clipmapResolutions[clipmapIndex]))
        {
            GlobalSDFOutput[atlasNode] = 0.0;
            return;
        }
    }
    else
    {
        clipmapIndex = GlobalSDFRegion.clipmapIndex;
        if (clipmapIndex >= GlobalSDFConstants.clipmapCount) return;
        localNode = dispatchThreadId + GlobalSDFRegion.minimum;
        if (any(localNode >= GlobalSDFConstants.clipmapResolutions[clipmapIndex])) return;
    }
    VXGIGlobalSDFClipmap clipmap = GlobalSDFConstants.clipmaps[clipmapIndex];
    float3 worldPosition = clipmap.origin + float3(localNode) * clipmap.cellSize;
    // Clamping to a local narrow band lets unchanged distant texels survive
    // object motion: only the old/new world bounds expanded by this band can
    // have values that differ from the saturation value.
    float distanceLower = clipmap.distanceLimit;
    [loop] for (uint index = 0; index < GlobalSDFConstants.instanceCount; ++index)
    {
        VXGIMeshSDFInstance instance = VXGIMeshSDFInstances[index];
        if (instance.assetIndex >= GlobalSDFConstants.assetCount)
        {
            distanceLower = 0.0;
            break;
        }
        VXGIMeshSDFAsset asset = VXGIMeshSDFAssets[instance.assetIndex];
        distanceLower = min(distanceLower,
            VXGIGlobalSDFMeshDistanceLowerBound(worldPosition, instance, asset, clipmap.distanceLimit));
    }
    GlobalSDFOutput[VXGIGlobalSDFPhysicalNode(GlobalSDFConstants, clipmapIndex, localNode)] = distanceLower;
}
