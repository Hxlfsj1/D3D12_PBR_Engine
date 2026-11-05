# SSGI

## Active screen-probe gather (2026-11-04)

`Settings/Pipeline.json` selects `ssgi.sampling_mode: "screen_probes"`.
`trace_downsample_factor` is the only primary receiver spacing: 8 means one
receiver in each 8x8 **internal render pixel** cell, including when DLSS is active.
There is no additional camera-distance 2x2/4x4 sharing in this path.

```json
"ssgi": {
  "enabled": true,
  "sampling_mode": "screen_probes",
  "trace_downsample_factor": 8,
  "screen_probes": { "rays_per_probe": 128, "history_updates": 4 },
  "world_sampling": { "mesh_min_hit_distance": 0.0 },
  "intensity": 1
}
```

Each fresh receiver traces the complete 128-ray budget across an 8x8
equal-solid-angle hemisphere. Screen tracing, near Mesh SDF and far Global SDF
continue the same directions instead of allocating independent ray budgets.
Geometry/parallax-validated probes can retain their previous result on alternate
checkerboard frames; new/disoccluded probes trace immediately. Thus 128 is the
budget per fresh update, not a promise of 128 new rays at every point every frame.

Directional Li is reprojected and accumulated before a plane/normal/hit-distance
guided 5x5 binomial filter (probe step 2). Spatially filtered values never feed
the directional history. Irradiance SH2 is then evaluated with **each full-resolution
pixel's normal**, followed by full-resolution temporal history and two Atrous passes.
History-clamp statistics use the probe spacing rather than adjacent correlated
pixels. The final slow-history cap remains 16; it was not extended to hide noise.

Settings take effect after restart. Rays must be >=64 and divisible by 64;
history_updates accepts 1..32. There is no new arbitrary ray ceiling, but GPU time,
memory and the D3D12 atlas dimension limit still apply. Existing world radiance
probes, SDF generation, DLSS, HBAO and shadow configuration remain separate.

See [implementation, measurements and rollback](Docs/ScreenProbeRefactorReport.md).
The old path is retained under `sampling_mode: "legacy"`; the sections below
describe that path and earlier experiments, not the active screen-probe policy.

## Legacy path and historical experiments

World display reconstruction can additionally filter the separate Mesh/Global
contribution with a depth/normal-guided 5x5 binomial kernel:
`ssgi.world_sharing.spatial_filter: { "enabled": true, "step_width": 2 }`.
Supported GI-texel step widths are 1, 2 and 4. The loader defaults to disabled;
the project trial enables step 2. Disabling this block restores center-only world
addition, independently of `accumulation_updates`. Restart to apply changes.
The filter skips missing checkerboard display values instead of treating them
as black. It changes display/final pixel history, never the owner atlas feedback.
Geometry rejection preserves surface boundaries but cannot distinguish lighting
boundaries on the same plane; contact shadows and gradients can soften.

The measured trial on 2026-11-04 enables
`ssgi.world_sharing.accumulation_updates: 4`. A genuinely fresh Mesh/Global owner
batch is blended with the geometrically reprojected owner estimate before it is
shared with receivers. Reprojection/replicated slots do not increment the batch
count. The setting is independent of final pixel history; `1` restores the old
raw-batch replacement behavior. Supported history settings are integers 1–32;
only 4 and 8 were compared in this trial. Restart to apply changes.

The existing owner atlas is intentionally now a filtered owner history when
this setting exceeds 1. Its legacy internal `Raw` names are retained to preserve
the binding layout; capture metadata records `filtered_feedback` and the window.
Do not interpret those values as unfiltered Monte Carlo observations. Source
changes and settings changes invalidate history as before. This does not extend
the screen-space sample budget or final 16-sample slow-history cap.

Near-origin Mesh SDF tolerance hits again use FP32 triangle confirmation through
`SSGI_MESH_ORIGIN_CONFIRMATION_ENABLED = 1` in
`Shaders/SSGIMeshTracePolicy.hlsli`. The configured and loader-default
`ssgi.world_sampling.mesh_min_hit_distance` are both zero: there is no 0.09 m
blind interval. Screen-space tracing, probe baking and the far Global SDF query
are unchanged. The unsuccessful blind-interval experiment remains available in
code, but is disabled by the zero minimum distance. This restoration was included
in the subsequent 2026-11-04 builds and controlled denoising runs.

