#pragma once

#include "lu/renderer/render_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lu::renderer::lu_import {

struct LuShaderInfo {
    int32_t id = -1;
    int32_t game_value = -1;
    std::string label;
};

struct LuShaderPolicy {
    LegacyShaderFamily shader_family = LegacyShaderFamily::LegoppLighting;
    LegoppShaderVariant legopp_variant = LegoppShaderVariant::None;
    RenderAlphaMode alpha_mode = RenderAlphaMode::Opaque;
    RenderCullMode cull_mode = RenderCullMode::Backface;
    bool depth_write = true;
    bool force_alpha_blend = false;
    bool force_alpha_test = false;
    uint8_t alpha_threshold = 0;
    ShaderPortStatus port_status = ShaderPortStatus::Unported;
    ShaderAlphaSemantic alpha_semantic = ShaderAlphaSemantic::Unknown;
    std::string source_file;
    std::string source_technique;
    std::string source_status_note;
    std::string validation_status_note;
    bool uses_vertex_color = false;
    bool uses_texture = true;
    bool uses_material_diffuse = false;
    bool uses_lighting = true;
    bool uses_fog = true;
    bool uses_specular = true;
    bool uses_reflection = true;
    bool uses_shadow_terrain = true;
    std::string reflection_map;
    std::string reflection_semantic;
    bool uses_uv_animation = false;
    bool uses_alpha_animation = false;
    Vec2 uv_motion_layer1 = {0.0f, 0.0f};
    Vec2 uv_motion_layer2 = {0.0f, 0.0f};
    Vec4 glow_color = {0.0f, 1.0f, 1.0f, 1.0f};
    float glow_lightness = 1.0f;
    float grayscale_lerp = 1.0f;
    float grayscale_lightness = 0.2f;
    float fade_up_height = 0.0f;
    float shiny_glint_height = 0.0f;
    float shiny_glint_size_power = 0.0f;
    Vec4 shiny_glint_color = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct ResolvedLuShader {
    LuShaderInfo shader;
    LuShaderPolicy policy;
    bool resolved = false;
    ShaderResolutionSource resolution_source = ShaderResolutionSource::Unresolved;
    std::string metadata;
    bool asset_is_multishader = false;
    std::optional<int32_t> multishader_prefix_id;
};

class ShaderDatabase {
public:
    static std::optional<ShaderDatabase> loadFromClientRoot(const std::filesystem::path& client_root);
    static ShaderDatabase fromRecords(
        const std::vector<LuShaderInfo>& shaders,
        const std::vector<std::pair<std::string, int32_t>>& asset_shaders = {});

    static std::filesystem::path normalizeClientRoot(std::filesystem::path root);
    static std::string normalizeAssetPath(std::string value);
    static std::string assetPathRelativeToRes(
        const std::filesystem::path& res_root,
        const std::filesystem::path& asset_path);

    ResolvedLuShader resolveAssetMeshShader(
        const std::string& asset_path,
        const std::string& mesh_name) const;

    std::optional<LuShaderInfo> shaderInfo(int32_t shader_id) const;
    std::optional<LuShaderInfo> shaderInfoByGameValue(int32_t game_value) const;
    ResolvedLuShader resolveNifMaterialShader(const std::string& material_name) const;
    std::vector<LuShaderInfo> shaders() const;
    std::vector<std::string> assetPathsForShader(int32_t shader_id) const;

private:
    std::unordered_map<int32_t, LuShaderInfo> shader_by_id_;
    std::unordered_map<int32_t, int32_t> shader_id_by_game_value_;
    std::unordered_map<std::string, int32_t> shader_id_by_asset_;
};

LuShaderInfo fallbackShaderInfo();
LuShaderPolicy shaderPolicyFromInfo(const LuShaderInfo& shader);
LuShaderPolicy applyFxShaderMetadataPolicyOverrides(
    LuShaderPolicy policy,
    const std::string& material_name);
LegacyShaderFamily shaderFamilyFromInfo(const LuShaderInfo& shader);
std::optional<int32_t> parseMultishaderPrefix(const std::string& mesh_name);
std::optional<int32_t> parseNiMultiShaderGameValue(const std::string& material_name);
std::optional<int32_t> inferShaderIdFromFxShaderMetadata(const std::string& material_name);
bool isLegoppFrontendAlphaTestTechnique(const std::string& source_technique);

} // namespace lu::renderer::lu_import
