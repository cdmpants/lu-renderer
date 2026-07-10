#include "lu/renderer/lu_import/nif_importer.h"
#include "lu/renderer/lu_import/shader_database.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct Args {
    std::filesystem::path client_root;
    std::filesystem::path fx_root;
    std::string shader_label_filter;
    std::string asset_filter;
    std::optional<int32_t> shader_id_filter;
    bool dump_shaders = false;
    bool dump_lego_family = false;
    bool dump_lego_source = false;
    bool verify_lego_source = false;
    bool verify_lego_family = false;
    bool verify_lego_goal_assets = false;
    bool dump_assets = false;
    bool find_shader_users = false;
    bool lego_user_coverage = false;
    bool fxshader_coverage = false;
    bool candidate_only = false;
    bool only_unmapped_source = false;
    bool per_mesh = false;
    bool show_help = false;
    size_t limit = 50;
    std::vector<std::filesystem::path> nifs;
};

struct ShaderStats {
    int32_t shader_id = -1;
    int32_t game_value = -1;
    std::string label;
    std::string program;
    std::string port_status;
    std::string variant;
    std::string resolution_source;
    std::string metadata;
    std::string alpha_semantic;
    std::string source_file;
    std::string source_technique;
    std::string source_note;
    std::string validation_note;
    std::string alpha;
    std::string cull;
    bool depth_write = true;
    bool resolved = false;
    bool multishader = false;
    bool mesh_has_vertex_colors = false;
    bool uses_vertex_color = false;
    bool uses_texture = false;
    bool has_dark_texture = false;
    bool has_detail_texture = false;
    bool has_gloss_texture = false;
    bool has_glow_texture = false;
    bool uses_material_diffuse = false;
    bool uses_fog = false;
    bool uses_specular = false;
    bool uses_reflection = false;
    bool uses_shadow_terrain = false;
    std::string reflection_map;
    std::string reflection_semantic;
    bool uses_uv_animation = false;
    bool uses_alpha_animation = false;
    lu::renderer::Vec2 uv_motion_layer1 = {0.0f, 0.0f};
    lu::renderer::Vec2 uv_motion_layer2 = {0.0f, 0.0f};
    float emissive_max = 0.0f;
    size_t mesh_count = 0;
    std::vector<std::string> samples;
    std::string diffuse_texture_sample;
    std::string dark_texture_sample;
    std::string detail_texture_sample;
    std::string gloss_texture_sample;
    std::string glow_texture_sample;
};

struct FxCheckSummary {
    size_t ok = 0;
    size_t inferred = 0;
    size_t missing_file = 0;
    size_t missing_technique = 0;
    size_t unknown = 0;
    size_t not_checked = 0;
};

struct SourceTechnique {
    std::filesystem::path path;
    std::string source_file;
    std::string technique;
    size_t line = 0;
};

struct SourceCoverageSummary {
    size_t techniques = 0;
    size_t mapped = 0;
    size_t unmapped = 0;
    size_t not_checked = 0;
    size_t printed = 0;
};

struct LegoUserCoverage {
    lu::renderer::lu_import::LuShaderInfo shader;
    lu::renderer::lu_import::LuShaderPolicy policy;
    size_t direct_assets = 0;
    size_t prefix_candidate_nifs = 0;
    size_t metadata_candidate_nifs = 0;
    size_t imported_candidate_nifs = 0;
    size_t validated_meshes = 0;
    std::vector<std::string> samples;
};

struct FxShaderTokenCoverage {
    size_t nif_count = 0;
    std::map<int32_t, size_t> cdclient_shader_counts;
    std::vector<std::string> samples;
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
        if (arg == "--help" || arg == "-h") {
            args.show_help = true;
        } else if (arg == "--client-root" && i + 1 < argc) {
            args.client_root = argv[++i];
        } else if (arg == "--fx-root" && i + 1 < argc) {
            args.fx_root = argv[++i];
        } else if (arg == "--dump-shaders") {
            args.dump_shaders = true;
        } else if (arg == "--dump-lego-family") {
            args.dump_shaders = true;
            args.dump_lego_family = true;
        } else if (arg == "--verify-lego-family") {
            args.verify_lego_family = true;
        } else if (arg == "--verify-lego-goal-assets") {
            args.verify_lego_goal_assets = true;
        } else if (arg == "--dump-lego-source") {
            args.dump_lego_source = true;
        } else if (arg == "--verify-lego-source") {
            args.verify_lego_source = true;
        } else if (arg == "--dump-assets") {
            args.dump_assets = true;
        } else if (arg == "--find-shader-users") {
            args.find_shader_users = true;
        } else if (arg == "--lego-user-coverage") {
            args.lego_user_coverage = true;
        } else if (arg == "--fxshader-coverage") {
            args.fxshader_coverage = true;
        } else if (arg == "--candidate-only") {
            args.candidate_only = true;
        } else if (arg == "--only-unmapped-source") {
            args.only_unmapped_source = true;
        } else if (arg == "--per-mesh") {
            args.per_mesh = true;
        } else if (arg == "--shader-id" && i + 1 < argc) {
            args.shader_id_filter = std::stoi(argv[++i]);
        } else if (arg == "--limit" && i + 1 < argc) {
            args.limit = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--shader-label" && i + 1 < argc) {
            args.shader_label_filter = argv[++i];
        } else if (arg == "--asset-filter" && i + 1 < argc) {
            args.asset_filter = argv[++i];
        } else {
            args.nifs.emplace_back(arg);
        }
    }
    return args;
}

void printUsage() {
    std::cerr << "Usage: lu_shader_audit [--client-root <res-or-client-root>] [--fx-root <shader-source-root>] <nif> [...]\n"
              << "       lu_shader_audit [--client-root <res-or-client-root>] [--fx-root <shader-source-root>] --per-mesh [--shader-id <id>] <nif> [...]\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --dump-shaders [--shader-label <text>]\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --dump-lego-family\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --fx-root <shader-source-root> --verify-lego-family\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --verify-lego-goal-assets\n"
              << "       lu_shader_audit --fx-root <shader-source-root> [--client-root <res-or-client-root>] --dump-lego-source [--only-unmapped-source]\n"
              << "       lu_shader_audit --fx-root <shader-source-root> [--client-root <res-or-client-root>] --verify-lego-source\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --dump-assets --shader-id <id>\n"
              << "       lu_shader_audit --client-root <res-or-client-root> [--fx-root <shader-source-root>] --lego-user-coverage [--asset-filter <text>]\n"
              << "       lu_shader_audit --client-root <res-or-client-root> [--fx-root <shader-source-root>] --fxshader-coverage [--asset-filter <text>]\n"
              << "       lu_shader_audit --client-root <res-or-client-root> --find-shader-users --shader-id <id> [--asset-filter <text>] [--candidate-only] [--limit <n>]\n";
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

bool containsTechniqueDeclaration(const std::string& source, const std::string& technique_name) {
    size_t pos = 0;
    while ((pos = source.find("technique", pos)) != std::string::npos) {
        const bool token_start = pos == 0 ||
            (!std::isalnum(static_cast<unsigned char>(source[pos - 1])) &&
             source[pos - 1] != '_');
        size_t cursor = pos + std::string_view{"technique"}.size();
        const bool token_end = cursor >= source.size() ||
            (!std::isalnum(static_cast<unsigned char>(source[cursor])) &&
             source[cursor] != '_');
        if (!token_start || !token_end) {
            pos = cursor;
            continue;
        }

        while (cursor < source.size() &&
               std::isspace(static_cast<unsigned char>(source[cursor]))) {
            ++cursor;
        }

        const size_t name_start = cursor;
        while (cursor < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[cursor])) ||
                source[cursor] == '_')) {
            ++cursor;
        }

        if (source.compare(name_start, cursor - name_start, technique_name) == 0) {
            return true;
        }
        pos = cursor;
    }
    return false;
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
    if (!containsTechniqueDeclaration(source, source_technique)) return "missing technique";
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
    case LegacyShaderFamily::BasicTwoLayer: return "basic-2layer";
    case LegacyShaderFamily::LegoppEffect: return "legopp-fx";
    case LegacyShaderFamily::LegoppNoAmbient: return "legopp-noambient";
    case LegacyShaderFamily::LegoppEmissive: return "legopp-emissive";
    case LegacyShaderFamily::Metallic: return "metallic";
    case LegacyShaderFamily::TerrainRim: return "terrain-rim";
    case LegacyShaderFamily::OceanDistort: return "ocean-distort";
    case LegacyShaderFamily::OceanDistortDirectional: return "ocean-dir";
    case LegacyShaderFamily::OceanDistortFx: return "ocean-fx";
    case LegacyShaderFamily::OceanDistortUnlit: return "ocean-unlit";
    case LegacyShaderFamily::LegoppLighting: return "legopp";
    case LegacyShaderFamily::ClearPlastic: return "clear";
    }
    return "?";
}

