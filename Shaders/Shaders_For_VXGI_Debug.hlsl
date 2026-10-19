// Stage 1 occupancy/probe diagnostics. Centers are per-instance input data;
// fixed line geometry is generated in the vertex shader, with no geometry cap.
cbuffer DebugConstants : register(b0)
{
    float4x4 worldViewProjection;
    float voxelSize;
    uint drawProbes;
};

struct DebugOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

DebugOutput VSMain(float3 center : POSITION, uint vertexId : SV_VertexID)
{
    DebugOutput output;
    float3 offset;
    if (drawProbes != 0)
    {
        // Fixed cross size relative to the voxel, independent of the camera.
        const uint axis = vertexId / 2;
        offset = 0.0;
        offset[axis] = ((vertexId & 1) != 0 ? 1.0 : -1.0) * voxelSize * 0.18;
        output.color = float3(1.0, 0.75, 0.12);
    }
    else
    {
        // Eight corner bit indices and the twelve exact cell edges.
        static const uint edges[24] = {
            0, 1, 2, 3, 4, 5, 6, 7,
            0, 2, 1, 3, 4, 6, 5, 7,
            0, 4, 1, 5, 2, 6, 3, 7
        };
        const uint corner = edges[vertexId];
        offset = (float3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1) - 0.5) * voxelSize;
        output.color = float3(0.1, 0.65, 1.0);
    }
    output.position = mul(float4(center + offset, 1.0), worldViewProjection);
    return output;
}

float4 PSMain(DebugOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
