#include "lu/renderer_bgfx/bgfx_renderer.h"

#include "microsoft/dds/dds_reader.h"
#include "microsoft/dds/dds_types.h"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

namespace lu::renderer::bgfx_backend {

namespace {

constexpr bgfx::ViewId kViewWorld = 0;
constexpr uint32_t kDdsCaps2Cubemap = 0x00000200u;
constexpr uint32_t kDdsCubemapAllFaces = 0x0000fc00u;
constexpr uint64_t kWrapLinearSampler = 0;
constexpr uint64_t kClampLinearSampler = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

struct GpuVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    float u2, v2;
    uint32_t abgr;

    static bgfx::VertexLayout layout;
    static void initLayout() {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    }
};

bgfx::VertexLayout GpuVertex::layout;

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

const char* shaderProfileDir(bgfx::RendererType::Enum renderer) {
    switch (renderer) {
    case bgfx::RendererType::Direct3D11: return "dxbc";
    case bgfx::RendererType::Direct3D12: return "dxil";
    case bgfx::RendererType::Metal: return "metal";
    case bgfx::RendererType::OpenGL: return "glsl";
    case bgfx::RendererType::OpenGLES: return "essl";
    case bgfx::RendererType::Vulkan: return "spirv";
    default: return "glsl";
    }
}

bgfx::TextureFormat::Enum ddsTextureFormat(const lu::assets::DdsFile& dds) {
    if (dds.is_compressed) {
        switch (dds.four_cc) {
        case lu::assets::FOURCC_DXT1: return bgfx::TextureFormat::BC1;
        case lu::assets::FOURCC_DXT3: return bgfx::TextureFormat::BC2;
        case lu::assets::FOURCC_DXT5: return bgfx::TextureFormat::BC3;
        default: return bgfx::TextureFormat::Unknown;
        }
    }

    const auto& pf = dds.header.pixel_format;
    if (dds.bits_per_pixel == 32) {
        if (pf.r_bit_mask == 0x00ff0000 && pf.g_bit_mask == 0x0000ff00 &&
            pf.b_bit_mask == 0x000000ff && pf.a_bit_mask == 0xff000000) {
            return bgfx::TextureFormat::BGRA8;
        }
        if (pf.r_bit_mask == 0x000000ff && pf.g_bit_mask == 0x0000ff00 &&
            pf.b_bit_mask == 0x00ff0000 && pf.a_bit_mask == 0xff000000) {
            return bgfx::TextureFormat::RGBA8;
        }
    }

    return bgfx::TextureFormat::Unknown;
}

const char* boolText(bool value) {
    return value ? "yes" : "no";
}

const char* alphaModeName(RenderAlphaMode mode) {
    switch (mode) {
    case RenderAlphaMode::Opaque: return "opaque";
    case RenderAlphaMode::AlphaTest: return "test";
    case RenderAlphaMode::AlphaBlend: return "blend";
    case RenderAlphaMode::Additive: return "add";
    }
    return "?";
}

const char* cullModeName(RenderCullMode mode) {
    switch (mode) {
    case RenderCullMode::Backface: return "back";
    case RenderCullMode::Clockwise: return "cw";
    case RenderCullMode::CounterClockwise: return "ccw";
    case RenderCullMode::TwoSided: return "two";
    }
    return "?";
}

const char* portStatusName(ShaderPortStatus status) {
    switch (status) {
    case ShaderPortStatus::Unported: return "unported";
    case ShaderPortStatus::Placeholder: return "placeholder";
    case ShaderPortStatus::Inferred: return "inferred";
    case ShaderPortStatus::Verified: return "verified";
    }
    return "?";
}

const char* shaderFamilyName(LegacyShaderFamily family) {
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

const char* legoppVariantName(LegoppShaderVariant variant) {
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

const char* resolutionSourceName(ShaderResolutionSource source) {
    switch (source) {
    case ShaderResolutionSource::Unresolved: return "unresolved";
    case ShaderResolutionSource::CdClientAsset: return "cdclient";
    case ShaderResolutionSource::CdClientMultishaderPrefix: return "cdclient-prefix";
    case ShaderResolutionSource::NifMultiShaderGameValue: return "nif-nims";
    case ShaderResolutionSource::NifMaterialName: return "nif-mat";
    case ShaderResolutionSource::NifFxShaderName: return "nif-fx";
    case ShaderResolutionSource::Fallback: return "fallback";
    }
    return "?";
}

const char* alphaSemanticName(ShaderAlphaSemantic semantic) {
    switch (semantic) {
    case ShaderAlphaSemantic::Unknown: return "unknown";
    case ShaderAlphaSemantic::OutputAlpha: return "output";
    case ShaderAlphaSemantic::AlphaTest: return "test";
    case ShaderAlphaSemantic::ControlGlow: return "ctl-glow";
    case ShaderAlphaSemantic::ControlEmissive: return "ctl-em";
    case ShaderAlphaSemantic::ControlDarkling: return "ctl-dark";
    case ShaderAlphaSemantic::Ignored: return "ignored";
    }
    return "?";
}

Vec3 sampleVec3Keys(const std::vector<Vec3Key>& keys, float time, Vec3 fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const Vec3Key& a = keys[i];
        const Vec3Key& b = keys[i + 1];
        if (time < a.time || time > b.time) continue;

        const float duration = std::max(0.0001f, b.time - a.time);
        const float t = std::clamp((time - a.time) / duration, 0.0f, 1.0f);
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        return (a.value * h00) +
            (a.forward_tangent * (h10 * duration)) +
            (b.value * h01) +
            (b.backward_tangent * (h11 * duration));
    }
    return fallback;
}

Vec3 materialEmissiveAtTime(const MaterialAsset& material, float elapsed_seconds) {
    if (!material.material_emissive_controller) return material.emissive;

    const float start = material.material_emissive_controller_start;
    const float stop = material.material_emissive_controller_stop;
    float controller_time =
        material.material_emissive_controller_phase +
        elapsed_seconds * material.material_emissive_controller_frequency;
    if (stop > start) {
        const float duration = stop - start;
        controller_time = std::fmod(controller_time - start, duration);
        if (controller_time < 0.0f) controller_time += duration;
        controller_time += start;
    }

    return sampleVec3Keys(
        material.material_emissive_controller_keys,
        controller_time,
        material.material_emissive_controller_default);
}

bool isLegoppFamily(LegacyShaderFamily family) {
    return family == LegacyShaderFamily::LegoppLighting ||
           family == LegacyShaderFamily::LegoppNoAmbient ||
           family == LegacyShaderFamily::LegoppEmissive ||
           family == LegacyShaderFamily::LegoppEffect;
}

uint64_t diffuseSamplerFlagsForMaterial(const MaterialAsset& material) {
    if (!isLegoppFamily(material.shader_family)) {
        return kWrapLinearSampler;
    }

    switch (material.legopp_variant) {
    case LegoppShaderVariant::Item:
    case LegoppShaderVariant::ItemGlow:
    case LegoppShaderVariant::FrontEnd:
    case LegoppShaderVariant::ThreeLight:
        return kClampLinearSampler;
    default:
        return kWrapLinearSampler;
    }
}

uint64_t auxiliarySamplerFlagsForMaterial(const MaterialAsset& material) {
    if (isLegoppFamily(material.shader_family)) {
        return kWrapLinearSampler;
    }
    return diffuseSamplerFlagsForMaterial(material);
}

bool useColorInLegoppDiffuse(const MaterialAsset& material, bool has_texture) {
    if (!isLegoppFamily(material.shader_family)) {
        return material.lu_shader_uses_material_diffuse;
    }
    return material.lu_shader_uses_material_diffuse && !has_texture;
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool usesLowLegoppSource(const MaterialAsset& material) {
    const std::string source_file = lowerCopy(material.lu_shader_source_file);
    return source_file.find("_low") != std::string::npos ||
           source_file.find("lighting_low") != std::string::npos;
}

std::string shortText(const std::string& value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars <= 3) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

} // namespace

BgfxRenderer::~BgfxRenderer() {
    shutdown();
}

bool BgfxRenderer::init(const RendererInit& init) {
    shader_dir_ = init.shader_dir;
    reflection_map_dir_ = init.reflection_map_dir;
    width_ = init.width;
    height_ = init.height;
    features_ = init.features;

    bgfx::Init bgfx_init;
    bgfx_init.type = bgfx::RendererType::Count;
    bgfx_init.platformData.nwh = init.native_window;
    bgfx_init.platformData.ndt = init.native_display;
    bgfx_init.platformData.type = init.native_window_type;
    bgfx_init.callback = init.callback;
    bgfx_init.resolution.width = width_;
    bgfx_init.resolution.height = height_;
    bgfx_init.resolution.reset = resetFlags();

    if (!bgfx::init(bgfx_init)) {
        last_error_ = "bgfx::init failed";
        return false;
    }

    initialized_ = true;
    start_time_ = std::chrono::steady_clock::now();
    GpuVertex::initLayout();

    bgfx::setDebug(BGFX_DEBUG_TEXT);
    bgfx::setViewClear(kViewWorld, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20232aff, 1.0f, 0);
    bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);

    s_diffuse_ = bgfx::createUniform("s_diffuse", bgfx::UniformType::Sampler);
    s_dark_ = bgfx::createUniform("s_dark", bgfx::UniformType::Sampler);
    s_lu_env_ = bgfx::createUniform("s_luEnv", bgfx::UniformType::Sampler);
    u_material_diffuse_ = bgfx::createUniform("u_materialDiffuse", bgfx::UniformType::Vec4);
    u_light_dir_ambient_ = bgfx::createUniform("u_lightDirAmbient", bgfx::UniformType::Vec4);
    u_light_color_ = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    u_lu_light_dir_fade_ = bgfx::createUniform("u_luLightDirFade", bgfx::UniformType::Vec4);
    u_lu_light_color_shadow_ = bgfx::createUniform("u_luLightColorShadow", bgfx::UniformType::Vec4);
    u_lu_ambient_ = bgfx::createUniform("u_luAmbient", bgfx::UniformType::Vec4);
    u_lu_upper_hemi_ = bgfx::createUniform("u_luUpperHemi", bgfx::UniformType::Vec4);
    u_lu_lower_hemi_ = bgfx::createUniform("u_luLowerHemi", bgfx::UniformType::Vec4);
    u_lu_specular_ = bgfx::createUniform("u_luSpecular", bgfx::UniformType::Vec4);
    u_lu_camera_pos_ = bgfx::createUniform("u_luCameraPos", bgfx::UniformType::Vec4);
    u_lu_fog_color_ = bgfx::createUniform("u_luFogColor", bgfx::UniformType::Vec4);
    u_lu_fog_params_ = bgfx::createUniform("u_luFogParams", bgfx::UniformType::Vec4);
    u_lu_shader_flags_ = bgfx::createUniform("u_luShaderFlags", bgfx::UniformType::Vec4);
    u_lu_variant_flags_ = bgfx::createUniform("u_luVariantFlags", bgfx::UniformType::Vec4);
    u_lu_effect_time_ = bgfx::createUniform("u_luEffectTime", bgfx::UniformType::Vec4);
    u_lu_uv_motion1_ = bgfx::createUniform("u_luUvMotion1", bgfx::UniformType::Vec4);
    u_lu_uv_motion2_ = bgfx::createUniform("u_luUvMotion2", bgfx::UniformType::Vec4);
    u_lu_effect_params_ = bgfx::createUniform("u_luEffectParams", bgfx::UniformType::Vec4);
    u_lu_glow_color_ = bgfx::createUniform("u_luGlowColor", bgfx::UniformType::Vec4);
    u_lu_shiny_glint_ = bgfx::createUniform("u_luShinyGlint", bgfx::UniformType::Vec4);
    u_lu_shiny_glint_color_ = bgfx::createUniform("u_luShinyGlintColor", bgfx::UniformType::Vec4);
    u_lu_bbb_light_dir1_ = bgfx::createUniform("u_luBbbLightDir1", bgfx::UniformType::Vec4);
    u_lu_bbb_light_dir2_ = bgfx::createUniform("u_luBbbLightDir2", bgfx::UniformType::Vec4);
    u_lu_bbb_light_color1_ = bgfx::createUniform("u_luBbbLightColor1", bgfx::UniformType::Vec4);
    u_lu_bbb_light_color2_ = bgfx::createUniform("u_luBbbLightColor2", bgfx::UniformType::Vec4);
    u_material_emissive_ = bgfx::createUniform("u_materialEmissive", bgfx::UniformType::Vec4);
    white_texture_ = createSolidTexture(0xffffffffu);
    missing_texture_ = createSolidTexture(0xffff00ffu);
    neutral_env_texture_ = loadReflectionCubeTexture("default_reflection.dds");
    if (!bgfx::isValid(neutral_env_texture_)) {
        neutral_env_texture_ = createSolidCubeTexture(0xffd8d8d8u);
    }
    legacy_program_ = loadProgram("vs_legacy_mesh.sc.bin", "fs_legacy_mesh.sc.bin");
    basic_program_ = loadProgram("vs_legacy_mesh.sc.bin", "fs_basic.sc.bin");
    basic_lit_program_ = loadProgram("vs_legacy_mesh.sc.bin", "fs_basic_lit.sc.bin");
    basic_two_layer_program_ = loadProgram("vs_legacy_mesh.sc.bin", "fs_basic_two_layer.sc.bin");
    alpha_as_alpha_program_ = loadProgram("vs_legacy_mesh.sc.bin", "fs_alpha_as_alpha.sc.bin");
    alpha_uv_scroll_program_ = loadProgram("vs_uv_scroll_alpha.sc.bin", "fs_uv_scroll_alpha.sc.bin");
    legopp_program_ = loadProgram("vs_lu_lit_mesh.sc.bin", "fs_legopp_lighting.sc.bin");
    legopp_no_ambient_program_ = loadProgram("vs_lu_lit_mesh_no_ambient.sc.bin", "fs_legopp_lighting.sc.bin");
    legopp_emissive_program_ = loadProgram("vs_lu_lit_mesh.sc.bin", "fs_legopp_emissive.sc.bin");
    metallic_brushed_program_ = loadProgram("vs_metallic.sc.bin", "fs_metallic_brushed.sc.bin");
    metallic_polished_program_ = loadProgram("vs_metallic.sc.bin", "fs_metallic_polished.sc.bin");
    terrain_rim_program_ = loadProgram("vs_terrain_rim.sc.bin", "fs_terrain_rim.sc.bin");
    ocean_distort_program_ = loadProgram("vs_ocean_distort.sc.bin", "fs_ocean_distort.sc.bin");
    ocean_distort_directional_program_ = loadProgram("vs_ocean_distort.sc.bin", "fs_ocean_distort.sc.bin");
    ocean_distort_unlit_program_ = loadProgram("vs_ocean_distort_unlit.sc.bin", "fs_ocean_distort.sc.bin");
    ocean_distort_fx_program_ = loadProgram("vs_ocean_distort_fx.sc.bin", "fs_ocean_distort_fx.sc.bin");
    clear_plastic_program_ = loadProgram("vs_lu_lit_mesh.sc.bin", "fs_clear_plastic.sc.bin");

    if (!bgfx::isValid(legacy_program_) ||
        !bgfx::isValid(basic_program_) ||
        !bgfx::isValid(basic_lit_program_) ||
        !bgfx::isValid(basic_two_layer_program_) ||
        !bgfx::isValid(alpha_as_alpha_program_) ||
        !bgfx::isValid(alpha_uv_scroll_program_) ||
        !bgfx::isValid(legopp_program_) ||
        !bgfx::isValid(legopp_no_ambient_program_) ||
        !bgfx::isValid(legopp_emissive_program_) ||
        !bgfx::isValid(metallic_brushed_program_) ||
        !bgfx::isValid(metallic_polished_program_) ||
        !bgfx::isValid(terrain_rim_program_) ||
        !bgfx::isValid(ocean_distort_program_) ||
        !bgfx::isValid(ocean_distort_directional_program_) ||
        !bgfx::isValid(ocean_distort_unlit_program_) ||
        !bgfx::isValid(ocean_distort_fx_program_) ||
        !bgfx::isValid(clear_plastic_program_)) {
        last_error_ = "Failed to load LU mesh shaders from " + shader_dir_.string();
        return false;
    }

    return true;
}

void BgfxRenderer::shutdown() {
    if (!initialized_) return;

    bgfx::frame();
    clearWorld();
    destroyTextureCache();
    for (auto& [_, handle] : cube_texture_cache_) {
        if (bgfx::isValid(handle) && handle.idx != neutral_env_texture_.idx) bgfx::destroy(handle);
    }
    cube_texture_cache_.clear();
    if (bgfx::isValid(white_texture_)) bgfx::destroy(white_texture_);
    if (bgfx::isValid(missing_texture_)) bgfx::destroy(missing_texture_);
    if (bgfx::isValid(neutral_env_texture_)) bgfx::destroy(neutral_env_texture_);
    if (bgfx::isValid(legacy_program_)) bgfx::destroy(legacy_program_);
    if (bgfx::isValid(basic_program_)) bgfx::destroy(basic_program_);
    if (bgfx::isValid(basic_lit_program_)) bgfx::destroy(basic_lit_program_);
    if (bgfx::isValid(basic_two_layer_program_)) bgfx::destroy(basic_two_layer_program_);
    if (bgfx::isValid(alpha_as_alpha_program_)) bgfx::destroy(alpha_as_alpha_program_);
    if (bgfx::isValid(alpha_uv_scroll_program_)) bgfx::destroy(alpha_uv_scroll_program_);
    if (bgfx::isValid(legopp_program_)) bgfx::destroy(legopp_program_);
    if (bgfx::isValid(legopp_no_ambient_program_)) bgfx::destroy(legopp_no_ambient_program_);
    if (bgfx::isValid(legopp_emissive_program_)) bgfx::destroy(legopp_emissive_program_);
    if (bgfx::isValid(metallic_brushed_program_)) bgfx::destroy(metallic_brushed_program_);
    if (bgfx::isValid(metallic_polished_program_)) bgfx::destroy(metallic_polished_program_);
    if (bgfx::isValid(terrain_rim_program_)) bgfx::destroy(terrain_rim_program_);
    if (bgfx::isValid(ocean_distort_program_)) bgfx::destroy(ocean_distort_program_);
    if (bgfx::isValid(ocean_distort_directional_program_)) bgfx::destroy(ocean_distort_directional_program_);
    if (bgfx::isValid(ocean_distort_unlit_program_)) bgfx::destroy(ocean_distort_unlit_program_);
    if (bgfx::isValid(ocean_distort_fx_program_)) bgfx::destroy(ocean_distort_fx_program_);
    if (bgfx::isValid(clear_plastic_program_)) bgfx::destroy(clear_plastic_program_);
    if (bgfx::isValid(s_diffuse_)) bgfx::destroy(s_diffuse_);
    if (bgfx::isValid(s_dark_)) bgfx::destroy(s_dark_);
    if (bgfx::isValid(s_lu_env_)) bgfx::destroy(s_lu_env_);
    if (bgfx::isValid(u_material_diffuse_)) bgfx::destroy(u_material_diffuse_);
    if (bgfx::isValid(u_light_dir_ambient_)) bgfx::destroy(u_light_dir_ambient_);
    if (bgfx::isValid(u_light_color_)) bgfx::destroy(u_light_color_);
    if (bgfx::isValid(u_lu_light_dir_fade_)) bgfx::destroy(u_lu_light_dir_fade_);
    if (bgfx::isValid(u_lu_light_color_shadow_)) bgfx::destroy(u_lu_light_color_shadow_);
    if (bgfx::isValid(u_lu_ambient_)) bgfx::destroy(u_lu_ambient_);
    if (bgfx::isValid(u_lu_upper_hemi_)) bgfx::destroy(u_lu_upper_hemi_);
    if (bgfx::isValid(u_lu_lower_hemi_)) bgfx::destroy(u_lu_lower_hemi_);
    if (bgfx::isValid(u_lu_specular_)) bgfx::destroy(u_lu_specular_);
    if (bgfx::isValid(u_lu_camera_pos_)) bgfx::destroy(u_lu_camera_pos_);
    if (bgfx::isValid(u_lu_fog_color_)) bgfx::destroy(u_lu_fog_color_);
    if (bgfx::isValid(u_lu_fog_params_)) bgfx::destroy(u_lu_fog_params_);
    if (bgfx::isValid(u_lu_shader_flags_)) bgfx::destroy(u_lu_shader_flags_);
    if (bgfx::isValid(u_lu_variant_flags_)) bgfx::destroy(u_lu_variant_flags_);
    if (bgfx::isValid(u_lu_effect_time_)) bgfx::destroy(u_lu_effect_time_);
    if (bgfx::isValid(u_lu_uv_motion1_)) bgfx::destroy(u_lu_uv_motion1_);
    if (bgfx::isValid(u_lu_uv_motion2_)) bgfx::destroy(u_lu_uv_motion2_);
    if (bgfx::isValid(u_lu_effect_params_)) bgfx::destroy(u_lu_effect_params_);
    if (bgfx::isValid(u_lu_glow_color_)) bgfx::destroy(u_lu_glow_color_);
    if (bgfx::isValid(u_lu_shiny_glint_)) bgfx::destroy(u_lu_shiny_glint_);
    if (bgfx::isValid(u_lu_shiny_glint_color_)) bgfx::destroy(u_lu_shiny_glint_color_);
    if (bgfx::isValid(u_lu_bbb_light_dir1_)) bgfx::destroy(u_lu_bbb_light_dir1_);
    if (bgfx::isValid(u_lu_bbb_light_dir2_)) bgfx::destroy(u_lu_bbb_light_dir2_);
    if (bgfx::isValid(u_lu_bbb_light_color1_)) bgfx::destroy(u_lu_bbb_light_color1_);
    if (bgfx::isValid(u_lu_bbb_light_color2_)) bgfx::destroy(u_lu_bbb_light_color2_);
    if (bgfx::isValid(u_material_emissive_)) bgfx::destroy(u_material_emissive_);
    bgfx::frame();

    bgfx::shutdown();
    initialized_ = false;
}

void BgfxRenderer::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    bgfx::reset(width_, height_, resetFlags());
    bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);
}

