#pragma once

#include "lu/renderer/math.h"

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
    LegoppEffect,
    LegoppNoAmbient,
    LegoppEmissive,
    TerrainRim,
    OceanDistort,
    OceanDistortDirectional,
    LegoppLighting,
    ClearPlastic
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

struct Vertex {
    Vec3 position;
    Vec3 normal = {0.0f, 1.0f, 0.0f};
    Vec2 uv;
    uint32_t color_rgba8 = 0xffffffffu;
};

struct MaterialAsset {
    std::string name;
    SurfaceModel surface_model = SurfaceModel::LegacyLU;
    LegacyShaderFamily shader_family = LegacyShaderFamily::LegoppLighting;
    int32_t lu_shader_id = -1;
    int32_t lu_shader_game_value = -1;
    std::string lu_shader_label;
    std::string lu_shader_source_file;
    std::string lu_shader_source_technique;
    ShaderPortStatus lu_shader_port_status = ShaderPortStatus::Unported;
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
    std::string lu_shader_reflection_map;
    std::string lu_shader_reflection_semantic;
    bool lu_shader_uses_uv_animation = false;
    bool lu_shader_uses_alpha_animation = false;
    Vec2 lu_uv_motion_layer1 = {0.0f, 0.0f};
    Vec2 lu_uv_motion_layer2 = {0.0f, 0.0f};
    RenderAlphaMode alpha_mode = RenderAlphaMode::Opaque;
    RenderCullMode cull_mode = RenderCullMode::Backface;
    Vec4 diffuse = {0.7f, 0.7f, 0.7f, 1.0f};
    Vec3 ambient = {0.25f, 0.25f, 0.25f};
    Vec3 emissive = {0.0f, 0.0f, 0.0f};
    std::string diffuse_texture_path;
    bool alpha_blend = false;
    bool alpha_test = false;
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
};

struct RenderWorld {
    std::string source_asset_path;
    std::vector<MeshAsset> meshes;
    EnvironmentState environment;
};

} // namespace lu::renderer