Mesh threshold queries and the Global SDF march now step by the full
`safeDistance`, without the additional 0.9 multiplier. Mesh interpolation uses
its position-dependent error bound (at most sqrt(3)/2 cells); Global sampling
subtracts sqrt(3)/2 cells. Both retain their roundoff allowance and finite-range
or clipmap-boundary clamping. The 0.9-cell HIT tolerance and the separate exact
triangle/baking traversal step policy are unchanged. This step-scale change was
included in the subsequent 2026-11-04 builds and controlled denoising runs.

An optional probe distance/plane cache experiment is available through
`vxgi.probes.distance_visibility` (default false, startup configuration).
It uses one bindless SRV without changing the eight-parameter root signature.
It has residual leaks and is not accepted for production; see
[measured results](Docs/VXGIProbeDistanceExperiment.md).

Diffuse GI for the deferred pipeline, using screen tracing and optional world-space fallback.

The current implementation applies camera-distance quality to Mesh SDF and Global
SDF contributions together. Screen-space SSGI retains its original receiver
positions, 0-1 m interval, member directions and refresh policy. The
retained behavior and final checks are recorded in
[the closeout](Docs/SSGIOptimizationCloseout.md).
[The camera-quality design record](Docs/SSGICameraQualityPlan.md) describes the
initial implementation; the policies below and current settings take precedence.
As of 2026-10-31, the fine/coarse unified interpolation experiment is removed;
native coarse reconstruction (policy 9) remains. Optimization experiments are
paused. Temporary experiment sources, reports and captures have been cleared.

## Camera quality and ray-distance bands

Camera distance is the Euclidean distance from the camera to the current GBuffer
receiver, computed from view position. It selects world sampling density only.
Ray path distance still controls screen/Mesh/Global coverage and first-hit
contribution. Pixel blocks below refer to the GI trace grid after scene-resolution
reduction; they do not move screen receiver origins.

| Contribution | Camera quality | Shared observations per estimate | Normal refresh interval |
|---|---|---|---|
| Screen SSGI, 0-1 m | Fixed 2x2 | One direction per valid member, up to four | Two frames |
| Mesh SDF, 0-2 m | Near: 2x2; far: 4x4 | Configurable near/far count; both default to eight | Two frames |
| Global SDF, (2,8] m | Near: 2x2; far: 4x4 | Configurable near/far count; both default to eight | Four frames |

The pre-screen-probe Pipeline.json set near/far counts to 32/128; the loader defaults remain
8/8. The checked-in near threshold is 2 m, with an exit threshold of 2.2 m. A
representative at distance <= 2 m requests fine 2x2 world sampling. Beyond 2.2 m
it requests coarse 4x4 sampling. In the intervening shell, a geometry-checked
reprojection inherits the previous surface's layout; without valid matching
history it starts coarse. The classification examines the first valid
representative of each 2x2 child. If any valid child requests fine sampling, its
4x4 parent uses four fine children. This promotes layout rather than invalidating
the parent; it does not restore the tile-wide geometry veto. Representative
classification can miss a thinner near surface elsewhere in a child; local
geometry checks and Repair retain their existing role.

For a configured count R, fine world estimates trace R/4 directions per valid
member, average those directions, then average by the valid member count. A
coarse 4x4 estimate selects one valid origin from each nonempty 2x2 quadrant using
STBN and traces R/4 directions there. Its quadrant contributes with weight Nq/N,
where Nq is that quadrant's valid member count and N is the whole parent's valid
member count. Thus a fully valid block uses R directions, while partial blocks
do not divide by sixteen or by the number of hits. Black hits and misses remain
zero observations. Selection follows the submitted frame's STBN slice; it is not
a new per-cache update counter.

### World ray counts and disabled guiding

The bright-probe guide is disabled. The main shader no longer includes or calls
its helper: there is no target search, direction cone, mixture PDF, role selection
or extra guide group memory. The unused standalone helper has been removed.
All active world directions use STBN cosine-hemisphere sampling. The user reported
no visible gain and worse performance for the top-1 trial. That route remains
removed; its temporary experiment files have been cleared.

