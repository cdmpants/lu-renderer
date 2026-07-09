#pragma once

#include "lu/renderer/render_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace lu::renderer::lu_import {

struct LuShaderInfo {
    int32_t id = -1;
    int32_t game_value = -1;
    std::string label;
};

struct LuShaderPolicy {
    LegacyShaderFamily shader_family = LegacyShaderFamily::LegoppLighting;
    RenderAlphaMode alpha_mode = RenderAlphaMode::Opaque;
    RenderCullMode cull_mode = RenderCullMode::Backface;
    ShaderPortStatus port_status = ShaderPortStatus::Unported;
    std::string source_file;
    std::string source_technique;
    bool uses_vertex_color = false;
    bool uses_texture = true;
    bool uses_material_diffuse = false;
    bool uses_lighting = true;
    bool uses_fog = true;
    bool uses_specular = true;
    bool uses_reflection = true;
    std::string reflection_map;
    std::string reflection_semantic;
    bool uses_uv_animation = false;
    bool uses_alpha_animation = false;
    Vec2 uv_motion_layer1 = {0.0f, 0.0f};
    Vec2 uv_motion_layer2 = {0.0f, 0.0f};
};

struct ResolvedLuShader {
    LuShaderInfo shader;
    LuShaderPolicy policy;
    bool resolved = false;
    bool asset_is_multishader = false;
    std::optional<int32_t> multishader_prefix_id;
};

class ShaderDatabase {
public:
    static std::optional<ShaderDatabase> loadFromClientRoot(const std::filesystem::path& client_root);

    static std::filesystem::path normalizeClientRoot(std::filesystem::path root);
    static std::string normalizeAssetPath(std::string value);
    static std::string assetPathRelativeToRes(
        const std::filesystem::path& res_root,
        const std::filesystem::path& asset_path);

    ResolvedLuShader resolveAssetMeshShader(
        const std::string& asset_path,
        const std::string& mesh_name) const;

    std::optional<LuShaderInfo> shaderInfo(int32_t shader_id) const;

private:
    std::unordered_map<int32_t, LuShaderInfo> shader_by_id_;
    std::unordered_map<std::string, int32_t> shader_id_by_asset_;
};

LuShaderInfo fallbackShaderInfo();
LuShaderPolicy shaderPolicyFromInfo(const LuShaderInfo& shader);
LegacyShaderFamily shaderFamilyFromInfo(const LuShaderInfo& shader);
std::optional<int32_t> parseMultishaderPrefix(const std::string& mesh_name);

} // namespace lu::renderer::lu_import
