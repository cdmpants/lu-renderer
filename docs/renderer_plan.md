# Renderer Plan

`lu-renderer` is the future renderer package for LU-Rebuilt. The first
milestone is intentionally small: prove a bgfx-backed render path with a NIF
viewer while keeping the architecture shaped for the eventual client.

## Milestone 1

- bgfx backend, GLFW viewer host
- `lu-assets` NIF import
- static opaque mesh rendering
- legacy LU-style material shader
- ambient + directional light
- orbit camera and model framing

## Future Milestones

- richer NIF material/alpha support
- terrain viewer using LU `.raw` terrain data
- PSB particle rendering
- full LUZ/LVL zone viewer
- clustered forward lighting
- sidecar-authored PBR material overrides
- reflection probes, shadow maps, AO, SSR/SSSR-style post effects

## Design Defaults

- Renderer core does not depend on GLFW, Qt, or SDL.
- bgfx is the only graphics backend dependency.
- Original LU files are parsed through `lu-assets`.
- Original client assets and FXO bytecode are never copied into this repo.