Near and far refer to the camera-selected 2x2/4x4 quality layouts, not the Mesh and
Global ray-distance intervals. Both world backends use near_ray_count in the fine
layout and far_ray_count in the coarse layout. Each count defaults to 8 and must
be an integer from 4 to 64 divisible by 4. A full block at 8 uses two directions
at each of its original four sampling slots; empty members/strata do not add rays.
Point Repair uses the entire configured R for its receiver's quality. Screen
SSGI stays at its original four observations and has separate normalization.

The first matching direction still shares the screen/Mesh prefix; additional
world directions check screen visibility without adding screen radiance. Mesh
and Global share actual identical world prefixes. Geometry/noise handles and the
lazy world bias are prepared once per origin and reused across its ray loop.
The main budgets, bias and first-hit rules are otherwise unchanged. Changing
either configured count invalidates world-source history through the CPU source
key; screen-only mode ignores these world fields. Configurable ray counts do not
add a pass or group barrier, and normal cache periods remain 2/2/4 frames.
The removed Global 50/50 temporal average and A-trous phi=4 remain removed.

### Shared-owner raw cache for Mesh and Global

World sharing retains a packed ping-pong raw-owner atlas: Mesh on the left,
Global on the right. Separate textures hold RGB and true integer age (RGBA16F),
fresh-batch state (RG32F), and receiver identities (RG32_UINT). The state holds
only a batch count capped at two and the last fresh frame stamp. It has no
luminance moments or denoising weights. Reused frames copy this state unchanged;
coarse-slot replication does not count as additional observations.

Cold owners choose their first refresh interval from 1..2 (Mesh) or 1..4 (Global)
from a stable hash of a 32x32 GI region. This keeps complete SharedWorld thread
groups together. Later deadlines remain two/four frames, with real raw ages.
The scheduling region does not change the 2x2/4x4 sampling layout, ray counts,
checker parity, or aligned Mesh/Global direction prefixes.

Owner reprojection selects one compatible previous owner with the same fine/coarse
layout. Raw colors, counts, stamps and ages are never interpolated. An expired
observation retains its maturity state for the next fresh batch without delaying
tracing. Point Repair keeps its independent per-receiver raw cache.

The additional 3x3 owner spatial filter, its luminance moments, filtered atlas,
compute pipeline, tuning constants and temporary T switch have been removed after
the runtime comparison showed limited visible benefit. World display now reuses
the normalized raw Mesh/Global gather directly. This matches the former T-OFF
path while eliminating its redundant raw/display texture reads.

For active pixels, RawGI contains screen and WorldDisplay contains raw Mesh+Global.
Reconstruct applies its existing 5x5 kernel to screen, then adds the world value
once at the center. Checker-skipped pixels still carry combined history unchanged.
The final Temporal, HistoryClamp and both A-trous passes retain their existing
parameters and behavior.

The trace constants stay 1024 bytes. Policy version 7 introduced compact owner
state; version 8 added coupled dual-world Repair directions, and version 9 adds
native coarse reconstruction. At 1280x800 GI, the three packed owner textures total about
23.44 MiB across both history buffers, down from 31.25 MiB with luminance moments.
The full-GI world display texture remains; there is no filtered-owner allocation
or WorldOwnerFilter dispatch.

Screen, Mesh and Global shared atlases all use the finest GI/2 storage layout.
A coarse world estimate and its representative are replicated into four slots,
but coarse reconstruction reads that owner once using its native four-GI spacing.
Coarse tent nodes use phase 4*k+1 and half-width four; this is centered between the
existing fine lattice nodes at 4*k and 4*k+2, retaining their historical phase.
It is not a change to ray origins or a claim that the original fine phase was the
physical footprint center.

