// Editor utility only: unlit object IDs and a visible-surface outline.
#ifdef OBJECT_IDS
cbuffer ObjectConstants : register(b0)
{
    float4x4 worldViewProjection;
    uint objectId;
    uint materialId;
    float alphaCutoff; // Negative for opaque, 0.5 for cutout, 0.05 for transparent picking.
};

struct MaterialData
{
    uint albedoIdx, normalIdx, ormIdx, emissiveIdx;
    float4 baseColorFactor;
    uint isUnlit;
    uint3 padding;
};
StructuredBuffer<MaterialData> materials : register(t0);
SamplerState materialSampler : register(s0);

struct GeometryOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

GeometryOutput IdVS(float3 position : POSITION, float2 uv : TEXCOORD0)
{
    GeometryOutput output;
    output.position = mul(float4(position, 1.0), worldViewProjection);
    output.uv = uv;
    return output;
}

uint IdPS(GeometryOutput input) : SV_Target0
{
    if (alphaCutoff >= 0.0)
    {
        MaterialData material = materials[materialId];
        Texture2D<float4> albedo = ResourceDescriptorHeap[material.albedoIdx];
        clip(albedo.Sample(materialSampler, input.uv).a * material.baseColorFactor.a - alphaCutoff);
    }
    return objectId;
}
#else
Texture2D<uint> objectIds : register(t0);
cbuffer OutlineConstants : register(b0)
{
    uint selectedId;
    int radiusX, radiusY;
};

float4 OutlineVS(uint vertexId : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 OutlinePS(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (selectedId == 0 || objectIds.Load(int3(pixel, 0)) != selectedId)
        discard;

    uint width, height;
    objectIds.GetDimensions(width, height);
    bool boundary = false;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = clamp(pixel + int2(x * radiusX, y * radiusY),
                int2(0, 0), int2(width - 1, height - 1));
            boundary = boundary || objectIds.Load(int3(samplePixel, 0)) != selectedId;
        }
    }
    if (!boundary)
        discard;
    // Inner contour: never paint through an occluding object, no fill/tint.
    return float4(1.0, 0.55, 0.05, 1.0);
}
#endif
