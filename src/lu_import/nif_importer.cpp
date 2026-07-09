#include "lu/renderer/lu_import/nif_importer.h"

#include "lu/renderer/lu_import/shader_database.h"

#include "gamebryo/nif/nif_geometry.h"
#include "gamebryo/nif/nif_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

bool isShaderMetadataString(const std::string& value) {
    return containsCaseInsensitive(value, "NiMultiShader") ||
           containsCaseInsensitive(value, "_NIMS") ||
           containsCaseInsensitive(value, "FXshader_");
}

bool fileContainsAscii(std::span<const uint8_t> data, std::string_view needle) {
    if (needle.empty() || data.size() < needle.size()) return false;
    return std::search(
        data.begin(),
        data.end(),
        needle.begin(),
        needle.end()) != data.end();
}

class RawBlockReader {
public:
    explicit RawBlockReader(std::span<const uint8_t> data) : data_(data) {}

    bool canRead(size_t bytes) const {
        return pos_ <= data_.size() && bytes <= data_.size() - pos_;
    }

    void skip(size_t bytes) {
        pos_ = std::min(data_.size(), pos_ + bytes);
    }

    int32_t readS32() {
        if (!canRead(4)) return -1;
        uint32_t bits = static_cast<uint32_t>(data_[pos_]) |
            (static_cast<uint32_t>(data_[pos_ + 1]) << 8u) |
            (static_cast<uint32_t>(data_[pos_ + 2]) << 16u) |
            (static_cast<uint32_t>(data_[pos_ + 3]) << 24u);
        pos_ += 4;
        return static_cast<int32_t>(bits);
    }