A coarse receiver visits four native parent cells. Each coarse parent supplies
one owner. A fine parent supplies only the fine nodes whose two-GI tent covers the
receiver; an empty first fine child does not hide its valid siblings. The same
normal/plane geometry weights, finite-value rejection and final normalization
apply. Uniform coarse planar affine data reconstructs linearly in the interior;
mixed levels preserve constants and use continuously varying spatial weights on
compatible surfaces, but do not promise exact affine reproduction on the adaptive
grid. Geometry rejection can still create a discontinuity. Pure coarse
work has four candidate owners, with at most seven positive-weight geometry taps
at mixed neighborhoods. Atlas storage bounds are checked before the Global offset.

Fine receivers and screen retain the previous two-GI Gather path. A fine world
receiver rejects coarse world taps. A coarse receiver may use geometrically
compatible fine neighbors, and its result remains tagged coarse. Screen taps have no
world-quality tag. Per-receiver normal/plane tests and geometry weights remain;
there is no all-member normal/position prepass or whole-tile compatibility veto.
Mixed-surface tiles can still average lighting from different member surfaces;
these checks do not guarantee elimination of all cross-surface mixing.

There are now three independent full-GI-resolution RGB/age histories: screen,
Mesh and Global. The former combined screen+Mesh near cache has been separated.
RGB stores only the corresponding contribution, and alpha stores integer cache
age. World history additionally uses a ping-pong R32_UINT policy texture: bit 0
stores fine/coarse layout; bits 1-2 and 3-4 independently store Mesh and Global
cache kinds (invalid, fine shared, coarse shared or point Repair). Reprojection
rejects mismatched shared kinds. A shared owner cannot inherit a point Repair
estimate; per-receiver Repair may reuse a geometrically valid point history.
A single world reprojection cannot mix point and shared estimates, so a coarse
shared value cannot lose its layout identity by being relabeled as point history.
A refresh cannot reset an old observation's age merely to relabel its quality.

Final-color checkerboarding remains hardcoded on, without a JSON, UI or runtime
switch. Valid final surface history plus frame parity decides a skip independently
of the three band ages. A skipped receiver carries screen/Mesh/Global cache values
and their world kinds; a failed transfer does not turn the skip into new tracing.
An active receiver gathers the three contributions independently. Repair packs
three independent request flags and traces only missing contributions when their
own histories cannot be reused: four screen directions and the configured R
each for Mesh and Global, with 2/2/4-frame periods. At the default R=8, a receiver
whose three histories all fail can request twenty Repair observations; common
prefixes, checker and cache success determine actual work. Diagnostic nonshared
tracing uses the same separated contributions and configured world counts.

Repair resolves all requested histories before tracing. When Mesh and Global both
need fresh samples, the same R index16 directions provide both estimates through
the existing shared-prefix query. Screen, if also fresh, contributes only its
original first four observations. Each world estimate is still normalized by R,
including zero observations, and only fresh estimates get age zero / point kind.
If either world history is reusable, its age and kind are retained and no coupled
trace is forced. In particular, fresh screen plus fresh Global with cached Mesh
keeps independent screen index16 and Global index32 paths. Legacy/Mesh-only Repair,
SharedWorld, the serial owner layout, and nonshared diagnostic tracing are unchanged.
This changes the correlation of dual-fresh world estimates; it does not promise
pixel-identical noise or a measured performance gain.

The band limits accept lighting only from a hit in that distance interval. A
nearer surface still occludes the ray, including black or unsupported hits: a
far-band ray must not simply start at 2 m and skip foreground geometry. Misses
and unfinished queries add zero. The original screen/scene cache supplies hit
radiance; no proxy emitter or sphere-light estimate is added.

## Settings

Settings are nested in `Settings/Pipeline.json` and read at startup:

```json
"ssgi": {
  "enabled": true,
  "world_sharing": {
    "enabled": true
  },
  "world_sampling": {
    "near_distance": 2.0,
    "near_exit_distance": 2.2,
    "near_ray_count": 8,
    "far_ray_count": 8
  },
  "trace_downsample_factor": 2,
  "intensity": 1.0
}
```

There is one automatic world-sampling policy, with no manual quality tiers or
checkerboard switch. Screen sampling does not participate in camera-distance
classification. HBAO and DLSS retain their separate quality settings.

