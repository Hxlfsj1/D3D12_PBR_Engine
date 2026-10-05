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
5. Independent temporal history uses geometry-checked reprojection, neighborhood variance
   clipping and a maximum history length of 16. Clipped history adapts with a shorter weight.
6. Bilateral upsampling applies receiver albedo, diffuse energy response and SSGI intensity,
   then adds the result to HDR scene lighting before sky/transparency and post-processing.

Misses return zero; black geometry still terminates rays. RGB stores E/pi and averages
all rays, including misses. Hit fraction is not used to renormalize lighting. Temporal
alpha stores history length. Thickness 0.15 and normal bias 0.02 remain named constants
at the trace call site. Ceil-sized reductions use physical pixel cell coordinates so
odd image sizes remain aligned. Different ray mips use nonuniform descriptor indexing.

Each bindless pass binds the intended descriptor heap before its root signature and
arguments. SSGI history remains independent of HBAO and anti-aliasing.

## Limits

Single-layer screen information cannot represent off-screen or hidden surfaces. Small
bright sources remain difficult to sample. Iteration exhaustion returns a miss; no sky
fallback is added. Temporal reprojection currently uses camera matrices rather than
per-object motion vectors. Coarse-cell depth ranges and the seven-level hierarchy can
limit traversal efficiency in complex views.

## Diagnostic cleanup

Temporary GPU counters, color probes, readback buffers, CSV writers, pure-GI preview
branches and local SSGI test tools were removed after validation. No diagnostic UAVs or
readback passes are attached to the production graph. Normal rendering error reporting
remains available. Before removing the tools, GPU regression checks passed for trace
normalization, odd sizes, black hits, finite radii, and the emission cap/visible-color
separation. Debug and Release builds are checked during cleanup.
