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
- sidecar-authored PBR material overrides
- reflection probes, shadow maps, AO, SSR/SSSR-style post effects

## Forward Renderer Shape

The renderer should stay a conventional forward renderer for the foreseeable
future. Clustered forward/Forward+ is not needed for the current LU target:
most scenes can be lit by the imported LVL directional/ambient/hemi state plus
select authored extras later. The important architecture is pass ownership, not
large dynamic light lists.

Recommended pass order:

1. Optional depth prepass for opaque/alpha-test geometry.
2. Directional shadow map pass, initially one non-cascaded map with PCSS.
3. Forward opaque pass into a scene color target plus depth.
4. Forward alpha pass for blended/transparent legacy materials.
5. Screen-space passes that consume scene color/depth/normals: GTAO, SSR.
6. Post stack: LUT/color grading, bloom from emissive/glow output, vignette,
   DoF, film grain, then backbuffer composite.

The current `RenderFeatureSettings` object is the runtime control surface for
these passes. The NIF viewer exposes the same settings so effect constants can
be tuned live before they become serialized/profile defaults. Continuous
graphics parameters should have both sliders and exact value fields in the
NIF viewer; the current Win32 diagnostics panel follows that pattern for PBR,
SSR/GTAO, post, PCSS, LUT, and reflection-probe controls.

The current depth-dependent post path allocates an explicit view-normal/depth
prepass when DoF, SSR, or GTAO are active. The pass renders opaque and
alpha-test depth writers into a color normal target with its own depth
attachment, then post decodes that buffer for DoF focus depth, SSR reflection
vectors, and GTAO horizons. This keeps the default forward path lean while
avoiding depth-gradient normal inference as the primary source for effects that
need stable surface orientation. The normal pass shares diffuse alpha, material
alpha, vertex alpha, UV scroll, and the imported alpha-test threshold so cutout
materials do not stamp solid depth or normals into DoF/SSR/GTAO.
Family-specific alpha tricks that are not represented by the shared threshold
still need case-by-case validation on real assets.
GTAO currently uses a small deterministic axial/diagonal tap pattern rather
than time-varying random rotation, because the current TAA path is not yet a
motion-reprojected denoiser and time-varying AO noise is too visible in stable
views.

TAA has a first optional history path rather than a finished anti-aliasing
solution. When enabled, the camera projection receives an 8-sample Halton
jitter, post processing writes into a two-buffer color history, then the result
is copied back to the backbuffer. The post shader blends the current frame with
the previous history using tunable feedback and clamps the history around the
current color to reduce obvious stale-frame drag. Until motion-vector
reprojection exists, the renderer invalidates history whenever the camera mode,
eye, target, yaw, pitch, or distance changes, so accumulation is limited to
stable views. The NIF viewer exposes TAA enable, feedback, and jitter
controls. Motion vectors, camera/object reprojection, disocclusion tests, and
velocity-aware sharpening are still future work before this should be
considered final-quality TAA.

MSAA is a backbuffer/reset-level feature in bgfx, not only a draw state. Keep it
owned by renderer feature settings so resize and live sample-count changes use
the same reset flags. When post processing is active, the scene color target now
uses a matching bgfx MSAA render-target mode and is recreated when the sample
count changes. Depth-dependent post effects (DoF, SSR, GTAO) sample depth from
the single-sample normal/depth prepass instead of the scene framebuffer depth,
because bgfx does not provide a generic depth resolve path for the shader's
ordinary depth sampler contract.

The directional shadow depth pass shares the same diffuse alpha, material
alpha, vertex alpha, UV scroll, and imported alpha-test threshold contract as
the screen-space normal prepass. This keeps PCSS shadows from treating legacy
cutout geometry as solid cards. Blended and additive materials remain skipped
as shadow casters.

The forward world pass now builds one visible draw list per frame, keeps
opaque/alpha-test source order stable, and submits those materials before
alpha-blended and additive materials. Transparent materials are sorted
back-to-front by mesh bounds center, still depth-test against the opaque scene,
and no longer write depth. The NIF viewer has a `--transparent-test-scene`
synthetic scene for smoke-testing overlapping alpha-blended quads when unpacked
client assets are not available.

Reflection probes use the same material binding contract as the legacy LU
reflection maps. When probes are enabled, Lego shader families bind the global
probe instead of their authored reflection-map cubemap, and the viewer's probe
intensity scales the environment reflection contribution. The current renderer
captures a 128px six-face global cubemap from the loaded world once when the
probe becomes dirty; imported environment lighting, hemisphere, and fog colors
remain as the generated fallback when a capture target cannot be allocated.
Each captured cubemap face has its own depth target so probe capture uses the
same ordinary depth rejection as the forward world pass instead of depending on
submission order. Probe capture is currently limited to opaque and alpha-test
geometry, because alpha-blended materials need per-face sorting/compositing to
capture correctly. The first probe implementation is still intentionally global
and low-resolution; localized probes, blending, transparent probe capture,
refresh policy, and runtime authoring remain future work. The global probe is
marked dirty when world/environment state changes, when probes are enabled, and
when PBR mode or PBR BRDF settings change while probes are enabled.

