# Legacy LU Rendering Notes

The renderer aims to reproduce the original LU shader contract from evidence,
not reinterpret material channels using modern PBR conventions. The original
effect technique decides what each channel means. In particular, vertex and
texture alpha are not globally synonymous with transparency.

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

## Authoritative Shader Assignment

CDClient's `RenderComponent.shader_id` is authoritative for a render asset.
Material names and strings such as `FXshader_Armor`, `NiMultiShader*`, and
`_NIMS` do not override that assignment; retain them only for diagnostics and
source-audit work.

Shader ID 100 is the one exception because it explicitly means Multishader.
For a Multishader asset, read the numeric `S##_` prefix from the submesh name.
If the submesh is unprefixed, walk upward and use the nearest prefixed parent
object. A child prefix wins over a parent prefix. The prefix value is the
authoritative shader ID for that submesh.

If CDClient has no assignment, or a Multishader mesh and its ancestors have no
valid prefix, render the visible diagnostic fallback and report the unresolved
assignment. Do not invent an assignment from material-name metadata.

These rules are covered in `tests/lu_import/test_shader_database.cpp`.

## Alpha Is Technique Data

Alpha handling is part of `LuShaderPolicy`, not a generic NIF-material rule.

| Semantic | Original use | Transparency policy |
| --- | --- | --- |
| `OutputAlpha` | Pixel output alpha/fade | May use authored blend/test render state |
| `AlphaTest` | Cutout threshold | Alpha test, not ordinary blending |
| `ControlGlow` | Selects/tints glow | Must not promote the mesh to transparent |
| `ControlEmissive` | Controls emissive interpolation | Must not promote the mesh to transparent |
| `ControlDarkling` | Darkling window/mask control | Must not promote the mesh to transparent |

The recovered `LEGOPPLighting.fx` is explicit about SuperEmissive. Its vertex
color variant interpolates from lit color toward `IN.Color.rgb * 10.0` using
`IN.Color.a * g_materialEmissive.r`, while the output alpha is the separate
fade value. The vertex alpha therefore controls emissive strength; treating it
as framebuffer transparency makes opaque armor disappear except for its bright
control regions.

When adding a shader policy, record the exact source file, technique, RGB
inputs, alpha meaning, blend/test state, depth-write state, culling, and
texture/sampler roles in `LuShaderPolicy` and add a real-asset regression case.
Do not infer blending solely from the presence of vertex alpha, texture alpha,
or `NiAlphaProperty`.

## Known Real-Asset Contracts

| Asset | Evidence and expected result |
| --- | --- |
| `mesh/factionkit2/minifig_accessory_spacerangerkit3_armor.nif` | CDClient shader 19 (`LEGO-SuperEmissive`) is authoritative; `ArmorShade:FXshader_Armor` is diagnostic noise; `Technique_LEGOPPLightingVertColor_SuperEmissive`; opaque/depth-writing; vertex alpha controls emissive intensity. |
| `mesh/pets/ptmg_corn_cob.nif` | CDClient shader 82 (`Pet Taming LEGO In Cloud`) is authoritative; `NiMultiShader1` does not override it; `Technique_ImaginationCloud`; unlit alpha blend with no depth write. |

## Milestone 1 Lighting

The initial legacy shader uses:

- one directional light
- ambient term
- diffuse texture when available
- NIF material diffuse color
- vertex color

No shadows, environment maps, specular approximation, fog transitions, alpha
sorting, skinning, particles, or terrain blending are part of milestone 1.