const char* legoppVariantName(lu::renderer::LegoppShaderVariant variant) {
    using lu::renderer::LegoppShaderVariant;
    switch (variant) {
    case LegoppShaderVariant::None: return "none";
    case LegoppShaderVariant::Base: return "base";
    case LegoppShaderVariant::NoAmbient: return "noambient";
    case LegoppShaderVariant::Emissive: return "emissive";
    case LegoppShaderVariant::SuperEmissive: return "superemissive";
    case LegoppShaderVariant::Glow: return "glow";
    case LegoppShaderVariant::GlowIgnoreVertAlpha: return "glow-ignore-va";
    case LegoppShaderVariant::Grayscale: return "grayscale";
    case LegoppShaderVariant::Darkling: return "darkling";
    case LegoppShaderVariant::DarklingSpecular: return "darkling-spec";
    case LegoppShaderVariant::DarklingStructure: return "darkling-structure";
    case LegoppShaderVariant::DarklingShinyGlint: return "darkling-glint";
    case LegoppShaderVariant::DarklingSpecularShinyGlint: return "darkling-spec-glint";
    case LegoppShaderVariant::DarklingStructureShinyGlint: return "darkling-structure-glint";
    case LegoppShaderVariant::Item: return "item";
    case LegoppShaderVariant::ItemGlow: return "item-glow";
    case LegoppShaderVariant::FrontEnd: return "frontend";
    case LegoppShaderVariant::MaskedNonDecal: return "masked-nondecal";
    case LegoppShaderVariant::Reveal: return "reveal";
    case LegoppShaderVariant::FadeUp: return "fadeup";
    case LegoppShaderVariant::AnimUv: return "animuv";
    case LegoppShaderVariant::NoLight: return "nolight";
    case LegoppShaderVariant::FaceCreate: return "facecreate";
    case LegoppShaderVariant::PetTamingCloud: return "pet-cloud";
    case LegoppShaderVariant::ThreeLight: return "3lights";
    case LegoppShaderVariant::ShinyGlint: return "shiny-glint";
    }
    return "?";
}

const char* resolutionSourceName(lu::renderer::ShaderResolutionSource source) {
    using lu::renderer::ShaderResolutionSource;
    switch (source) {
    case ShaderResolutionSource::Unresolved: return "unresolved";
    case ShaderResolutionSource::CdClientAsset: return "cdclient";
    case ShaderResolutionSource::CdClientMultishaderPrefix: return "cdclient-prefix";
    case ShaderResolutionSource::NifMultiShaderGameValue: return "nif-nimultishader";
    case ShaderResolutionSource::NifMaterialName: return "nif-material";
    case ShaderResolutionSource::NifFxShaderName: return "nif-fxshader";
    case ShaderResolutionSource::Fallback: return "fallback";
    }
    return "?";
}

const char* alphaSemanticName(lu::renderer::ShaderAlphaSemantic semantic) {
    using lu::renderer::ShaderAlphaSemantic;
    switch (semantic) {
    case ShaderAlphaSemantic::Unknown: return "unknown";
    case ShaderAlphaSemantic::OutputAlpha: return "output";
    case ShaderAlphaSemantic::AlphaTest: return "test";
    case ShaderAlphaSemantic::ControlGlow: return "control-glow";
    case ShaderAlphaSemantic::ControlEmissive: return "control-emissive";
    case ShaderAlphaSemantic::ControlDarkling: return "control-darkling";
    case ShaderAlphaSemantic::Ignored: return "ignored";
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

bool isVerifiedPortStatus(lu::renderer::ShaderPortStatus status) {
    return status == lu::renderer::ShaderPortStatus::Verified;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool labelMatches(const std::string& label, const std::string& filter) {
    if (filter.empty()) return true;
    return lowerCopy(label).find(lowerCopy(filter)) != std::string::npos;
}

bool isLegoppFamily(lu::renderer::LegacyShaderFamily family) {
    using lu::renderer::LegacyShaderFamily;
    return family == LegacyShaderFamily::LegoppLighting ||
           family == LegacyShaderFamily::LegoppNoAmbient ||
           family == LegacyShaderFamily::LegoppEmissive ||
           family == LegacyShaderFamily::LegoppEffect;
}

bool isLegoFamilyLabel(const std::string& label) {
    static constexpr std::string_view kTokens[] = {
        "lego",
        "glow",
        "emissive",
        "superemissive",
        "darkling",
        "darking",
        "noambient",
        "masked",
        "reveal",
        "fade",
        "animuv",
        "item",
        "frontend",
        "front_end",
        "front end",
        "grayscale",
        "shiny",
        "glint"
    };
    const std::string lower = lowerCopy(label);
    for (std::string_view token : kTokens) {
        if (lower.find(token) != std::string::npos) return true;
    }
    return false;
}

bool isStrictLegoFamilyLabel(const std::string& label) {
    static constexpr std::string_view kTokens[] = {
        "lego",
        "legopp",
        "darkling",
        "darking"
    };
    const std::string lower = lowerCopy(label);
    for (std::string_view token : kTokens) {
        if (lower.find(token) != std::string::npos) return true;
    }
    return false;
}

bool isLegoFamilyShader(const std::string& label, const lu::renderer::lu_import::LuShaderPolicy& policy) {
    return isLegoFamilyLabel(label) ||
           (isLegoppFamily(policy.shader_family) &&
            policy.legopp_variant != lu::renderer::LegoppShaderVariant::None);
}

bool isStrictLegoFamilyShader(const std::string& label, const lu::renderer::lu_import::LuShaderPolicy& policy) {
    return isStrictLegoFamilyLabel(label) ||
           (isLegoppFamily(policy.shader_family) &&
            policy.legopp_variant != lu::renderer::LegoppShaderVariant::None);
}

bool assetMatches(const std::string& asset, const std::string& filter) {
    if (filter.empty()) return true;
    return lowerCopy(asset).find(lowerCopy(filter)) != std::string::npos;
}

void addSample(std::vector<std::string>& samples, const std::string& sample) {
    if (sample.empty() || samples.size() >= 3) return;
    if (std::find(samples.begin(), samples.end(), sample) != samples.end()) return;
    samples.push_back(sample);
}

std::string joinSamples(const std::vector<std::string>& samples) {
    if (samples.empty()) return "-";
    std::string joined;
    for (const std::string& sample : samples) {
        if (!joined.empty()) joined += "; ";
        joined += sample;
    }
    return joined;
}

std::string joinShaderCounts(
    const std::map<int32_t, size_t>& counts,
    const lu::renderer::lu_import::ShaderDatabase& shader_database) {

    if (counts.empty()) return "-";
    std::string joined;
    for (const auto& [shader_id, count] : counts) {
        if (!joined.empty()) joined += "; ";
        joined += std::to_string(shader_id);
        if (auto shader = shader_database.shaderInfo(shader_id)) {
            joined += "/" + std::to_string(shader->game_value) + ":" + shader->label;
        }
        joined += "=" + std::to_string(count);
    }
    return joined;
}

bool hasNifExtension(const std::string& asset_path) {
    return lowerCopy(std::filesystem::path(asset_path).extension().string()) == ".nif";
}

bool fileContainsAnyNeedle(const std::filesystem::path& path, const std::vector<std::string>& needles) {
    size_t max_needle = 0;
    for (const std::string& needle : needles) {
        max_needle = std::max(max_needle, needle.size());
    }
    if (max_needle == 0) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    constexpr size_t kChunkSize = 1024u * 1024u;
    std::string chunk(kChunkSize, '\0');
    std::string carry;
    std::string window;
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize read_count = file.gcount();
        if (read_count <= 0) break;

        window.clear();
        window.reserve(carry.size() + static_cast<size_t>(read_count));
        window.append(carry);
        window.append(chunk.data(), static_cast<size_t>(read_count));

        for (const std::string& needle : needles) {
            if (!needle.empty() && window.find(needle) != std::string::npos) {
                return true;
            }
        }

        const size_t carry_size = std::min(max_needle - 1u, window.size());
        carry.assign(window.end() - static_cast<std::ptrdiff_t>(carry_size), window.end());
    }

    return false;
}

std::set<int32_t> fileMatchingNeedleIds(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, int32_t>>& needles) {

    size_t max_needle = 0;
    for (const auto& [needle, _] : needles) {
        max_needle = std::max(max_needle, needle.size());
    }
    if (max_needle == 0) return {};

    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    constexpr size_t kChunkSize = 1024u * 1024u;
    std::string chunk(kChunkSize, '\0');
    std::string carry;
    std::string window;
    std::set<int32_t> matches;
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize read_count = file.gcount();
        if (read_count <= 0) break;

        window.clear();
        window.reserve(carry.size() + static_cast<size_t>(read_count));
        window.append(carry);
        window.append(chunk.data(), static_cast<size_t>(read_count));

        for (const auto& [needle, shader_id] : needles) {
            if (!needle.empty() && window.find(needle) != std::string::npos) {
                matches.insert(shader_id);
            }
        }

        const size_t carry_size = std::min(max_needle - 1u, window.size());
        carry.assign(window.end() - static_cast<std::ptrdiff_t>(carry_size), window.end());
    }

    return matches;
}

std::vector<std::string> nifShaderHintNeedles(
    const lu::renderer::lu_import::LuShaderInfo& shader,
    const lu::renderer::lu_import::LuShaderPolicy& policy) {

    std::vector<std::string> needles;
    if (shader.game_value >= 0) {
        needles.push_back("NiMultiShader" + std::to_string(shader.game_value));
    }
    if (!policy.source_technique.empty() &&
        policy.source_technique != "unknown" &&
        policy.source_technique.rfind("Technique_", 0) == 0) {
        needles.push_back("FXshader_" + policy.source_technique.substr(std::string_view{"Technique_"}.size()));
    }
    return needles;
}

struct NifShaderHintIds {
    std::set<int32_t> prefix_ids;
    std::set<int32_t> metadata_ids;
    std::set<std::string> fxshader_tokens;
};

std::string readBinaryString(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) return {};
    std::string data(static_cast<size_t>(size), '\0');
    file.read(data.data(), size);
    return data;
}

