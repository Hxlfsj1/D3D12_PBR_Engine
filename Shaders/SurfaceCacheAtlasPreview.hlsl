#include "FullscreenTriangle.hlsli"
cbuffer PreviewConstants : register(b0)
{
    uint albedoIndex,normalIndex,emissiveIndex,depthIndex;
    uint mode,outputWidth,outputHeight,atlasWidth;
    uint atlasHeight,lightingIndex; uint2 previewPadding;
};
float4 VSMain(uint id:SV_VertexID):SV_POSITION
{
    return GetFullscreenTrianglePosition(GetFullscreenTriangleTexCoord(id));
}
float4 PSMain(float4 position:SV_POSITION):SV_Target0
{
    const float3 background=float3(0.018,0.022,0.03);
    float scale=min(float(outputWidth)/atlasWidth,float(outputHeight)/atlasHeight);
    float2 origin=(float2(outputWidth,outputHeight)-float2(atlasWidth,atlasHeight)*scale)*0.5;
    float2 p=(position.xy-origin)/scale;
    if(any(p<0)||p.x>=atlasWidth||p.y>=atlasHeight)return float4(background,1);
    uint2 texel=uint2(p);
    if(mode==29 || mode==30)
    {
        Texture2D<float4> lightingTexture=ResourceDescriptorHeap[lightingIndex];
        float4 lighting=lightingTexture.Load(int3(texel,0));
        if(mode==30)return float4(lighting.aaa,1);
        return float4(pow(max(lighting.rgb,0)/(1+max(lighting.rgb,0)),1.0/2.2),1);
    }
    Texture2D<float> depthTexture=ResourceDescriptorHeap[depthIndex];
    float depth=depthTexture.Load(int3(texel,0));
    if(depth>=1)return float4(background,1);
    Texture2D<float4> albedoTexture=ResourceDescriptorHeap[albedoIndex];
    Texture2D<float4> normalTexture=ResourceDescriptorHeap[normalIndex];
    Texture2D<float4> emissiveTexture=ResourceDescriptorHeap[emissiveIndex];
    float4 albedo=albedoTexture.Load(int3(texel,0));
    float4 normal=normalTexture.Load(int3(texel,0));
    float4 emissive=emissiveTexture.Load(int3(texel,0));
    if(mode==20)return float4(pow(max(albedo.rgb,0),1.0/2.2),1);
    if(mode==21)return float4(normal.xyz*0.5+0.5,1);
    if(mode==22)return float4(pow(max(emissive.rgb,0)/(1+max(emissive.rgb,0)),1.0/2.2),1);
    if(mode==23)return float4((1-depth).xxx,1);
    if(mode==24)return float4(albedo.aaa,1);
    return float4(normal.aaa,1);
}
