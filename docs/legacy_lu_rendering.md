# Legacy LU Rendering Notes

The first renderer profile aims for a small vanilla-LU slice rather than a full
shader recreation.

## Runtime Shader Policy

Original LU `.fxo` files are Direct3D 9 effect bytecode and are not portable.
Use them only as metadata/reference material. Runtime shaders are bgfx `.sc`
sources compiled through `shaderc`.

Useful original shader references live in the unpacked client under:

```text
res/shaders/precompile/
```

If decompiled `.fx`/`.fxh` sources are available, prefer those over `.fxo`
bytecode for understanding the original material math. Keep those reference
files outside this repository just like the rest of the proprietary client
assets.

Notable files:

- `basicshaders.fxo`
- `legopplighting.fxo`
- `legopplighting_noenv.fxo`
- `terraindiffuse.fxo`
- `particles.fxo`
- `postprocessingshaders.fxo`

## Milestone 1 Lighting

The initial legacy shader uses:

- one directional light
- ambient term
- diffuse texture when available
- NIF material diffuse color
- vertex color

No shadows, environment maps, specular approximation, fog transitions, alpha
sorting, skinning, particles, or terrain blending are part of milestone 1.
