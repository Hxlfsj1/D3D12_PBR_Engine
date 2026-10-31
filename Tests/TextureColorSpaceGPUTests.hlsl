Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
struct SampleOutput { float4 midpoint; float4 mip; float4 firstMip; float4 secondMip; };
RWStructuredBuffer<SampleOutput> Outputs : register(u0);
cbuffer TestConstants : register(b0) { uint OutputIndex; uint LegacyDecode; uint LastMip; }

[numthreads(1, 1, 1)]
void CSMain()
{
    SampleOutput result;
    result.midpoint = Source.SampleLevel(LinearClamp, float2(.5, .5), 0);
    result.mip = Source.SampleLevel(LinearClamp, float2(.5, .5), LastMip);
    result.firstMip = Source.SampleLevel(LinearClamp, float2(.5, .5), 1);
    result.secondMip = Source.SampleLevel(LinearClamp, float2(.5, .5), 2);
    // Negative control reproduces the old production ordering: UNORM filtering
    // followed by a shader decode. It must differ from the linear .5 average.
    if (LegacyDecode != 0)
    {
        result.midpoint.rgb = pow(result.midpoint.rgb, 2.2);
        result.mip.rgb = pow(result.mip.rgb, 2.2);
        result.firstMip.rgb = pow(result.firstMip.rgb, 2.2);
        result.secondMip.rgb = pow(result.secondMip.rgb, 2.2);
    }
    Outputs[OutputIndex] = result;
}