void BgfxRenderer::clearWorld() {
    for (auto& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
}

void BgfxRenderer::loadWorld(const RenderWorld& world) {
    clearWorld();
    environment_ = world.environment;
    source_asset_path_ = world.source_asset_path;
    meshes_.reserve(world.meshes.size());

    for (const auto& mesh : world.meshes) {
        if (mesh.vertices.empty() || mesh.indices.empty()) continue;

        std::vector<GpuVertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices) {
            vertices.push_back({
                v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z,
                v.uv.x, v.uv.y,
                v.uv2.x, v.uv2.y,
                v.color_rgba8
            });
        }

        GpuMesh gpu;
        gpu.name = mesh.name;
        gpu.material = mesh.material;
        gpu.index_count = static_cast<uint32_t>(mesh.indices.size());
        gpu.has_lod_range = mesh.has_lod_range;
        gpu.lod_parent_block = mesh.lod_parent_block;
        gpu.lod_level = mesh.lod_level;
        gpu.lod_near = mesh.lod_near;
        gpu.lod_far = mesh.lod_far;
        gpu.lod_center = mesh.lod_center;
        gpu.has_vertex_color = false;
        for (const auto& vertex : mesh.vertices) {
            if (vertex.color_rgba8 != 0xffffffffu) {
                gpu.has_vertex_color = true;
                break;
            }
        }
        gpu.vertex_buffer = bgfx::createVertexBuffer(
            bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(GpuVertex))),
            GpuVertex::layout);
        gpu.index_buffer = bgfx::createIndexBuffer(
            bgfx::copy(mesh.indices.data(), static_cast<uint32_t>(mesh.indices.size() * sizeof(uint32_t))),
            BGFX_BUFFER_INDEX32);
        const uint64_t diffuse_sampler_flags = diffuseSamplerFlagsForMaterial(mesh.material);
        const uint64_t auxiliary_sampler_flags = auxiliarySamplerFlagsForMaterial(mesh.material);
        gpu.texture = loadTexture(mesh.material.diffuse_texture_path, diffuse_sampler_flags);
        if (!bgfx::isValid(gpu.texture)) {
            gpu.texture = mesh.material.diffuse_texture_path.empty() ? white_texture_ : missing_texture_;
        }
        std::string auxiliary_texture_path = mesh.material.dark_texture_path;
        if (mesh.material.shader_family == LegacyShaderFamily::Metallic) {
            auxiliary_texture_path = mesh.material.detail_texture_path;
            if (auxiliary_texture_path.empty() && !reflection_map_dir_.empty()) {
                auxiliary_texture_path = (reflection_map_dir_ / "metal_reflection_brushed_noise.dds").string();
            }
        }
        gpu.dark_texture = loadTexture(auxiliary_texture_path, auxiliary_sampler_flags);
        if (!bgfx::isValid(gpu.dark_texture)) {
            gpu.dark_texture = auxiliary_texture_path.empty() ? white_texture_ : missing_texture_;
        }
        gpu.reflection_texture = loadReflectionCubeTexture(mesh.material.lu_shader_reflection_map);
        if (!bgfx::isValid(gpu.reflection_texture)) {
            gpu.reflection_texture = neutral_env_texture_;
        }
        meshes_.push_back(gpu);
    }
}

