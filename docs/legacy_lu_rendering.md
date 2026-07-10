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
Do not infer blending solely from vertex alpha, texture alpha, diffuse alpha,
or the mere presence of an `NiAlphaProperty` block. Decode its enable bits and
factors, then resolve them against the selected technique's render-state
contract. A control-alpha technique does not suppress an explicitly enabled
NiAlpha blend state: shader-data meaning and framebuffer state are independent.

## NIF Render-State Evidence

LU's Gamebryo properties are independent authored state, not one generic
"transparent" switch. Preserve the raw property, its decoded fields, and the
node that supplied it before deriving GPU state.

- `NiAlphaProperty`: blend enable, source/destination factors, alpha-test
  enable/function/reference, and no-sort.
- `NiZBufferProperty`: depth-test enable, depth-write enable, and comparison.
- `NiVertexColorProperty`: source vertex mode and lighting mode.
- `NiSpecularProperty` and `NiShadeProperty`: specular enable and shade mode.
- `NiStencilProperty`: stencil state plus face draw mode.
- `NiSortAdjustNode`: subtree sorting inheritance/off state.

Resolve property candidates from the geometry node toward the root, with the
nearest property of each type winning. Keep provenance and cycle/multiple-parent
diagnostics. When the selected FX technique declares `UsesNiRenderState=true`,
the importer applies inherited NiAlpha and NiZ state exactly. Techniques such as
`Technique_ImaginationCloud` declare `UsesNiRenderState=false`; their policy
state wins and the NIF candidates remain diagnostic only.

Blend enable and depth writes are independent. A blended draw can write depth,
and the bgfx backend must not silently remove `WRITE_Z`. NiAlpha source and
destination factors map individually, all eight comparison functions are
supported, reference zero remains valid, and the no-sort bit plus inherited
`NiSortAdjustNode` sorting-off mode are honored.

`NiVertexColorProperty` is also independent from the existence of a color
stream. `SourceVertexMode=Ignore` prevents that stream from selecting or
feeding a vertex-color technique. `SourceVertexMode=Emissive` and
`SourceVertexMode=AmbientDiffuse` permit it, but cannot invent a stream that is
not present. The selected LU technique still decides whether RGB/alpha are
diffuse color, output alpha, emissive control, glow control, or another effect
input. An evenly distributed sample of the 10,534 LU mesh NIFs found only the
two normal authoring combinations: flags 8 (source ignored, no stream) and
flags 40 (ambient/diffuse source with full lighting, stream present).

`NiSpecularProperty` gates the specular term of a technique that supports
specular; it never adds specular to a no-specular technique. An absent property
leaves the technique policy unchanged. This distinction matters for shader 1
assets: many contain a disabled property, while a smaller set explicitly
enable it.

`NiStencilProperty` supplies the stencil comparison, reference, read mask, and
fail/depth-fail/pass operations. LU's D3D stencil buffer is represented by the
low eight bits in bgfx. Its draw mode also controls face culling even when the
stencil test itself is disabled. The sampled LU stencil fixture,
`mesh/env/sound_trigger_sphere.nif`, uses `DRAW_BOTH` and is therefore rendered
two-sided instead of retaining the generic backface-cull policy.

`lu_shader_audit --per-mesh` reports the node path, property provenance, decoded
state, vertex-alpha range, texture alpha-format hint, requested state, and the
state the current backend actually submits.

Current real-asset evidence:

- Spaceranger armor inherits `NiZBufferProperty` flags 15 from the root:
  depth test and writes enabled with less-equal comparison. It directly carries
  `NiSpecularProperty` flags 0 and `NiAlphaProperty` flags 237.
- The corn cob also inherits Z-buffer flags 15, but
  `Technique_ImaginationCloud` declares `UsesNiRenderState=false` and explicitly
  disables Z writes, so the technique overrides that inherited candidate.
- Airport clear lights have blend flags 237 and no explicit Z-buffer property.
  They therefore use source-alpha/inverse-source-alpha blending and retain the
  technique's default depth write instead of losing it to a transparency
  classification.

Remaining format limitations are explicit: transparent ordering is stable
whole-mesh bounds-center sorting rather than triangle sorting, and flat
`NiShadeProperty` rendering would require face-normal geometry expansion. No
flat-shaded property was found in the representative LU asset sample, so that
geometry rewrite is not enabled without a real fixture.

## Known Real-Asset Contracts

| Asset | Evidence and expected result |
| --- | --- |
| `mesh/factionkit2/minifig_accessory_spacerangerkit3_armor.nif` | CDClient shader 19 (`LEGO-SuperEmissive`) is authoritative; `ArmorShade:FXshader_Armor` is diagnostic noise; `Technique_LEGOPPLightingVertColor_SuperEmissive`; authored alpha blending and depth writes coexist; vertex alpha controls emissive intensity while output/fade alpha controls framebuffer blending. |
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
