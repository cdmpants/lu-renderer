#include "lu/renderer/lu_import/shader_database.h"

#include "netdevil/database/fdb/fdb_reader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace lu::renderer::lu_import {

namespace {

constexpr int32_t kLegoShaderId = 1;
constexpr int32_t kMultishaderId = 100;

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) return {};
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::optional<int32_t> findColumn(const lu::assets::FdbTable& table, const std::string& name) {
    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (table.columns[i].name == name) {
            return static_cast<int32_t>(i);
        }
    }
    return std::nullopt;
}

const lu::assets::FdbValue* fieldAt(const lu::assets::FdbRow& row, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= row.fields.size()) return nullptr;
    return &row.fields[static_cast<size_t>(index)];
}

std::optional<int32_t> intField(const lu::assets::FdbRow& row, int32_t index) {
    const lu::assets::FdbValue* value = fieldAt(row, index);
    if (!value) return std::nullopt;
    if (const auto* v = std::get_if<int32_t>(value)) return *v;
    if (const auto* v = std::get_if<int64_t>(value)) return static_cast<int32_t>(*v);
    return std::nullopt;
}

std::optional<std::string> stringField(const lu::assets::FdbRow& row, int32_t index) {
    const lu::assets::FdbValue* value = fieldAt(row, index);
    if (!value) return std::nullopt;
    if (const auto* v = std::get_if<std::string>(value)) return *v;
    return std::nullopt;
}

} // namespace

std::optional<ShaderDatabase> ShaderDatabase::loadFromClientRoot(const std::filesystem::path& client_root) {
    std::filesystem::path res_root = normalizeClientRoot(client_root);
    if (res_root.empty()) return std::nullopt;

    std::vector<uint8_t> data = readFile(res_root / "cdclient.fdb");
    if (data.empty()) return std::nullopt;

    ShaderDatabase database;
    auto tables = lu::assets::fdb_parse_full(std::span<const uint8_t>(data.data(), data.size()));
    for (const auto& table : tables) {
        if (table.name == "mapShaders") {
            auto id_col = findColumn(table, "id");
            auto label_col = findColumn(table, "label");
            auto game_value_col = findColumn(table, "gameValue");
            if (!id_col || !label_col || !game_value_col) continue;

            for (const auto& row : table.rows) {
                auto id = intField(row, *id_col);
                auto label = stringField(row, *label_col);
                auto game_value = intField(row, *game_value_col);
                if (!id) continue;

                LuShaderInfo info;
                info.id = *id;
                info.game_value = game_value.value_or(-1);
                info.label = label.value_or(std::string{});
                database.shader_by_id_.emplace(info.id, std::move(info));
            }
        } else if (table.name == "RenderComponent") {
            auto asset_col = findColumn(table, "render_asset");
            auto shader_col = findColumn(table, "shader_id");
            if (!asset_col || !shader_col) continue;

            for (const auto& row : table.rows) {
                auto asset = stringField(row, *asset_col);
                auto shader_id = intField(row, *shader_col);
                if (!asset || !shader_id) continue;

                database.shader_id_by_asset_.emplace(normalizeAssetPath(*asset), *shader_id);
            }
        }
    }

    return database;
}

std::filesystem::path ShaderDatabase::normalizeClientRoot(std::filesystem::path root) {
    if (root.empty()) return {};
    if (root.filename() == "res") return root;
    std::filesystem::path res = root / "res";
    if (std::filesystem::exists(res)) return res;
    return root;
}

std::string ShaderDatabase::normalizeAssetPath(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    value = lowerCopy(value);
    while (value.rfind(".\\", 0) == 0) value.erase(0, 2);
    while (value.rfind("..\\", 0) == 0) value.erase(0, 3);
    std::string collapsed;
    collapsed.reserve(value.size());
    bool previous_was_separator = false;
    for (char c : value) {
        const bool is_separator = c == '\\';
        if (!is_separator || !previous_was_separator) {
            collapsed.push_back(c);
        }
        previous_was_separator = is_separator;
    }
    value = std::move(collapsed);
    return value;
}

std::string ShaderDatabase::assetPathRelativeToRes(
    const std::filesystem::path& res_root,
    const std::filesystem::path& asset_path) {
    if (res_root.empty() || asset_path.empty()) return {};

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(asset_path, res_root, ec);
    if (ec || relative.empty() || relative.string().starts_with("..")) {
        relative = asset_path;
    }
    return normalizeAssetPath(relative.string());
}

