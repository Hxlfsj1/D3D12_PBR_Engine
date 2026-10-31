// Import the actual production G-buffer MaterialData declaration and ORM helper.
// Unused raster entry points/resources are removed from this compute shader.
#include "../Shaders/Shaders_For_GBuffer.hlsl"

struct FactorOutput
{
    float4 orm;
    MaterialData material;
};

StructuredBuffer<float4> ORMSamples : register(t0);
RWStructuredBuffer<FactorOutput> Outputs : register(u0);
cbuffer TestConstants : register(b2) { uint CaseCount; }

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CaseCount) return;
    MaterialData material = gMaterialData[id.x];
    FactorOutput result;
    result.orm = float4(ComposeMaterialORM(ORMSamples[material.ormIdx].rgb,
        ORMSamples[material.occlusionIdx].r, material.metallicFactor,
        material.roughnessFactor, material.occlusionStrength), 0);
    result.material = material;
    Outputs[id.x] = result;
}
