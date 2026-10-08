/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
// This software contains source code provided by NVIDIA Corporation.
// Adapted diffuse-only RELAX history clamping. See README.md for source and scope.

#ifndef SSGI_RELAX_DIFFUSE
#define SSGI_RELAX_DIFFUSE

#include "../../../Shaders/ColorCommon.hlsli"

// Luminance from NVIDIA MathLib v11 (MathLib-LICENSE.txt).
float RelaxLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Means/moments come from the 5x5 neighborhood of fast history and noisy input.
// This keeps RELAX's diffuse clamp, acceleration limit and noise-aware reset equations.
void RelaxClampDiffuse(float3 slow, inout float3 fast, float3 noisy, float historyLength,
    float3 fastMean, float3 fastMoment2, float3 noisyMean, float noisyMoment2,
    float sigmaScale, float accelerationAmount, float resetAmount,
    float spatialSigmaScale, float temporalSigmaScale, out float3 result)
{
    float3 sigma = sqrt(max(0, fastMoment2 - fastMean * fastMean));
    float3 fastYCoCg = RGBToYCoCg(fast);
    float3 low = min(fastMean - sigmaScale * sigma, fastYCoCg);
    float3 high = max(fastMean + sigmaScale * sigma, fastYCoCg);
    float3 slowYCoCg = RGBToYCoCg(slow);
    float3 clipped = clamp(slowYCoCg, low, high);
    result = YCoCgToRGB(clipped);

    // NRD's short-history threshold is 3. There is no HistoryFix pass in this port.
    // The two histories accumulate identically at startup; preserve fast history here.
    if (historyLength <= 3) result = fast;
    float clampFactor = clipped.x == slowYCoCg.x ? 0 : saturate((clipped.x - slowYCoCg.x) / (fastYCoCg.x - slowYCoCg.x));
    if (historyLength <= 3) clampFactor = 1;
    float difference = 10.0 * accelerationAmount * RelaxLuminance(abs(fast - slow)) * clampFactor;
    if (historyLength <= 3) difference = 0;
    float3 toNoisy = noisyMean - fast;
    float distance = RelaxLuminance(abs(toNoisy));
    float3 acceleration = distance == 0 ? 0 : toNoisy * difference / distance;
    float accelerationL = RelaxLuminance(abs(acceleration));
    float ratio = accelerationL == 0 ? 0 : distance / accelerationL;
    if (ratio < 1) acceleration *= ratio;
    if (ratio <= 0) acceleration = 0;
    result += acceleration;
    fast += acceleration;

    float slowL = RelaxLuminance(slow);
    float noisyL = RelaxLuminance(noisyMean);
    // These names/scales match NRD; both estimates here are gathered spatially.
    float noisySigma = temporalSigmaScale * sqrt(max(0, noisyMoment2 - noisyL * noisyL));
    float fastSigma = spatialSigmaScale * sigma.x;
    float reset = saturate(resetAmount * max(0, abs(slowL - noisyL) - fastSigma - noisySigma)
        / (1e-6 + max(slowL, noisyL) + fastSigma + noisySigma));
    result = lerp(result, noisy, reset);
    fast = lerp(fast, noisy, reset);
    // The caller corrects the second moment separately; slow GI alpha holds history length.
}
#endif
