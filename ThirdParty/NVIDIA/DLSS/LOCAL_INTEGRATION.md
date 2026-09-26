# Local DLSS integration

This repository keeps the NGX/DLSS headers, license, documentation, utilities and
runtime DLLs. The x64 library directory is trimmed to the variants used by the
MSBuild project:

- Debug: `nvsdk_ngx_d_dbg.lib`
- Release: `nvsdk_ngx_d.lib`

The unused static-CRT (`_s`) and explicit iterator-debug variants were removed.
Both `dev/nvngx_dlss.dll` and `rel/nvngx_dlss.dll` remain and are copied for their
respective build configurations. This is a project-specific SDK subset, not an
unmodified SDK distribution. If CRT or iterator-debug settings change, restore
and link the matching library from the same SDK version.
