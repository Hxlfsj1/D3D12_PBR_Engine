# SSGI

Independent half-resolution screen-space diffuse GI for the deferred pipeline.

## Settings

Settings are nested in `Settings/Pipeline.json` and read at startup:

```json
"ssgi": {
  "enabled": true,
  "intensity": 1.0,
  "quality": 4,
  "radius": 2.0
}
```

- `enabled`: the pipeline switch. `false` skips SSGI initialization, history allocation,
  BounceSource and every SSGI pass. `true` enables the chain in the deferred pipeline.
  Change the JSON value and restart to apply it; no debug hotkey overrides this setting.
- `intensity`: nonnegative multiplier applied once during final HDR composition.
- `quality`: 1..4 selects 4/8/16/32 rays with 64/64/64/96 maximum traversal iterations.
- `radius`: positive finite ray distance in scene world units.

Ray counts follow UE SSGI; traversal budgets are project choices. Settings are not
modified by diagnostics. SSGI runs at half the internal scene resolution, including
when DLSS reduces that resolution further.

## Rendering

1. Deferred lighting writes `SSGI.BounceSource`: outgoing direct diffuse plus emission.
   Only GI-source emission is capped to a peak RGB component of **2**, scaling RGB
   uniformly to preserve color ratios. Visible emission and direct diffuse remain
   unchanged. This is an intentional energy/stability tradeoff, not a physical constant.
   SH, environment specular, AO and previous GI are excluded from BounceSource.
2. Up to seven independent RG32_FLOAT depth reductions store nearest/furthest positive
   view-Z bounds. Empty cells use `(1e20, 0)`. Only depth is reduced; trace hit color
   comes directly from full-resolution BounceSource, with no radiance pyramid.
   Original depth is mip 0; reduced mips 1..7 cover 2x2 through 128x128 source pixels
   per cell. Reduction stops early if the texture reaches 1x1.
3. Cosine-weighted hemisphere rays traverse Hi-Z grid cells. Start at full resolution
   and leave the origin cell. Intersect the complete in-cell ray segment with the
   conservative depth interval; skip disjoint cells and coarsen, refine potential hits.
   At full resolution, verify radius and facing before reading source color. This
   follows the stackless traversal structure in UE's `LumenScreenTracing.ush`, adapted
   to this project's min/max depth hierarchy and ordinary device Z.
   Ray directions use NVIDIA vector-2 spatiotemporal blue noise (128x128x64 RG8),
   indexed on the half-resolution receiver grid and advanced one slice per frame.
   Multiple rays use fixed R2 spatial offsets. Raw data, conversion and licensing
   live in `ThirdParty/NVIDIA/STBN`; the shared loader uploads it once on SSGI startup.
4. A 5x5 binomial spatial reconstruction uses normal and symmetric tangent-plane weights.
5. Independent temporal history uses geometry-checked reprojection and two accumulation
   speeds. Previous bilinear taps are checked against normal agreement and symmetric
   tangent-plane distance. No Euclidean `3 * footprint` cutoff is applied: it rejected
   valid coplanar history at grazing angles. Plane tolerance and history weights are
   unchanged. Slow history uses a 16-sample cap; fast history uses NRD's six-previous-frame
   parameter (steady-state weight 1/7). A separate pass uses RELAX's 5x5 fast-history
   YCoCg clamp, bounded update acceleration and noise-aware reset. No single-frame RGB
   threshold shortens history to three samples. See `ThirdParty/NVIDIA/NRD/README.md`
   for the pinned source, license, port scope and validation.
6. Two half-resolution variance-guided A-trous passes (steps 1 and 2) filter the clamped
   temporal signal. Luminance second moments use the slow accumulation weight and are
   corrected when clamping changes its color. Depth/normal weights preserve geometry
   boundaries; history shorter than three samples uses a 5x5 variance estimate. Only
   the composite consumes spatial output, so blur does not accumulate in history.
   This adds two compute dispatches, not another depth hierarchy or additional rays.
7. Bilateral upsampling applies receiver albedo, diffuse energy response and SSGI intensity,
   then adds the result to HDR scene lighting before sky/transparency and post-processing.

Misses return zero; black geometry still terminates rays. RGB stores E/pi and averages
all rays, including misses. Hit fraction is not used to renormalize lighting. Temporal
alpha stores history length; fast-history alpha stores the slow luminance second moment,
and spatial-output alpha stores variance. Thickness 0.15 and normal bias 0.02 remain named constants
at the trace call site. Ceil-sized reductions use physical pixel cell coordinates so
odd image sizes remain aligned. Different ray mips use nonuniform descriptor indexing.

Each bindless pass binds the intended descriptor heap before its root signature and
arguments. SSGI history remains independent of HBAO and anti-aliasing.

## C++ organization