- `world_sampling.near_ray_count / far_ray_count`: world directions per complete
  shared estimate, not per pixel. Both default to 8; accepted values are 4, 8,
  12, ... with no fixed 64-ray ceiling. Counts must remain divisible by four for
  the four spatial strata, and fit the host `int` type. Both Mesh and Global use
  these camera-quality counts; fixed screen-space ray budgets are unchanged.
  Restart after editing the JSON. Invalid values report an error rather than
  being silently truncated, wrapped, or clamped to a different ray count.

- `enabled`: the pipeline switch. `false` skips SSGI initialization, history
  allocation, BounceSource and all SSGI passes. Restart to apply JSON changes.
- `intensity`: nonnegative multiplier applied once during final HDR composition.
- `world_sharing.enabled`: the checked-in `true` enables shared screen/world
  estimates and the corresponding caches. Diagnostic `false` traces independent
  per-receiver contributions while retaining the same geometry coverage.
- `world_sampling.near_distance`: finite positive camera distance, default 2 m.
- `world_sampling.near_exit_distance`: finite distance >= near_distance, default
  2.2 m; its square must remain finite. These fields affect both world backends,
  not the fixed screen interval. Policy changes invalidate the relevant source
  identity rather than reinterpreting an old shared estimate as a new quality.
- `trace_downsample_factor`: any positive integer representable as host `int`,
  relative to internal scene resolution (after DLSS input-resolution selection).
  The current value is 1: full internal-resolution GI. Larger factors reduce
  GI grid resolution using ceiling division; block sizes are in that grid.
  Trace, shared/Repair, spatial filters, reprojection, temporal accumulation and
  composition use this same factor. Shader zero retains the old diagnostic
  half-resolution fallback only; production configuration rejects zero.

These range/full-resolution changes were not compiled or run, per the user's
instruction. Startup configuration exceptions are caught and logged as a failed
initialization instead of escaping to CRT `abort()`. Removing the fixed setting
ceilings does not remove device resource limits or the cost of dynamic ray loops.

The production program can load the saved cavity pose for a bounded capture:

```powershell
.\x64\Release\LearnDirectX.exe --hidden --frames=2112 --capture-frame=2048 --pass-view=gi --pass-result=output --pass-gi-source=default --camera-preset=Settings/CameraPresets/VXGICavityCamera.json
```

`--camera-preset` changes the initial pose only. Ordinary launches retain their
usual camera. The saved pose remains available after cleanup. The final retained
version passed Debug/Release builds, shader checks and a 64-frame hidden run with
D3D12 validation. Historical captures and experiment snapshots have been cleared;
the closeout records the retained behavior and validation limits.

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
   At full resolution, verify the fixed near distance and facing before reading source color. This
   follows the stackless traversal structure in UE's `LumenScreenTracing.ush`, adapted
   to this project's min/max depth hierarchy and ordinary device Z.
   Ray directions use NVIDIA vector-2 spatiotemporal blue noise (128x128x64 RG8),
   indexed on the GI receiver grid and advanced one slice per frame.
   Shared blocks use their member pixels as ray origins. Raw data, conversion and licensing
   live in `ThirdParty/NVIDIA/STBN`; the shared loader uploads it once on SSGI startup.
   SharedWorld keeps screen, Mesh and Global requests independent and reuses a
   screen/Mesh prefix only for an actually identical member origin and cosine
   direction. A screen hit reads BounceSource only when `needScreen` is true;
   extra world directions use that hit solely to block world continuation. Black
   screen hits block as well. A Mesh hit contributes only to the requested Mesh
   estimate and blocks Global; only a completed Mesh MISS can continue to (2,8] m.
   Screen member directions and the two-frame period remain fixed. World fine and
   coarse estimates use their configured world count, with Mesh/Global periods
   of two/four frames. Each contribution has independent normalization and age.
   Repair and diagnostic nonshared paths use independent configured world counts
   for all three contributions; no extra screen observations are added by world
   quality. The new cache/policy resources and unchanged CB ABI are described below.
   Before ray work, Trace performs the existing current-geometry history test once
   per valid receiver and stores its reprojected slow/fast results in two transient
   RGBA32_FLOAT textures. Rejected histories store zero. No pass is added; float32
   avoids introducing an extra half-float rounding before temporal accumulation.