void BgfxRenderer::setEnvironment(const EnvironmentState& environment) {
    environment_ = environment;
}

void BgfxRenderer::setFeatureSettings(const RenderFeatureSettings& features) {
    const uint32_t old_reset = resetFlags();
    features_ = features;
    const uint32_t new_reset = resetFlags();
    if (initialized_ && old_reset != new_reset) {
        bgfx::reset(width_, height_, new_reset);
        bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);
    }
}

void BgfxRenderer::render(const OrbitCamera& camera) {
    if (!initialized_) return;

    float aspect = height_ == 0 ? 1.0f : static_cast<float>(width_) / static_cast<float>(height_);
    Vec3 eye = camera.position();
    Vec3 target = camera.target();
    float view[16];
    float proj[16];
    bx::mtxLookAt(view, bx::Vec3{eye.x, eye.y, eye.z}, bx::Vec3{target.x, target.y, target.z});
    const float near_clip = std::max(0.02f, camera.distance() * 0.005f);
    const float far_clip = std::max(100.0f, camera.distance() * 20.0f);
    bx::mtxProj(proj, 60.0f, aspect, near_clip, far_clip, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewTransform(kViewWorld, view, proj);
    bgfx::touch(kViewWorld);

    drawGrid();

    Vec3 lu_light_vec = normalize(environment_.sun.direction);
    float light_ambient[4] = {
        -lu_light_vec.x, -lu_light_vec.y, -lu_light_vec.z,
        (environment_.ambient.x + environment_.ambient.y + environment_.ambient.z) / 3.0f
    };
    float light_color[4] = {
        environment_.sun.color.x * environment_.sun.intensity,
        environment_.sun.color.y * environment_.sun.intensity,
        environment_.sun.color.z * environment_.sun.intensity,
        1.0f
    };
    float lu_light_dir_fade[4] = {
        lu_light_vec.x, lu_light_vec.y, lu_light_vec.z, 1.0f
    };
    float lu_light_color_shadow[4] = {
        light_color[0], light_color[1], light_color[2], 1.0f
    };
    float lu_ambient[4] = {
        environment_.ambient.x, environment_.ambient.y, environment_.ambient.z, 1.0f
    };
    float lu_upper_hemi[4] = {
        environment_.upper_hemi.x, environment_.upper_hemi.y, environment_.upper_hemi.z, 1.0f
    };
    float lu_lower_hemi[4] = {
        environment_.lower_hemi.x, environment_.lower_hemi.y, environment_.lower_hemi.z, 1.0f
    };
    float lu_specular[4] = {
        environment_.specular.x, environment_.specular.y, environment_.specular.z, 1.0f
    };
    float lu_camera_pos[4] = {eye.x, eye.y, eye.z, 1.0f};
    float lu_fog_color[4] = {
        environment_.fog_color.x, environment_.fog_color.y, environment_.fog_color.z, 1.0f
    };
    float lu_fog_params_base[4] = {
        environment_.fog_near,
        environment_.fog_far,
        environment_.fog_enabled ? 1.0f : 0.0f,
        0.0f
    };
    const auto now = std::chrono::steady_clock::now();
    const float effect_time = std::chrono::duration<float>(now - start_time_).count();

    const float identity_mtx[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    size_t active_meshes = 0;
    for (const auto& mesh : meshes_) {
        if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
        if (mesh.has_lod_range) {
            const float lod_distance = length(eye - mesh.lod_center);
            if (lod_distance < mesh.lod_near || lod_distance >= mesh.lod_far) {
                continue;
            }
        }
        ++active_meshes;

        float diffuse[4] = {
            mesh.material.diffuse.x,
            mesh.material.diffuse.y,
            mesh.material.diffuse.z,
            mesh.material.diffuse.w
        };
        Vec3 animated_emissive = materialEmissiveAtTime(mesh.material, effect_time);
        float emissive[4] = {
            animated_emissive.x,
            animated_emissive.y,
            animated_emissive.z,
            1.0f
        };
        float mesh_lu_light_color_shadow[4] = {
            lu_light_color_shadow[0],
            lu_light_color_shadow[1],
            lu_light_color_shadow[2],
            mesh.material.lu_shader_uses_shadow_terrain ? lu_light_color_shadow[3] : 1.0f
        };

        bgfx::setTransform(identity_mtx);
        bgfx::setVertexBuffer(0, mesh.vertex_buffer);
        bgfx::setIndexBuffer(mesh.index_buffer);
        bgfx::setTexture(0, s_diffuse_, mesh.texture);
        bgfx::setTexture(1, s_lu_env_, mesh.reflection_texture);
        bgfx::setTexture(2, s_dark_, mesh.dark_texture);
        bgfx::setUniform(u_material_diffuse_, diffuse);
        bgfx::setUniform(u_material_emissive_, emissive);
        bgfx::setUniform(u_light_dir_ambient_, light_ambient);
        bgfx::setUniform(u_light_color_, light_color);
        bgfx::setUniform(u_lu_light_dir_fade_, lu_light_dir_fade);
        bgfx::setUniform(u_lu_light_color_shadow_, mesh_lu_light_color_shadow);
        bgfx::setUniform(u_lu_ambient_, lu_ambient);
        bgfx::setUniform(u_lu_upper_hemi_, lu_upper_hemi);
        bgfx::setUniform(u_lu_lower_hemi_, lu_lower_hemi);
        bgfx::setUniform(u_lu_specular_, lu_specular);
        bgfx::setUniform(u_lu_camera_pos_, lu_camera_pos);
        bgfx::setUniform(u_lu_fog_color_, lu_fog_color);

        const bool has_texture = mesh.material.lu_shader_uses_texture;
        float alpha_threshold = -1.0f;
        if (mesh.material.alpha_test || mesh.material.alpha_mode == RenderAlphaMode::AlphaTest) {
            const uint8_t threshold = mesh.material.alpha_threshold > 0 ? mesh.material.alpha_threshold : 127;
            alpha_threshold = static_cast<float>(threshold) / 255.0f;
        }
        float shader_flags[4] = {
            has_texture ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_vertex_color ? 1.0f : 0.0f,
            useColorInLegoppDiffuse(mesh.material, has_texture) ? 1.0f : 0.0f,
            alpha_threshold
        };
        bgfx::setUniform(u_lu_shader_flags_, shader_flags);
        float variant_flags[4] = {
            static_cast<float>(mesh.material.legopp_variant),
            usesLowLegoppSource(mesh.material) ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_specular ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_reflection ? 1.0f : 0.0f
        };
        bgfx::setUniform(u_lu_variant_flags_, variant_flags);
        float lu_fog_params[4] = {
            lu_fog_params_base[0],
            lu_fog_params_base[1],
            mesh.material.lu_shader_uses_fog ? lu_fog_params_base[2] : 0.0f,
            lu_fog_params_base[3]
        };
        bgfx::setUniform(u_lu_fog_params_, lu_fog_params);
        float effect_time_uniform[4] = {
            effect_time,
            mesh.material.lu_shader_uses_uv_animation ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_alpha_animation ? 1.0f : 0.0f,
            1.0f
        };
        float uv_motion1[4] = {
            mesh.material.lu_uv_motion_layer1.x,
            mesh.material.lu_uv_motion_layer1.y,
            0.0f,
            0.0f
        };
        float uv_motion2[4] = {
            mesh.material.lu_uv_motion_layer2.x,
            mesh.material.lu_uv_motion_layer2.y,
            0.0f,
            0.0f
        };
        bgfx::setUniform(u_lu_effect_time_, effect_time_uniform);
        bgfx::setUniform(u_lu_uv_motion1_, uv_motion1);
        bgfx::setUniform(u_lu_uv_motion2_, uv_motion2);
        float effect_params[4] = {
            mesh.material.lu_grayscale_lerp,
            mesh.material.lu_grayscale_lightness,
            mesh.material.lu_glow_lightness,
            mesh.material.lu_fade_up_height
        };
        float glow_color[4] = {
            mesh.material.lu_glow_color.x,
            mesh.material.lu_glow_color.y,
            mesh.material.lu_glow_color.z,
            mesh.material.lu_glow_color.w
        };
        bgfx::setUniform(u_lu_effect_params_, effect_params);
        bgfx::setUniform(u_lu_glow_color_, glow_color);
        float shiny_glint[4] = {
            mesh.material.lu_shiny_glint_height,
            mesh.material.lu_shiny_glint_size_power,
            0.0f,
            0.0f
        };
        float shiny_glint_color[4] = {
            mesh.material.lu_shiny_glint_color.x,
            mesh.material.lu_shiny_glint_color.y,
            mesh.material.lu_shiny_glint_color.z,
            mesh.material.lu_shiny_glint_color.w
        };
        bgfx::setUniform(u_lu_shiny_glint_, shiny_glint);
        bgfx::setUniform(u_lu_shiny_glint_color_, shiny_glint_color);
        float bbb_light_dir1[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        float bbb_light_dir2[4] = {-1.0f, -1.0f, -1.0f, 0.0f};
        float bbb_light_color1[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        float bbb_light_color2[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        bgfx::setUniform(u_lu_bbb_light_dir1_, bbb_light_dir1);
        bgfx::setUniform(u_lu_bbb_light_dir2_, bbb_light_dir2);
        bgfx::setUniform(u_lu_bbb_light_color1_, bbb_light_color1);
        bgfx::setUniform(u_lu_bbb_light_color2_, bbb_light_color2);

        bgfx::setState(renderStateForMaterial(mesh.material));
        bgfx::submit(kViewWorld, programForMaterial(mesh.material));
    }

    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(0, 0, 0x0f, "LU Renderer - %s", bgfx::getRendererName(bgfx::getRendererType()));
    bgfx::dbgTextPrintf(0, 1, 0x0f, "Asset: %s", source_asset_path_.empty() ? "<debug>" : source_asset_path_.c_str());
    bgfx::dbgTextPrintf(0, 2, 0x0f, "Meshes: %zu/%zu active", active_meshes, meshes_.size());
    bgfx::dbgTextPrintf(0, 3, 0x0f, "MSAA: %s x%u | SSR %s %.2f | GTAO %s %.2f | Bloom %s %.2f | PBR %s | Probes %s",
        features_.msaa.enabled ? "on" : "off",
        static_cast<unsigned>(features_.msaa.samples),
        features_.screen_space.ssr_enabled ? "on" : "off",
        features_.screen_space.ssr_strength,
        features_.screen_space.gtao_enabled ? "on" : "off",
        features_.screen_space.gtao_intensity,
        features_.post.bloom_enabled ? "on" : "off",
        features_.post.bloom_intensity,
        features_.lego_surface_model == SurfaceModel::PBR ? "on" : "off",
        features_.reflection_probe.enabled ? "on" : "off");
    bgfx::dbgTextPrintf(0, 4, 0x0f, "mesh | shader label(id/gv) | variant program/port | source | res/meta | alpha/z | tex/dark | vc/meshvc | lod | fog/spec/refl/mat/sh | env | anim/ctrl | em");

    const size_t debug_count = std::min<size_t>(meshes_.size(), 8);
    for (size_t i = 0; i < debug_count; ++i) {
        const auto& mesh = meshes_[i];
        const auto& material = mesh.material;
        std::string name = shortText(mesh.name.empty() ? std::string{"<unnamed>"} : mesh.name, 18);
        const std::string label = shortText(material.lu_shader_label.empty() ? std::string{"<unknown>"} : material.lu_shader_label, 20);
        const char* texture_text = material.diffuse_texture_path.empty() ? "no" : "yes";
        const char* dark_texture_text = material.dark_texture_path.empty() ? "no" : "yes";
        char lod_text[48];
        if (mesh.has_lod_range) {
            std::snprintf(lod_text, sizeof(lod_text), "L%u %.0f-%.0f",
                mesh.lod_level, mesh.lod_near, mesh.lod_far);
        } else {
            std::snprintf(lod_text, sizeof(lod_text), "none");
        }
        bgfx::dbgTextPrintf(
            0,
            static_cast<uint16_t>(5 + i),
            material.lu_shader_resolved ? 0x0f : 0x0e,
            "%s | %s(%d/%d) | %s/%s/%s | %s | %s %s/%d | %s/%s t%d b%d z%d | %s/%s | %d/%d | %s | %d/%d/%d/%d/%d | %s | u%d a%d c%d e%d k%zu | %.2f",
            name.c_str(),
            label.c_str(),
            material.lu_shader_id,
            material.lu_shader_game_value,
            shortText(legoppVariantName(material.legopp_variant), 12).c_str(),
            shaderFamilyName(material.shader_family),
            portStatusName(material.lu_shader_port_status),
            shortText(material.lu_shader_source_technique, 20).c_str(),
            shortText(resolutionSourceName(material.lu_shader_resolution_source), 12).c_str(),
            shortText(material.lu_shader_metadata.empty() ? std::string{"none"} : material.lu_shader_metadata, 10).c_str(),
            material.lu_multishader_prefix_id,
            alphaModeName(material.alpha_mode),
            alphaSemanticName(material.lu_shader_alpha_semantic),
            material.alpha_test ? 1 : 0,
            material.alpha_blend ? 1 : 0,
            material.depth_write ? 1 : 0,
            texture_text,
            dark_texture_text,
            material.lu_shader_uses_vertex_color ? 1 : 0,
            material.mesh_has_vertex_colors ? 1 : 0,
            lod_text,
            material.lu_shader_uses_fog ? 1 : 0,
            material.lu_shader_uses_specular ? 1 : 0,
            material.lu_shader_uses_reflection ? 1 : 0,
            material.lu_shader_uses_material_diffuse ? 1 : 0,
            material.lu_shader_uses_shadow_terrain ? 1 : 0,
            shortText(material.lu_shader_reflection_map.empty() ? std::string{"none"} : material.lu_shader_reflection_map, 14).c_str(),
            material.lu_shader_uses_uv_animation ? 1 : 0,
            material.lu_shader_uses_alpha_animation ? 1 : 0,
            material.nif_has_material_color_controller ? 1 : 0,
            material.material_emissive_controller ? 1 : 0,
            material.material_emissive_controller_keys.size(),
            std::max({material.emissive.x, material.emissive.y, material.emissive.z}));
    }
    if (meshes_.size() > debug_count) {
        bgfx::dbgTextPrintf(0, static_cast<uint16_t>(5 + debug_count), 0x07,
            "... %zu more mesh(es)", meshes_.size() - debug_count);
    }
    bgfx::frame();
}

bgfx::TextureHandle BgfxRenderer::loadReflectionCubeTexture(const std::string& name) {
    if (name.empty() || reflection_map_dir_.empty()) return BGFX_INVALID_HANDLE;
    std::filesystem::path path = reflection_map_dir_ / name;
    return loadCubeTextureDds(path, kWrapLinearSampler);
}

bgfx::TextureHandle BgfxRenderer::loadCubeTextureDds(const std::filesystem::path& path, uint64_t sampler_flags) {
    if (path.empty()) return BGFX_INVALID_HANDLE;
    const std::string cache_key = path.string() + "#" + std::to_string(sampler_flags);
    auto cached = cube_texture_cache_.find(cache_key);
    if (cached != cube_texture_cache_.end()) return cached->second;

    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) return BGFX_INVALID_HANDLE;

    try {
        auto dds = lu::assets::dds_parse_header(std::span<const uint8_t>(data.data(), data.size()));
        const bool is_cube =
            (dds.header.caps2 & kDdsCaps2Cubemap) != 0 &&
            (dds.header.caps2 & kDdsCubemapAllFaces) == kDdsCubemapAllFaces;
        bgfx::TextureFormat::Enum format = ddsTextureFormat(dds);
        if (!is_cube || format == bgfx::TextureFormat::Unknown || dds.data_offset >= data.size()) {
            return BGFX_INVALID_HANDLE;
        }

        const uint8_t* pixels = data.data() + dds.data_offset;
        uint32_t pixel_size = static_cast<uint32_t>(data.size() - dds.data_offset);
        bgfx::TextureHandle handle = bgfx::createTextureCube(
            static_cast<uint16_t>(dds.width),
            dds.mip_count > 1,
            1,
            format,
            sampler_flags,
            bgfx::copy(pixels, pixel_size));
        if (bgfx::isValid(handle)) {
            cube_texture_cache_[cache_key] = handle;
        }
        return handle;
    } catch (...) {
        return BGFX_INVALID_HANDLE;
    }
}

bgfx::ShaderHandle BgfxRenderer::loadShader(const char* name) {
    const char* profile = shaderProfileDir(bgfx::getRendererType());
    std::filesystem::path path = shader_dir_ / profile / name;
    std::vector<uint8_t> data = readFile(path);
    if (data.empty() && std::string(profile) == "dxil") {
        data = readFile(shader_dir_ / "dxbc" / name);
    }
    if (data.empty()) {
        std::cerr << "Missing shader: " << path << "\n";
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(data.data(), static_cast<uint32_t>(data.size()));
    return bgfx::createShader(mem);
}

bgfx::ProgramHandle BgfxRenderer::loadProgram(const char* vs_name, const char* fs_name) {
    bgfx::ShaderHandle vs = loadShader(vs_name);
    bgfx::ShaderHandle fs = loadShader(fs_name);
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::createProgram(vs, fs, true);
}

bgfx::TextureHandle BgfxRenderer::loadTexture(const std::string& path, uint64_t sampler_flags) {
    if (path.empty()) return BGFX_INVALID_HANDLE;
    const std::string cache_key = path + "#" + std::to_string(sampler_flags);
    auto cached = texture_cache_.find(cache_key);
    if (cached != texture_cache_.end()) return cached->second;

    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) return BGFX_INVALID_HANDLE;

    try {
        auto dds = lu::assets::dds_parse_header(std::span<const uint8_t>(data.data(), data.size()));
        bgfx::TextureFormat::Enum format = ddsTextureFormat(dds);
        if (format == bgfx::TextureFormat::Unknown || dds.data_offset >= data.size()) {
            return BGFX_INVALID_HANDLE;
        }

        const uint8_t* pixels = data.data() + dds.data_offset;
        uint32_t pixel_size = static_cast<uint32_t>(data.size() - dds.data_offset);
        bgfx::TextureHandle handle = bgfx::createTexture2D(
            static_cast<uint16_t>(dds.width),
            static_cast<uint16_t>(dds.height),
            dds.mip_count > 1,
            1,
            format,
            sampler_flags,
            bgfx::copy(pixels, pixel_size));

        texture_cache_[cache_key] = handle;
        return handle;
    } catch (...) {
        return BGFX_INVALID_HANDLE;
    }
}

bgfx::TextureHandle BgfxRenderer::createSolidTexture(uint32_t rgba) {
    std::array<uint32_t, 4> pixels = {rgba, rgba, rgba, rgba};
    return bgfx::createTexture2D(2, 2, false, 1, bgfx::TextureFormat::RGBA8, 0,
        bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(uint32_t))));
}

bgfx::TextureHandle BgfxRenderer::createSolidCubeTexture(uint32_t rgba) {
    std::array<uint32_t, 6> pixels = {rgba, rgba, rgba, rgba, rgba, rgba};
    return bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, 0,
        bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(uint32_t))));
}

