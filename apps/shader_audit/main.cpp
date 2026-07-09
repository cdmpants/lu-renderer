#include "lu/renderer/lu_import/nif_importer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
    std::filesystem::path client_root;
    std::filesystem::path fx_root;
    std::vector<std::filesystem::path> nifs;
};

struct ShaderStats {
    int32_t shader_id = -1;
    int32_t game_value = -1;
    std::string label;
    std::string program;
    std::string port_status;
    std::string source_file;
    std::string source_technique;
    std::string alpha;
    std::string cull;
    bool resolved = false;
    bool multishader = false;
    bool mesh_has_vertex_colors = false;
    bool uses_vertex_color = false;
    bool uses_texture = false;
    bool uses_material_diffuse = false;
    bool uses_fog = false;
    bool uses_specular = false;
    bool uses_reflection = false;
    std::string reflection_map;
    std::string reflection_semantic;
    bool uses_uv_animation = false;
    bool uses_alpha_animation = false;
    lu::renderer::Vec2 uv_motion_layer1 = {0.0f, 0.0f};
    lu::renderer::Vec2 uv_motion_layer2 = {0.0f, 0.0f};
    float emissive_max = 0.0f;
    size_t mesh_count = 0;
    std::vector<std::string> samples;
};

struct FxCheckSummary {
    size_t ok = 0;
    size_t inferred = 0;
    size_t missing_file = 0;
    size_t missing_technique = 0;
    size_t unknown = 0;
    size_t not_checked = 0;
};

Args parseArgs(int argc, char** argv) {
    Args args;
    if (const char* env = std::getenv("LU_CLIENT_ROOT")) {
        args.client_root = env;
    }
    if (const char* env = std::getenv("LU_SHADER_SOURCE_ROOT")) {
        args.fx_root = env;
    }
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--client-root" && i + 1 < argc) {
            args.client_root = argv[++i];
        } else if (arg == "--fx-root" && i + 1 < argc) {
            args.fx_root = argv[++i];
        } else {
            args.nifs.emplace_back(arg);
        }
    }
    return args;
}

std::filesystem::path inferClientRoot(const std::filesystem::path& nif_path) {
    for (auto current = nif_path.parent_path(); !current.empty(); current = current.parent_path()) {
        if (current.filename() == "res") return current;
        if (current == current.parent_path()) break;
    }
    return {};
}

std::filesystem::path findSourceFile(const std::filesystem::path& root, const std::string& source_file) {
    if (root.empty() || source_file.empty() || source_file == "unknown") return {};

    std::filesystem::path direct = root / source_file;
    std::error_code ec;
    if (std::filesystem::exists(direct, ec)) return direct;

    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == source_file) return it->path();
    }

    return {};
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const char* sourceCheck(
    const std::filesystem::path& fx_root,
    const std::string& source_file,
    const std::string& source_technique,
    const std::string& port_status) {
    if (port_status == "inferred") return "inferred";
    if (fx_root.empty()) return "not checked";
    if (source_file.empty() || source_file == "unknown" ||
        source_technique.empty() || source_technique == "unknown") {
        return "unknown";
    }

    std::filesystem::path source_path = findSourceFile(fx_root, source_file);
    if (source_path.empty()) return "missing file";

    std::string source = readTextFile(source_path);
    if (source.find(source_technique) == std::string::npos) return "missing technique";
    return "ok";
}

void addFxCheck(FxCheckSummary& summary, const char* check) {
    std::string value = check;
    if (value == "ok") {
        ++summary.ok;
    } else if (value == "inferred") {
        ++summary.inferred;
    } else if (value == "missing file") {
        ++summary.missing_file;
    } else if (value == "missing technique") {
        ++summary.missing_technique;
    } else if (value == "unknown") {
        ++summary.unknown;
    } else if (value == "not checked") {
        ++summary.not_checked;
    }
}

const char* boolText(bool value) {
    return value ? "yes" : "no";
}

const char* alphaModeName(lu::renderer::RenderAlphaMode mode) {
    using lu::renderer::RenderAlphaMode;
    switch (mode) {
    case RenderAlphaMode::Opaque: return "opaque";
    case RenderAlphaMode::AlphaTest: return "test";
    case RenderAlphaMode::AlphaBlend: return "blend";
    case RenderAlphaMode::Additive: return "add";
    }
    return "?";
}

