#include "lu/renderer/lu_import/nif_importer.h"

#include "lu/renderer/lu_import/shader_database.h"

#include "gamebryo/nif/nif_geometry.h"
#include "gamebryo/nif/nif_reader.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <vector>

namespace lu::renderer::lu_import {

namespace {

uint32_t packColor(float r, float g, float b, float a) {
    auto toByte = [](float v) -> uint32_t {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    uint32_t rr = toByte(r);
    uint32_t gg = toByte(g);
    uint32_t bb = toByte(b);
    uint32_t aa = toByte(a);
    return rr | (gg << 8u) | (bb << 16u) | (aa << 24u);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path);

std::filesystem::path withDdsExtension(std::filesystem::path path) {
    std::string ext = lowerCopy(path.extension().string());
    if (ext == ".tga") {
        path.replace_extension(".dds");
    }
    return path;
}

std::filesystem::path resolveTexturePath(
    const std::filesystem::path& res_root,
    const std::filesystem::path& nif_path,
    const std::string& texture) {

    if (texture.empty()) return {};

    std::string normalized = texture;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path tex_path(normalized);

    std::vector<std::filesystem::path> candidates;
    if (tex_path.is_absolute()) {
        candidates.push_back(tex_path);
        candidates.push_back(withDdsExtension(tex_path));
    } else {
        candidates.push_back(res_root / tex_path);
        candidates.push_back(res_root / "textures" / tex_path);
        candidates.push_back(nif_path.parent_path() / tex_path);
        candidates.push_back(withDdsExtension(res_root / tex_path));
        candidates.push_back(withDdsExtension(res_root / "textures" / tex_path));
        candidates.push_back(withDdsExtension(nif_path.parent_path() / tex_path));
    }

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return tex_path;
}

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

bool isLegoppVariant(LegacyShaderFamily family) {
    return family == LegacyShaderFamily::LegoppLighting ||
           family == LegacyShaderFamily::LegoppNoAmbient ||
           family == LegacyShaderFamily::LegoppEmissive;
}

void applyLegoppGeometryVariant(MaterialAsset& material, bool has_vertex_colors, bool has_texture) {
    if (!isLegoppVariant(material.shader_family)) return;

    material.lu_shader_uses_vertex_color = has_vertex_colors;
    material.lu_shader_uses_material_diffuse = true;

    const bool no_ambient = material.shader_family == LegacyShaderFamily::LegoppNoAmbient;
    const bool emissive = material.shader_family == LegacyShaderFamily::LegoppEmissive;

    if (emissive) {
        if (has_vertex_colors && has_texture) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColorTextured_Emissive";
        } else if (has_vertex_colors) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColor_Emissive";
        } else if (has_texture) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingTextured_Emissive";
        } else {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingOK_Emissive";
        }
        return;
    }

    if (no_ambient) {
        if (has_vertex_colors && has_texture) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColorTextured_NoAmbient";
        } else if (has_vertex_colors) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColor_NoAmbient";
        } else if (has_texture) {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingTextured_NoAmbient";
        } else {
            material.lu_shader_source_technique = "Technique_LEGOPPLightingOK_NoAmbient";
        }
        return;
    }

    if (has_vertex_colors && has_texture) {
        material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColorTextured";
    } else if (has_vertex_colors) {
        material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColor";
    } else if (has_texture) {
        material.lu_shader_source_technique = "Technique_LEGOPPLightingTextured";
    } else {
        material.lu_shader_source_technique = "Technique_LEGOPPLightingOK";
    }
}

} // namespace