bgfx::ProgramHandle BgfxRenderer::programForMaterial(const MaterialAsset& material) const {
    switch (material.shader_family) {
    case LegacyShaderFamily::Basic:
        return bgfx::isValid(basic_program_) ? basic_program_ : legacy_program_;
    case LegacyShaderFamily::BasicLit:
        return bgfx::isValid(basic_lit_program_) ? basic_lit_program_ : legacy_program_;
    case LegacyShaderFamily::BasicTwoLayer:
        return bgfx::isValid(basic_two_layer_program_) ? basic_two_layer_program_ : basic_program_;
    case LegacyShaderFamily::AlphaAsAlpha:
        return bgfx::isValid(alpha_as_alpha_program_) ? alpha_as_alpha_program_ : legacy_program_;
    case LegacyShaderFamily::AlphaUvScroll:
        return bgfx::isValid(alpha_uv_scroll_program_) ? alpha_uv_scroll_program_ : alpha_as_alpha_program_;
    case LegacyShaderFamily::LegoppNoAmbient:
        return bgfx::isValid(legopp_no_ambient_program_) ? legopp_no_ambient_program_ : legopp_program_;
    case LegacyShaderFamily::LegoppEmissive:
        return bgfx::isValid(legopp_emissive_program_) ? legopp_emissive_program_ : legopp_program_;
    case LegacyShaderFamily::TerrainRim:
        return bgfx::isValid(terrain_rim_program_) ? terrain_rim_program_ : legacy_program_;
    case LegacyShaderFamily::OceanDistort:
        return bgfx::isValid(ocean_distort_program_) ? ocean_distort_program_ : legacy_program_;
    case LegacyShaderFamily::OceanDistortDirectional:
        return bgfx::isValid(ocean_distort_directional_program_) ? ocean_distort_directional_program_ : legacy_program_;
    case LegacyShaderFamily::OceanDistortFx:
        return bgfx::isValid(ocean_distort_fx_program_) ? ocean_distort_fx_program_ : ocean_distort_program_;
    case LegacyShaderFamily::OceanDistortUnlit:
        return bgfx::isValid(ocean_distort_unlit_program_) ? ocean_distort_unlit_program_ : ocean_distort_program_;
    case LegacyShaderFamily::LegoppEffect:
    case LegacyShaderFamily::LegoppLighting:
        return bgfx::isValid(legopp_program_) ? legopp_program_ : legacy_program_;
    case LegacyShaderFamily::ClearPlastic:
        return bgfx::isValid(clear_plastic_program_) ? clear_plastic_program_ : legacy_program_;
    case LegacyShaderFamily::Metallic:
        if (material.lu_shader_reflection_map.find("polished") != std::string::npos) {
            return bgfx::isValid(metallic_polished_program_) ? metallic_polished_program_ : metallic_brushed_program_;
        }
        return bgfx::isValid(metallic_brushed_program_) ? metallic_brushed_program_ : legacy_program_;
    case LegacyShaderFamily::LegacyMesh:
    default:
        return legacy_program_;
    }
}