`Passes/SSGIPass.h` owns the GPU constant layouts and one `AddToGraph` entry point.
Algorithm parameters are assigned at their call sites, not duplicated as layout
member defaults. Shared depth/normal/ORM/albedo views are created once per graph;
each stage still declares its own RDG read/write dependencies. All transient UAV
textures use `CreateUAVTexture`; the dedicated depth SRV remains R32_FLOAT.

`Graphics/PipelineManager.h` owns SSGI PSOs and its root signature, using the existing
`ConstantBufferBinding`. Initialization remains conditional on deferred SSGI being
enabled. `RenderFormats` in `Passes/RenderStructs.h` is the shared format contract;
`PipelineManager::Formats` aliases it for existing consumers.

`ResourceManager` owns SSGI history textures and their ping-pong index, like HBAO/TAA.
`SSGIHistoryState` contains only CPU metadata. Application history invalidation and
post-submission commit use common entry points. Histories advance only if written
by the submitted frame; failed/unsubmitted frames do not advance HBAO/TAA either.
SSGI retains its own jittered matrices, camera-cut thresholds and independent
validity. Its matrices must not be replaced by the unjittered motion-vector history.

## Shader organization

The Shaders directory contains three SSGI shader source files:
- `Shaders_For_SSGI.hlsl`: Pyramid, Trace and Composite.
- `Shaders_For_SSGI_Spatial.hlsl`: Reconstruct and Atrous.
- `Shaders_For_SSGI_Temporal.hlsl`: Temporal and HistoryClamp.

Compilation selects a stage using `SSGI_PASS_<STAGE>` and its explicit entry point
(`CSMain_<Stage>`, or `VSMain`/`PSMain` for Composite). Atrous retains the
`SSGI_ATROUS_FIRST` variant. Stage-specific declarations are isolated at compile
time; GPU passes, dispatches and constant layouts are unchanged.

`SSGICommon.hlsli` is the fourth SSGI-specific file and contains view-normal decoding,
constant-depth pixel footprint, symmetric plane distance and the shared
Reconstruct/Composite bilateral weighting formula. Position reconstruction lives in
`GeometryCommon.hlsli`, shared with HBAO, Deferred and temporal reconstruction.
The supplied inverse matrix determines whether positions are in view or world space.
`ColorCommon.hlsli` supplies RGB/YCoCg conversion for RELAX and TAA/TSR; it introduces
no samplers or history state. Callers retain their homogeneous-w validity checks.
Each pass retains its own tolerance, rejection thresholds and spatial kernel;
NRD A-trous weights and temporal accumulation rules remain separate.
Normal decoding uses the shared `MaterialCommon.hlsli` contract, as HBAO does.
Each stage has a named `SSGI<Stage>Constants` buffer, with field order/layout matching
`Passes/SSGIPass.h`. The two production NRD helper includes remain separately
under `ThirdParty/NVIDIA/NRD`, with their licenses and source references.

## Limits

Single-layer screen information cannot represent off-screen or hidden surfaces. Small
bright sources remain difficult to sample. Iteration exhaustion returns a miss; no sky
fallback is added. Temporal reprojection currently uses camera matrices rather than
per-object motion vectors. Coarse-cell depth ranges and the seven-level hierarchy can
limit traversal efficiency in complex views.

Known traversal precision issue: `RayCell` still uses a fixed 0.0001-pixel address
offset. At sufficiently large FP32 pixel coordinates (for example 2049), the offset
can round away, revisiting a boundary until the iteration budget is exhausted.
This remains separate from the fixed grazing-angle temporal rejection; the cleanup
does not change traversal or the scene-tested filtering settings.

## Diagnostic cleanup

Temporary GPU counters, color probes, readback buffers, CSV writers, pure-GI preview
branches and local SSGI test tools were removed after validation. No diagnostic UAVs or
readback passes are attached to the production graph. Normal rendering error reporting
remains available. Before removing the tools, GPU regression checks passed for trace
normalization, odd sizes, black hits, finite radii, and the emission cap/visible-color
separation. Debug and Release builds are checked during cleanup.

The final cleanup also removes the unused `tmp/nrd-reference` comparison sources.
Production NRD/STBN helpers, assets and licenses remain.
The T-key spatial-filter comparison switch is gone; both A-trous passes are enabled
whenever the configured SSGI pipeline runs. No test-only shader variants remain.

## Completed static-camera investigation

The temporary A/B/C capture and its CSV/text reports have been removed. This
investigation used the old algorithm and does not measure the new RELAX-based path.
In the completed 256-frame quality-1 capture, disabling the old brightness clamp and its history reduction lowered
fixed-screen frame-difference RMS by about 86%, without darkening the output.
This motivated the fast/slow history replacement. No automatic probe readback or
extra comparison histories run in the new pipeline.