NifShaderHintIds scanNifShaderHints(
    const std::filesystem::path& path,
    const lu::renderer::lu_import::ShaderDatabase& shader_database) {

    NifShaderHintIds hints;
    const std::string data = readBinaryString(path);
    if (data.empty()) return hints;
    const std::string lower = lowerCopy(data);

    auto scanPrefix = [&](char marker) {
        size_t pos = 0;
        while ((pos = data.find(marker, pos)) != std::string::npos) {
            if (pos > 0) {
                const unsigned char previous = static_cast<unsigned char>(data[pos - 1]);
                if (std::isalnum(previous)) {
                    pos += 1;
                    continue;
                }
            }
            size_t cursor = pos + 1;
            while (cursor < data.size() &&
                   std::isdigit(static_cast<unsigned char>(data[cursor]))) {
                ++cursor;
            }
            if (cursor < data.size() && data[cursor] == '_') {
                const std::string token = data.substr(pos, cursor - pos + 1);
                if (auto shader_id = lu::renderer::lu_import::parseMultishaderPrefix(token)) {
                    hints.prefix_ids.insert(*shader_id);
                }
            }
            pos = std::max(cursor, pos + 1);
        }
    };
    scanPrefix('S');
    scanPrefix('s');

    size_t pos = 0;
    constexpr std::string_view kNiMultiShader = "nimultishader";
    while ((pos = lower.find(kNiMultiShader, pos)) != std::string::npos) {
        size_t cursor = pos + kNiMultiShader.size();
        int32_t game_value = 0;
        bool has_digit = false;
        while (cursor < data.size() &&
               std::isdigit(static_cast<unsigned char>(data[cursor]))) {
            has_digit = true;
            game_value = game_value * 10 + (data[cursor] - '0');
            ++cursor;
        }
        if (has_digit) {
            if (auto shader = shader_database.shaderInfoByGameValue(game_value)) {
                hints.metadata_ids.insert(shader->id);
            }
        }
        pos = cursor;
    }

    pos = 0;
    constexpr std::string_view kFxShader = "fxshader_";
    while ((pos = lower.find(kFxShader, pos)) != std::string::npos) {
        size_t cursor = pos;
        while (cursor < data.size()) {
            const unsigned char token_char = static_cast<unsigned char>(data[cursor]);
            if (!std::isalnum(token_char) && data[cursor] != '_') break;
            ++cursor;
        }
        if (cursor > pos) {
            std::string token = data.substr(pos, cursor - pos);
            hints.fxshader_tokens.insert(token);
            if (auto shader_id = lu::renderer::lu_import::inferShaderIdFromFxShaderMetadata(token)) {
                hints.metadata_ids.insert(*shader_id);
            }
        }
        pos = cursor;
    }

    pos = 0;
    constexpr std::string_view kNims = "_nims";
    while ((pos = lower.find(kNims, pos)) != std::string::npos) {
        const size_t context_start = pos > 96 ? pos - 96 : 0;
        const std::string_view context(lower.data() + context_start, pos - context_start);
        if (context.find("darkling") != std::string_view::npos ||
            context.find("darking") != std::string_view::npos) {
            hints.metadata_ids.insert(65);
        }
        pos += kNims.size();
    }

    return hints;
}

bool isLegoppSourceFile(const std::filesystem::path& path) {
    const std::string filename = lowerCopy(path.filename().string());
    return filename.rfind("legopplighting", 0) == 0 &&
           (path.extension() == ".fx" || path.extension() == ".FX");
}

bool isMacResourceFork(const std::filesystem::path& path) {
    return path.filename().string().rfind("._", 0) == 0 ||
           lowerCopy(path.string()).find("__macosx") != std::string::npos;
}

std::string trimLeft(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    return value;
}

std::optional<std::string> parseTechniqueName(const std::string& line) {
    const std::string trimmed = trimLeft(line);
    constexpr std::string_view kKeyword = "technique";
    if (trimmed.size() <= kKeyword.size()) return std::nullopt;
    if (lowerCopy(trimmed.substr(0, kKeyword.size())) != kKeyword) return std::nullopt;
    if (!std::isspace(static_cast<unsigned char>(trimmed[kKeyword.size()]))) return std::nullopt;

    size_t index = kKeyword.size();
    while (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index]))) {
        ++index;
    }

    size_t end = index;
    while (end < trimmed.size()) {
        const unsigned char c = static_cast<unsigned char>(trimmed[end]);
        if (std::isspace(c) || trimmed[end] == '{' || trimmed[end] == '<') break;
        ++end;
    }
    if (end == index) return std::nullopt;
    return trimmed.substr(index, end - index);
}

std::vector<SourceTechnique> collectLegoppSourceTechniques(const std::filesystem::path& fx_root) {
    std::vector<SourceTechnique> techniques;
    if (fx_root.empty()) return techniques;

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             fx_root,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path path = it->path();
        if (isMacResourceFork(path) || !isLegoppSourceFile(path)) continue;

        std::ifstream file(path);
        if (!file) continue;

        std::string line;
        size_t line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            auto technique = parseTechniqueName(line);
            if (!technique) continue;
            techniques.push_back(SourceTechnique{
                path,
                path.filename().string(),
                *technique,
                line_number,
            });
        }
    }

    std::sort(techniques.begin(), techniques.end(), [](const SourceTechnique& a, const SourceTechnique& b) {
        if (a.source_file != b.source_file) return a.source_file < b.source_file;
        if (a.line != b.line) return a.line < b.line;
        return a.technique < b.technique;
    });
    return techniques;
}

using TechniqueCatalogMap = std::unordered_map<std::string, std::vector<std::string>>;

std::string techniqueKey(const std::string& source_file, const std::string& technique) {
    return lowerCopy(source_file) + "|" + technique;
}

TechniqueCatalogMap buildTechniqueCatalog(const lu::renderer::lu_import::ShaderDatabase& shader_database) {
    TechniqueCatalogMap catalog;
    for (const auto& shader : shader_database.shaders()) {
        const auto policy = lu::renderer::lu_import::shaderPolicyFromInfo(shader);
        if (!isLegoFamilyShader(shader.label, policy)) continue;
        if (policy.source_file.empty() || policy.source_file == "unknown" ||
            policy.source_file == "inferred" ||
            policy.source_technique.empty() || policy.source_technique == "unknown") {
            continue;
        }
        catalog[techniqueKey(policy.source_file, policy.source_technique)].push_back(
            std::to_string(shader.id) + "/" + std::to_string(shader.game_value) + " " + shader.label);
    }
    return catalog;
}

TechniqueCatalogMap buildFxInferredTechniqueCatalog(const std::vector<SourceTechnique>& techniques) {
    TechniqueCatalogMap catalog;
    for (const auto& technique : techniques) {
        auto shader_id = lu::renderer::lu_import::inferShaderIdFromFxShaderMetadata(
            "FXshader_" + technique.technique);
        if (!shader_id) continue;

        lu::renderer::lu_import::LuShaderInfo shader{
            *shader_id,
            -1,
            "fx-inferred",
        };
        auto policy = lu::renderer::lu_import::shaderPolicyFromInfo(shader);
        policy = lu::renderer::lu_import::applyFxShaderMetadataPolicyOverrides(
            std::move(policy),
            "FXshader_" + technique.technique);
        if (policy.source_file.empty() || policy.source_file == "unknown" ||
            policy.source_file == "inferred") {
            continue;
        }
        catalog[techniqueKey(technique.source_file, technique.technique)].push_back(
            std::to_string(*shader_id) + "/? " + policy.source_file + " " +
            legoppVariantName(policy.legopp_variant) + " fx-inferred");
    }
    return catalog;
}

SourceCoverageSummary printSourceCoverage(
    const std::filesystem::path& fx_root,
    const lu::renderer::lu_import::ShaderDatabase* shader_database,
    bool only_unmapped) {
    const auto techniques = collectLegoppSourceTechniques(fx_root);
    SourceCoverageSummary summary;
    summary.techniques = techniques.size();
    TechniqueCatalogMap catalog;
    bool can_check_catalog = true;
    if (shader_database) {
        catalog = buildTechniqueCatalog(*shader_database);
        auto inferred_catalog = buildFxInferredTechniqueCatalog(techniques);
        for (auto& [key, entries] : inferred_catalog) {
            auto& target = catalog[key];
            target.insert(target.end(), entries.begin(), entries.end());
        }
    } else {
        catalog = buildFxInferredTechniqueCatalog(techniques);
    }

    std::cout << "fx | line | technique | catalog\n";
    for (const auto& technique : techniques) {
        const auto catalog_it = catalog.find(techniqueKey(technique.source_file, technique.technique));
        const bool has_catalog_match = can_check_catalog && catalog_it != catalog.end();
        if (!can_check_catalog) {
            ++summary.not_checked;
        } else if (has_catalog_match) {
            ++summary.mapped;
        } else {
            ++summary.unmapped;
        }
        if (only_unmapped && (!can_check_catalog || has_catalog_match)) continue;

        std::cout << technique.source_file
                  << " | " << technique.line
                  << " | " << technique.technique
                  << " | ";
        if (!can_check_catalog) {
            std::cout << "not checked";
        } else if (!has_catalog_match) {
            std::cout << "unmapped";
        } else {
            for (size_t i = 0; i < catalog_it->second.size(); ++i) {
                if (i > 0) std::cout << "; ";
                std::cout << catalog_it->second[i];
            }
        }
        std::cout << "\n";
        ++summary.printed;
    }

    std::cout << "summary | techniques=" << summary.techniques
              << " mapped=" << summary.mapped
              << " unmapped=" << summary.unmapped
              << " notChecked=" << summary.not_checked
              << " printed=" << summary.printed
              << "\n";
    return summary;
}

std::string textureName(const std::string& path) {
    if (path.empty()) return "-";
    return std::filesystem::path(path).filename().string();
}

const char* blendFactorName(uint8_t value) {
    static constexpr const char* names[] = {
        "one", "zero", "srcColor", "invSrcColor", "dstColor", "invDstColor",
        "srcAlpha", "invSrcAlpha", "dstAlpha", "invDstAlpha", "srcAlphaSat"
    };
    return value < std::size(names) ? names[value] : "invalid";
}

const char* testFunctionName(uint8_t value) {
    static constexpr const char* names[] = {
        "always", "less", "equal", "lessEqual", "greater", "notEqual",
        "greaterEqual", "never"
    };
    return value < std::size(names) ? names[value] : "invalid";
}

