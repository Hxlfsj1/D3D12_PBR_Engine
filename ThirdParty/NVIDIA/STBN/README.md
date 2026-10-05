# NVIDIA spatiotemporal blue noise data

Source: https://github.com/NVIDIA-RTX/STBN

Pinned revision: `48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b`.

Upstream archive:
https://raw.githubusercontent.com/NVIDIA-RTX/STBN/48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b/Assets/STBN.zip

`License.txt` is the complete, unmodified upstream license, including its
non-commercial and commercial-use sections. The derived texture remains subject
to those terms; it is not relabeled as MIT/public-domain data.

## Decoded asset

`stbn_vec2_128x128x64.rg8` contains R/G bytes from the 64 upstream
`STBN/stbn_vec2_2Dx1D_128x128x64_{frame}.png` images, in numeric frame order 0..63.
This is uniform vector-2 data, not unit-vector data. Conversion drops unused B/A
and preserves R/G bytes, top-to-bottom rows, and spatial/temporal ordering.
No color conversion, resizing, filtering, compression, or runtime PNG decoding.

- Width/height: 128 x 128; 64 array slices; 2 bytes per texel (R then G).
- Layout: `((frame * 128 + y) * 128 + x) * 2 + channel`.
- Row pitch: 256 bytes; slice pitch: 32768 bytes; total: 2097152 bytes (2 MiB).
- Upload as `R8G8_UINT`, one mip, `Texture2DArray<uint2>`.
- Decode samples as `(byte + 0.5) / 256`, avoiding exact 0 and 1.
- CPU/HLSL dimensions are shared through `STBNConfig.h`.
- Asset SHA-256: `b3d8b7e55f1cb89683d1a4f51c494fe8071a81a36875eecbdc00f27add5ec0b1`.
- Source ZIP SHA-256: `f262aaa79704b913ad1ac22b11674931c5c16a788688f8ce49ec43d59eb5c747`.

Reproduce with Pillow: `python ThirdParty/NVIDIA/STBN/decode.py path/to/STBN.zip`.

## Renderer integration

`Resources/BlueNoiseTexture.h` uploads and owns the immutable texture once. Other
passes can share it via ResourceManager and register a read-only RDG dependency.
It is currently requested only when deferred SSGI is enabled. Ship the raw asset
and license with the renderer, keeping the same project-relative path as its shaders.

`Shaders/BlueNoise.hlsli` indexes the consumer's pixel grid and advances the array
slice using `frame % 64`. Multiple rays use fixed R2-distributed XY offsets; there
is no per-frame XY hashing. This follows NVIDIA's multiple-samples guidance:
https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-2/

SSGI maps each uniform 2D sample to its cosine-weighted hemisphere, replacing the
hash-rotated Hammersley sequence. Ray counts, radius and E/pi normalization are
unchanged. This does not introduce light importance sampling or guarantee lower
variance in every scene. Spatial tiling repeats at 128 consumer pixels and the
time sequence repeats after 64 frames.