uint64_t BgfxRenderer::renderStateForMaterial(const MaterialAsset& material) const {
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                     BGFX_STATE_DEPTH_TEST_LESS;

    if (features_.msaa.enabled) {
        state |= BGFX_STATE_MSAA;
    }

    if (!material.depth_write) {
        state &= ~BGFX_STATE_WRITE_Z;
    }

    if (material.cull_mode == RenderCullMode::Clockwise) {
        state |= BGFX_STATE_CULL_CW;
    } else if (material.cull_mode == RenderCullMode::Backface ||
               material.cull_mode == RenderCullMode::CounterClockwise) {
        state |= BGFX_STATE_CULL_CCW;
    }

    if (material.alpha_mode == RenderAlphaMode::Additive) {
        state |= BGFX_STATE_BLEND_ADD;
    } else if (material.alpha_blend || material.alpha_mode == RenderAlphaMode::AlphaBlend) {
        state |= BGFX_STATE_BLEND_ALPHA;
    }

    return state;
}

uint32_t BgfxRenderer::resetFlags() const {
    uint32_t flags = BGFX_RESET_VSYNC;
    if (!features_.msaa.enabled) {
        return flags;
    }

    if (features_.msaa.samples >= 16) {
        flags |= BGFX_RESET_MSAA_X16;
    } else if (features_.msaa.samples >= 8) {
        flags |= BGFX_RESET_MSAA_X8;
    } else if (features_.msaa.samples >= 4) {
        flags |= BGFX_RESET_MSAA_X4;
    } else if (features_.msaa.samples >= 2) {
        flags |= BGFX_RESET_MSAA_X2;
    }
    return flags;
}