const char* alphaFormatName(uint32_t value) {
    switch (value) {
    case 0: return "none";
    case 1: return "binary";
    case 2: return "smooth";
    case 3: return "default";
    default: return "unknown";
    }
}

std::string propertySourceText(const lu::renderer::NifPropertySource& source) {
    if (!source.present) return "-";
    std::ostringstream stream;
    stream << (source.inheritance_depth == 0 ? "direct" : "inherited")
           << "#" << source.property_block
           << "@node" << source.owner_node_block
           << ":depth" << source.inheritance_depth;
    if (source.duplicates_on_owner > 0) {
        stream << ":duplicates" << source.duplicates_on_owner;
    }
    return stream.str();
}

std::string nodePathText(const lu::renderer::MeshAsset& mesh) {
    if (mesh.source_node_chain.empty()) return "-";
    std::ostringstream stream;
    for (size_t i = 0; i < mesh.source_node_chain.size(); ++i) {
        if (i > 0) stream << "<-";
        const auto& node = mesh.source_node_chain[i];
        stream << (node.name.empty() ? node.type_name : node.name) << "#" << node.block_index;
    }
    if (mesh.parent_cycle_detected) stream << "[cycle]";
    if (mesh.multiple_parents_detected) stream << "[multi-parent]";
    return stream.str();
}

void printPerMeshHeader() {
    std::cout << "asset | mesh | id/gv | label | variant | program | port | fxCheck | fx | technique | sourceNote | validationNote"
              << " | alpha | alphaSemantic | cull | zwrite | blend | test | effectiveState | nifAlpha | resolved | source | metadata"
              << " | multi | prefix | flags | textures | material"
              << " | nodePath | propertySources | decodedAlpha | decodedZ | vertexColorProperty"
              << " | specShade | stencil | sort | vertexAlpha | textureAlpha | currentSubmitted\n";
}

void printPerMeshRow(
    const std::filesystem::path& fx_root,
    const std::string& asset,
    const lu::renderer::MeshAsset& mesh) {
    const auto& material = mesh.material;
    std::cout << asset
              << " | " << mesh.name
              << " | " << material.lu_shader_id << "/" << material.lu_shader_game_value
              << " | " << material.lu_shader_label
              << " | " << legoppVariantName(material.legopp_variant)
              << " | " << shaderFamilyName(material.shader_family)
              << " | " << portStatusName(material.lu_shader_port_status)
              << " | " << sourceCheck(
                     fx_root,
                     material.lu_shader_source_file,
                     material.lu_shader_source_technique,
                     portStatusName(material.lu_shader_port_status))
              << " | " << material.lu_shader_source_file
              << " | " << material.lu_shader_source_technique
              << " | " << (material.lu_shader_source_status_note.empty() ? "-" : material.lu_shader_source_status_note)
              << " | " << (material.lu_shader_validation_status_note.empty() ? "-" : material.lu_shader_validation_status_note)
              << " | " << alphaModeName(material.alpha_mode)
              << " | " << alphaSemanticName(material.lu_shader_alpha_semantic)
              << " | " << cullModeName(material.cull_mode)
              << " | " << boolText(material.depth_write)
              << " | " << boolText(material.alpha_blend)
              << " | " << boolText(material.alpha_test)
              << " | usesNi=" << boolText(material.lu_shader_uses_ni_render_state)
              << ",depthTest=" << boolText(material.depth_test)
              << ",depthFunc=" << testFunctionName(material.depth_test_function)
              << ",src=" << blendFactorName(material.source_blend)
              << ",dst=" << blendFactorName(material.destination_blend)
              << ",testFunc=" << testFunctionName(material.alpha_test_function)
              << ",noSort=" << boolText(material.disable_transparent_sort)
              << " | " << boolText(material.has_alpha_property)
              << "/" << material.alpha_flags
              << "/" << static_cast<int>(material.alpha_threshold)
              << " | " << boolText(material.lu_shader_resolved)
              << " | " << resolutionSourceName(material.lu_shader_resolution_source)
              << " | " << (material.lu_shader_metadata.empty() ? "none" : material.lu_shader_metadata)
              << " | " << boolText(material.lu_shader_asset_is_multishader)
              << " | " << material.lu_multishader_prefix_id
              << " | vc=" << boolText(material.lu_shader_uses_vertex_color)
              << ",meshvc=" << boolText(material.mesh_has_vertex_colors)
              << ",tex=" << boolText(material.lu_shader_uses_texture)
              << ",mat=" << boolText(material.lu_shader_uses_material_diffuse)
              << ",fog=" << boolText(material.lu_shader_uses_fog)
              << ",spec=" << boolText(material.lu_shader_uses_specular)
              << ",refl=" << boolText(material.lu_shader_uses_reflection)
              << ",shadowTerrain=" << boolText(material.lu_shader_uses_shadow_terrain)
              << ",uvanim=" << boolText(material.lu_shader_uses_uv_animation)
              << ",alphaanim=" << boolText(material.lu_shader_uses_alpha_animation)
              << ",matctrl=" << boolText(material.nif_has_material_color_controller)
              << ",emctrl=" << boolText(material.material_emissive_controller)
              << ",emkeys=" << material.material_emissive_controller_keys.size()
              << " | diffuse=" << textureName(material.diffuse_texture_path)
              << ",dark=" << textureName(material.dark_texture_path)
              << ",detail=" << textureName(material.detail_texture_path)
              << ",gloss=" << textureName(material.gloss_texture_path)
              << ",glow=" << textureName(material.glow_texture_path)
              << " | diffuse=" << material.diffuse.x << "/" << material.diffuse.y << "/"
              << material.diffuse.z << "/" << material.diffuse.w
              << ",emissive=" << material.emissive.x << "/" << material.emissive.y << "/"
              << material.emissive.z
              << ",emctrlDefault=" << material.material_emissive_controller_default.x << "/"
              << material.material_emissive_controller_default.y << "/"
              << material.material_emissive_controller_default.z
              << ",emctrlRange=" << material.material_emissive_controller_start << "/"
              << material.material_emissive_controller_stop
              << " | " << nodePathText(mesh)
              << " | mat=" << propertySourceText(material.nif_resolved_state.sources.material)
              << ",tex=" << propertySourceText(material.nif_resolved_state.sources.texturing)
              << ",alpha=" << propertySourceText(material.nif_resolved_state.sources.alpha)
              << ",vc=" << propertySourceText(material.nif_resolved_state.sources.vertex_color)
              << ",z=" << propertySourceText(material.nif_resolved_state.sources.z_buffer)
              << ",spec=" << propertySourceText(material.nif_resolved_state.sources.specular)
              << ",shade=" << propertySourceText(material.nif_resolved_state.sources.shade)
              << ",stencil=" << propertySourceText(material.nif_resolved_state.sources.stencil)
              << " | present=" << boolText(material.nif_resolved_state.alpha.present)
              << ",raw=" << material.nif_resolved_state.alpha.raw_flags
              << ",blend=" << boolText(material.nif_resolved_state.alpha.blend_enabled)
              << ",src=" << blendFactorName(material.nif_resolved_state.alpha.source_blend)
              << ",dst=" << blendFactorName(material.nif_resolved_state.alpha.destination_blend)
              << ",test=" << boolText(material.nif_resolved_state.alpha.test_enabled)
              << ",func=" << testFunctionName(material.nif_resolved_state.alpha.test_function)
              << ",ref=" << static_cast<int>(material.nif_resolved_state.alpha.threshold)
              << ",noSort=" << boolText(material.nif_resolved_state.alpha.no_sorter)
              << " | present=" << boolText(material.nif_resolved_state.z_buffer.present)
              << ",raw=" << material.nif_resolved_state.z_buffer.raw_flags
              << ",test=" << boolText(material.nif_resolved_state.z_buffer.test_enabled)
              << ",write=" << boolText(material.nif_resolved_state.z_buffer.write_enabled)
              << ",func=" << testFunctionName(material.nif_resolved_state.z_buffer.test_function)
              << " | present=" << boolText(material.nif_resolved_state.vertex_color.present)
              << ",raw=" << material.nif_resolved_state.vertex_color.raw_flags
              << ",colorMode=" << static_cast<int>(material.nif_resolved_state.vertex_color.color_mode)
              << ",lightMode=" << static_cast<int>(material.nif_resolved_state.vertex_color.lighting_mode)
              << ",sourceMode=" << static_cast<int>(material.nif_resolved_state.vertex_color.source_vertex_mode)
              << " | spec=" << boolText(material.nif_resolved_state.has_specular)
              << "/" << boolText(material.nif_resolved_state.specular_enabled)
              << ",shade=" << boolText(material.nif_resolved_state.has_shade)
              << "/" << (material.nif_resolved_state.smooth_shading ? "smooth" : "hard")
              << " | present=" << boolText(material.nif_resolved_state.stencil.present)
              << ",raw=" << material.nif_resolved_state.stencil.raw_flags
              << ",enabled=" << boolText(material.nif_resolved_state.stencil.enabled)
              << ",draw=" << static_cast<int>(material.nif_resolved_state.stencil.draw_mode)
              << ",func=" << static_cast<int>(material.nif_resolved_state.stencil.test_function)
              << ",ref=" << material.nif_resolved_state.stencil.reference
              << ",mask=" << material.nif_resolved_state.stencil.mask
              << " | adjust=" << boolText(material.nif_resolved_state.has_sort_adjust)
              << ",node=" << material.nif_resolved_state.sort_adjust_node_block
              << ",depth=" << material.nif_resolved_state.sort_adjust_inheritance_depth
              << ",mode=" << material.nif_resolved_state.sorting_mode
              << " | min=" << material.nif_vertex_alpha_min
              << ",max=" << material.nif_vertex_alpha_max
              << ",nonOpaque=" << material.nif_vertex_alpha_non_opaque_count
              << " | authored=" << boolText(material.nif_diffuse_texture_has_alpha_format)
              << "/" << alphaFormatName(material.nif_diffuse_texture_alpha_format)
              << " | requestedZ=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).requested_depth_write)
              << ",submittedZ=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).submitted_depth_write)
              << ",depthTest=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).submitted_depth_test)
              << ",depthFunc=" << testFunctionName(lu::renderer::currentRenderStateDiagnostic(material).submitted_depth_test_function)
              << ",transparent=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).transparent_classification)
              << ",alphaBlend=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).submitted_alpha_blend)
              << ",src=" << blendFactorName(lu::renderer::currentRenderStateDiagnostic(material).submitted_source_blend)
              << ",dst=" << blendFactorName(lu::renderer::currentRenderStateDiagnostic(material).submitted_destination_blend)
              << ",additive=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).submitted_additive_blend)
              << ",alphaTest=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).shader_alpha_test)
              << ",testFunc=" << testFunctionName(lu::renderer::currentRenderStateDiagnostic(material).shader_alpha_test_function)
              << ",ref=" << static_cast<int>(lu::renderer::currentRenderStateDiagnostic(material).shader_alpha_reference)
              << ",noSort=" << boolText(lu::renderer::currentRenderStateDiagnostic(material).transparent_sort_disabled)
              << "\n";
}

