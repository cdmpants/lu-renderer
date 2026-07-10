#include "lu/renderer/lu_import/shader_database.h"

#include "netdevil/database/fdb/fdb_reader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lu::renderer::lu_import {

namespace {

constexpr int32_t kLegoShaderId = 1;
constexpr int32_t kMultishaderId = 100;
constexpr int32_t kDarklingShaderId = 65;

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

bool containsCaseInsensitive(const std::string& haystack, std::string_view needle) {
    return lowerCopy(haystack).find(lowerCopy(std::string(needle))) != std::string::npos;
}

std::string compactShaderToken(std::string_view value) {
    std::string compact;
    compact.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            compact.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return compact;
}

std::optional<std::string> legoppFxSourceTechniqueFromMaterialName(const std::string& material_name) {
    constexpr std::string_view kMarker = "fxshader_";
    std::string lower = lowerCopy(material_name);
    size_t pos = lower.find(kMarker);
    if (pos == std::string::npos) return std::nullopt;

    std::string token = material_name.substr(pos + kMarker.size());
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
        token.erase(token.begin());
    }

    size_t end = 0;
    while (end < token.size()) {
        unsigned char c = static_cast<unsigned char>(token[end]);
        if (!std::isalnum(c) && c != '_') break;
        ++end;
    }
    token.resize(end);

    if (token.empty() || !containsCaseInsensitive(token, "legopplighting")) {
        return std::nullopt;
    }
    if (containsCaseInsensitive(token, "technique_")) {
        return token;
    }
    return "Technique_" + token;
}

bool sourceTechniqueHasToken(const std::string& lower_technique, std::string_view token) {
    return lower_technique.find(lowerCopy(std::string(token))) != std::string::npos;
}

void applyLegoppSourceTechniqueFlags(
    LuShaderPolicy& policy,
    const std::string& source_technique) {
    policy.source_technique = source_technique;

    const std::string lower = lowerCopy(source_technique);
    const bool darkling = sourceTechniqueHasToken(lower, "darkling") ||
        sourceTechniqueHasToken(lower, "darking");
    const bool has_vertex_color = sourceTechniqueHasToken(lower, "vertcolor");
    const bool has_texture = sourceTechniqueHasToken(lower, "textured") || darkling;

    policy.uses_vertex_color = has_vertex_color;
    policy.uses_texture = has_texture;
    policy.uses_material_diffuse = !has_vertex_color && !has_texture;

    const bool no_light =
        sourceTechniqueHasToken(lower, "_nl") ||
        lower.size() >= 3 && lower.rfind("_nl") == lower.size() - 3;
    const bool textured_no_light = sourceTechniqueHasToken(lower, "textured_nl");
    if (no_light) {
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_mode = textured_no_light ? RenderAlphaMode::AlphaBlend : RenderAlphaMode::Opaque;
    }

    policy.uses_uv_animation =
        textured_no_light ||
        sourceTechniqueHasToken(lower, "animuv") ||
        sourceTechniqueHasToken(lower, "reveal") ||
        sourceTechniqueHasToken(lower, "emissive");

    if (!policy.uses_reflection) {
        policy.reflection_map.clear();
        policy.reflection_semantic.clear();
    }
}