void BgfxRenderer::destroyTextureCache() {
    for (auto& [_, handle] : texture_cache_) {
        if (bgfx::isValid(handle)) bgfx::destroy(handle);
    }
    texture_cache_.clear();
}

void BgfxRenderer::destroyMesh(GpuMesh& mesh) {
    if (bgfx::isValid(mesh.vertex_buffer)) bgfx::destroy(mesh.vertex_buffer);
    if (bgfx::isValid(mesh.index_buffer)) bgfx::destroy(mesh.index_buffer);
    mesh.vertex_buffer = BGFX_INVALID_HANDLE;
    mesh.index_buffer = BGFX_INVALID_HANDLE;
    mesh.texture = BGFX_INVALID_HANDLE;
    mesh.dark_texture = BGFX_INVALID_HANDLE;
}

void BgfxRenderer::drawGrid() {
    constexpr int kHalfLines = 20;
    constexpr float kStep = 1.0f;
    constexpr uint32_t kColor = 0x66505050u;

    bgfx::TransientVertexBuffer tvb;
    uint32_t vertex_count = static_cast<uint32_t>((kHalfLines * 2 + 1) * 4);
    if (bgfx::getAvailTransientVertexBuffer(vertex_count, GpuVertex::layout) < vertex_count) return;
    bgfx::allocTransientVertexBuffer(&tvb, vertex_count, GpuVertex::layout);

    auto* verts = reinterpret_cast<GpuVertex*>(tvb.data);
    uint32_t idx = 0;
    for (int i = -kHalfLines; i <= kHalfLines; ++i) {
        float p = static_cast<float>(i) * kStep;
        verts[idx++] = {-kHalfLines * kStep, 0, p, 0, 1, 0, 0, 0, 0, 0, kColor};
        verts[idx++] = { kHalfLines * kStep, 0, p, 0, 1, 0, 0, 0, 0, 0, kColor};
        verts[idx++] = {p, 0, -kHalfLines * kStep, 0, 1, 0, 0, 0, 0, 0, kColor};
        verts[idx++] = {p, 0,  kHalfLines * kStep, 0, 1, 0, 0, 0, 0, 0, kColor};
    }

    float diffuse[4] = {0.7f, 0.7f, 0.7f, 1.0f};
    float light_ambient[4] = {0.0f, -1.0f, 0.0f, 1.0f};
    float light_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float shader_flags[4] = {0.0f, 1.0f, 1.0f, -1.0f};
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setTexture(0, s_diffuse_, white_texture_);
    bgfx::setUniform(u_material_diffuse_, diffuse);
    bgfx::setUniform(u_light_dir_ambient_, light_ambient);
    bgfx::setUniform(u_light_color_, light_color);
    bgfx::setUniform(u_lu_shader_flags_, shader_flags);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(kViewWorld, legacy_program_);
}

} // namespace lu::renderer::bgfx_backend