const char* cullModeName(lu::renderer::RenderCullMode mode) {
    using lu::renderer::RenderCullMode;
    switch (mode) {
    case RenderCullMode::Backface: return "back";
    case RenderCullMode::Clockwise: return "cw";
    case RenderCullMode::CounterClockwise: return "ccw";
    case RenderCullMode::TwoSided: return "two";
    }
    return "?";
}

const char* shaderFamilyName(lu::renderer::LegacyShaderFamily family) {
    using lu::renderer::LegacyShaderFamily;
    switch (family) {
    case LegacyShaderFamily::LegacyMesh: return "legacy";
    case LegacyShaderFamily::Basic: return "basic";
    case LegacyShaderFamily::BasicLit: return "basic-lit";
    case LegacyShaderFamily::AlphaAsAlpha: return "alpha";
    case LegacyShaderFamily::AlphaUvScroll: return "alpha-scroll";
    case LegacyShaderFamily::LegoppEffect: return "legopp-fx";
    case LegacyShaderFamily::LegoppNoAmbient: return "legopp-noambient";
    case LegacyShaderFamily::LegoppEmissive: return "legopp-emissive";
    case LegacyShaderFamily::TerrainRim: return "terrain-rim";
    case LegacyShaderFamily::OceanDistort: return "ocean-distort";
    case LegacyShaderFamily::OceanDistortDirectional: return "ocean-dir";
    case LegacyShaderFamily::LegoppLighting: return "legopp";
    case LegacyShaderFamily::ClearPlastic: return "clear";
    }
    return "?";
}