LUT color grading is implemented as an optional post step that accepts 2D DDS
strip LUTs laid out horizontally as `N*N x N` or vertically as `N x N*N`.
The renderer keeps a generated neutral 16^3 LUT bound as a fallback, so the
post path remains stable when no unpacked LU LUT has been selected yet. The
public `Squareville/claude-client-re-docs` repository could not be fetched from
the available references during this pass, so native LU post-processing intent
is still an open investigation item; the hook is deliberately asset-driven so
verified LU DDS LUTs can be dropped in without changing the renderer contract.
The current workspace only contains renderer reflection-map DDS files and bgfx
sample DDS textures, not identifiable LU color-grading LUTs. The NIF viewer
therefore exposes both a path field and a DDS browse button for selecting LUTs
from an unpacked client install when those assets are available.

Bloom is gated by an authored mask rather than a global scene-brightness pass.
The mask is rendered from material intent signals such as LU Glow, ItemGlow,
Emissive, SuperEmissive, glow textures, emissive controllers, and nonzero
material emissive color. Non-bloom meshes still render black into the mask pass
with depth writes, so glow is occluded by foreground geometry. The bloom
threshold and intensity controls remain for live tuning of how strongly marked
surfaces bleed.

SSR is gated by a reflection-intent mask rather than a global material
roughness input, because legacy LU shaders do not expose PBR roughness. The
mask requires `lu_shader_uses_reflection` and an authored/fallback reflection
map, then weights stronger reflection intent for polished metal, brushed metal,
clear plastic, and shiny/glint Lego variants. The viewer exposes SSR strength,
max distance, and depth thickness as live controls.
Reflection and bloom mask rendering shares the same diffuse alpha, material
alpha, vertex alpha, UV scroll, and imported alpha-test threshold contract as
the shadow and normal prepasses, so animated cutout materials do not stamp
solid SSR/bloom masks. The SSR reflection mask is limited to the same
opaque/alpha-test surfaces that provide screen-space depth and normals. Bloom
uses opaque/alpha-test mask depth for foreground occlusion, then permits
transparent glow materials to contribute as blended, no-depth-write mask draws.

## Static Shadow Cache Metadata

The LU formats parsed so far do not expose a direct per-mesh "static shadow
caster" flag. There is still enough metadata to build a conservative shadow
cache classifier.

Evidence in parsed LU data:

- LVL lighting contains separate `static_obj_distance` and
  `dynamic_obj_distance`, so native LU distinguished these classes at scene
  culling policy level.
- LVL objects carry `object_id`, `lot`, `glom_id`, transform, LDF config,
  render technique data, and `node_type` on version 38+ scenes. Static
  candidates are `EnvironmentObj` and `Building`; dynamic candidates are
  `Enemy`, `NPC`, `Rebuilder`, `Spawned`, `Cannon`, `Bouncer`, `Exhibit`,
  `MovingPlatform`, `Springpad`, `GenericPlaceholder`, `ErrorMarker`, and
  `PlayerStart`. `Sound` and `Particle` nodes should not cast mesh shadows.
- LUZ paths explicitly identify `NPC`, `MovingPlatform`, `Spawner`, `Racing`,
  and `Rail` paths. Moving platform paths should force associated placed
  objects into the dynamic bucket; spawner paths should force spawned LOTs and
  the referenced `spawner_object_id` into dynamic or non-cached handling.
- NIF import currently exposes `is_skinned`, skin block references, bone node
  names, LOD ranges, material color controllers, material emissive controllers,
  shader UV animation flags, and shader alpha animation flags. KF/KFM import
  exposes controlled blocks for external animation clips.

Current renderer gap: `importLvlEnvironment()` only consumes LVL lighting,
fog, and draw-distance data. Full zone object placement and LUZ path metadata
are available in `lu-assets`, but they are not yet part of the renderer world
model. Until the zone loader is wired in, the NIF viewer can only classify by
mesh/material animation signals and by whether an object's transform remains
stable at runtime.

Recommendation: implement shadow caching as a mixed static cache plus dynamic
overlay instead of relying on a single authoring flag.

1. On zone load, classify LVL `EnvironmentObj` and `Building` instances as
   static candidates when their NIFs are not skinned and have no active
   transform animation. Treat path-driven, spawned, character, interactive, and
   player-start nodes as dynamic. Ignore sound and particle nodes for mesh
   shadow casting.
2. Use NIF/KF signals to override the LVL class. Skinned meshes, bound KF/KFM
   transform controllers, and moving-platform ownership are dynamic. Pure UV or
   emissive animation does not require a geometry shadow refresh; alpha
   animation should be conservative and dynamic only when that material can cast
   alpha-tested or masked shadows.
3. Track a transform dirty bit for every shadow caster. If a static candidate
   moves beyond a small position/rotation/scale epsilon, render it in the
   dynamic overlay and mark its static-cache entry dirty. After it has remained
   stable for a debounce window, let it sleep and refresh or merge it back into
   the static cache.
4. Keep sidecar overrides as the escape hatch, not the primary data source. A
   coarse table keyed by normalized NIF path, LOT ID, or LVL object ID is enough
   for the first cache, with values like `static`, `dynamic`, `neverCast`, and
   `forceCached`. Submesh granularity is unnecessary.
5. Invalidate the static cache on zone load, relevant sidecar changes, sun
   direction changes, terrain/building streaming changes, or when a previously
   static caster is explicitly edited.

This preserves LU-authored intent where the formats expose it, while still
handling the practical cases the data cannot identify perfectly.

## Design Defaults

- Renderer core does not depend on GLFW, Qt, or SDL.
- bgfx is the only graphics backend dependency.
- Original LU files are parsed through `lu-assets`.
- Original client assets and FXO bytecode are never copied into this repo.