struct GoalAssetExpectation {
    std::string name;
    std::string asset;
    int32_t shader_id = -1;
    std::optional<lu::renderer::ShaderResolutionSource> source;
    std::optional<lu::renderer::ShaderAlphaSemantic> alpha_semantic;
    std::optional<lu::renderer::RenderAlphaMode> alpha_mode;
    std::optional<lu::renderer::LegoppShaderVariant> variant;
    std::string source_file;
    std::string source_technique;
    lu::renderer::ShaderPortStatus port_status = lu::renderer::ShaderPortStatus::Verified;
    std::string metadata_contains;
    size_t min_meshes = 1;
};

bool materialMatchesGoalExpectation(
    const lu::renderer::MaterialAsset& material,
    const GoalAssetExpectation& expectation) {

    if (!material.lu_shader_resolved) return false;
    if (material.lu_shader_id != expectation.shader_id) return false;
    if (expectation.source && material.lu_shader_resolution_source != *expectation.source) return false;
    if (expectation.alpha_semantic && material.lu_shader_alpha_semantic != *expectation.alpha_semantic) return false;
    if (expectation.alpha_mode && material.alpha_mode != *expectation.alpha_mode) return false;
    if (expectation.variant && material.legopp_variant != *expectation.variant) return false;
    if (material.lu_shader_port_status != expectation.port_status) return false;
    if (!expectation.source_file.empty() &&
        material.lu_shader_source_file != expectation.source_file) {
        return false;
    }
    if (!expectation.source_technique.empty() &&
        material.lu_shader_source_technique != expectation.source_technique) {
        return false;
    }
    if (!expectation.metadata_contains.empty() &&
        material.lu_shader_metadata.find(expectation.metadata_contains) == std::string::npos) {
        return false;
    }
    return true;
}

