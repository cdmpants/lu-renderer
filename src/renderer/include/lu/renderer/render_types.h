#pragma once

#include "lu/renderer/math.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lu::renderer {

enum class SurfaceModel {
    LegacyLU,
    PBR
};

enum class RenderProfile {
    OriginalLU,
    OriginalPlus,
    Modern
};

enum class LegacyShaderFamily {
    LegacyMesh,
    Basic,
    BasicLit,
    AlphaAsAlpha,
    AlphaUvScroll,
    BasicTwoLayer,
    LegoppEffect,
    LegoppNoAmbient,
    LegoppEmissive,
    Metallic,
    TerrainRim,
    OceanDistort,
    OceanDistortDirectional,
    OceanDistortFx,
    OceanDistortUnlit,
    LegoppLighting,
    ClearPlastic
};

enum class LegoppShaderVariant {
    None,
    Base,
    NoAmbient,
    Emissive,
    SuperEmissive,
    Glow,
    GlowIgnoreVertAlpha,
    Grayscale,
    Darkling,
    DarklingSpecular,
    DarklingStructure,
    DarklingShinyGlint,
    DarklingSpecularShinyGlint,
    DarklingStructureShinyGlint,
    Item,
    ItemGlow,
    FrontEnd,
    MaskedNonDecal,
    Reveal,
    FadeUp,
    AnimUv,
    NoLight,
    FaceCreate,
    PetTamingCloud,
    ThreeLight,
    ShinyGlint
};

enum class ShaderPortStatus {
    Unported,
    Placeholder,
    Inferred,
    Verified
};

enum class RenderAlphaMode {
    Opaque,
    AlphaTest,
    AlphaBlend,
    Additive
};

enum class RenderCullMode {
    Backface,
    Clockwise,
    CounterClockwise,
    TwoSided
};

enum class ShaderResolutionSource {
    Unresolved,
    CdClientAsset,
    CdClientMultishaderPrefix,
    NifMultiShaderGameValue,
    NifMaterialName,
    NifFxShaderName,
    Fallback
};

enum class ShaderAlphaSemantic {
    Unknown,
    OutputAlpha,
    AlphaTest,
    ControlGlow,
    ControlEmissive,
    ControlDarkling,
    Ignored
};