ResolvedLuShader ShaderDatabase::resolveAssetMeshShader(
    const std::string& asset_path,
    const std::string& mesh_name) const {
    ResolvedLuShader resolved;
    resolved.shader = fallbackShaderInfo();
    resolved.policy = shaderPolicyFromInfo(resolved.shader);

    auto asset_it = shader_id_by_asset_.find(normalizeAssetPath(asset_path));
    if (asset_it == shader_id_by_asset_.end()) {
        return resolved;
    }

    int32_t shader_id = asset_it->second;
    resolved.asset_is_multishader = shader_id == kMultishaderId;
    if (resolved.asset_is_multishader) {
        resolved.multishader_prefix_id = parseMultishaderPrefix(mesh_name);
        if (!resolved.multishader_prefix_id) {
            return resolved;
        }
        shader_id = *resolved.multishader_prefix_id;
    }

    auto shader = shaderInfo(shader_id);
    if (!shader) {
        return resolved;
    }

    resolved.shader = *shader;
    resolved.policy = shaderPolicyFromInfo(resolved.shader);
    resolved.resolved = !resolved.asset_is_multishader || resolved.multishader_prefix_id.has_value();
    return resolved;
}

std::optional<LuShaderInfo> ShaderDatabase::shaderInfo(int32_t shader_id) const {
    auto it = shader_by_id_.find(shader_id);
    if (it == shader_by_id_.end()) return std::nullopt;
    return it->second;
}

LuShaderInfo fallbackShaderInfo() {
    return {kLegoShaderId, 5, "LEGO"};
}

LuShaderPolicy shaderPolicyFromInfo(const LuShaderInfo& shader) {
    LuShaderPolicy policy;
    policy.source_file = "unknown";
    policy.source_technique = "unknown";
    policy.port_status = ShaderPortStatus::Unported;

    switch (shader.id) {
    case 0:
    case 20:
    case 45:
    case 99:
        policy.shader_family = LegacyShaderFamily::LegacyMesh;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_Material_NoLighting";
        policy.uses_material_diffuse = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Placeholder;
        break;
    case 1:
        policy.shader_family = LegacyShaderFamily::LegoppLighting;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK";
        policy.uses_material_diffuse = true;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 25:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor_NoTexture";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 27:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 30:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 29:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor_NoTexture";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 72:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "inferred";
        policy.source_technique = "Opaque NL VC NT NoFog";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 74:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "inferred";
        policy.source_technique = "Opaque NL VC NoFog";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 84:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting";
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 3:
        policy.shader_family = LegacyShaderFamily::ClearPlastic;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::Clockwise;
        policy.source_file = "ClearPlastic.fx";
        policy.source_technique = "Technique_ClearPlastic";
        policy.uses_texture = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 5:
    case 7:
        policy.shader_family = LegacyShaderFamily::AlphaAsAlpha;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaBlend";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 47:
        policy.shader_family = LegacyShaderFamily::AlphaAsAlpha;
        policy.alpha_mode = RenderAlphaMode::AlphaTest;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaTest";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 48:
    case 49:
        policy.shader_family = LegacyShaderFamily::AlphaAsAlpha;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaBlend_NoTexture";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 17:
        policy.shader_family = LegacyShaderFamily::TerrainRim;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.source_file = "TerrainDiffuse.fx";
        policy.source_technique = "Technique_TerrainMeshLighting_Rim";
        policy.uses_vertex_color = true;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 46:
        policy.shader_family = LegacyShaderFamily::LegoppEmissive;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Emissive";
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 60:
        policy.shader_family = LegacyShaderFamily::OceanDistort;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.uv_motion_layer1 = {0.018f, 0.006f};
        policy.uv_motion_layer2 = {-0.011f, 0.014f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 61:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 64:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 77:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.alpha_mode = RenderAlphaMode::Additive;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Placeholder;
        break;
    case 78:
        policy.shader_family = LegacyShaderFamily::LegoppNoAmbient;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_NoAmbient";
        policy.uses_material_diffuse = true;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 79:
        policy.shader_family = LegacyShaderFamily::OceanDistortDirectional;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_Directional_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.uv_motion_layer1 = {0.026f, 0.012f};
        policy.uv_motion_layer2 = {-0.014f, 0.021f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    default:
        break;
    }

    if (policy.uses_reflection && policy.reflection_map.empty()) {
        policy.reflection_semantic = "glow";
        policy.reflection_map = "default_reflection.dds";
    }

    switch (shader.id) {
    case 48:
    case 49:
        policy.cull_mode = RenderCullMode::Backface;
        break;
    default:
        break;
    }

    return policy;
}

LegacyShaderFamily shaderFamilyFromInfo(const LuShaderInfo& shader) {
    return shaderPolicyFromInfo(shader).shader_family;
}

std::optional<int32_t> parseMultishaderPrefix(const std::string& mesh_name) {
    if (mesh_name.size() < 3) return std::nullopt;
    if (mesh_name[0] != 'S' && mesh_name[0] != 's') return std::nullopt;

    size_t index = 1;
    int32_t shader_id = 0;
    bool has_digit = false;
    while (index < mesh_name.size() && std::isdigit(static_cast<unsigned char>(mesh_name[index]))) {
        has_digit = true;
        shader_id = shader_id * 10 + (mesh_name[index] - '0');
        ++index;
    }

    if (!has_digit || index >= mesh_name.size() || mesh_name[index] != '_') {
        return std::nullopt;
    }
    return shader_id;
}

} // namespace lu::renderer::lu_import