int verifyLegoGoalAssets(const Args& args) {
    using lu::renderer::LegoppShaderVariant;
    using lu::renderer::RenderAlphaMode;
    using lu::renderer::ShaderAlphaSemantic;
    using lu::renderer::ShaderResolutionSource;

    const std::vector<GoalAssetExpectation> expectations = {
        {
            "darking pirate uses NIMS Darkling control alpha",
            "mesh/creatures/cre_darking-pirate.nif",
            65,
            ShaderResolutionSource::NifMaterialName,
            ShaderAlphaSemantic::ControlDarkling,
            RenderAlphaMode::Opaque,
            LegoppShaderVariant::Darkling,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingVertColorSkinned_Darkling",
            lu::renderer::ShaderPortStatus::Verified,
            "DARKLING_PIRATE_GRUNT_NIMS",
            3
        },
        {
            "construction troll resolves NiMultiShader5 as LEGO",
            "mesh/creatures/cre_constructiontroll.nif",
            1,
            ShaderResolutionSource::NifMultiShaderGameValue,
            ShaderAlphaSemantic::OutputAlpha,
            std::nullopt,
            LegoppShaderVariant::Base,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingTexturedSkinned",
            lu::renderer::ShaderPortStatus::Verified,
            "NiMultiShader5",
            1
        },
        {
            "valiant knight accessory uses SuperEmissive control alpha",
            "mesh/valiantweapons/minifig_accessory_knight_valiant.nif",
            19,
            ShaderResolutionSource::CdClientAsset,
            ShaderAlphaSemantic::ControlEmissive,
            RenderAlphaMode::Opaque,
            LegoppShaderVariant::SuperEmissive,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingVertColor_SuperEmissive",
            lu::renderer::ShaderPortStatus::Verified,
            "",
            3
        },
        {
            "Paradox scene validates LEGO Emissive multishader prefix",
            "mesh/env/env_won_fv_paradox_scene_glom.nif",
            46,
            ShaderResolutionSource::CdClientMultishaderPrefix,
            ShaderAlphaSemantic::ControlEmissive,
            RenderAlphaMode::Opaque,
            LegoppShaderVariant::Emissive,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingVertColor_Emissive",
            lu::renderer::ShaderPortStatus::Verified,
            "",
            1
        },
        {
            "AG battlefield validates Darkling specular multishader prefix",
            "mesh/env/ag_new/env_ag_battlefield_1.nif",
            66,
            ShaderResolutionSource::CdClientMultishaderPrefix,
            ShaderAlphaSemantic::ControlDarkling,
            RenderAlphaMode::Opaque,
            LegoppShaderVariant::DarklingSpecular,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingVertColor_Darkling_Specular",
            lu::renderer::ShaderPortStatus::Verified,
            "",
            1
        },
        {
            "ghost cube validates Darkling Structure",
            "mesh/env/fx_ag_ghost_cube.nif",
            67,
            ShaderResolutionSource::CdClientAsset,
            ShaderAlphaSemantic::ControlDarkling,
            RenderAlphaMode::Opaque,
            LegoppShaderVariant::DarklingStructure,
            "LEGOPPLighting.fx",
            "Technique_LEGOPPLightingVertColor_Darkling_NonDecal",
            lu::renderer::ShaderPortStatus::Verified,
            "",
            1
        }
    };

    const std::filesystem::path res_root =
        lu::renderer::lu_import::ShaderDatabase::normalizeClientRoot(args.client_root);
    size_t failures = 0;

    std::cout << "check | asset | expectedShader | matches | result | samples\n";
    for (const auto& expectation : expectations) {
        lu::renderer::lu_import::NifImportOptions options;
        options.client_root = args.client_root;
        options.nif_path = res_root / std::filesystem::path(expectation.asset);

        auto imported = lu::renderer::lu_import::importNif(options);
        if (!imported.error.empty()) {
            ++failures;
            std::cout << expectation.name
                      << " | " << expectation.asset
                      << " | " << expectation.shader_id
                      << " | 0"
                      << " | FAIL import: " << imported.error
                      << " | -\n";
            continue;
        }

        size_t matches = 0;
        std::vector<std::string> samples;
        std::vector<std::string> fallback_samples;
        for (const auto& mesh : imported.world.meshes) {
            const auto& material = mesh.material;
            if (material.lu_shader_resolution_source == ShaderResolutionSource::Fallback ||
                !material.lu_shader_resolved) {
                addSample(fallback_samples, mesh.name + ":" + std::to_string(material.lu_shader_id));
            }
            if (!materialMatchesGoalExpectation(material, expectation)) continue;
            ++matches;
            addSample(
                samples,
                mesh.name + " " +
                    std::to_string(material.lu_shader_id) + "/" +
                    std::to_string(material.lu_shader_game_value) + " " +
                    resolutionSourceName(material.lu_shader_resolution_source) + " " +
                    alphaSemanticName(material.lu_shader_alpha_semantic) + " " +
                    material.lu_shader_source_technique);
        }

        const bool pass = matches >= expectation.min_meshes && fallback_samples.empty();
        if (!pass) ++failures;

        std::cout << expectation.name
                  << " | " << expectation.asset
                  << " | " << expectation.shader_id
                  << " | " << matches
                  << " | " << (pass ? "PASS" : "FAIL");
        if (!fallback_samples.empty()) {
            std::cout << " fallback=" << joinSamples(fallback_samples);
        }
        std::cout << " | " << joinSamples(samples) << "\n";
    }

    std::cout << "summary | checks=" << expectations.size()
              << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);
    if (args.show_help) {
        printUsage();
        return 0;
    }

    if (args.dump_lego_source || args.verify_lego_source) {
        if (args.fx_root.empty()) {
            std::cerr << "--dump-lego-source/--verify-lego-source requires --fx-root <shader-source-root>\n";
            return 2;
        }

        std::optional<lu::renderer::lu_import::ShaderDatabase> shader_database;
        if (!args.client_root.empty()) {
            shader_database = lu::renderer::lu_import::ShaderDatabase::loadFromClientRoot(args.client_root);
            if (!shader_database) {
                std::cerr << "Could not load CDClient shader database from " << args.client_root << "\n";
                return 1;
            }
        }

        const SourceCoverageSummary summary = printSourceCoverage(
            args.fx_root,
            shader_database ? &*shader_database : nullptr,
            args.verify_lego_source ? true : args.only_unmapped_source);
        if (args.verify_lego_source) {
            return summary.unmapped == 0 && summary.not_checked == 0 ? 0 : 1;
        }
        return 0;
    }

    if (args.dump_shaders || args.dump_assets || args.find_shader_users ||
        args.lego_user_coverage || args.fxshader_coverage || args.verify_lego_family ||
        args.verify_lego_goal_assets) {
        if (args.client_root.empty()) {
            std::cerr << "--dump-shaders/--dump-lego-family/--verify-lego-family/--verify-lego-goal-assets/--dump-assets/--find-shader-users/--lego-user-coverage/--fxshader-coverage requires --client-root <res-or-client-root>\n";
            return 2;
        }
        if (args.verify_lego_family && args.fx_root.empty()) {
            std::cerr << "--verify-lego-family requires --fx-root <shader-source-root>\n";
            return 2;
        }
        auto shader_database = lu::renderer::lu_import::ShaderDatabase::loadFromClientRoot(args.client_root);
        if (!shader_database) {
            std::cerr << "Could not load CDClient shader database from " << args.client_root << "\n";
            return 1;
        }

        if (args.verify_lego_goal_assets) {
            return verifyLegoGoalAssets(args);
        }

        if (args.dump_assets) {
            if (!args.shader_id_filter) {
                std::cerr << "--dump-assets requires --shader-id <id>\n";
                return 2;
            }
            auto shader = shader_database->shaderInfo(*args.shader_id_filter);
            auto assets = shader_database->assetPathsForShader(*args.shader_id_filter);
            std::cout << "Assets for shader id " << *args.shader_id_filter;
            if (shader) {
                std::cout << " (" << shader->label << ", gameValue=" << shader->game_value << ")";
            }
            std::cout << ": " << assets.size() << "\n";
            for (const auto& asset : assets) {
                std::cout << asset << "\n";
            }
            return 0;
        }

        if (args.fxshader_coverage) {
            const std::filesystem::path res_root =
                lu::renderer::lu_import::ShaderDatabase::normalizeClientRoot(args.client_root);
            const std::filesystem::path mesh_root = res_root / "mesh";

            std::map<std::string, FxShaderTokenCoverage> coverage_by_token;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(
                     mesh_root,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec), end;
                 !ec && it != end;
                 it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".nif") continue;

                const std::string asset =
                    lu::renderer::lu_import::ShaderDatabase::assetPathRelativeToRes(res_root, it->path());
                if (!assetMatches(asset, args.asset_filter)) continue;

                const NifShaderHintIds hints = scanNifShaderHints(it->path(), *shader_database);
                const auto asset_shader = shader_database->resolveAssetMeshShader(asset, std::string{});
                for (const std::string& token : hints.fxshader_tokens) {
                    auto& coverage = coverage_by_token[token];
                    ++coverage.nif_count;
                    if (asset_shader.resolved && !asset_shader.asset_is_multishader) {
                        ++coverage.cdclient_shader_counts[asset_shader.shader.id];
                    }
                    addSample(coverage.samples, asset);
                }
            }

            std::cout << "token | nifs | cdclientShaders | shader | gameValue | label | legoFamily | variant | program | port | fxCheck | fx | technique | sourceNote | samples\n";
            for (const auto& [token, coverage] : coverage_by_token) {
                std::optional<int32_t> shader_id =
                    lu::renderer::lu_import::inferShaderIdFromFxShaderMetadata(token);
                lu::renderer::lu_import::LuShaderInfo shader;
                lu::renderer::lu_import::LuShaderPolicy policy;
                bool resolved = false;
                if (shader_id) {
                    if (auto info = shader_database->shaderInfo(*shader_id)) {
                        shader = *info;
                        policy = lu::renderer::lu_import::applyFxShaderMetadataPolicyOverrides(
                            lu::renderer::lu_import::shaderPolicyFromInfo(shader),
                            token);
                        resolved = true;
                    }
                }

                const bool lego_family = resolved && isLegoFamilyShader(shader.label, policy);
                const char* fx_check = resolved
                    ? sourceCheck(
                          args.fx_root,
                          policy.source_file,
                          policy.source_technique,
                          portStatusName(policy.port_status))
                    : "unresolved";
                std::cout << token
                          << " | " << coverage.nif_count
                          << " | " << joinShaderCounts(coverage.cdclient_shader_counts, *shader_database)
                          << " | " << (resolved ? std::to_string(shader.id) : "-")
                          << " | " << (resolved ? std::to_string(shader.game_value) : "-")
                          << " | " << (resolved ? shader.label : "-")
                          << " | " << boolText(lego_family)
                          << " | " << (resolved ? legoppVariantName(policy.legopp_variant) : "-")
                          << " | " << (resolved ? shaderFamilyName(policy.shader_family) : "-")
                          << " | " << (resolved ? portStatusName(policy.port_status) : "-")
                          << " | " << fx_check
                          << " | " << (resolved ? policy.source_file : "-")
                          << " | " << (resolved ? policy.source_technique : "-")
                          << " | " << (resolved && !policy.source_status_note.empty()
                              ? policy.source_status_note
                              : "-")
                          << " | " << joinSamples(coverage.samples)
                          << "\n";
            }
            return 0;
        }

        if (args.lego_user_coverage) {
            const std::filesystem::path res_root =
                lu::renderer::lu_import::ShaderDatabase::normalizeClientRoot(args.client_root);

            std::map<int32_t, LegoUserCoverage> coverage_by_shader;
            for (const auto& shader : shader_database->shaders()) {
                const auto policy = lu::renderer::lu_import::shaderPolicyFromInfo(shader);
                if (!isLegoFamilyShader(shader.label, policy)) continue;

                LegoUserCoverage coverage;
                coverage.shader = shader;
                coverage.policy = policy;
                coverage_by_shader.emplace(shader.id, std::move(coverage));
            }

            for (auto& [shader_id, coverage] : coverage_by_shader) {
                const auto assets = shader_database->assetPathsForShader(shader_id);
                for (const auto& asset : assets) {
                    if (!assetMatches(asset, args.asset_filter)) continue;
                    ++coverage.direct_assets;
                    addSample(coverage.samples, "cdclient:" + asset);
                }
            }

            std::set<int32_t> shaders_needing_candidate_import;
            for (const auto& [shader_id, coverage] : coverage_by_shader) {
                if (coverage.direct_assets == 0) {
                    shaders_needing_candidate_import.insert(shader_id);
                }
            }

            std::map<std::string, std::filesystem::path> candidate_paths;
            std::map<std::string, std::set<int32_t>> candidate_shader_ids_by_asset;
            std::map<int32_t, size_t> scheduled_imports_by_shader;
            const size_t import_cap_per_shader = args.limit == 0
                ? static_cast<size_t>(-1)
                : std::min(args.limit, static_cast<size_t>(3));
            auto scheduleCandidateImport = [&](
                const std::string& asset,
                const std::filesystem::path& path,
                int32_t shader_id) {
                if (shaders_needing_candidate_import.find(shader_id) == shaders_needing_candidate_import.end()) {
                    return;
                }
                if (scheduled_imports_by_shader[shader_id] >= import_cap_per_shader) {
                    return;
                }
                const std::string normalized_asset =
                    lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset);
                auto& candidate_ids = candidate_shader_ids_by_asset[normalized_asset];
                if (candidate_ids.insert(shader_id).second) {
                    candidate_paths.emplace(normalized_asset, path);
                    ++scheduled_imports_by_shader[shader_id];
                }
            };

            std::set<std::string> multishader_assets;
            for (const auto& asset : shader_database->assetPathsForShader(100)) {
                if (!hasNifExtension(asset)) continue;
                multishader_assets.insert(
                    lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset));
            }
            const std::filesystem::path mesh_root = res_root / "mesh";
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(
                     mesh_root,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec), end;
                 !ec && it != end;
                 it.increment(ec)) {
                if (!it->is_regular_file(ec) || it->path().extension() != ".nif") continue;

                const std::string asset =
                    lu::renderer::lu_import::ShaderDatabase::assetPathRelativeToRes(res_root, it->path());
                if (!assetMatches(asset, args.asset_filter)) continue;

                const std::string normalized_asset =
                    lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset);
                const NifShaderHintIds hints = scanNifShaderHints(it->path(), *shader_database);

                if (multishader_assets.find(normalized_asset) != multishader_assets.end()) {
                    for (int32_t shader_id : hints.prefix_ids) {
                        auto coverage_it = coverage_by_shader.find(shader_id);
                        if (coverage_it == coverage_by_shader.end()) continue;
                        ++coverage_it->second.prefix_candidate_nifs;
                        addSample(coverage_it->second.samples, "prefix:" + asset);
                        scheduleCandidateImport(asset, it->path(), shader_id);
                    }
                }

                for (int32_t shader_id : hints.metadata_ids) {
                    auto coverage_it = coverage_by_shader.find(shader_id);
                    if (coverage_it == coverage_by_shader.end()) continue;
                    ++coverage_it->second.metadata_candidate_nifs;
                    addSample(coverage_it->second.samples, "metadata:" + asset);
                    scheduleCandidateImport(asset, it->path(), shader_id);
                }
            }

            for (const auto& [asset, path] : candidate_paths) {
                const auto candidate_ids_it = candidate_shader_ids_by_asset.find(asset);
                if (candidate_ids_it != candidate_shader_ids_by_asset.end()) {
                    for (int32_t shader_id : candidate_ids_it->second) {
                        auto coverage_it = coverage_by_shader.find(shader_id);
                        if (coverage_it != coverage_by_shader.end()) {
                            ++coverage_it->second.imported_candidate_nifs;
                        }
                    }
                }

                lu::renderer::lu_import::NifImportOptions options;
                options.client_root = args.client_root;
                options.nif_path = path;
                auto imported = lu::renderer::lu_import::importNif(options);
                if (!imported.error.empty()) continue;

                for (const auto& mesh : imported.world.meshes) {
                    auto coverage_it = coverage_by_shader.find(mesh.material.lu_shader_id);
                    if (coverage_it == coverage_by_shader.end()) continue;
                    ++coverage_it->second.validated_meshes;
                    addSample(
                        coverage_it->second.samples,
                        "mesh:" + imported.world.source_asset_path + "::" + mesh.name);
                }
            }

            std::cout << "id | gameValue | label | variant | program | port | fxCheck | fx | technique | directAssets | prefixCandidates | metadataCandidates | importedCandidates | validatedMeshes | samples | sourceNote | validationNote\n";
            for (const auto& [_, coverage] : coverage_by_shader) {
                const char* fx_check = sourceCheck(
                    args.fx_root,
                    coverage.policy.source_file,
                    coverage.policy.source_technique,
                    portStatusName(coverage.policy.port_status));
                std::cout << coverage.shader.id
                          << " | " << coverage.shader.game_value
                          << " | " << coverage.shader.label
                          << " | " << legoppVariantName(coverage.policy.legopp_variant)
                          << " | " << shaderFamilyName(coverage.policy.shader_family)
                          << " | " << portStatusName(coverage.policy.port_status)
                          << " | " << fx_check
                          << " | " << coverage.policy.source_file
                          << " | " << coverage.policy.source_technique
                          << " | " << coverage.direct_assets
                          << " | " << coverage.prefix_candidate_nifs
                          << " | " << coverage.metadata_candidate_nifs
                          << " | " << coverage.imported_candidate_nifs
                          << " | " << coverage.validated_meshes
                          << " | " << joinSamples(coverage.samples)
                          << " | " << (coverage.policy.source_status_note.empty()
                              ? "-"
                              : coverage.policy.source_status_note)
                          << " | " << (coverage.policy.validation_status_note.empty()
                              ? "-"
                              : coverage.policy.validation_status_note)
                          << "\n";
            }
            return 0;
        }

        if (args.find_shader_users) {
            if (!args.shader_id_filter) {
                std::cerr << "--find-shader-users requires --shader-id <id>\n";
                return 2;
            }

            const int32_t target_shader_id = *args.shader_id_filter;
            const std::filesystem::path res_root =
                lu::renderer::lu_import::ShaderDatabase::normalizeClientRoot(args.client_root);
            auto shader = shader_database->shaderInfo(target_shader_id);
            std::cout << "Resolved users for shader id " << target_shader_id;
            if (shader) {
                std::cout << " (" << shader->label << ", gameValue=" << shader->game_value << ")";
            }
            std::cout << "\n";

            size_t resolved_hits = 0;
            size_t candidate_hits = 0;
            size_t examined_multishader_nifs = 0;
            size_t imported_multishader_nifs = 0;
            size_t prefix_candidate_nifs = 0;
            size_t examined_metadata_nifs = 0;
            size_t metadata_candidate_nifs = 0;
            size_t imported_metadata_nifs = 0;
            bool limit_reached = false;
            std::set<std::string> seen_assets;
            const auto exact_assets = shader_database->assetPathsForShader(target_shader_id);
            for (const auto& asset : exact_assets) {
                if (!assetMatches(asset, args.asset_filter)) continue;
                seen_assets.insert(lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset));
                std::cout << "cdclient | " << asset << "\n";
                ++resolved_hits;
                if (args.limit != 0 && resolved_hits + candidate_hits >= args.limit) {
                    limit_reached = true;
                    break;
                }
            }

            if (!limit_reached) {
                const auto multishader_assets = shader_database->assetPathsForShader(100);
                for (const auto& asset : multishader_assets) {
                    if (!assetMatches(asset, args.asset_filter)) continue;
                    if (!hasNifExtension(asset)) continue;
                    const std::filesystem::path nif_path = res_root / std::filesystem::path(asset);
                    ++examined_multishader_nifs;
                    const NifShaderHintIds hints = scanNifShaderHints(nif_path, *shader_database);
                    if (hints.prefix_ids.find(target_shader_id) == hints.prefix_ids.end()) continue;
                    seen_assets.insert(lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset));
                    ++prefix_candidate_nifs;
                    if (args.candidate_only) {
                        std::cout << "multishader-prefix-candidate | " << asset << "\n";
                        ++candidate_hits;
                        if (args.limit != 0 && resolved_hits + candidate_hits >= args.limit) {
                            limit_reached = true;
                            break;
                        }
                        continue;
                    }

                    ++imported_multishader_nifs;
                    lu::renderer::lu_import::NifImportOptions options;
                    options.client_root = args.client_root;
                    options.nif_path = nif_path;
                    auto imported = lu::renderer::lu_import::importNif(options);
                    if (!imported.error.empty()) continue;

                    for (const auto& mesh : imported.world.meshes) {
                        if (mesh.material.lu_shader_id != target_shader_id) continue;
                        std::cout << "multishader | " << asset << " | " << mesh.name
                                  << " | metadata=" << mesh.material.lu_shader_metadata
                                  << " | technique=" << mesh.material.lu_shader_source_technique
                                  << "\n";
                        ++resolved_hits;
                        if (args.limit != 0 && resolved_hits + candidate_hits >= args.limit) {
                            limit_reached = true;
                            break;
                        }
                    }
                    if (limit_reached) break;
                }
            }

            if (!limit_reached && shader) {
                std::filesystem::path mesh_root = res_root / "mesh";
                std::error_code ec;
                for (std::filesystem::recursive_directory_iterator it(
                         mesh_root,
                         std::filesystem::directory_options::skip_permission_denied,
                         ec), end;
                     !ec && it != end;
                     it.increment(ec)) {
                    if (!it->is_regular_file(ec) || it->path().extension() != ".nif") continue;
                    const std::string asset =
                        lu::renderer::lu_import::ShaderDatabase::assetPathRelativeToRes(res_root, it->path());
                    if (!assetMatches(asset, args.asset_filter)) continue;
                    if (seen_assets.find(lu::renderer::lu_import::ShaderDatabase::normalizeAssetPath(asset)) != seen_assets.end()) {
                        continue;
                    }
                    ++examined_metadata_nifs;
                    const NifShaderHintIds hints = scanNifShaderHints(it->path(), *shader_database);
                    if (hints.metadata_ids.find(target_shader_id) == hints.metadata_ids.end()) continue;
                    ++metadata_candidate_nifs;
                    if (args.candidate_only) {
                        std::cout << "nif-metadata-candidate | " << asset << "\n";
                        ++candidate_hits;
                        if (args.limit != 0 && resolved_hits + candidate_hits >= args.limit) {
                            limit_reached = true;
                            break;
                        }
                        continue;
                    }

                    ++imported_metadata_nifs;
                    lu::renderer::lu_import::NifImportOptions options;
                    options.client_root = args.client_root;
                    options.nif_path = it->path();
                    auto imported = lu::renderer::lu_import::importNif(options);
                    if (!imported.error.empty()) continue;

                    for (const auto& mesh : imported.world.meshes) {
                        if (mesh.material.lu_shader_id != target_shader_id) continue;
                        std::cout << "nif-metadata | " << asset << " | " << mesh.name
                                  << " | source=" << resolutionSourceName(mesh.material.lu_shader_resolution_source)
                                  << " | metadata=" << mesh.material.lu_shader_metadata
                                  << " | technique=" << mesh.material.lu_shader_source_technique
                                  << "\n";
                        ++resolved_hits;
                        if (args.limit != 0 && resolved_hits + candidate_hits >= args.limit) {
                            limit_reached = true;
                            break;
                        }
                    }
                    if (limit_reached) break;
                }
            }

            std::cout << "resolvedHits=" << resolved_hits
                      << " candidateOnlyHits=" << candidate_hits
                      << " prefixCandidateNifs=" << prefix_candidate_nifs
                      << " examinedMultishaderNifs=" << examined_multishader_nifs
                      << " importedMultishaderNifs=" << imported_multishader_nifs
                      << " metadataCandidateNifs=" << metadata_candidate_nifs
                      << " examinedMetadataNifs=" << examined_metadata_nifs
                      << " importedMetadataNifs=" << imported_metadata_nifs
                      << " limitReached=" << boolText(limit_reached)
                      << "\n";
            return 0;
        }

        if (args.verify_lego_family) {
            size_t candidates = 0;
            size_t strict_total = 0;
            size_t verified = 0;
            size_t issues = 0;
            size_t outside_family = 0;
            std::cout << "id | gameValue | label | scope | port | fxCheck | fx | technique | sourceNote | validationNote | issue\n";
            for (const auto& shader : shader_database->shaders()) {
                const auto policy = lu::renderer::lu_import::shaderPolicyFromInfo(shader);
                if (!isLegoFamilyShader(shader.label, policy)) continue;
                ++candidates;
                const bool strict_family = isStrictLegoFamilyShader(shader.label, policy);
                if (strict_family) {
                    ++strict_total;
                }

                const char* fx_check = sourceCheck(
                    args.fx_root,
                    policy.source_file,
                    policy.source_technique,
                    portStatusName(policy.port_status));
                const bool port_verified = isVerifiedPortStatus(policy.port_status);
                const bool source_verified = std::string(fx_check) == "ok";
                if (strict_family && port_verified && source_verified) {
                    ++verified;
                    continue;
                }
                if (!strict_family) {
                    ++outside_family;
                    std::cout << shader.id
                              << " | " << shader.game_value
                              << " | " << shader.label
                              << " | candidate"
                              << " | " << portStatusName(policy.port_status)
                              << " | " << fx_check
                              << " | " << policy.source_file
                              << " | " << policy.source_technique
                              << " | " << (policy.source_status_note.empty() ? "-" : policy.source_status_note)
                              << " | " << (policy.validation_status_note.empty() ? "-" : policy.validation_status_note)
                              << " | outside-lego-family";
                    if (!port_verified) {
                        std::cout << ",port-status=" << portStatusName(policy.port_status);
                    }
                    if (!source_verified) {
                        std::cout << ",source-check=" << fx_check;
                    }
                    std::cout << "\n";
                    continue;
                }

                ++issues;
                std::vector<std::string> reasons;
                if (!port_verified) {
                    reasons.push_back(std::string{"port-status="} + portStatusName(policy.port_status));
                }
                if (!source_verified) {
                    reasons.push_back(std::string{"source-check="} + fx_check);
                }

                std::cout << shader.id
                          << " | " << shader.game_value
                          << " | " << shader.label
                          << " | strict"
                          << " | " << portStatusName(policy.port_status)
                          << " | " << fx_check
                          << " | " << policy.source_file
                          << " | " << policy.source_technique
                          << " | " << (policy.source_status_note.empty() ? "-" : policy.source_status_note)
                          << " | " << (policy.validation_status_note.empty() ? "-" : policy.validation_status_note)
                          << " | ";
                for (size_t i = 0; i < reasons.size(); ++i) {
                    if (i > 0) std::cout << ",";
                    std::cout << reasons[i];
                }
                std::cout << "\n";
            }

            std::cout << "summary | candidateRows=" << candidates
                      << " strictLegoRows=" << strict_total
                      << " verified=" << verified
                      << " outsideLegoFamily=" << outside_family
                      << " issues=" << issues << "\n";
            return issues == 0 ? 0 : 1;
        }

        std::cout << "id | gameValue | label | variant | program | port | fx | technique | sourceNote | validationNote | alpha | alphaSemantic | cull | zwrite | flags\n";
        for (const auto& shader : shader_database->shaders()) {
            if (!labelMatches(shader.label, args.shader_label_filter)) continue;
            const auto policy = lu::renderer::lu_import::shaderPolicyFromInfo(shader);
            if (args.dump_lego_family && !isLegoFamilyShader(shader.label, policy)) continue;
            std::cout << shader.id
                      << " | " << shader.game_value
                      << " | " << shader.label
                      << " | " << legoppVariantName(policy.legopp_variant)
                      << " | " << shaderFamilyName(policy.shader_family)
                      << " | " << portStatusName(policy.port_status)
                      << " | " << policy.source_file
                      << " | " << policy.source_technique
                      << " | " << (policy.source_status_note.empty() ? "-" : policy.source_status_note)
                      << " | " << (policy.validation_status_note.empty() ? "-" : policy.validation_status_note)
                      << " | " << alphaModeName(policy.alpha_mode)
                      << " | " << alphaSemanticName(policy.alpha_semantic)
                      << " | " << cullModeName(policy.cull_mode)
                      << " | " << boolText(policy.depth_write)
                      << " | vc=" << boolText(policy.uses_vertex_color)
                      << ",tex=" << boolText(policy.uses_texture)
                      << ",mat=" << boolText(policy.uses_material_diffuse)
                      << ",fog=" << boolText(policy.uses_fog)
                      << ",spec=" << boolText(policy.uses_specular)
                      << ",refl=" << boolText(policy.uses_reflection)
                      << ",shadowTerrain=" << boolText(policy.uses_shadow_terrain)
                      << ",uvanim=" << boolText(policy.uses_uv_animation)
                      << ",alphaanim=" << boolText(policy.uses_alpha_animation)
                      << "\n";
        }
        return 0;
    }

    if (args.nifs.empty()) {
        printUsage();
        return 2;
    }

    std::map<int32_t, ShaderStats> stats_by_shader;
    size_t total_meshes = 0;
    size_t total_assets = 0;
    if (args.per_mesh) {
        printPerMeshHeader();
    }

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
            if (args.shader_id_filter && mesh.material.lu_shader_id != *args.shader_id_filter) {
                continue;
            }
            if (args.per_mesh) {
                printPerMeshRow(args.fx_root, imported.world.source_asset_path, mesh);
            }
            const auto& material = mesh.material;
            ShaderStats& stats = stats_by_shader[material.lu_shader_id];
            stats.shader_id = material.lu_shader_id;
            stats.game_value = material.lu_shader_game_value;
            stats.label = material.lu_shader_label;
            stats.program = shaderFamilyName(material.shader_family);
            stats.port_status = portStatusName(material.lu_shader_port_status);
            stats.variant = legoppVariantName(material.legopp_variant);
            stats.resolution_source = resolutionSourceName(material.lu_shader_resolution_source);
            stats.metadata = material.lu_shader_metadata;
            stats.alpha_semantic = alphaSemanticName(material.lu_shader_alpha_semantic);
            stats.source_file = material.lu_shader_source_file;
            stats.source_technique = material.lu_shader_source_technique;
            if (stats.source_note.empty() && !material.lu_shader_source_status_note.empty()) {
                stats.source_note = material.lu_shader_source_status_note;
            }
            if (stats.validation_note.empty() && !material.lu_shader_validation_status_note.empty()) {
                stats.validation_note = material.lu_shader_validation_status_note;
            }
            stats.alpha = alphaModeName(material.alpha_mode);
            stats.cull = cullModeName(material.cull_mode);
            stats.depth_write = stats.depth_write && material.depth_write;
            stats.resolved = stats.resolved || material.lu_shader_resolved;
            stats.multishader = stats.multishader || material.lu_shader_asset_is_multishader;
            stats.mesh_has_vertex_colors = stats.mesh_has_vertex_colors || material.mesh_has_vertex_colors;
            stats.uses_vertex_color = stats.uses_vertex_color || material.lu_shader_uses_vertex_color;
            stats.uses_texture = stats.uses_texture || material.lu_shader_uses_texture;
            stats.has_dark_texture = stats.has_dark_texture || !material.dark_texture_path.empty();
            stats.has_detail_texture = stats.has_detail_texture || !material.detail_texture_path.empty();
            stats.has_gloss_texture = stats.has_gloss_texture || !material.gloss_texture_path.empty();
            stats.has_glow_texture = stats.has_glow_texture || !material.glow_texture_path.empty();
            if (stats.diffuse_texture_sample.empty() && !material.diffuse_texture_path.empty()) {
                stats.diffuse_texture_sample = std::filesystem::path(material.diffuse_texture_path).filename().string();
            }
            if (stats.dark_texture_sample.empty() && !material.dark_texture_path.empty()) {
                stats.dark_texture_sample = std::filesystem::path(material.dark_texture_path).filename().string();
            }
            if (stats.detail_texture_sample.empty() && !material.detail_texture_path.empty()) {
                stats.detail_texture_sample = std::filesystem::path(material.detail_texture_path).filename().string();
            }
            if (stats.gloss_texture_sample.empty() && !material.gloss_texture_path.empty()) {
                stats.gloss_texture_sample = std::filesystem::path(material.gloss_texture_path).filename().string();
            }
            if (stats.glow_texture_sample.empty() && !material.glow_texture_path.empty()) {
                stats.glow_texture_sample = std::filesystem::path(material.glow_texture_path).filename().string();
            }
            stats.uses_material_diffuse = stats.uses_material_diffuse || material.lu_shader_uses_material_diffuse;
            stats.uses_fog = stats.uses_fog || material.lu_shader_uses_fog;
            stats.uses_specular = stats.uses_specular || material.lu_shader_uses_specular;
            stats.uses_reflection = stats.uses_reflection || material.lu_shader_uses_reflection;
            stats.uses_shadow_terrain = stats.uses_shadow_terrain || material.lu_shader_uses_shadow_terrain;
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

    if (args.per_mesh) {
        return 0;
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
    std::cout << "count | id/gv | label | variant | program | port | fxCheck | fx | technique | sourceNote | validationNote | alpha | alphaSemantic | cull | zwrite | resolved | source | metadata | multi | flags | emissive | samples\n";
    for (const auto& stats : sorted) {
        std::cout << stats.mesh_count
                  << " | " << stats.shader_id << "/" << stats.game_value
                  << " | " << stats.label
                  << " | " << stats.variant
                  << " | " << stats.program
                  << " | " << stats.port_status
                  << " | " << sourceCheck(args.fx_root, stats.source_file, stats.source_technique, stats.port_status)
                  << " | " << stats.source_file
                  << " | " << stats.source_technique
                  << " | " << (stats.source_note.empty() ? "-" : stats.source_note)
                  << " | " << (stats.validation_note.empty() ? "-" : stats.validation_note)
                  << " | " << stats.alpha
                  << " | " << stats.alpha_semantic
                  << " | " << stats.cull
                  << " | " << boolText(stats.depth_write)
                  << " | " << boolText(stats.resolved)
                  << " | " << stats.resolution_source
                  << " | " << (stats.metadata.empty() ? "none" : stats.metadata)
                  << " | " << boolText(stats.multishader)
            << " | vc=" << boolText(stats.uses_vertex_color)
            << ",meshvc=" << boolText(stats.mesh_has_vertex_colors)
            << ",tex=" << boolText(stats.uses_texture)
            << ",darkTex=" << boolText(stats.has_dark_texture)
            << ",detailTex=" << boolText(stats.has_detail_texture)
            << ",glossTex=" << boolText(stats.has_gloss_texture)
            << ",glowTex=" << boolText(stats.has_glow_texture)
            << ",texNames="
            << (stats.diffuse_texture_sample.empty() ? "-" : stats.diffuse_texture_sample)
            << "/"
            << (stats.dark_texture_sample.empty() ? "-" : stats.dark_texture_sample)
            << "/"
            << (stats.detail_texture_sample.empty() ? "-" : stats.detail_texture_sample)
            << "/"
            << (stats.gloss_texture_sample.empty() ? "-" : stats.gloss_texture_sample)
            << "/"
            << (stats.glow_texture_sample.empty() ? "-" : stats.glow_texture_sample)
                  << ",mat=" << boolText(stats.uses_material_diffuse)
                  << ",fog=" << boolText(stats.uses_fog)
                  << ",spec=" << boolText(stats.uses_specular)
                  << ",refl=" << boolText(stats.uses_reflection)
                  << ",shadowTerrain=" << boolText(stats.uses_shadow_terrain)
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
