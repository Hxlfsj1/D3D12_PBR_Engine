# NRD RELAX diffuse temporal and spatial adaptation

Source: NVIDIA-RTX/NRD, commit `c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28` (retrieved 2026-10-05).

- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_TemporalAccumulation.cs.hlsl
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_HistoryClamping.cs.hlsl
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_AtrousSmem.cs.hlsl
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_Atrous.cs.hlsl
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_Common.hlsli
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Shaders/RELAX_Config.hlsli
- https://github.com/NVIDIA-RTX/NRD/blob/c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28/Include/NRDSettings.h

`RelaxDiffuse.hlsli` adapts the diffuse history-clamping equations, fast-history
YCoCg neighborhood box, center expansion, bounded history acceleration, and
noise-aware partial reset. The accumulation weights follow RELAX. This software
contains source code provided by NVIDIA Corporation. The NVIDIA RTX SDKs license
is retained in `LICENSE.txt`; the original source copyright notice is retained.
Color conversion and luminance functions match NVIDIA MathLib v11 `ml.hlsli`.
RGB/YCoCg conversion is shared with TAA/TSR through `Shaders/ColorCommon.hlsli`;
the adapter includes that project helper. The copyright/permission notice is
retained in both that header and `MathLib-LICENSE.txt`.

## Project integration choices

This adapts diffuse temporal and spatial stages, not the complete NRD library/RELAX pipeline.
Existing SSGI geometry reprojection and 5x5 binomial reconstruction are retained.
The local reprojection now omits its old Euclidean `3 * footprint` rejection,
which discarded coplanar samples at grazing angles. Normal agreement and symmetric
tangent-plane checks remain. This is a local fix, not NRD's full reprojection port.
The reconstructed GI is the noisy input for this temporal stage. A new half-resolution
fast RGB history is ping-ponged alongside the existing slow GI/depth/normal history.
A first dispatch produces both accumulated signals into transient textures. A second
dispatch reads their completed neighborhoods before writing persistent histories.
No pass reads neighboring texels while another thread writes that same texture.

Main history is capped at 16 including the current sample (project choice).
A 31-sample comparison was reverted after scene testing showed more visible
changes during camera movement than the 16-sample setting.
NRD's `maxFastAccumulatedFrameNum=6` is used: the steady-state fast weight is 1/7,
while the main weight is 1/16. At startup both weights use actual history length.
Clamp parameters match the pinned RELAX defaults: sigma=2, acceleration=0.3,
reset=0.5, spatial sigma=4.5, noisy-input sigma=0.5; acceleration internal scale=10.
They are constants in the pass, not new Pipeline.json settings.

The clamp uses a 5x5 neighborhood, including valid black GI, with edge coordinates
clamped and invalid background/material pixels excluded. Like RELAX, it does not
apply the reprojection same-surface test to the color-box neighborhood. The 12x12
shared tile for an 8x8 workgroup avoids 25 global reads per pixel per signal.

Local differences: `GI.a` still holds history length; fast-history alpha now stores
the slow luminance second moment (not fast-history variance or a validity flag).
Validity comes from the slow history length, so valid black GI is included. Outputs
are nonnegative to satisfy this renderer's radiance contract. The moments needed
by clamp/reset are gathered from the current 5x5 fast/noisy neighborhood.
NRD's short-history <=3 clamping branch is retained, but there
is no separate HistoryFix pass. NRD specular/SH, checkerboard, material permutations,
confidence input and SDK resource abstraction are not imported.

## Two-pass spatial filtering

`Shaders/Shaders_For_SSGI_Spatial.hlsl` and `RelaxAtrous.hlsli` adapt RELAX's first and subsequent
diffuse A-trous stages. Two passes run at half resolution with steps 1 and 2. These
steps change tap spacing on the same image; they do not construct or sample a new
Hi-Z hierarchy. Both variants use a 12x12 shared tile for an 8x8 workgroup.

Temporal accumulation now tracks luminance squared with the same weight as slow RGB.
History clamping corrects it by adding `newLuminance^2 - oldLuminance^2`, preserving
variance when the color changes. The first spatial pass uses RGB plus this second
moment; later passes use RGB plus variance and propagate variance with squared weights.
At history length <3, the first pass estimates variance from a 5x5 neighborhood and
boosts it by `max(1, 4/(historyLength+1))`, following RELAX's short-history path.

Luminance phi=2, depth threshold=0.003 and diffuse lobe fraction=0.5 follow the pinned
defaults. Local adaptations: view-space positions and normals, full `acos` instead
of MathLib's approximation, no material/confidence weight, history-dependent normal
relaxation in both passes, and a plane-distance test also in the short-history 5x5
branch. The renderer currently uses perspective cameras. This is not a bit-exact
port of the complete NRD spatial pipeline.

The clamp emits a transient RGBA16_FLOAT color/moment texture; each spatial pass
produces another transient RGBA16_FLOAT texture. Persistent texture count stays the
same because the moment uses the existing fast-history alpha channel. Only composite
reads the final spatial RGB. Spatial results never feed back into temporal histories.
The long cap remains 16 and fast parameter remains 6. No JSON controls are added.

## Verification

A temporary D3D12 harness compared the adapted clamp to a separately compiled
reference extracted from the pinned original diffuse block: 37,296 FP16 output
components, maximum difference 0. Cases include black/constant input, bright-to-dark,
dark-to-bright, randomized sparse input, invalid neighbors and short histories,
at 1x1, 17x9 and 32x16. Separate accumulation cases cover initialization, accepted
history, normal rejection and valid zero-radiance samples. D3D12 debug errors: 0.
This verifies the math/bindings, not final scene quality or moving-object ghosting.
Build and GPU check logs are under `tmp/ssgi-relax-*.log`.
That reference comparison predates the moment/spatial extension above.

The spatial extension passes Debug/Release builds and 27 headless D3D12 GPU cases:
constant and black preservation, rejected depth/normal discontinuities, invalid
background, short/long histories, and 1x1, 17x9 and 32x16 dimensions; separate cases
exercise second-moment initialization, accumulation, rejection and clamp correction.
Synthetic planar grayscale-noise RMS at history length 16 falls from 0.2901 to 0.0649
(17x9) and from 0.2806 to 0.0471 (32x16). These are fixture results, not measurements
of the car scene or frame-to-frame noise. D3D12 debug errors: 0. Logs are under
`tmp/ssgi-atrous-*.log`; temporary GPU fixture code is removed after validation.
These fixture checks do not establish scene quality or GPU frame cost. The user
subsequently accepted the two-pass spatial result and confirmed that removing the
grazing-angle temporal distance cutoff resolved the reported boundary artifact.
The temporary T-key comparison switch and `tmp/nrd-reference` source comparison
copies have been removed; the production adaptations, licenses and pinned links remain.