4. A 5x5 binomial spatial reconstruction uses normal and symmetric tangent-plane weights.
   Shared-band reconstruction uses current surface geometry to reject incompatible
   cached receivers. Raw RGB stores the combined estimate.
5. Independent temporal history uses geometry-checked reprojection and two accumulation
   speeds. Previous bilinear taps are checked against normal agreement and symmetric
   tangent-plane distance. No Euclidean `3 * footprint` cutoff is applied: it rejected
   valid coplanar history at grazing angles. Plane tolerance and history weights are
   unchanged. Slow history uses a 16-sample cap; fast history uses NRD's six-previous-frame
   parameter (steady-state weight 1/7). A separate pass uses RELAX's 5x5 fast-history
   YCoCg clamp, bounded update acceleration and noise-aware reset. No single-frame RGB
   threshold shortens history to three samples. See `ThirdParty/NVIDIA/NRD/README.md`
   for the pinned source, license, port scope and validation.
   Depth and normal history always store the current frame's
   geometry. Temporal directly consumes Trace's reprojected history and validity;
   it does not repeat reprojection, surface testing or previous-history gathering.
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

## Global SDF continuation

Debug > Pass > GI offers three cumulative pure-light views: SSGI only,
SSGI + near Mesh SDF, and the default including Global SDF GI (GDF GI).
Reduced views use a separate diagnostic history; switching tiers clears that
history and the color AA histories. Normal full GI continues to run while
displaying a reduced tier. `--pass-view=gi --pass-result=output` accepts
`--pass-gi-source=ssgi`, `mesh-sdf` or `default` for the same views.

With VXGI enabled, a world direction whose screen segment misses can use Mesh
SDF within the fixed 0-2 m interval. Screen and Mesh lighting now have separate
estimates and histories: screen hits belong only to the fixed screen estimate,
and a Mesh fallback hit belongs only to the requested Mesh estimate. Additional
world directions do not add their screen hits to SSGI. Their screen segments
still test foreground visibility, so screen occluders are not bypassed.
With `vxgi.global_sdf.enabled`, completed near misses can continue along the same
world direction through Global SDF, accepting only 2-8 m lighting. Invalid or
unfinished queries stop. The world normal offset is spent out of the distance
budgets; it must not move the final limit beyond 8 m.

Global SDF finds coarse geometric candidates. Each candidate is confirmed along
the original ray with the existing fine Mesh SDF before reading SH: tight instance
AABBs skip proven empty gaps, and short windows test every intersecting instance.
This avoids accepting the coarse distance shell as a surface, including grazing
rays leaving a floor. Confirmation is bounded to eight windows and 512 shared
fine samples; the Global SDF queries share `max_trace_steps`. Unfinished work
returns an explicit failure, never a proven miss.

A confirmed hit reads the existing neighboring raw SH at that fine hit position
and evaluates incoming radiance along the original ray direction. On 2026-11-03, the
per-contributor exact visibility experiment was reverted after the user reported
a severe frame-rate regression (about 60 to 8 FPS in GI-heavy views). Production
again reads the eight valid raw records without additional contributor rays;
cross-wall cache interpolation remains a known quality limitation. The separate
baked sunlight source-self-hit fix remains enabled.
Black or unsupported hits terminate the ray. There is no search for a brighter surface,
new surface shading, or Surface Cache. Complete misses add zero GI; the existing
IBL base remains in deferred lighting.

Production SSGI reuses the existing six-face probe-neighbor topology buffer to
accelerate the eight interpolation-corner lookups. It finds the base probe once,
then follows allocated neighbor indices where available. An unavailable chain
falls back to the original coordinate search for that corner; a missing parent
does not imply that a diagonal probe is absent. Raw SH validity, weights and
interpolation order are unchanged. The precomputed data stores only topology,
not baked lighting or ray-hit positions. Topology changes update the existing
lookup and neighbor buffers together; ordinary lighting updates keep
using those indices. Consumers without a neighbor descriptor retain the original
lookup path. The bindless descriptor reuses trace byte 768, preserving the 1024B
constant layout, world-mode alias at byte 604 and the original eight-parameter
root signature. The five extra geometry/material root bindings from the reverted
visibility experiment are removed.