    uint16_t readU16() {
        if (!canRead(2)) return 0;
        uint16_t value = static_cast<uint16_t>(data_[pos_]) |
            (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return value;
    }

    uint32_t readU32() {
        return static_cast<uint32_t>(readS32());
    }

    float readF32() {
        uint32_t bits = readU32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

struct MaterialEmissiveController {
    bool valid = false;
    float frequency = 1.0f;
    float phase = 0.0f;
    float start = 0.0f;
    float stop = 0.0f;
    Vec3 default_value = {0.0f, 0.0f, 0.0f};
    std::vector<Vec3Key> keys;
};

struct UvTiling {
    bool u = false;
    bool v = false;
};

std::string blockTypeName(const lu::assets::NifFile& nif, int32_t block_ref) {
    if (block_ref < 0 || static_cast<size_t>(block_ref) >= nif.block_type_indices.size()) return {};
    uint16_t type_index = nif.block_type_indices[static_cast<size_t>(block_ref)];
    if (type_index >= nif.block_types.size()) return {};
    return nif.block_types[type_index];
}

std::span<const uint8_t> blockData(const lu::assets::NifFile& nif, int32_t block_ref) {
    if (block_ref < 0 || static_cast<size_t>(block_ref) >= nif.block_data.size()) return {};
    return std::span<const uint8_t>(nif.block_data[static_cast<size_t>(block_ref)].data(),
        nif.block_data[static_cast<size_t>(block_ref)].size());
}

int32_t materialControllerRef(const lu::assets::NifFile& nif, int32_t material_block) {
    RawBlockReader r(blockData(nif, material_block));
    if (!r.canRead(12)) return -1;
    r.readS32(); // name
    uint32_t extra_count = r.readU32();
    if (extra_count > 1024 || !r.canRead((static_cast<size_t>(extra_count) + 1u) * 4u)) return -1;
    r.skip(static_cast<size_t>(extra_count) * 4u);
    return r.readS32();
}

Vec3 readVec3(RawBlockReader& r) {
    return {r.readF32(), r.readF32(), r.readF32()};
}

std::vector<Vec3Key> parseNiPosData(std::span<const uint8_t> data) {
    RawBlockReader r(data);
    std::vector<Vec3Key> keys;
    if (!r.canRead(8)) return keys;

    uint32_t key_count = r.readU32();
    uint32_t key_type = r.readU32();
    if (key_count > 4096) return {};

    keys.reserve(key_count);
    for (uint32_t i = 0; i < key_count; ++i) {
        if (!r.canRead(16)) break;
        Vec3Key key;
        key.time = r.readF32();
        key.value = readVec3(r);
        if (key_type == 2) {
            if (!r.canRead(24)) break;
            key.forward_tangent = readVec3(r);
            key.backward_tangent = readVec3(r);
        } else if (key_type == 3) {
            if (r.canRead(12)) r.skip(12);
        }
        keys.push_back(key);
    }
    return keys;
}

MaterialEmissiveController parseMaterialEmissiveController(
    const lu::assets::NifFile& nif,
    int32_t controller_ref) {

    std::unordered_set<int32_t> visited;
    while (controller_ref >= 0 && visited.insert(controller_ref).second) {
        if (blockTypeName(nif, controller_ref) != "NiMaterialColorController") return {};

        RawBlockReader r(blockData(nif, controller_ref));
        if (!r.canRead(32)) return {};

        int32_t next_ref = r.readS32();
        r.readU16(); // flags
        float frequency = r.readF32();
        float phase = r.readF32();
        float start = r.readF32();
        float stop = r.readF32();
        r.readS32(); // target material
        int32_t interpolator_ref = r.readS32();
        uint16_t target_color = r.readU16();

        if (target_color != 3) {
            controller_ref = next_ref;
            continue;
        }

        if (blockTypeName(nif, interpolator_ref) != "NiPoint3Interpolator") return {};
        RawBlockReader interp(blockData(nif, interpolator_ref));
        if (!interp.canRead(16)) return {};

        MaterialEmissiveController controller;
        controller.valid = true;
        controller.frequency = frequency;
        controller.phase = phase;
        controller.start = start;
        controller.stop = stop;
        controller.default_value = readVec3(interp);
        int32_t data_ref = interp.readS32();

        if (blockTypeName(nif, data_ref) == "NiPosData") {
            controller.keys = parseNiPosData(blockData(nif, data_ref));
        }
        return controller;
    }
    return {};
}

std::unordered_map<uint32_t, MaterialEmissiveController> collectMaterialEmissiveControllers(
    const lu::assets::NifFile& nif) {

    std::unordered_map<uint32_t, MaterialEmissiveController> controllers_by_node;
    for (const auto& node : nif.nodes) {
        for (int32_t prop_ref : node.properties) {
            if (prop_ref < 0 || blockTypeName(nif, prop_ref) != "NiMaterialProperty") continue;
            MaterialEmissiveController controller =
                parseMaterialEmissiveController(nif, materialControllerRef(nif, prop_ref));
            if (controller.valid) {
                controllers_by_node[node.block_index] = std::move(controller);
                break;
            }
        }
    }
    return controllers_by_node;
}

std::string joinShaderMetadata(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        if (joined.find(value) != std::string::npos) continue;
        if (!joined.empty()) joined += ";";
        joined += value;
    }
    return joined;
}

std::string collectNifShaderMetadata(const lu::assets::NifFile& nif) {
    std::vector<std::string> values;
    for (const auto& value : nif.string_table) {
        if (isShaderMetadataString(value)) {
            values.push_back(value);
        }
    }
    return joinShaderMetadata(values);
}

std::string uniqueNifShaderMetadata(const std::string& nif_metadata) {
    if (nif_metadata.empty()) return {};
    if (nif_metadata.find(';') != std::string::npos) return {};
    return nif_metadata;
}

std::string shaderMetadataForMaterial(const std::string& material_name) {
    if (isShaderMetadataString(material_name)) return material_name;
    return {};
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

TextureAddressMode textureAddressFromClampMode(bool has_clamp_mode, uint8_t clamp_mode) {
    TextureAddressMode address;
    address.authored = has_clamp_mode;
    switch (clamp_mode & 0x03u) {
    case 0: // Clamp S, clamp T.
        address.wrap_u = false;
        address.wrap_v = false;
        break;
    case 1: // Clamp S, wrap T.
        address.wrap_u = false;
        address.wrap_v = true;
        break;
    case 2: // Wrap S, clamp T.
        address.wrap_u = true;
        address.wrap_v = false;
        break;
    case 3: // Wrap S, wrap T.
    default:
        address.wrap_u = true;
        address.wrap_v = true;
        break;
    }
    return address;
}

TextureAddressMode allowRepeatForTiledUv(TextureAddressMode address, UvTiling tiling) {
    if (tiling.u) address.wrap_u = true;
    if (tiling.v) address.wrap_v = true;
    return address;
}

UvTiling detectUvTiling(const lu::assets::NifRenderMesh& mesh, bool use_second_uv) {
    constexpr float kUvRepeatEpsilon = 0.001f;
    UvTiling tiling;
    for (const auto& vertex : mesh.vertices) {
        const float u = use_second_uv ? vertex.uv2[0] : vertex.uv[0];
        const float v = use_second_uv ? vertex.uv2[1] : vertex.uv[1];
        tiling.u = tiling.u || u < -kUvRepeatEpsilon || u > 1.0f + kUvRepeatEpsilon;
        tiling.v = tiling.v || v < -kUvRepeatEpsilon || v > 1.0f + kUvRepeatEpsilon;
    }
    return tiling;
}

UvTiling mergeUvTiling(UvTiling a, UvTiling b) {
    return {a.u || b.u, a.v || b.v};
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
           family == LegacyShaderFamily::LegoppEmissive ||
           family == LegacyShaderFamily::LegoppEffect;
}

bool hasExactLegoppFxShaderMetadata(const MaterialAsset& material) {
    return containsCaseInsensitive(material.lu_shader_metadata, "FXshader_") &&
           containsCaseInsensitive(material.lu_shader_metadata, "LEGOPPLighting") &&
           containsCaseInsensitive(material.lu_shader_source_technique, "Technique_LEGOPPLighting");
}

std::string legoppGeometryPrefix(bool has_vertex_colors, bool has_texture, bool is_skinned) {
    if (is_skinned) {
        if (has_vertex_colors && has_texture) return "Technique_LEGOPPLightingVertColorTexturedSkinned";
        if (has_vertex_colors) return "Technique_LEGOPPLightingVertColorSkinned";
        if (has_texture) return "Technique_LEGOPPLightingTexturedSkinned";
        return "Technique_LEGOPPLightingSkinnedOK";
    }

    if (has_vertex_colors && has_texture) return "Technique_LEGOPPLightingVertColorTextured";
    if (has_vertex_colors) return "Technique_LEGOPPLightingVertColor";
    if (has_texture) return "Technique_LEGOPPLightingTextured";
    return "Technique_LEGOPPLightingOK";
}

std::string legoppItemTechnique(bool has_vertex_colors, bool has_texture, bool glow) {
    std::string suffix = glow ? "_Item_Glow" : "_Item";
    if (has_vertex_colors && has_texture) return "Technique_LEGOPPLightingVertColorTextured" + suffix;
    if (has_vertex_colors) return "Technique_LEGOPPLightingVertColor" + suffix;
    if (has_texture) return "Technique_LEGOPPLightingTextured" + suffix;
    return "Technique_LEGOPPLighting" + suffix;
}

std::string legoppFrontEndTechnique(bool has_vertex_colors, bool has_texture, bool is_skinned) {
    return legoppGeometryPrefix(has_vertex_colors, has_texture, is_skinned) + "_FrontEnd";
}

std::string legoppDarklingTechnique(LegoppShaderVariant variant, bool is_skinned) {
    std::string suffix = "_Darkling";
    if (variant == LegoppShaderVariant::DarklingSpecular ||
        variant == LegoppShaderVariant::DarklingSpecularShinyGlint) {
        suffix = "_Darkling_Specular";
    } else if (variant == LegoppShaderVariant::DarklingStructure ||
               variant == LegoppShaderVariant::DarklingStructureShinyGlint) {
        suffix = "_Darkling_NonDecal";
    }
    return is_skinned
        ? "Technique_LEGOPPLightingVertColorSkinned" + suffix
        : "Technique_LEGOPPLightingVertColor" + suffix;
}

enum class LegoppSourceFlavor {
    Default,
    Low,
    NoEnv,
    NoEnvNoSpec,
};

LegoppSourceFlavor legoppSourceFlavor(const std::string& source_file) {
    const std::string lower = lowerCopy(source_file);
    if (lower.find("noenv_nospec") != std::string::npos) return LegoppSourceFlavor::NoEnvNoSpec;
    if (lower.find("noenv") != std::string::npos) return LegoppSourceFlavor::NoEnv;
    if (lower.find("_low") != std::string::npos || lower.find("lighting_low") != std::string::npos) {
        return LegoppSourceFlavor::Low;
    }
    return LegoppSourceFlavor::Default;
}

std::string legoppFlavorToken(LegoppSourceFlavor flavor) {
    switch (flavor) {
    case LegoppSourceFlavor::Low: return "_low";
    case LegoppSourceFlavor::NoEnv: return "_noenv";
    case LegoppSourceFlavor::NoEnvNoSpec: return "_noenv_nospec";
    case LegoppSourceFlavor::Default: return "";
    }
    return "";
}

std::string legoppVariantSuffix(LegoppShaderVariant variant) {
    switch (variant) {
    case LegoppShaderVariant::NoAmbient: return "_NoAmbient";
    case LegoppShaderVariant::Emissive: return "_Emissive";
    case LegoppShaderVariant::SuperEmissive: return "_SuperEmissive";
    case LegoppShaderVariant::Glow: return "_Glow";
    case LegoppShaderVariant::GlowIgnoreVertAlpha: return "_Glow_IgnoreVertAlpha";
    case LegoppShaderVariant::Grayscale: return "_Grayscale";
    case LegoppShaderVariant::MaskedNonDecal: return "_Masked_NonDecal";
    case LegoppShaderVariant::Reveal: return "_Reveal";
    case LegoppShaderVariant::FadeUp: return "_FadeUp";
    case LegoppShaderVariant::AnimUv: return "_AnimUV";
    case LegoppShaderVariant::ShinyGlint:
    case LegoppShaderVariant::DarklingShinyGlint:
    case LegoppShaderVariant::DarklingSpecularShinyGlint:
    case LegoppShaderVariant::DarklingStructureShinyGlint:
        return "_ShinyGlint";
    default: return "";
    }
}

std::string legoppFlavoredTechnique(
    const std::string& geometry_prefix,
    LegoppShaderVariant variant,
    LegoppSourceFlavor flavor) {
    const std::string flavor_token = legoppFlavorToken(flavor);
    if (flavor == LegoppSourceFlavor::Default) {
        return geometry_prefix + legoppVariantSuffix(variant);
    }

    switch (variant) {
    case LegoppShaderVariant::Base:
    case LegoppShaderVariant::None:
        return geometry_prefix + flavor_token;
    case LegoppShaderVariant::NoAmbient:
        return geometry_prefix + "_NoAmbient" + flavor_token;
    case LegoppShaderVariant::Reveal:
        return geometry_prefix + "_Reveal" + flavor_token;
    case LegoppShaderVariant::MaskedNonDecal:
        return geometry_prefix + "_NonDecal" + flavor_token;
    case LegoppShaderVariant::AnimUv:
        return geometry_prefix + "_AnimUV" + flavor_token;
    case LegoppShaderVariant::Grayscale:
    case LegoppShaderVariant::Glow:
    case LegoppShaderVariant::GlowIgnoreVertAlpha:
    case LegoppShaderVariant::FadeUp:
    case LegoppShaderVariant::Emissive:
    case LegoppShaderVariant::SuperEmissive:
    case LegoppShaderVariant::ShinyGlint:
    case LegoppShaderVariant::DarklingShinyGlint:
    case LegoppShaderVariant::DarklingSpecularShinyGlint:
    case LegoppShaderVariant::DarklingStructureShinyGlint:
        return geometry_prefix + flavor_token + legoppVariantSuffix(variant);
    default:
        return geometry_prefix + legoppVariantSuffix(variant);
    }
}

bool shaderAllowsAuthoredAlpha(RenderAlphaMode alpha_mode, ShaderAlphaSemantic semantic) {
    if (alpha_mode == RenderAlphaMode::AlphaBlend ||
        alpha_mode == RenderAlphaMode::AlphaTest ||
        alpha_mode == RenderAlphaMode::Additive) {
        return true;
    }
    return semantic == ShaderAlphaSemantic::OutputAlpha ||
           semantic == ShaderAlphaSemantic::AlphaTest ||
           semantic == ShaderAlphaSemantic::Unknown;
}

void applyLegoppGeometryVariant(MaterialAsset& material, bool has_vertex_colors, bool has_texture, bool is_skinned) {
    if (!isLegoppVariant(material.shader_family)) return;

    if (hasExactLegoppFxShaderMetadata(material)) {
        return;
    }

    material.lu_shader_uses_vertex_color = has_vertex_colors;
    material.lu_shader_uses_texture = has_texture;
    const LegoppSourceFlavor source_flavor = legoppSourceFlavor(material.lu_shader_source_file);

    if (material.legopp_variant == LegoppShaderVariant::Darkling ||
        material.legopp_variant == LegoppShaderVariant::DarklingSpecular ||
        material.legopp_variant == LegoppShaderVariant::DarklingStructure) {
        material.lu_shader_uses_vertex_color = true;
        material.lu_shader_uses_texture = true;
        material.lu_shader_source_technique = legoppDarklingTechnique(material.legopp_variant, is_skinned);
        if (source_flavor == LegoppSourceFlavor::NoEnv) {
            material.lu_shader_source_technique =
                is_skinned
                    ? "Technique_LEGOPPLightingVertColorSkinned_noenv_Darkling_Specular"
                    : "Technique_LEGOPPLightingVertColor_noenv_Darkling_Specular";
        }
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::Item ||
        material.legopp_variant == LegoppShaderVariant::ItemGlow) {
        material.lu_shader_source_technique = legoppItemTechnique(
            has_vertex_colors,
            has_texture,
            material.legopp_variant == LegoppShaderVariant::ItemGlow) +
            legoppFlavorToken(source_flavor);
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::ThreeLight) {
        material.lu_shader_source_file = "LEGOPPLighting_BBB.fx";
        material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColor_3Lights";
        material.lu_shader_uses_vertex_color = true;
        material.lu_shader_uses_texture = false;
        material.lu_shader_uses_material_diffuse = false;
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::NoLight) {
        if (is_skinned && has_vertex_colors && !has_texture) {
            material.lu_shader_uses_vertex_color = true;
            material.lu_shader_uses_texture = false;
            material.lu_shader_source_technique = "Technique_LEGOPPLightingVertColorSkinned_NL";
        } else {
            material.lu_shader_uses_vertex_color = false;
            material.lu_shader_uses_texture = true;
            material.lu_shader_source_technique = "Technique_LEGOPPLightingTextured_NL";
        }
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::FrontEnd) {
        material.lu_shader_source_technique = legoppFrontEndTechnique(has_vertex_colors, has_texture, is_skinned);
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::MaskedNonDecal &&
        source_flavor != LegoppSourceFlavor::Default) {
        material.lu_shader_uses_vertex_color = true;
        material.lu_shader_uses_texture = true;
        material.lu_shader_source_technique = legoppFlavoredTechnique(
            is_skinned
                ? "Technique_LEGOPPLightingVertColorTexturedSkinned"
                : "Technique_LEGOPPLightingVertColorTextured",
            material.legopp_variant,
            source_flavor);
        return;
    }

    if (material.legopp_variant == LegoppShaderVariant::FaceCreate ||
        material.legopp_variant == LegoppShaderVariant::PetTamingCloud) {
        return;
    }

    material.lu_shader_source_technique = legoppFlavoredTechnique(
        legoppGeometryPrefix(has_vertex_colors, has_texture, is_skinned),
        material.legopp_variant,
        source_flavor);
}

std::string metallicBaseTechnique(const MaterialAsset& material) {
    return containsCaseInsensitive(material.lu_shader_reflection_map, "polished")
        ? "PolishedMetal"
        : "BrushedSteel";
}

void applyMetallicGeometryVariant(MaterialAsset& material, bool has_vertex_colors, bool has_texture, bool is_skinned) {
    if (material.shader_family != LegacyShaderFamily::Metallic) return;

    const bool item = containsCaseInsensitive(material.lu_shader_label, "Item");
    const std::string skinned = is_skinned ? "Skinned_" : "";
    const std::string base = metallicBaseTechnique(material);

    material.lu_shader_uses_vertex_color = has_vertex_colors || item;
    material.lu_shader_uses_texture = has_texture;
    material.lu_shader_uses_material_diffuse = !has_vertex_colors && !has_texture;
    material.lu_shader_uses_specular = true;
    material.lu_shader_uses_reflection = true;

    std::string suffix;
    if (item) {
        if (has_vertex_colors && has_texture) {
            suffix = "_VertColorTex_Item";
        } else if (has_texture) {
            suffix = "_Tex_Item";
        } else {
            suffix = "_VertColor_Item";
        }
    } else if (has_vertex_colors && has_texture) {
        suffix = "_VertColorTex";
    } else if (has_vertex_colors) {
        suffix = "_VertColor";
    } else if (has_texture) {
        suffix = "_Tex";
    }

    material.lu_shader_source_file = "Metallic.fx";
    material.lu_shader_source_technique = "Technique_Lighting_" + skinned + base + suffix;
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
        std::string nif_shader_metadata = collectNifShaderMetadata(nif);
        std::string nif_shader_metadata_fallback = uniqueNifShaderMetadata(nif_shader_metadata);
        const bool nif_has_material_color_controller =
            fileContainsAscii(std::span<const uint8_t>(data.data(), data.size()), "NiMaterialColorController");
        auto emissive_controllers = collectMaterialEmissiveControllers(nif);
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
                if (!resolved_shader.resolved) {
                    ResolvedLuShader nif_shader = shader_database->resolveNifMaterialShader(mesh.material.name);
                    if (!nif_shader.resolved && !nif_shader_metadata_fallback.empty()) {
                        nif_shader = shader_database->resolveNifMaterialShader(nif_shader_metadata_fallback);
                    }
                    if (nif_shader.resolved || !nif_shader.metadata.empty()) {
                        resolved_shader = std::move(nif_shader);
                    }
                }
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
            out.is_skinned = mesh.is_skinned;
            out.skin_instance_block = mesh.skin_instance_block;
            out.skeleton_root_block = mesh.skeleton_root_block;
            out.skin_bone_node_blocks = mesh.skin_bone_node_blocks;
            out.skin_bone_names = mesh.skin_bone_names;

            out.material.name = mesh.material.name;
            out.material.shader_family = resolved_shader.policy.shader_family;
            out.material.legopp_variant = resolved_shader.policy.legopp_variant;
            out.material.lu_shader_id = resolved_shader.shader.id;
            out.material.lu_shader_game_value = resolved_shader.shader.game_value;
            out.material.lu_shader_label = resolved_shader.shader.label;
            out.material.lu_shader_resolution_source = resolved_shader.resolution_source;
            out.material.lu_shader_metadata = resolved_shader.metadata.empty()
                ? shaderMetadataForMaterial(mesh.material.name)
                : resolved_shader.metadata;
            out.material.lu_shader_source_file = resolved_shader.policy.source_file;
            out.material.lu_shader_source_technique = resolved_shader.policy.source_technique;
            out.material.lu_shader_source_status_note = resolved_shader.policy.source_status_note;
            out.material.lu_shader_validation_status_note = resolved_shader.policy.validation_status_note;
            out.material.lu_shader_port_status = resolved_shader.policy.port_status;
            out.material.lu_shader_alpha_semantic = resolved_shader.policy.alpha_semantic;
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
            out.material.lu_shader_uses_shadow_terrain = resolved_shader.policy.uses_shadow_terrain;
            out.material.lu_shader_reflection_map = resolved_shader.policy.reflection_map;
            out.material.lu_shader_reflection_semantic = resolved_shader.policy.reflection_semantic;
            out.material.lu_shader_uses_uv_animation = resolved_shader.policy.uses_uv_animation;
            out.material.lu_shader_uses_alpha_animation = resolved_shader.policy.uses_alpha_animation;
            out.material.nif_has_material_color_controller = nif_has_material_color_controller;
            auto controller_it = emissive_controllers.find(mesh.source_node_block);
            if (controller_it != emissive_controllers.end()) {
                const MaterialEmissiveController& controller = controller_it->second;
                out.material.material_emissive_controller = true;
                out.material.material_emissive_controller_frequency = controller.frequency;
                out.material.material_emissive_controller_phase = controller.phase;
                out.material.material_emissive_controller_start = controller.start;
                out.material.material_emissive_controller_stop = controller.stop;
                out.material.material_emissive_controller_default = controller.default_value;
                out.material.material_emissive_controller_keys = controller.keys;
            }
            out.material.lu_uv_motion_layer1 = resolved_shader.policy.uv_motion_layer1;
            out.material.lu_uv_motion_layer2 = resolved_shader.policy.uv_motion_layer2;
            out.material.lu_glow_color = resolved_shader.policy.glow_color;
            out.material.lu_glow_lightness = resolved_shader.policy.glow_lightness;
            out.material.lu_grayscale_lerp = resolved_shader.policy.grayscale_lerp;
            out.material.lu_grayscale_lightness = resolved_shader.policy.grayscale_lightness;
            out.material.lu_fade_up_height = resolved_shader.policy.fade_up_height;
            out.material.lu_shiny_glint_height = resolved_shader.policy.shiny_glint_height;
            out.material.lu_shiny_glint_size_power = resolved_shader.policy.shiny_glint_size_power;
            out.material.lu_shiny_glint_color = resolved_shader.policy.shiny_glint_color;
            out.material.alpha_mode = resolved_shader.policy.alpha_mode;
            out.material.cull_mode = resolved_shader.policy.cull_mode;
            out.material.depth_write = resolved_shader.policy.depth_write;
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
            const UvTiling uv0_tiling = detectUvTiling(mesh, false);
            const UvTiling uv1_tiling = detectUvTiling(mesh, true);
            const UvTiling any_uv_tiling = mergeUvTiling(uv0_tiling, uv1_tiling);

            auto texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.diffuse_texture);
            if (!texture_path.empty()) {
                out.material.diffuse_texture_path = texture_path.string();
            }
            out.material.diffuse_texture_address = allowRepeatForTiledUv(
                textureAddressFromClampMode(
                    mesh.material.diffuse_texture_has_clamp_mode,
                    mesh.material.diffuse_texture_clamp_mode),
                uv0_tiling);
            auto dark_texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.dark_texture);
            if (!dark_texture_path.empty()) {
                out.material.dark_texture_path = dark_texture_path.string();
            }
            out.material.dark_texture_address = allowRepeatForTiledUv(
                textureAddressFromClampMode(
                    mesh.material.dark_texture_has_clamp_mode,
                    mesh.material.dark_texture_clamp_mode),
                any_uv_tiling);
            auto detail_texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.detail_texture);
            if (!detail_texture_path.empty()) {
                out.material.detail_texture_path = detail_texture_path.string();
            }
            out.material.detail_texture_address = allowRepeatForTiledUv(
                textureAddressFromClampMode(
                    mesh.material.detail_texture_has_clamp_mode,
                    mesh.material.detail_texture_clamp_mode),
                uv0_tiling);
            auto gloss_texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.gloss_texture);
            if (!gloss_texture_path.empty()) {
                out.material.gloss_texture_path = gloss_texture_path.string();
            }
            out.material.gloss_texture_address = allowRepeatForTiledUv(
                textureAddressFromClampMode(
                    mesh.material.gloss_texture_has_clamp_mode,
                    mesh.material.gloss_texture_clamp_mode),
                uv0_tiling);
            auto glow_texture_path = resolveTexturePath(
                res_root, options.nif_path, mesh.material.glow_texture);
            if (!glow_texture_path.empty()) {
                out.material.glow_texture_path = glow_texture_path.string();
            }
            out.material.glow_texture_address = allowRepeatForTiledUv(
                textureAddressFromClampMode(
                    mesh.material.glow_texture_has_clamp_mode,
                    mesh.material.glow_texture_clamp_mode),
                uv0_tiling);
            applyLegoppGeometryVariant(
                out.material,
                mesh.has_vertex_colors,
                !out.material.diffuse_texture_path.empty(),
                mesh.is_skinned);
            applyMetallicGeometryVariant(
                out.material,
                mesh.has_vertex_colors,
                !out.material.diffuse_texture_path.empty(),
                mesh.is_skinned);

            const bool authored_alpha_can_blend =
                shaderAllowsAuthoredAlpha(out.material.alpha_mode, out.material.lu_shader_alpha_semantic);
            const bool material_alpha_source = mesh.material.diffuse[3] < 0.999f;
            out.material.alpha_blend =
                resolved_shader.policy.force_alpha_blend ||
                out.material.alpha_mode == RenderAlphaMode::AlphaBlend ||
                out.material.alpha_mode == RenderAlphaMode::Additive ||
                (authored_alpha_can_blend && material_alpha_source) ||
                (mesh.material.has_alpha_property &&
                 (mesh.material.alpha_flags & 0x0001u) != 0 &&
                 authored_alpha_can_blend);
            out.material.alpha_test =
                resolved_shader.policy.force_alpha_test ||
                out.material.alpha_mode == RenderAlphaMode::AlphaTest ||
                (authored_alpha_can_blend &&
                 mesh.material.has_alpha_property &&
                 mesh.material.alpha_threshold > 0);
            if (out.material.alpha_mode == RenderAlphaMode::Opaque) {
                if (out.material.alpha_blend) {
                    out.material.alpha_mode = RenderAlphaMode::AlphaBlend;
                } else if (out.material.alpha_test) {
                    out.material.alpha_mode = RenderAlphaMode::AlphaTest;
                }
            }
            out.material.has_alpha_property = mesh.material.has_alpha_property;
            out.material.alpha_flags = mesh.material.alpha_flags;
            out.material.alpha_threshold = resolved_shader.policy.alpha_threshold > 0
                ? resolved_shader.policy.alpha_threshold
                : mesh.material.alpha_threshold;
            if (isLegoppFrontendAlphaTestTechnique(out.material.lu_shader_source_technique)) {
                out.material.alpha_mode = RenderAlphaMode::AlphaTest;
                out.material.alpha_blend = false;
                out.material.alpha_test = true;
                out.material.alpha_threshold = 127;
                out.material.lu_shader_alpha_semantic = ShaderAlphaSemantic::AlphaTest;
            }

            for (size_t vertex_index = 0; vertex_index < mesh.vertices.size(); ++vertex_index) {
                const auto& v = mesh.vertices[vertex_index];
                Vertex rv;
                rv.position = {v.position[0], v.position[1], v.position[2]};
                rv.normal = {v.normal[0], v.normal[1], v.normal[2]};
                rv.uv = {v.uv[0], v.uv[1]};
                rv.uv2 = {v.uv2[0], v.uv2[1]};
                rv.color_rgba8 = packColor(v.color[0], v.color[1], v.color[2], v.color[3]);
                if (vertex_index < mesh.vertex_influences.size()) {
                    const auto& influences = mesh.vertex_influences[vertex_index];
                    const size_t count = std::min<size_t>(influences.size(), rv.bone_indices.size());
                    float weight_sum = 0.0f;
                    for (size_t i = 0; i < count; ++i) {
                        rv.bone_indices[i] = influences[i].bone_index;
                        rv.bone_weights[i] = influences[i].weight;
                        weight_sum += influences[i].weight;
                    }
                    if (weight_sum > 0.000001f) {
                        for (size_t i = 0; i < count; ++i) {
                            rv.bone_weights[i] /= weight_sum;
                        }
                    }
                }
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