struct DirectionalLight {
    Vec3 position = {1.0f, 1.0f, 1.0f};
    Vec3 direction = normalize({1.0f, 1.0f, 1.0f});
    Vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

struct EnvironmentState {
    Vec3 ambient = {0.1f, 0.1f, 0.1f};
    Vec3 specular = {0.3f, 0.3f, 0.3f};
    Vec3 upper_hemi = {1.0f, 1.0f, 1.0f};
    Vec3 lower_hemi = {1.0f, 1.0f, 1.0f};
    Vec3 fog_color = {0.75f, 0.88f, 0.91f};
    float fog_near = 0.0f;
    float fog_far = 0.0f;
    float post_fog_solid = 0.0f;
    float post_fog_fade = 0.0f;
    bool fog_enabled = false;
    DirectionalLight sun;
};

struct MsaaSettings {
    bool enabled = true;
    uint8_t samples = 4;
};

struct PostProcessSettings {
    bool taa_enabled = false;
    float taa_feedback = 0.88f;
    float taa_jitter = 1.0f;
    bool vignette_enabled = false;
    float vignette_strength = 0.25f;
    bool color_lut_enabled = false;
    float color_lut_intensity = 1.0f;
    std::string color_lut_path;
    bool bloom_enabled = false;
    float bloom_threshold = 0.85f;
    float bloom_intensity = 0.35f;
    bool dof_enabled = false;
    float dof_focus_distance = 15.0f;
    float dof_aperture = 0.15f;
    bool film_grain_enabled = false;
    float film_grain_strength = 0.025f;
};

struct ScreenSpaceSettings {
    bool ssr_enabled = false;
    float ssr_strength = 0.5f;
    float ssr_max_distance = 40.0f;
    float ssr_thickness = 0.025f;
    bool gtao_enabled = false;
    float gtao_radius = 1.5f;
    float gtao_intensity = 1.0f;
};

struct ShadowSettings {
    bool directional_shadows_enabled = false;
    float pcss_light_radius = 0.08f;
    float pcss_bias = 0.001f;
    float pcss_normal_bias = 2.0f;
    float pcss_slope_bias = 4.0f;
};

struct ReflectionProbeSettings {
    bool enabled = false;
    float intensity = 1.0f;
};

struct PbrBrdfSettings {
    float roughness = 0.38f;
    float metallic = 0.0f;
    float specular_intensity = 1.0f;
};

struct RenderFeatureSettings {
    RenderProfile profile = RenderProfile::OriginalPlus;
    SurfaceModel lego_surface_model = SurfaceModel::LegacyLU;
    PbrBrdfSettings pbr;
    MsaaSettings msaa;
    ScreenSpaceSettings screen_space;
    PostProcessSettings post;
    ShadowSettings shadows;
    ReflectionProbeSettings reflection_probe;
};

struct Vertex {
    Vec3 position;
    Vec3 normal = {0.0f, 1.0f, 0.0f};
    Vec2 uv;
    Vec2 uv2;
    uint32_t color_rgba8 = 0xffffffffu;
    std::array<uint16_t, 4> bone_indices = {0, 0, 0, 0};
    std::array<float, 4> bone_weights = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct Vec3Key {
    float time = 0.0f;
    Vec3 value = {0.0f, 0.0f, 0.0f};
    Vec3 forward_tangent = {0.0f, 0.0f, 0.0f};
    Vec3 backward_tangent = {0.0f, 0.0f, 0.0f};
};

struct TextureAddressMode {
    bool authored = false;
    bool wrap_u = true;
    bool wrap_v = true;
};

struct MaterialAsset {
    std::string name;
    SurfaceModel surface_model = SurfaceModel::LegacyLU;
    LegacyShaderFamily shader_family = LegacyShaderFamily::LegoppLighting;
    LegoppShaderVariant legopp_variant = LegoppShaderVariant::None;
    int32_t lu_shader_id = -1;
    int32_t lu_shader_game_value = -1;
    std::string lu_shader_label;
    ShaderResolutionSource lu_shader_resolution_source = ShaderResolutionSource::Unresolved;
    std::string lu_shader_metadata;
    std::string lu_shader_source_file;
    std::string lu_shader_source_technique;
    std::string lu_shader_source_status_note;
    std::string lu_shader_validation_status_note;
    ShaderPortStatus lu_shader_port_status = ShaderPortStatus::Unported;
    ShaderAlphaSemantic lu_shader_alpha_semantic = ShaderAlphaSemantic::Unknown;
    bool lu_shader_resolved = false;
    bool lu_shader_asset_is_multishader = false;
    int32_t lu_multishader_prefix_id = -1;
    bool mesh_has_vertex_colors = false;
    bool lu_shader_uses_vertex_color = false;
    bool lu_shader_uses_texture = true;
    bool lu_shader_uses_material_diffuse = false;
    bool lu_shader_uses_lighting = true;
    bool lu_shader_uses_fog = true;
    bool lu_shader_uses_specular = true;
    bool lu_shader_uses_reflection = true;
    bool lu_shader_uses_shadow_terrain = true;
    std::string lu_shader_reflection_map;
    std::string lu_shader_reflection_semantic;
    bool lu_shader_uses_uv_animation = false;
    bool lu_shader_uses_alpha_animation = false;
    bool nif_has_material_color_controller = false;
    bool material_emissive_controller = false;
    float material_emissive_controller_frequency = 1.0f;
    float material_emissive_controller_phase = 0.0f;
    float material_emissive_controller_start = 0.0f;
    float material_emissive_controller_stop = 0.0f;
    Vec3 material_emissive_controller_default = {0.0f, 0.0f, 0.0f};
    std::vector<Vec3Key> material_emissive_controller_keys;
    Vec2 lu_uv_motion_layer1 = {0.0f, 0.0f};
    Vec2 lu_uv_motion_layer2 = {0.0f, 0.0f};
    Vec4 lu_glow_color = {0.0f, 1.0f, 1.0f, 1.0f};
    float lu_glow_lightness = 1.0f;
    float lu_grayscale_lerp = 1.0f;
    float lu_grayscale_lightness = 0.2f;
    float lu_fade_up_height = 0.0f;
    float lu_shiny_glint_height = 0.0f;
    float lu_shiny_glint_size_power = 0.0f;
    Vec4 lu_shiny_glint_color = {1.0f, 1.0f, 1.0f, 1.0f};
    RenderAlphaMode alpha_mode = RenderAlphaMode::Opaque;
    RenderCullMode cull_mode = RenderCullMode::Backface;
    bool depth_write = true;
    Vec4 diffuse = {0.7f, 0.7f, 0.7f, 1.0f};
    Vec3 ambient = {0.25f, 0.25f, 0.25f};
    Vec3 emissive = {0.0f, 0.0f, 0.0f};
    std::string diffuse_texture_path;
    TextureAddressMode diffuse_texture_address;
    std::string dark_texture_path;
    TextureAddressMode dark_texture_address;
    std::string detail_texture_path;
    TextureAddressMode detail_texture_address;
    std::string gloss_texture_path;
    TextureAddressMode gloss_texture_address;
    std::string glow_texture_path;
    TextureAddressMode glow_texture_address;
    bool alpha_blend = false;
    bool alpha_test = false;
    bool has_alpha_property = false;
    uint16_t alpha_flags = 0;
    uint8_t alpha_threshold = 0;
};

struct MeshAsset {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    MaterialAsset material;
    bool has_lod_range = false;
    uint32_t lod_parent_block = 0;
    uint32_t lod_level = 0;
    float lod_near = 0.0f;
    float lod_far = 0.0f;
    Vec3 lod_center = {0.0f, 0.0f, 0.0f};
    bool is_skinned = false;
    uint32_t skin_instance_block = 0;
    int32_t skeleton_root_block = -1;
    std::vector<int32_t> skin_bone_node_blocks;
    std::vector<std::string> skin_bone_names;
};

struct AnimationTextKey {
    float time = 0.0f;
    std::string text;
};

struct AnimationControlledBlock {
    std::string node_name;
    std::string controller_type;
    std::string property_type;
    std::string controller_id;
    std::string interpolator_id;
    int32_t interpolator_ref = -1;
    int32_t controller_ref = -1;
    uint8_t priority = 0;
};

struct AnimationClip {
    std::string name;
    std::string source_path;
    uint32_t sequence_id = 0;
    uint32_t anim_index = 0;
    float start_time = 0.0f;
    float stop_time = 0.0f;
    float frequency = 1.0f;
    uint32_t cycle_type = 0;
    std::vector<AnimationControlledBlock> controlled_blocks;
    std::vector<AnimationTextKey> text_keys;
};

struct AnimationAsset {
    std::string source_path;
    std::string model_path;
    std::string model_root;
    std::vector<AnimationClip> clips;
};

struct RenderWorld {
    std::string source_asset_path;
    std::vector<MeshAsset> meshes;
    AnimationAsset animation;
    EnvironmentState environment;
};

} // namespace lu::renderer