The `vxgi.global_sdf` object also accepts `trace_distance` (finite positive world
distance, default 128) and `max_trace_steps` (integer 1..1024, default 256).
Those VXGI settings remain shared backend controls. SSGI caps its far contribution
at 8 m and clips coverage by the outer clipmap's actual bounds. Disabling this
object removes the far band; screen-only mode does not sample world caches.
The current three cubes have edge lengths 1/4/8 metres, with cell sizes
0.25/0.5/1 metre. `cells_per_axis: [4, 8, 8]` selects the independent layer
counts; `cell_size: 0.25` is doubled per layer. A scalar cell count still applies
to every layer. Each cube follows the camera on its own world-aligned grid.
Configuration changes invalidate world GI history. Ordinary clipmap scrolling
keeps history and uses the current frame's pending layout.

Trace constants retain their 1024-byte layout, Global parameters at byte 784,
probe-neighbor descriptor at byte 768 and world-mode SRV alias at byte 604.
The screen/policy descriptors and squared camera thresholds reuse reserved yzw
components of the existing depth-level uint4 array; the x components remain the
depth SRVs. Existing world-band descriptor offsets are preserved, rather than
appending new fields beyond the CB. The GDF texture is an explicit `R32_FLOAT
Texture3D` SRV declared as an RDG read; Trace uses its normal bindless heap and
adds no production diagnostic pass.

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
Each normal and diagnostic ping-pong set owns three independent full-GI-resolution
RGBA16F screen/Mesh/Global caches and a full-GI-resolution R32_UINT world-policy
texture. Their indices advance with the submitted GI history. Relative to the
previous two-band implementation, the extra screenBand and worldPolicy ping-pong
payload is 24 bytes per GI texel: about 10.44 MiB at 854x534 for the normal set.
An allocated diagnostic set adds its own payload, and sparse shared/transient
resources add separate costs. These are payload estimates, not measured total
VRAM or RDG committed peaks.
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
time. C++ and HLSL layouts must match for every compiled variant.

`SSGICameraQuality.hlsli` contains camera-distance classification, world-policy
packing and kind-aware world-band reprojection. `SSGICommon.hlsli` contains view-normal decoding,
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

Single-layer screen information cannot represent off-screen or hidden surfaces;
the optional world path is limited by clipmap coverage, Mesh SDF detail and SH support.
Small bright sources remain difficult to sample. Exhaustion contributes zero GI;
no sky fallback is added. Temporal reprojection currently uses camera matrices rather than
per-object motion vectors. Coarse-cell depth ranges and the seven-level hierarchy can
limit traversal efficiency in complex views.

Known traversal precision issue: `RayCell` still uses a fixed 0.0001-pixel address
offset. At sufficiently large FP32 pixel coordinates (for example 2049), the offset
can round away, revisiting a boundary until the iteration budget is exhausted.
This remains separate from the fixed grazing-angle temporal rejection and the
new distance-band scheduling.

## Historical diagnostic cleanup

Temporary GPU counters, color probes, readback buffers, CSV writers, pure-GI preview
branches and local SSGI test tools were removed after validation. No diagnostic UAVs or
readback passes are attached to the production graph. Normal rendering error reporting
remains available. Before removing the tools, GPU regression checks passed for trace
normalization, odd sizes, black hits, finite radii, and the emission cap/visible-color
separation. Debug and Release builds are checked during cleanup.

The final cleanup also removes unused comparison sources.
Production NRD/STBN helpers, assets and licenses remain.
The T-key spatial-filter comparison switch is gone; both A-trous passes are enabled
whenever the configured SSGI pipeline runs. No test-only shader variants remain.

## Completed static-camera investigation

The temporary A/B/C capture and its CSV/text reports have been removed. This
investigation used the old algorithm and does not measure the new RELAX-based path.
In the historical 256-frame four-ray capture, disabling the old brightness clamp and its history reduction lowered
fixed-screen frame-difference RMS by about 86%, without darkening the output.
This motivated the fast/slow history replacement. No automatic probe readback or
extra comparison histories run in the new pipeline.