ResolvedLuShader unresolvedShader() {
    ResolvedLuShader resolved;
    resolved.shader = fallbackShaderInfo();
    resolved.policy = shaderPolicyFromInfo(resolved.shader);
    resolved.resolution_source = ShaderResolutionSource::Fallback;
    return resolved;
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
                database.shader_id_by_game_value_.emplace(info.game_value, info.id);
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

ShaderDatabase ShaderDatabase::fromRecords(
    const std::vector<LuShaderInfo>& shaders,
    const std::vector<std::pair<std::string, int32_t>>& asset_shaders) {
    ShaderDatabase database;
    for (const LuShaderInfo& shader : shaders) {
        database.shader_id_by_game_value_.emplace(shader.game_value, shader.id);
        database.shader_by_id_.emplace(shader.id, shader);
    }
    for (const auto& [asset_path, shader_id] : asset_shaders) {
        database.shader_id_by_asset_.emplace(normalizeAssetPath(asset_path), shader_id);
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
    const std::string& mesh_name,
    const std::string& parent_object_name) const {
    ResolvedLuShader resolved = unresolvedShader();

    auto asset_it = shader_id_by_asset_.find(normalizeAssetPath(asset_path));
    if (asset_it == shader_id_by_asset_.end()) {
        return resolved;
    }

    int32_t shader_id = asset_it->second;
    resolved.asset_is_multishader = shader_id == kMultishaderId;
    if (resolved.asset_is_multishader) {
        resolved.multishader_prefix_id = parseMultishaderPrefix(mesh_name);
        if (!resolved.multishader_prefix_id && !parent_object_name.empty()) {
            resolved.multishader_prefix_id = parseMultishaderPrefix(parent_object_name);
        }
        if (!resolved.multishader_prefix_id) {
            return resolved;
        }
        shader_id = *resolved.multishader_prefix_id;
        resolved.resolution_source = ShaderResolutionSource::CdClientMultishaderPrefix;
    } else {
        resolved.resolution_source = ShaderResolutionSource::CdClientAsset;
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

std::optional<LuShaderInfo> ShaderDatabase::shaderInfoByGameValue(int32_t game_value) const {
    auto game_it = shader_id_by_game_value_.find(game_value);
    if (game_it == shader_id_by_game_value_.end()) return std::nullopt;
    return shaderInfo(game_it->second);
}

std::vector<LuShaderInfo> ShaderDatabase::shaders() const {
    std::vector<LuShaderInfo> shaders;
    shaders.reserve(shader_by_id_.size());
    for (const auto& [_, shader] : shader_by_id_) {
        shaders.push_back(shader);
    }
    std::sort(shaders.begin(), shaders.end(), [](const LuShaderInfo& a, const LuShaderInfo& b) {
        return a.id < b.id;
    });
    return shaders;
}

std::vector<std::string> ShaderDatabase::assetPathsForShader(int32_t shader_id) const {
    std::vector<std::string> assets;
    for (const auto& [asset_path, asset_shader_id] : shader_id_by_asset_) {
        if (asset_shader_id == shader_id) {
            assets.push_back(asset_path);
        }
    }
    std::sort(assets.begin(), assets.end());
    return assets;
}

ResolvedLuShader ShaderDatabase::resolveNifMaterialShader(const std::string& material_name) const {
    ResolvedLuShader resolved = unresolvedShader();
    if (material_name.empty()) return resolved;

    if (auto game_value = parseNiMultiShaderGameValue(material_name)) {
        resolved.metadata = material_name;
        resolved.resolution_source = ShaderResolutionSource::NifMultiShaderGameValue;
        if (auto shader = shaderInfoByGameValue(*game_value)) {
            resolved.shader = *shader;
            resolved.policy = shaderPolicyFromInfo(resolved.shader);
            resolved.resolved = true;
        }
        return resolved;
    }

    if (containsCaseInsensitive(material_name, "darkling") &&
        containsCaseInsensitive(material_name, "_nims")) {
        resolved.metadata = material_name;
        resolved.resolution_source = ShaderResolutionSource::NifMaterialName;
        if (auto shader = shaderInfo(kDarklingShaderId)) {
            resolved.shader = *shader;
            resolved.policy = shaderPolicyFromInfo(resolved.shader);
            resolved.resolved = true;
        }
        return resolved;
    }

    if (containsCaseInsensitive(material_name, "fxshader_")) {
        resolved.metadata = material_name;
        resolved.resolution_source = ShaderResolutionSource::NifFxShaderName;
        if (auto shader_id = inferShaderIdFromFxShaderMetadata(material_name)) {
            if (auto shader = shaderInfo(*shader_id)) {
                resolved.shader = *shader;
                resolved.policy = shaderPolicyFromInfo(resolved.shader);
                resolved.policy = applyFxShaderMetadataPolicyOverrides(
                    std::move(resolved.policy),
                    material_name);
                resolved.resolved = true;
            }
        }
    }

    return resolved;
}

LuShaderInfo fallbackShaderInfo() {
    return {kLegoShaderId, 5, "LEGO"};
}

LuShaderPolicy shaderPolicyFromInfo(const LuShaderInfo& shader) {
    LuShaderPolicy policy;
    policy.source_file = "unknown";
    policy.source_technique = "unknown";
    policy.port_status = ShaderPortStatus::Unported;
    policy.alpha_semantic = ShaderAlphaSemantic::Ignored;

    switch (shader.id) {
    case 0:
    case 20:
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
    case 9:
        policy.shader_family = LegacyShaderFamily::LegoppLighting;
        policy.legopp_variant = LegoppShaderVariant::Base;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 6:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::Reveal;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Reveal";
        policy.uses_material_diffuse = true;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 11:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::FrontEnd;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::Clockwise;
        policy.source_file = "LEGOPPLighting_FrontEnd.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_FrontEnd";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 14:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::MaskedNonDecal;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Masked_NonDecal";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 19:
        policy.shader_family = LegacyShaderFamily::LegoppEmissive;
        policy.legopp_variant = LegoppShaderVariant::SuperEmissive;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_SuperEmissive";
        policy.uses_material_diffuse = true;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlEmissive;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 21:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::AnimUv;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_AnimUV";
        policy.uses_material_diffuse = true;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 33:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "inferred";
        policy.source_technique = "VertColor_Alpha_Fade";
        policy.source_status_note =
            "No VertColor_Alpha_Fade technique found in original shader dump; real NIFs reference NiMultiShader9 metadata.";
        policy.uses_vertex_color = true;
        policy.uses_lighting = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 23:
        policy.shader_family = LegacyShaderFamily::LegoppLighting;
        policy.legopp_variant = LegoppShaderVariant::Item;
        policy.source_file = "LEGOPPLighting_Item.fx";
        policy.source_technique = "Technique_LEGOPPLighting_Item";
        policy.uses_material_diffuse = true;
        policy.uses_shadow_terrain = false;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 26:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting";
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
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
    case 28:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor_UVScrolling_SimpleV";
        policy.source_status_note =
            "No non-vertex-color Basic NoLighting UVScrolling technique found in original shader dump; using the available no-light textured UV-scroll Basic source path.";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
        policy.port_status = ShaderPortStatus::Inferred;
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
    case 31:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor_UVScrolling_SimpleV";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
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
    case 70:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_Material_NoLighting_NoTexture";
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 71:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "inferred";
        policy.source_technique = "ScrollingUV NL AnimAlpha NoFog";
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Inferred;
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
    case 73:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.source_file = "inferred";
        policy.source_technique = "Opaque NL NoFog";
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
    case 75:
        policy.shader_family = LegacyShaderFamily::BasicLit;
        policy.source_file = "inferred";
        policy.source_technique = "Opaque VC NT NoFog";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 76:
        policy.shader_family = LegacyShaderFamily::BasicLit;
        policy.source_file = "inferred";
        policy.source_technique = "Opaque VC NoFog";
        policy.uses_vertex_color = true;
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
        policy.depth_write = false;
        policy.source_file = "ClearPlastic.fx";
        policy.source_technique = "Technique_ClearPlastic";
        policy.uses_texture = false;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 5:
    case 7:
    case 22:
    case 50:
        policy.shader_family = LegacyShaderFamily::AlphaAsAlpha;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.depth_write = false;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_NoLighting_VertColor_AlphaBlend";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        if (shader.id == 22) {
            policy.validation_status_note =
                "Validated via S22__ multishader meshes in mesh\\env\\env_nstrack_trackmesh.nif.";
        }
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
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::AlphaTest;
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
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 52:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlphaSkinned_UVScrolling_SimpleV_NoLighting_AlphaAnim";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 57:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "AlphaAsAlpha.fx";
        policy.source_technique = "Technique_AlphaAsAlpha_UVScrolling_SimpleV_NoLighting_AlphaAnim";
        policy.validation_status_note =
            "No resolved CDClient, multishader-prefix, or shader-metadata NIF users found in current client dump.";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.0f, 0.16f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 17:
        policy.shader_family = LegacyShaderFamily::TerrainRim;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "TerrainDiffuse.fx";
        policy.source_technique = "Technique_TerrainMeshLighting_Rim";
        policy.uses_vertex_color = true;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 46:
        policy.shader_family = LegacyShaderFamily::LegoppEmissive;
        policy.legopp_variant = LegoppShaderVariant::Emissive;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Emissive";
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlEmissive;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 37:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::FaceCreate;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::Clockwise;
        policy.force_alpha_blend = true;
        policy.force_alpha_test = true;
        policy.alpha_threshold = 127;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLighting_FaceCreate";
        policy.uses_vertex_color = true;
        policy.alpha_semantic = ShaderAlphaSemantic::AlphaTest;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 38:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::Glow;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Glow";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; metadata candidates import with no shader-38 mesh.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlGlow;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 39:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::Grayscale;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Grayscale";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S39_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 40:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::GlowIgnoreVertAlpha;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_Glow_IgnoreVertAlpha";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; metadata candidates import with no shader-40 mesh.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlGlow;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 41:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::ItemGlow;
        policy.source_file = "LEGOPPLighting_Item.fx";
        policy.source_technique = "Technique_LEGOPPLighting_Item_Glow";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S41_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.uses_shadow_terrain = false;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlGlow;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 45:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::NoLight;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingTextured_NL";
        policy.uses_material_diffuse = false;
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 43:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::FadeUp;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_FadeUp";
        policy.validation_status_note =
            "No resolved CDClient, multishader-prefix, or shader-metadata NIF users found in current client dump.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 60:
        policy.shader_family = LegacyShaderFamily::OceanDistort;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.018f, 0.006f};
        policy.uv_motion_layer2 = {-0.011f, 0.014f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 61:
        policy.shader_family = LegacyShaderFamily::AlphaUvScroll;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.cull_mode = RenderCullMode::TwoSided;
        policy.depth_write = false;
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
        policy.depth_write = false;
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
        policy.depth_write = false;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_Basic_NoLighting_VertColor";
        policy.source_status_note =
            "No additive static-mesh no-light vertex-color technique found in original shader dump; CDClient has zero direct users.";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.port_status = ShaderPortStatus::Placeholder;
        break;
    case 78:
        policy.shader_family = LegacyShaderFamily::LegoppNoAmbient;
        policy.legopp_variant = LegoppShaderVariant::NoAmbient;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_NoAmbient";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 58:
        policy.shader_family = LegacyShaderFamily::LegoppEmissive;
        policy.legopp_variant = LegoppShaderVariant::Emissive;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingVertColorTextured_Emissive";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = false;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlEmissive;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 63:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::ShinyGlint;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_ShinyGlint";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S63_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 65:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::Darkling;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingVertColor_Darkling";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlDarkling;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 66:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::DarklingSpecular;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingVertColor_Darkling_Specular";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = false;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlDarkling;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 67:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::DarklingStructure;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingVertColor_Darkling_NonDecal";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::ControlDarkling;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 82:
        policy.shader_family = LegacyShaderFamily::Basic;
        policy.legopp_variant = LegoppShaderVariant::PetTamingCloud;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "PostProcessingShaders.fx";
        policy.source_technique = "Technique_ImaginationCloud";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_fog = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 83:
        policy.shader_family = LegacyShaderFamily::BasicTwoLayer;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_TwoLayersAdded_NoLighting_VertColor_UVScrolling";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 88:
        policy.shader_family = LegacyShaderFamily::Metallic;
        policy.source_file = "Metallic.fx";
        policy.source_technique = "Technique_Lighting_PolishedMetal";
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.uses_specular = true;
        policy.uses_reflection = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.reflection_semantic = "metal";
        policy.reflection_map = "metal_reflection_polished.dds";
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 89:
        policy.shader_family = LegacyShaderFamily::Metallic;
        policy.source_file = "Metallic.fx";
        policy.source_technique = "Technique_Lighting_BrushedSteel";
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.uses_specular = true;
        policy.uses_reflection = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.reflection_semantic = "metal";
        policy.reflection_map = "metal_reflection_brushed.dds";
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 90:
        policy.shader_family = LegacyShaderFamily::Metallic;
        policy.source_file = "Metallic.fx";
        policy.source_technique = "Technique_Lighting_BrushedSteel_VertColor_Item";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_material_diffuse = true;
        policy.uses_specular = true;
        policy.uses_reflection = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.reflection_semantic = "metal";
        policy.reflection_map = "metal_reflection_brushed.dds";
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 92:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::DarklingShinyGlint;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_ShinyGlint";
        policy.source_status_note =
            "Original shader dump exposes ShinyGlint techniques but no distinct Darkling Shiny Glint technique; mapShaders row has no local users.";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S92_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 93:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::DarklingSpecularShinyGlint;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_ShinyGlint";
        policy.source_status_note =
            "Original shader dump exposes ShinyGlint techniques but no distinct Darkling Specular Shiny Glint technique; mapShaders row has no local users.";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S93_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 94:
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::DarklingStructureShinyGlint;
        policy.source_file = "LEGOPPLighting.fx";
        policy.source_technique = "Technique_LEGOPPLightingOK_ShinyGlint";
        policy.source_status_note =
            "Original shader dump exposes ShinyGlint techniques but no distinct Darkling Structure Shiny Glint technique; mapShaders row has no local users.";
        policy.validation_status_note =
            "No resolved CDClient or importer-validated NIF users found in current client dump; no valid S94_ prefix or shader metadata candidates found.";
        policy.uses_material_diffuse = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 95:
        policy.shader_family = LegacyShaderFamily::BasicTwoLayer;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "inferred";
        policy.source_technique = "Two Layers Blended NL VC AnimUV";
        policy.source_status_note =
            "No TwoLayersBlended technique found in original shader dump; real multishader NIF prefixes reference shader 95.";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 96:
        policy.shader_family = LegacyShaderFamily::BasicTwoLayer;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "inferred";
        policy.source_technique = "Two Layers Blended VC AnimUV";
        policy.source_status_note =
            "No TwoLayersBlended technique found in original shader dump; real multishader NIF prefixes reference shader 96.";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = true;
        policy.uses_lighting = true;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 97:
        policy.shader_family = LegacyShaderFamily::BasicTwoLayer;
        policy.source_file = "BasicShaders.fx";
        policy.source_technique = "Technique_TwoLayersAdded_NoLighting_VertColor_UVScrolling";
        policy.validation_status_note =
            "No resolved CDClient, multishader-prefix, or shader-metadata NIF users found in current client dump.";
        policy.uses_vertex_color = true;
        policy.uses_material_diffuse = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 79:
        policy.shader_family = LegacyShaderFamily::OceanDistortDirectional;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_Directional_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.026f, 0.012f};
        policy.uv_motion_layer2 = {-0.014f, 0.021f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 80:
        policy.shader_family = LegacyShaderFamily::OceanDistortFx;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_FX_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.018f, 0.006f};
        policy.uv_motion_layer2 = {-0.011f, 0.014f};
        policy.port_status = ShaderPortStatus::Verified;
        break;
    case 85:
        policy.shader_family = LegacyShaderFamily::OceanDistort;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_2Layers";
        policy.source_status_note =
            "No Ocean Distort NoDepth technique found in original shader dump; regular Ocean Distort source already renders alpha-blended with depth writes disabled.";
        policy.uses_vertex_color = true;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.018f, 0.006f};
        policy.uv_motion_layer2 = {-0.011f, 0.014f};
        policy.port_status = ShaderPortStatus::Inferred;
        break;
    case 91:
        policy.shader_family = LegacyShaderFamily::OceanDistortUnlit;
        policy.alpha_mode = RenderAlphaMode::AlphaBlend;
        policy.depth_write = false;
        policy.source_file = "Ocean.fx";
        policy.source_technique = "Technique_Ocean_Distort_Unlit_2Layers";
        policy.uses_vertex_color = true;
        policy.uses_lighting = false;
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_uv_animation = true;
        policy.uses_alpha_animation = true;
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.uv_motion_layer1 = {0.018f, 0.006f};
        policy.uv_motion_layer2 = {-0.011f, 0.014f};
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
    case 50:
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

LuShaderPolicy applyFxShaderMetadataPolicyOverrides(
    LuShaderPolicy policy,
    const std::string& material_name) {
    if (!containsCaseInsensitive(material_name, "fxshader_")) return policy;
    if (policy.source_file.empty() ||
        policy.source_file == "unknown" ||
        policy.source_file == "inferred") {
        return policy;
    }

    const std::string compact = compactShaderToken(material_name);
    const std::optional<std::string> source_technique =
        legoppFxSourceTechniqueFromMaterialName(material_name);
    auto has = [&](std::string_view needle) {
        return compact.find(compactShaderToken(needle)) != std::string::npos;
    };

    enum class Flavor {
        Default,
        Low,
        NoEnv,
        NoEnvNoSpec,
    };

    Flavor flavor = Flavor::Default;
    if (has("noenvnospec")) {
        flavor = Flavor::NoEnvNoSpec;
    } else if (has("noenv")) {
        flavor = Flavor::NoEnv;
    } else if (has("low")) {
        flavor = Flavor::Low;
    }

    if (has("3lights")) {
        policy.shader_family = LegacyShaderFamily::LegoppEffect;
        policy.legopp_variant = LegoppShaderVariant::ThreeLight;
        policy.source_file = "LEGOPPLighting_BBB.fx";
        policy.source_technique = "Technique_LEGOPPLightingVertColor_3Lights";
        policy.source_status_note =
            "LEGOPPLighting_BBB.fx uses envSampler NTM=dark; no dedicated dark cubemap DDS is present in renderer reflection_maps.";
        policy.uses_vertex_color = true;
        policy.uses_texture = false;
        policy.uses_material_diffuse = false;
        policy.reflection_semantic = "dark";
        policy.alpha_semantic = ShaderAlphaSemantic::OutputAlpha;
        policy.port_status = ShaderPortStatus::Verified;
        return policy;
    }

    const bool item =
        policy.legopp_variant == LegoppShaderVariant::Item ||
        policy.legopp_variant == LegoppShaderVariant::ItemGlow;
    const bool darkling =
        policy.legopp_variant == LegoppShaderVariant::Darkling ||
        policy.legopp_variant == LegoppShaderVariant::DarklingSpecular ||
        policy.legopp_variant == LegoppShaderVariant::DarklingStructure;
    if (darkling &&
        (policy.legopp_variant != LegoppShaderVariant::DarklingSpecular ||
         flavor != Flavor::NoEnv)) {
        return policy;
    }

    if (flavor == Flavor::Default) {
        if (source_technique) {
            applyLegoppSourceTechniqueFlags(policy, *source_technique);
        }
        return policy;
    }

    switch (flavor) {
    case Flavor::Low:
        policy.source_file = item ? "LEGOPPLighting_Item_low.fx" : "LEGOPPLighting_low.fx";
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_shadow_terrain = false;
        break;
    case Flavor::NoEnv:
        policy.source_file = item ? "LEGOPPLighting_Item_noenv.fx" : "LEGOPPLighting_noenv.fx";
        policy.uses_reflection = false;
        policy.uses_shadow_terrain = false;
        break;
    case Flavor::NoEnvNoSpec:
        policy.source_file = item ? "LEGOPPLighting_Item_noenv_nospec.fx" : "LEGOPPLighting_noenv_nospec.fx";
        policy.uses_specular = false;
        policy.uses_reflection = false;
        policy.uses_shadow_terrain = false;
        break;
    case Flavor::Default:
        break;
    }

    if (source_technique) {
        applyLegoppSourceTechniqueFlags(policy, *source_technique);
    }

    if (!policy.uses_reflection) {
        policy.reflection_map.clear();
        policy.reflection_semantic.clear();
    }

    return policy;
}

std::optional<int32_t> parseMultishaderPrefix(const std::string& mesh_name) {
    size_t pos = 0;
    while (pos < mesh_name.size()) {
        const size_t token_pos = mesh_name.find_first_of("Ss", pos);
        if (token_pos == std::string::npos) return std::nullopt;

        if (token_pos != 0) {
            const char previous = mesh_name[token_pos - 1];
            if (previous != '_' && previous != '-' && previous != ' ') {
                pos = token_pos + 1;
                continue;
            }
        }

        size_t index = token_pos + 1;
        int32_t shader_id = 0;
        bool has_digit = false;
        while (index < mesh_name.size() && std::isdigit(static_cast<unsigned char>(mesh_name[index]))) {
            has_digit = true;
            shader_id = shader_id * 10 + (mesh_name[index] - '0');
            ++index;
        }

        if (has_digit && index < mesh_name.size() && mesh_name[index] == '_') {
            return shader_id;
        }

        pos = token_pos + 1;
    }

    return std::nullopt;
}

std::optional<int32_t> parseNiMultiShaderGameValue(const std::string& material_name) {
    constexpr std::string_view kPrefix = "nimultishader";
    std::string lower = lowerCopy(material_name);
    size_t pos = lower.find(kPrefix);
    if (pos == std::string::npos) return std::nullopt;

    size_t index = pos + kPrefix.size();
    int32_t game_value = 0;
    bool has_digit = false;
    while (index < lower.size() && std::isdigit(static_cast<unsigned char>(lower[index]))) {
        has_digit = true;
        game_value = game_value * 10 + (lower[index] - '0');
        ++index;
    }

    if (!has_digit) return std::nullopt;
    return game_value;
}

std::optional<int32_t> inferShaderIdFromFxShaderMetadata(const std::string& material_name) {
    if (!containsCaseInsensitive(material_name, "fxshader_")) return std::nullopt;

    const std::string compact = compactShaderToken(material_name);
    auto has = [&](std::string_view needle) {
        return compact.find(compactShaderToken(needle)) != std::string::npos;
    };

    const bool darkling = has("darkling") || has("darking");
    const bool specular = has("specular");
    const bool structure = has("structure") || has("nondecal");
    const bool shiny_glint = has("shinyglint");

    if (darkling && shiny_glint && specular) return 93;
    if (darkling && shiny_glint && structure) return 94;
    if (darkling && shiny_glint) return 92;
    if (darkling && specular) return 66;
    if (darkling && structure) return 67;
    if (darkling) return 65;
    if (shiny_glint) return 63;

    if (has("itemglow")) return 41;
    if (has("item")) return 23;
    if (has("superemissive")) return 19;
    if (has("3lights")) return kLegoShaderId;
    if (has("glowignorevertalpha")) return 40;
    if (has("glow")) return 38;
    if (has("grayscale")) return 39;
    if (has("emissive")) {
        return has("vertcolortextured") ? 58 : 46;
    }
    if (has("noambient")) return 78;
    if (has("frontend")) return 11;
    if (has("maskednondecal")) return 14;
    if (has("reveal")) return 6;
    if (has("fadeup")) return 43;
    if (has("animuv")) return 21;
    if (has("facecreate")) return 37;
    if (has("legopplightingtexturednl") || has("legopplightingvertcolorskinnednl")) return 45;
    if (has("legopplighting")) return kLegoShaderId;

    return std::nullopt;
}

bool isLegoppFrontendAlphaTestTechnique(const std::string& source_technique) {
    return source_technique == "Technique_LEGOPPLightingVertColorTexturedSkinned_FrontEnd";
}

} // namespace lu::renderer::lu_import
