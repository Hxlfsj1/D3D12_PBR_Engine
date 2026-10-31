static const uint INVALID_MATERIAL_ID = 0xFFFFFFFF;

uint ResolveDepthMaterialID(uint instanceID, uint meshMaterialID)
{
    uint instanceMaterialID = gInstanceData[instanceID].customMaterialID;
    return (instanceMaterialID != INVALID_MATERIAL_ID) ? instanceMaterialID : meshMaterialID;
}

float4 SampleDepthAlbedo(
    uint resolvedMaterialID,
    float2 texCoord,
    float materialMipBias)
{
    Texture2D tAlbedo = ResourceDescriptorHeap[gMaterialData[resolvedMaterialID].albedoIdx];
    float4 albedo = tAlbedo.SampleBias(s1, texCoord, materialMipBias);
    albedo.a *= gMaterialData[resolvedMaterialID].baseColorFactor.a;
    return albedo;
}

void ApplyDepthAlphaTest(float alpha, uint resolvedMaterialID)
{
#ifdef ALPHA_TEST
    clip(alpha - gMaterialData[resolvedMaterialID].alphaCutoff);
#endif
}

void ClipByDepthVisibility(
    uint instanceID,
    uint meshMaterialID,
    float2 texCoord,
    float materialMipBias)
{
#ifdef ALPHA_TEST
    uint resolvedMaterialID = ResolveDepthMaterialID(instanceID, meshMaterialID);
    float alpha = SampleDepthAlbedo(
        resolvedMaterialID,
        texCoord,
        materialMipBias).a;
    ApplyDepthAlphaTest(alpha, resolvedMaterialID);
#endif
}