NifImportResult importNif(const NifImportOptions& options) {
    NifImportResult result;

    std::vector<uint8_t> data = readFile(options.nif_path);
    if (data.empty()) {
        result.error = "Could not read NIF file: " + options.nif_path.string();
        return result;
    }

    try {
        auto nif = lu::assets::nif_parse(std::span<const uint8_t>(data.data(), data.size()));
        auto extracted = lu::assets::extractNifRenderGeometry(nif);
        std::filesystem::path res_root = ShaderDatabase::normalizeClientRoot(options.client_root);
        std::optional<ShaderDatabase> shader_database = ShaderDatabase::loadFromClientRoot(res_root);
        std::string nif_asset_path = ShaderDatabase::assetPathRelativeToRes(res_root, options.nif_path);
        result.world.source_asset_path = nif_asset_path;

        result.world.meshes.reserve(extracted.meshes.size());
        for (const auto& mesh : extracted.meshes) {
            ResolvedLuShader resolved_shader;
            resolved_shader.shader = fallbackShaderInfo();
            resolved_shader.policy = shaderPolicyFromInfo(resolved_shader.shader);
            if (shader_database) {
                resolved_shader = shader_database->resolveAssetMeshShader(nif_asset_path, mesh.name);
            }

            MeshAsset out;
            out.name = mesh.name;
            out.vertices.reserve(mesh.vertices.size());
            out.indices = mesh.indices;
            out.has_lod_range = mesh.has_lod_range;
            out.lod_parent_block = mesh.lod_parent_block;
            out.lod_level = mesh.lod_level;
            out.lod_near = mesh.lod_near;
            out.lod_far = mesh.lod_far;
            out.lod_center = {mesh.lod_center[0], mesh.lod_center[1], mesh.lod_center[2]};

            out.material.name = mesh.material.name;
            out.material.shader_family = resolved_shader.policy.shader_family;
            out.material.lu_shader_id = resolved_shader.shader.id;
            out.material.lu_shader_game_value = resolved_shader.shader.game_value;
            out.material.lu_shader_label = resolved_shader.shader.label;
            out.material.lu_shader_source_file = resolved_shader.policy.source_file;
            out.material.lu_shader_source_technique = resolved_shader.policy.source_technique;
            out.material.lu_shader_port_status = resolved_shader.policy.port_status;
            out.material.lu_shader_resolved = resolved_shader.resolved;
            out.material.lu_shader_asset_is_multishader = resolved_shader.asset_is_multishader;
            out.material.lu_multishader_prefix_id = resolved_shader.multishader_prefix_id.value_or(-1);
            out.material.mesh_has_vertex_colors = mesh.has_vertex_colors;
            out.material.lu_shader_uses_vertex_color = resolved_shader.policy.uses_vertex_color;
            out.material.lu_shader_uses_texture = resolved_shader.policy.uses_texture;
            out.material.lu_shader_uses_material_diffuse = resolved_shader.policy.uses_material_diffuse;
            out.material.lu_shader_uses_lighting = resolved_shader.policy.uses_lighting;
            out.material.lu_shader_uses_fog = resolved_shader.policy.uses_fog;
            out.material.lu_shader_uses_specular = resolved_shader.policy.uses_specular;
            out.material.lu_shader_uses_reflection = resolved_shader.policy.uses_reflection;
            out.material.lu_shader_reflection_map = resolved_shader.policy.reflection_map;
            out.material.lu_shader_reflection_semantic = resolved_shader.policy.reflection_semantic;
            out.material.lu_shader_uses_uv_animation = resolved_shader.policy.uses_uv_animation;
            out.material.lu_shader_uses_alpha_animation = resolved_shader.policy.uses_alpha_animation;
            out.material.lu_uv_motion_layer1 = resolved_shader.policy.uv_motion_layer1;
            out.material.lu_uv_motion_layer2 = resolved_shader.policy.uv_motion_layer2;
            out.material.alpha_mode = resolved_shader.policy.alpha_mode;
            out.material.cull_mode = resolved_shader.policy.cull_mode;
            out.material.diffuse = {
                mesh.material.diffuse[0],
                mesh.material.diffuse[1],
                mesh.material.diffuse[2],
                mesh.material.diffuse[3]
            };
            out.material.ambient = {
                mesh.material.ambient[0],
                mesh.material.ambient[1],
                mesh.material.ambient[2]
            };
            out.material.emissive = {
                mesh.material.emissive[0],
                mesh.material.emissive[1],
                mesh.material.emissive[2]
            };
            const bool has_alpha_source =
                mesh.material.diffuse[3] < 0.999f || !mesh.material.diffuse_texture.empty();
            out.material.alpha_blend =
                out.material.alpha_mode == RenderAlphaMode::AlphaBlend ||
                out.material.alpha_mode == RenderAlphaMode::Additive ||
                mesh.material.diffuse[3] < 0.999f ||
                (mesh.material.has_alpha_property &&
                 (mesh.material.alpha_flags & 0x0001u) != 0 &&
                 has_alpha_source);
            out.material.alpha_test =
                out.material.alpha_mode == RenderAlphaMode::AlphaTest ||
                mesh.material.has_alpha_property && mesh.material.alpha_threshold > 0;
            if (out.material.alpha_mode == RenderAlphaMode::Opaque) {
                if (out.material.alpha_blend) {
                    out.material.alpha_mode = RenderAlphaMode::AlphaBlend;
                } else if (out.material.alpha_test) {
                    out.material.alpha_mode = RenderAlphaMode::AlphaTest;
                }
            }
            out.material.alpha_threshold = mesh.material.alpha_threshold;

            auto texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.diffuse_texture);
            if (!texture_path.empty()) {
                out.material.diffuse_texture_path = texture_path.string();
            }
            applyLegoppGeometryVariant(out.material, mesh.has_vertex_colors, !out.material.diffuse_texture_path.empty());

            for (const auto& v : mesh.vertices) {
                Vertex rv;
                rv.position = {v.position[0], v.position[1], v.position[2]};
                rv.normal = {v.normal[0], v.normal[1], v.normal[2]};
                rv.uv = {v.uv[0], v.uv[1]};
                rv.color_rgba8 = packColor(v.color[0], v.color[1], v.color[2], v.color[3]);
                out.vertices.push_back(rv);
            }

            result.world.meshes.push_back(std::move(out));
        }
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    return result;
}

} // namespace lu::renderer::lu_import
