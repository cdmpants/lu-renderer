# lu-renderer

Modern renderer experiments for the LU-Rebuilt client.

This repository contains a small C++20 renderer layer built around bgfx and a
standalone NIF viewer used to validate the renderer against original LEGO
Universe assets. It is intended to become a reusable renderer package for the
LU-Rebuilt client, not just a one-off tool.

## Current Milestone

- bgfx backend with GLFW host app
- static NIF mesh loading through `lu-assets`
- legacy LU-style opaque shader
- ambient + directional lighting
- orbit camera and model framing

Original LU client assets are never committed to this repository. Point the
viewer at a local unpacked client.

## Canonical Viewer

There is one supported visual-reference application: **LU NIF Viewer**. Build
and launch it from the repository root with:

```powershell
.\Run-NIF-Viewer.ps1
```

This always builds the canonical optimized viewer with debugging symbols and
publishes it to the deliberately configuration-free path:

```text
viewer\LU NIF Viewer.exe
```

Do not use an executable under `build\...\Debug` or
`build\...\RelWithDebInfo` as the visual reference. Those are CMake's
configuration-specific build artifacts. The viewer title identifies the
published build as `Canonical`; a Debug build identifies itself as a developer
build.

Arguments after the launcher name are passed to the viewer:

```powershell
$clientRoot = "<path-to-unpacked-client>"
.\Run-NIF-Viewer.ps1 `
  --client-root $clientRoot `
  --nif "$clientRoot\res\mesh\3dui\1150.nif"
```

`--client-root` may point either at the unpacked client root or directly at its
`res` directory.

You can also launch the viewer without `--nif` and use `File > Import...` to
open a `.nif` file. On Windows this uses the native file-open dialog. If
`--client-root` is omitted, the viewer tries to infer it from the selected path
by walking up to the nearest `res` directory.

For CI/smoke checks, the viewer can render a few frames and quit:

```powershell
.\Run-NIF-Viewer.ps1 --hidden --exit-after-frames 3
```

`lu_shader_audit` is a separate command-line inspection tool. It does not
render images and is not an alternate viewer.
