cbuffer ImageInfo : register(b0)
{
    uint width;
    uint height;
};

Texture2D<float4> hdrTexture : register(t0);

RWStructuredBuffer<float4> finalSHOutput : register(u0);

#define THREAD_COUNT 256

// Allocate a high-speed shared workspace in the GPU's local memory (L1 speed).
// Each thread gets its own slot [THREAD_COUNT] to safely store local sums without data races.
groupshared float3 sharedSH[9][THREAD_COUNT];
// Store the sum of weights. This corrects the mapping distortion where pole pixels in the HDR map take up less physical area on a sphere.
groupshared float sharedWeight[THREAD_COUNT];

static const float PI = 3.14159265f;

void ComputeBasis(float3 dir, out float Y[9])
{
    float3 n = normalize(dir);
    float x = n.x;
    float y = n.y;
    float z = n.z;

    Y[0] = 0.282095f;
    Y[1] = 0.488603f * y;
    Y[2] = 0.488603f * z;
    Y[3] = 0.488603f * x;
    Y[4] = 1.092548f * x * y;
    Y[5] = 1.092548f * y * z;
    Y[6] = 0.315392f * (3.0f * z * z - 1.0f);
    Y[7] = 1.092548f * x * z;
    Y[8] = 0.546274f * (x * x - y * y);
}

// The GPU dispatch pattern is set to 256 cores per group, arranged in a 16x16 matrix
[numthreads(16, 16, 1)]
// This shader is different—it operates on a per-GPU-core (per-thread) basis
void CSMain(uint3 GTid : SV_GroupThreadID)
{
    uint threadIndex = GTid.y * 16 + GTid.x;
    uint totalPixels = width * height;

    float3 localSH[9] = { (float3) 0, (float3) 0, (float3) 0, (float3) 0, (float3) 0, (float3) 0, (float3) 0, (float3) 0, (float3) 0 };
    float localWeight = 0.0f;

    // Grid-stepping (in chunks of 256) within the main loop
    for (uint i = threadIndex; i < totalPixels; i += THREAD_COUNT)
    {
        uint x = i % width;
        uint y = i / width;

        float v = (y + 0.5f) / float(height);
        float u = (x + 0.5f) / float(width);

        // Map 2D coordinates to 3D spherical angles (Theta & Phi)
        float theta = v * PI;
        float phi = (u - 0.5f) * 2.0f * PI;

        // Calculate the 3D physical direction and its corresponding 'solid angle' weight
        float3 dir = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
        float weight = sin(theta) * (2.0f * PI / float(width)) * (PI / float(height));

        // Calculate the infinitesimal contribution of a single pixel to the overall Spherical Harmonic (SH) coefficients
        float Y[9];
        ComputeBasis(dir, Y);

        float3 color = hdrTexture.Load(int3(x, y, 0)).rgb;

        for (int k = 0; k < 9; ++k)
        {
            localSH[k] += color * Y[k] * weight;
        }
        localWeight += weight;
    }

    // Switch storage location to groupshared memory (still running within a single thread)
    for (int k = 0; k < 9; ++k)
    {
        sharedSH[k][threadIndex] = localSH[k];
    }
    sharedWeight[threadIndex] = localWeight;

    // Initiate cross-thread operations
    GroupMemoryBarrierWithGroupSync();
    // Use parallel reduction to accumulate the values calculated by all threads (s >>= 1 equals s /= 2) 
    for (uint s = THREAD_COUNT / 2; s > 0; s >>= 1)
    {
        if (threadIndex < s)
        {
            for (int k = 0; k < 9; ++k)
            {
                sharedSH[k][threadIndex] += sharedSH[k][threadIndex + s];
            }
            sharedWeight[threadIndex] += sharedWeight[threadIndex + s];
        }
        
        GroupMemoryBarrierWithGroupSync();
    }

    // Process the reduction result (stored in index 0) and commit it to the final output
    if (threadIndex == 0)
    {
        float totalW = sharedWeight[0];
        float normalization = (4.0f * PI) / totalW;

        const float FinalMultipliers[9] =
        {
            0.282095f,
            0.325735f, 0.325735f, 0.325735f,
            0.273137f, 0.273137f,
            0.078848f,
            0.273137f,
            0.136569f
        };

        for (int i = 0; i < 9; ++i)
        {
            float3 finalVal = sharedSH[i][0] * normalization * FinalMultipliers[i];
            finalSHOutput[i] = float4(finalVal, 0.0f);
        }
    }
}