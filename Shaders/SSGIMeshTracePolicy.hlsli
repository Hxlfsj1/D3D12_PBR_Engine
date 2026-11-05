#ifndef SSGI_MESH_TRACE_POLICY_HLSLI
#define SSGI_MESH_TRACE_POLICY_HLSLI

// Shared C++/HLSL build switch. Confirm near-origin tolerance hits against
// triangles, using the existing FP32 runtime path and its host geometry bindings.
// The rejected blind-interval experiment remains available by setting this to 0
// and explicitly configuring a nonzero mesh_min_hit_distance.
#define SSGI_MESH_ORIGIN_CONFIRMATION_ENABLED 1

#endif
