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

## Build

```powershell
cmake -S . -B build -DLU_ASSETS_DIR=V:/Repositories/LU-Rebuilt/lu-assets
cmake --build build --config Debug
```

## Run

```powershell
$clientRoot = "<path-to-unpacked-client>"
.\build\apps\nif_viewer\Debug\lu_nif_viewer.exe `
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
.\build\apps\nif_viewer\Debug\lu_nif_viewer.exe --hidden --exit-after-frames 3
```