const char* portStatusName(lu::renderer::ShaderPortStatus status) {
    using lu::renderer::ShaderPortStatus;
    switch (status) {
    case ShaderPortStatus::Unported: return "unported";
    case ShaderPortStatus::Placeholder: return "placeholder";
    case ShaderPortStatus::Inferred: return "inferred";
    case ShaderPortStatus::Verified: return "verified";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);
    if (args.nifs.empty()) {
        std::cerr << "Usage: lu_shader_audit [--client-root <res-or-client-root>] [--fx-root <shader-source-root>] <nif> [...]\n";
        return 2;
    }

    std::map<int32_t, ShaderStats> stats_by_shader;
    size_t total_meshes = 0;
    size_t total_assets = 0;

    for (const auto& nif_path : args.nifs) {
        std::filesystem::path client_root = args.client_root.empty() ? inferClientRoot(nif_path) : args.client_root;
        lu::renderer::lu_import::NifImportOptions options;
        options.client_root = client_root;
        options.nif_path = nif_path;
        auto imported = lu::renderer::lu_import::importNif(options);
        if (!imported.error.empty()) {
            std::cerr << "ERROR " << nif_path << ": " << imported.error << "\n";
            continue;
        }

        ++total_assets;
        for (const auto& mesh : imported.world.meshes) {
            const auto& material = mesh.material;
            ShaderStats& stats = stats_by_shader[material.lu_shader_id];
            stats.shader_id = material.lu_shader_id;
            stats.game_value = material.lu_shader_game_value;
            stats.label = material.lu_shader_label;
            stats.program = shaderFamilyName(material.shader_family);
            stats.port_status = portStatusName(material.lu_shader_port_status);
            stats.source_file = material.lu_shader_source_file;
            stats.source_technique = material.lu_shader_source_technique;
            stats.alpha = alphaModeName(material.alpha_mode);
            stats.cull = cullModeName(material.cull_mode);
            stats.resolved = stats.resolved || material.lu_shader_resolved;
            stats.multishader = stats.multishader || material.lu_shader_asset_is_multishader;
            stats.mesh_has_vertex_colors = stats.mesh_has_vertex_colors || material.mesh_has_vertex_colors;
            stats.uses_vertex_color = stats.uses_vertex_color || material.lu_shader_uses_vertex_color;
            stats.uses_texture = stats.uses_texture || material.lu_shader_uses_texture;
            stats.uses_material_diffuse = stats.uses_material_diffuse || material.lu_shader_uses_material_diffuse;
            stats.uses_fog = stats.uses_fog || material.lu_shader_uses_fog;
            stats.uses_specular = stats.uses_specular || material.lu_shader_uses_specular;
            stats.uses_reflection = stats.uses_reflection || material.lu_shader_uses_reflection;
            if (material.lu_shader_uses_reflection && stats.reflection_map.empty()) {
                stats.reflection_map = material.lu_shader_reflection_map;
                stats.reflection_semantic = material.lu_shader_reflection_semantic;
            }
            stats.uses_uv_animation = stats.uses_uv_animation || material.lu_shader_uses_uv_animation;
            stats.uses_alpha_animation = stats.uses_alpha_animation || material.lu_shader_uses_alpha_animation;
            if (material.lu_shader_uses_uv_animation) {
                stats.uv_motion_layer1 = material.lu_uv_motion_layer1;
                stats.uv_motion_layer2 = material.lu_uv_motion_layer2;
            }
            stats.emissive_max = std::max(stats.emissive_max,
                std::max({material.emissive.x, material.emissive.y, material.emissive.z}));
            ++stats.mesh_count;
            ++total_meshes;
            if (stats.samples.size() < 3) {
                stats.samples.push_back(imported.world.source_asset_path + "::" + mesh.name);
            }
        }
    }

    std::vector<ShaderStats> sorted;
    sorted.reserve(stats_by_shader.size());
    for (const auto& [_, stats] : stats_by_shader) sorted.push_back(stats);
    std::sort(sorted.begin(), sorted.end(), [](const ShaderStats& a, const ShaderStats& b) {
        if (a.mesh_count != b.mesh_count) return a.mesh_count > b.mesh_count;
        return a.shader_id < b.shader_id;
    });

    std::cout << "Shader audit: assets=" << total_assets
              << " meshes=" << total_meshes
              << " uniqueShaders=" << sorted.size() << "\n";
    FxCheckSummary fx_summary;
    for (const auto& stats : sorted) {
        addFxCheck(fx_summary, sourceCheck(args.fx_root, stats.source_file, stats.source_technique, stats.port_status));
    }
    std::cout << "FX check: ok=" << fx_summary.ok
              << " inferred=" << fx_summary.inferred
              << " missingFile=" << fx_summary.missing_file
              << " missingTechnique=" << fx_summary.missing_technique
              << " unknown=" << fx_summary.unknown
              << " notChecked=" << fx_summary.not_checked << "\n";
    std::cout << "count | id/gv | label | program | port | fxCheck | fx | technique | alpha | cull | resolved | multi | flags | emissive | samples\n";
    for (const auto& stats : sorted) {
        std::cout << stats.mesh_count
                  << " | " << stats.shader_id << "/" << stats.game_value
                  << " | " << stats.label
                  << " | " << stats.program
                  << " | " << stats.port_status
                  << " | " << sourceCheck(args.fx_root, stats.source_file, stats.source_technique, stats.port_status)
                  << " | " << stats.source_file
                  << " | " << stats.source_technique
                  << " | " << stats.alpha
                  << " | " << stats.cull
                  << " | " << boolText(stats.resolved)
                  << " | " << boolText(stats.multishader)
            << " | vc=" << boolText(stats.uses_vertex_color)
            << ",meshvc=" << boolText(stats.mesh_has_vertex_colors)
            << ",tex=" << boolText(stats.uses_texture)
                  << ",mat=" << boolText(stats.uses_material_diffuse)
                  << ",fog=" << boolText(stats.uses_fog)
                  << ",spec=" << boolText(stats.uses_specular)
                  << ",refl=" << boolText(stats.uses_reflection)
                  << ",env=" << (stats.reflection_map.empty() ? "none" : stats.reflection_map)
                  << ",envSem=" << (stats.reflection_semantic.empty() ? "none" : stats.reflection_semantic)
                  << ",uvanim=" << boolText(stats.uses_uv_animation)
                  << ",alphaanim=" << boolText(stats.uses_alpha_animation)
                  << ",m1=" << stats.uv_motion_layer1.x << "/" << stats.uv_motion_layer1.y
                  << ",m2=" << stats.uv_motion_layer2.x << "/" << stats.uv_motion_layer2.y
                  << " | " << stats.emissive_max
                  << " | ";
        for (size_t i = 0; i < stats.samples.size(); ++i) {
            if (i > 0) std::cout << "; ";
            std::cout << stats.samples[i];
        }
        std::cout << "\n";
    }

    return 0;
}
