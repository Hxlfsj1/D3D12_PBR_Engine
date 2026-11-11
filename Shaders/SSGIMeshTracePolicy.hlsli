#ifndef SSGI_MESH_TRACE_POLICY_HLSLI
#define SSGI_MESH_TRACE_POLICY_HLSLI

// Shared C++/HLSL build switch. Confirm SDF tolerance hits against triangles
// before using their endpoints, with the existing FP32 path and geometry bindings.
// The rejected blind-interval experiment remains available by setting this to 0
// and explicitly configuring a nonzero mesh_min_hit_distance.
#define SSGI_MESH_ORIGIN_CONFIRMATION_ENABLED 1

#endif
