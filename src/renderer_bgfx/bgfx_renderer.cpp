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
#include <limits>
#include <span>
#include <string>

namespace lu::renderer::bgfx_backend {

namespace {

constexpr bgfx::ViewId kViewShadow = 0;
constexpr bgfx::ViewId kViewProbeFirst = 1;
constexpr bgfx::ViewId kViewWorld = 7;
constexpr bgfx::ViewId kViewSceneNormal = 8;
constexpr bgfx::ViewId kViewReflectionMask = 9;
constexpr bgfx::ViewId kViewBloomMask = 10;
constexpr bgfx::ViewId kViewGtaoRaw = 11;
constexpr bgfx::ViewId kViewGtaoDenoise = 12;
constexpr bgfx::ViewId kViewBloomExtract = 13;
constexpr bgfx::ViewId kViewBloomDownFirst = 14;
constexpr bgfx::ViewId kViewBloomUpFirst = 20;
constexpr bgfx::ViewId kViewPost = 26;
constexpr bgfx::ViewId kViewTemporalPost = 27;
constexpr uint16_t kShadowMapSize = 2048;
constexpr uint16_t kGlobalProbeSize = 128;
constexpr uint32_t kDdsCaps2Cubemap = 0x00000200u;
constexpr uint32_t kDdsCubemapAllFaces = 0x0000fc00u;
constexpr uint64_t kWrapLinearSampler = 0;

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

struct PostVertex {
    float x, y, z;
    float u, v;

    static bgfx::VertexLayout layout;
    static void initLayout() {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
    }
};

bgfx::VertexLayout PostVertex::layout;

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

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool textImpliesBloom(const std::string& value) {
    if (value.empty()) return false;
    const std::string lower = toLowerCopy(value);
    return lower.find("glow") != std::string::npos ||
           lower.find("emissive") != std::string::npos ||
           lower.find("bloom") != std::string::npos;
}

uint32_t packRgba8(Vec3 rgb, float alpha = 1.0f) {
    auto byte = [](float value) {
        return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return byte(rgb.x) | (byte(rgb.y) << 8u) | (byte(rgb.z) << 16u) | (byte(alpha) << 24u);
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

bool usesDepthPostEffects(const RenderFeatureSettings& features) {
    return (features.post.dof_enabled && features.post.dof_aperture > 0.0f) ||
           (features.screen_space.ssr_enabled && features.screen_space.ssr_strength > 0.0f) ||
           (features.screen_space.gtao_enabled && features.screen_space.gtao_intensity > 0.0f);
}

uint64_t msaaRenderTargetFlags(uint8_t samples) {
    if (samples >= 16) return BGFX_TEXTURE_RT_MSAA_X16;
    if (samples >= 8) return BGFX_TEXTURE_RT_MSAA_X8;
    if (samples >= 4) return BGFX_TEXTURE_RT_MSAA_X4;
    if (samples >= 2) return BGFX_TEXTURE_RT_MSAA_X2;
    return 0;
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

float bloomMaskForMaterial(
    const MaterialAsset& material,
    float elapsed_seconds,
    const std::string& mesh_name,
    const std::string& source_asset_path) {
    float mask = 0.0f;
    switch (material.legopp_variant) {
    case LegoppShaderVariant::SuperEmissive:
        mask = 1.0f;
        break;
    case LegoppShaderVariant::Emissive:
    case LegoppShaderVariant::Glow:
    case LegoppShaderVariant::GlowIgnoreVertAlpha:
    case LegoppShaderVariant::ItemGlow:
        mask = 0.85f;
        break;
    default:
        break;
    }

    if (material.shader_family == LegacyShaderFamily::LegoppEmissive) {
        mask = std::max(mask, 0.85f);
    }
    if (!material.glow_texture_path.empty()) {
        mask = std::max(mask, 0.75f);
    }
    if (material.lu_shader_alpha_semantic == ShaderAlphaSemantic::ControlGlow ||
        material.lu_shader_alpha_semantic == ShaderAlphaSemantic::ControlEmissive) {
        mask = std::max(mask, 0.65f);
    }
    if (textImpliesBloom(mesh_name) ||
        textImpliesBloom(source_asset_path) ||
        textImpliesBloom(material.name) ||
        textImpliesBloom(material.lu_shader_label) ||
        textImpliesBloom(material.diffuse_texture_path) ||
        textImpliesBloom(material.dark_texture_path) ||
        textImpliesBloom(material.detail_texture_path)) {
        mask = std::max(mask, 0.65f);
    }

    const Vec3 emissive = materialEmissiveAtTime(material, elapsed_seconds);
    const float emissive_max = std::max({emissive.x, emissive.y, emissive.z});
    mask = std::max(mask, std::clamp(emissive_max, 0.0f, 1.0f));
    if (material.material_emissive_controller) {
        mask = std::max(mask, std::clamp(emissive_max * 1.5f, 0.25f, 1.0f));
    }
    return std::clamp(mask * std::max(material.lu_glow_lightness, 0.0f), 0.0f, 1.0f);
}

float reflectionMaskForMaterial(const MaterialAsset& material) {
    if (!material.lu_shader_uses_reflection || material.lu_shader_reflection_map.empty()) {
        return 0.0f;
    }

    float mask = 0.55f;
    const std::string reflection_map = toLowerCopy(material.lu_shader_reflection_map);
    const std::string reflection_semantic = toLowerCopy(material.lu_shader_reflection_semantic);

    if (reflection_map.find("polished") != std::string::npos) {
        mask = std::max(mask, 1.0f);
    } else if (reflection_map.find("brushed") != std::string::npos) {
        mask = std::max(mask, 0.75f);
    }

    if (reflection_semantic.find("metal") != std::string::npos ||
        material.shader_family == LegacyShaderFamily::Metallic) {
        mask = std::max(mask, reflection_map.find("polished") != std::string::npos ? 1.0f : 0.78f);
    }
    if (material.shader_family == LegacyShaderFamily::ClearPlastic) {
        mask = std::max(mask, 0.8f);
    }
    if (reflection_semantic.find("dark") != std::string::npos ||
        reflection_semantic.find("glow") != std::string::npos) {
        mask = std::max(mask, 0.45f);
    }

    switch (material.legopp_variant) {
    case LegoppShaderVariant::DarklingSpecular:
    case LegoppShaderVariant::DarklingShinyGlint:
    case LegoppShaderVariant::DarklingSpecularShinyGlint:
    case LegoppShaderVariant::DarklingStructureShinyGlint:
    case LegoppShaderVariant::ShinyGlint:
        mask = std::max(mask, 0.65f);
        break;
    default:
        break;
    }

    return std::clamp(mask, 0.0f, 1.0f);
}

float halton(uint64_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

uint32_t samplerFlagsForAddressMode(const TextureAddressMode& address) {
    if (!address.authored) return kWrapLinearSampler;

    uint32_t flags = 0;
    if (!address.wrap_u) flags |= BGFX_SAMPLER_U_CLAMP;
    if (!address.wrap_v) flags |= BGFX_SAMPLER_V_CLAMP;
    return flags;
}

uint32_t diffuseSamplerFlagsForMaterial(const MaterialAsset& material) {
    return samplerFlagsForAddressMode(material.diffuse_texture_address);
}

bool useColorInLegoppDiffuse(const MaterialAsset& material, bool has_texture) {
    if (!isLegoppFamily(material.shader_family)) {
        return material.lu_shader_uses_material_diffuse;
    }
    return material.lu_shader_uses_material_diffuse && !has_texture;
}

float alphaThresholdForMaterial(const MaterialAsset& material) {
    if (material.alpha_test || material.alpha_mode == RenderAlphaMode::AlphaTest) {
        const uint8_t threshold = material.alpha_threshold > 0 ? material.alpha_threshold : 127;
        return static_cast<float>(threshold) / 255.0f;
    }
    return -1.0f;
}

std::array<float, 4> shaderFlagsForMaterial(const MaterialAsset& material) {
    const bool has_texture = material.lu_shader_uses_texture;
    return {
        has_texture ? 1.0f : 0.0f,
        material.lu_shader_uses_vertex_color ? 1.0f : 0.0f,
        useColorInLegoppDiffuse(material, has_texture) ? 1.0f : 0.0f,
        alphaThresholdForMaterial(material)
    };
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

bool isTransparentForwardMaterial(const MaterialAsset& material) {
    return material.alpha_mode == RenderAlphaMode::AlphaBlend ||
           material.alpha_mode == RenderAlphaMode::Additive ||
           material.alpha_blend;
}

bool participatesInDepthPrepass(const MaterialAsset& material) {
    return material.depth_write &&
           material.alpha_mode != RenderAlphaMode::AlphaBlend &&
           material.alpha_mode != RenderAlphaMode::Additive;
}

std::string shortText(const std::string& value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars <= 3) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

} // namespace

BgfxRenderer::BgfxRenderer() {
    global_probe_framebuffers_.fill(BGFX_INVALID_HANDLE);
    global_probe_depth_textures_.fill(BGFX_INVALID_HANDLE);
    temporal_history_textures_.fill(BGFX_INVALID_HANDLE);
    temporal_history_framebuffers_.fill(BGFX_INVALID_HANDLE);
    bloom_textures_.fill(BGFX_INVALID_HANDLE);
    bloom_framebuffers_.fill(BGFX_INVALID_HANDLE);
}

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
    bgfx_init.debug = init.bgfx_device_debug;
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
    PostVertex::initLayout();

    bgfx::setDebug(BGFX_DEBUG_TEXT);
    bgfx::setViewClear(kViewShadow, BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
    bgfx::setViewRect(kViewShadow, 0, 0, kShadowMapSize, kShadowMapSize);
    bgfx::setViewClear(kViewWorld, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20232aff, 1.0f, 0);
    bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);
    bgfx::setViewClear(kViewSceneNormal, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x8080ffff, 1.0f, 0);
    bgfx::setViewRect(kViewSceneNormal, 0, 0, width_, height_);
    bgfx::setViewClear(kViewReflectionMask, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::setViewRect(kViewReflectionMask, 0, 0, width_, height_);
    bgfx::setViewClear(kViewBloomMask, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::setViewRect(kViewBloomMask, 0, 0, width_, height_);
    bgfx::setViewClear(kViewGtaoRaw, BGFX_CLEAR_COLOR, 0xffffffff, 1.0f, 0);
    bgfx::setViewRect(kViewGtaoRaw, 0, 0, width_, height_);
    bgfx::setViewClear(kViewGtaoDenoise, BGFX_CLEAR_COLOR, 0xffffffff, 1.0f, 0);
    bgfx::setViewRect(kViewGtaoDenoise, 0, 0, width_, height_);
    for (size_t i = 0; i < kBloomMipCount; ++i) {
        const bgfx::ViewId down_view = i == 0
            ? kViewBloomExtract
            : static_cast<bgfx::ViewId>(kViewBloomDownFirst + i - 1);
        bgfx::setViewClear(down_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
        bgfx::setViewRect(down_view, 0, 0, 1, 1);
        if (i + 1 < kBloomMipCount) {
            const bgfx::ViewId up_view = static_cast<bgfx::ViewId>(kViewBloomUpFirst + i);
            bgfx::setViewClear(up_view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
            bgfx::setViewRect(up_view, 0, 0, 1, 1);
        }
    }
    bgfx::setViewClear(kViewPost, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    bgfx::setViewRect(kViewPost, 0, 0, width_, height_);
    bgfx::setViewClear(kViewTemporalPost, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    bgfx::setViewRect(kViewTemporalPost, 0, 0, width_, height_);
    for (uint8_t face = 0; face < 6; ++face) {
        const bgfx::ViewId view_id = static_cast<bgfx::ViewId>(kViewProbeFirst + face);
        bgfx::setViewClear(view_id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20232aff, 1.0f, 0);
        bgfx::setViewRect(view_id, 0, 0, kGlobalProbeSize, kGlobalProbeSize);
    }

    s_diffuse_ = bgfx::createUniform("s_diffuse", bgfx::UniformType::Sampler);
    s_dark_ = bgfx::createUniform("s_dark", bgfx::UniformType::Sampler);
    s_lu_env_ = bgfx::createUniform("s_luEnv", bgfx::UniformType::Sampler);
    s_scene_color_ = bgfx::createUniform("s_sceneColor", bgfx::UniformType::Sampler);
    s_scene_depth_ = bgfx::createUniform("s_sceneDepth", bgfx::UniformType::Sampler);
    s_scene_normal_ = bgfx::createUniform("s_sceneNormal", bgfx::UniformType::Sampler);
    s_history_color_ = bgfx::createUniform("s_historyColor", bgfx::UniformType::Sampler);
    s_reflection_mask_ = bgfx::createUniform("s_reflectionMask", bgfx::UniformType::Sampler);
    s_bloom_mask_ = bgfx::createUniform("s_bloomMask", bgfx::UniformType::Sampler);
    s_gtao_ = bgfx::createUniform("s_gtao", bgfx::UniformType::Sampler);
    s_color_lut_ = bgfx::createUniform("s_colorLut", bgfx::UniformType::Sampler);
    s_shadow_map_ = bgfx::createUniform("s_shadowMap", bgfx::UniformType::Sampler);
    u_shadow_matrix_ = bgfx::createUniform("u_shadowMatrix", bgfx::UniformType::Mat4);
    u_shadow_params_ = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
    u_shadow_bias_params_ = bgfx::createUniform("u_shadowBiasParams", bgfx::UniformType::Vec4);
    u_shadow_light_dir_ = bgfx::createUniform("u_shadowLightDir", bgfx::UniformType::Vec4);
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
    u_lu_pbr_params_ = bgfx::createUniform("u_luPbrParams", bgfx::UniformType::Vec4);
    u_lu_reflection_params_ = bgfx::createUniform("u_luReflectionParams", bgfx::UniformType::Vec4);
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
    u_post_params_ = bgfx::createUniform("u_postParams", bgfx::UniformType::Vec4);
    u_bloom_params_ = bgfx::createUniform("u_bloomParams", bgfx::UniformType::Vec4);
    u_dof_params_ = bgfx::createUniform("u_dofParams", bgfx::UniformType::Vec4);
    u_color_lut_params_ = bgfx::createUniform("u_colorLutParams", bgfx::UniformType::Vec4);
    u_screen_params_ = bgfx::createUniform("u_screenParams", bgfx::UniformType::Vec4);
    u_screen_space_params_ = bgfx::createUniform("u_screenSpaceParams", bgfx::UniformType::Vec4);
    u_depth_params_ = bgfx::createUniform("u_depthParams", bgfx::UniformType::Vec4);
    u_temporal_params_ = bgfx::createUniform("u_temporalParams", bgfx::UniformType::Vec4);
    u_reflection_mask_value_ = bgfx::createUniform("u_reflectionMaskValue", bgfx::UniformType::Vec4);
    white_texture_ = createSolidTexture(0xffffffffu);
    black_texture_ = createSolidTexture(0x000000ffu);
    missing_texture_ = createSolidTexture(0xffff00ffu);
    flat_normal_texture_ = createSolidTexture(0xffff8080u);
    neutral_lut_texture_ = createNeutralColorLutTexture();
    color_lut_texture_ = neutral_lut_texture_;
    if (!features_.post.color_lut_path.empty()) {
        color_lut_texture_ = loadColorLutTexture(features_.post.color_lut_path);
        color_lut_path_ = features_.post.color_lut_path;
        if (!bgfx::isValid(color_lut_texture_)) {
            color_lut_texture_ = neutral_lut_texture_;
            color_lut_size_ = 16.0f;
            color_lut_horizontal_ = 1.0f;
        }
    }
    neutral_env_texture_ = loadReflectionCubeTexture("default_reflection.dds");
    if (!bgfx::isValid(neutral_env_texture_)) {
        neutral_env_texture_ = createSolidCubeTexture(0xffd8d8d8u);
    }
    rebuildEnvironmentProbeTexture();
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
    shadow_depth_program_ = loadProgram("vs_shadow_depth.sc.bin", "fs_shadow_depth.sc.bin");
    bloom_program_ = loadProgram("vs_fullscreen.sc.bin", "fs_bloom.sc.bin");
    gtao_program_ = loadProgram("vs_fullscreen.sc.bin", "fs_gtao.sc.bin");
    gtao_denoise_program_ = loadProgram("vs_fullscreen.sc.bin", "fs_gtao_denoise.sc.bin");
    post_process_program_ = loadProgram("vs_fullscreen.sc.bin", "fs_post_effects.sc.bin");
    fullscreen_copy_program_ = loadProgram("vs_fullscreen.sc.bin", "fs_fullscreen_copy.sc.bin");
    reflection_mask_program_ = loadProgram("vs_reflection_mask.sc.bin", "fs_reflection_mask.sc.bin");
    view_normal_program_ = loadProgram("vs_view_normal.sc.bin", "fs_view_normal.sc.bin");

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
        !bgfx::isValid(clear_plastic_program_) ||
        !bgfx::isValid(shadow_depth_program_) ||
        !bgfx::isValid(bloom_program_) ||
        !bgfx::isValid(gtao_program_) ||
        !bgfx::isValid(gtao_denoise_program_) ||
        !bgfx::isValid(post_process_program_) ||
        !bgfx::isValid(fullscreen_copy_program_) ||
        !bgfx::isValid(reflection_mask_program_) ||
        !bgfx::isValid(view_normal_program_)) {
        last_error_ = "Failed to load LU mesh shaders from " + shader_dir_.string();
        return false;
    }

    return true;
}

void BgfxRenderer::shutdown() {
    if (!initialized_) return;

    bgfx::frame();
    clearWorld();
    destroyCapturedProbeTarget();
    destroyShadowTarget();
    destroySceneTarget();
    destroySceneNormalTarget();
    destroyTemporalHistoryTargets();
    destroyReflectionMaskTarget();
    destroyBloomMaskTarget();
    destroyGtaoTargets();
    destroyBloomChain();
    destroyTextureCache();
    for (auto& [_, handle] : cube_texture_cache_) {
        if (bgfx::isValid(handle) && handle.idx != neutral_env_texture_.idx) bgfx::destroy(handle);
    }
    cube_texture_cache_.clear();
    if (bgfx::isValid(white_texture_)) bgfx::destroy(white_texture_);
    if (bgfx::isValid(black_texture_)) bgfx::destroy(black_texture_);
    if (bgfx::isValid(missing_texture_)) bgfx::destroy(missing_texture_);
    if (bgfx::isValid(flat_normal_texture_)) bgfx::destroy(flat_normal_texture_);
    if (bgfx::isValid(color_lut_texture_) && color_lut_texture_.idx != neutral_lut_texture_.idx) bgfx::destroy(color_lut_texture_);
    if (bgfx::isValid(neutral_lut_texture_)) bgfx::destroy(neutral_lut_texture_);
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
    if (bgfx::isValid(shadow_depth_program_)) bgfx::destroy(shadow_depth_program_);
    if (bgfx::isValid(bloom_program_)) bgfx::destroy(bloom_program_);
    if (bgfx::isValid(gtao_program_)) bgfx::destroy(gtao_program_);
    if (bgfx::isValid(gtao_denoise_program_)) bgfx::destroy(gtao_denoise_program_);
    if (bgfx::isValid(post_process_program_)) bgfx::destroy(post_process_program_);
    if (bgfx::isValid(fullscreen_copy_program_)) bgfx::destroy(fullscreen_copy_program_);
    if (bgfx::isValid(reflection_mask_program_)) bgfx::destroy(reflection_mask_program_);
    if (bgfx::isValid(view_normal_program_)) bgfx::destroy(view_normal_program_);
    if (bgfx::isValid(s_diffuse_)) bgfx::destroy(s_diffuse_);
    if (bgfx::isValid(s_dark_)) bgfx::destroy(s_dark_);
    if (bgfx::isValid(s_lu_env_)) bgfx::destroy(s_lu_env_);
    if (bgfx::isValid(s_scene_color_)) bgfx::destroy(s_scene_color_);
    if (bgfx::isValid(s_scene_depth_)) bgfx::destroy(s_scene_depth_);
    if (bgfx::isValid(s_scene_normal_)) bgfx::destroy(s_scene_normal_);
    if (bgfx::isValid(s_history_color_)) bgfx::destroy(s_history_color_);
    if (bgfx::isValid(s_reflection_mask_)) bgfx::destroy(s_reflection_mask_);
    if (bgfx::isValid(s_bloom_mask_)) bgfx::destroy(s_bloom_mask_);
    if (bgfx::isValid(s_gtao_)) bgfx::destroy(s_gtao_);
    if (bgfx::isValid(s_color_lut_)) bgfx::destroy(s_color_lut_);
    if (bgfx::isValid(s_shadow_map_)) bgfx::destroy(s_shadow_map_);
    if (bgfx::isValid(u_shadow_matrix_)) bgfx::destroy(u_shadow_matrix_);
    if (bgfx::isValid(u_shadow_params_)) bgfx::destroy(u_shadow_params_);
    if (bgfx::isValid(u_shadow_bias_params_)) bgfx::destroy(u_shadow_bias_params_);
    if (bgfx::isValid(u_shadow_light_dir_)) bgfx::destroy(u_shadow_light_dir_);
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
    if (bgfx::isValid(u_lu_pbr_params_)) bgfx::destroy(u_lu_pbr_params_);
    if (bgfx::isValid(u_lu_reflection_params_)) bgfx::destroy(u_lu_reflection_params_);
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
    if (bgfx::isValid(u_post_params_)) bgfx::destroy(u_post_params_);
    if (bgfx::isValid(u_bloom_params_)) bgfx::destroy(u_bloom_params_);
    if (bgfx::isValid(u_dof_params_)) bgfx::destroy(u_dof_params_);
    if (bgfx::isValid(u_color_lut_params_)) bgfx::destroy(u_color_lut_params_);
    if (bgfx::isValid(u_screen_params_)) bgfx::destroy(u_screen_params_);
    if (bgfx::isValid(u_screen_space_params_)) bgfx::destroy(u_screen_space_params_);
    if (bgfx::isValid(u_depth_params_)) bgfx::destroy(u_depth_params_);
    if (bgfx::isValid(u_temporal_params_)) bgfx::destroy(u_temporal_params_);
    if (bgfx::isValid(u_reflection_mask_value_)) bgfx::destroy(u_reflection_mask_value_);
    bgfx::frame();

    bgfx::shutdown();
    initialized_ = false;
}

void BgfxRenderer::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    bgfx::reset(width_, height_, resetFlags());
    bgfx::setViewRect(kViewShadow, 0, 0, kShadowMapSize, kShadowMapSize);
    bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);
    bgfx::setViewRect(kViewSceneNormal, 0, 0, width_, height_);
    bgfx::setViewRect(kViewReflectionMask, 0, 0, width_, height_);
    bgfx::setViewRect(kViewBloomMask, 0, 0, width_, height_);
    bgfx::setViewRect(kViewGtaoRaw, 0, 0, width_, height_);
    bgfx::setViewRect(kViewGtaoDenoise, 0, 0, width_, height_);
    for (size_t i = 0; i < kBloomMipCount; ++i) {
        const bgfx::ViewId down_view = i == 0
            ? kViewBloomExtract
            : static_cast<bgfx::ViewId>(kViewBloomDownFirst + i - 1);
        bgfx::setViewClear(down_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
        if (i + 1 < kBloomMipCount) {
            const bgfx::ViewId up_view = static_cast<bgfx::ViewId>(kViewBloomUpFirst + i);
            bgfx::setViewClear(up_view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
        }
    }
    bgfx::setViewRect(kViewPost, 0, 0, width_, height_);
    bgfx::setViewRect(kViewTemporalPost, 0, 0, width_, height_);
    for (uint8_t face = 0; face < 6; ++face) {
        bgfx::setViewRect(static_cast<bgfx::ViewId>(kViewProbeFirst + face), 0, 0, kGlobalProbeSize, kGlobalProbeSize);
    }
    destroySceneTarget();
    destroySceneNormalTarget();
    destroyTemporalHistoryTargets();
    destroyReflectionMaskTarget();
    destroyBloomMaskTarget();
    destroyGtaoTargets();
    destroyBloomChain();
    global_probe_capture_dirty_ = true;
}

void BgfxRenderer::clearWorld() {
    for (auto& mesh : meshes_) {
        destroyMesh(mesh);
    }
    meshes_.clear();
    temporal_history_valid_ = false;
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
        gpu.bounds_min = mesh.vertices.front().position;
        gpu.bounds_max = mesh.vertices.front().position;
        gpu.has_vertex_color = false;
        for (const auto& vertex : mesh.vertices) {
            gpu.bounds_min.x = std::min(gpu.bounds_min.x, vertex.position.x);
            gpu.bounds_min.y = std::min(gpu.bounds_min.y, vertex.position.y);
            gpu.bounds_min.z = std::min(gpu.bounds_min.z, vertex.position.z);
            gpu.bounds_max.x = std::max(gpu.bounds_max.x, vertex.position.x);
            gpu.bounds_max.y = std::max(gpu.bounds_max.y, vertex.position.y);
            gpu.bounds_max.z = std::max(gpu.bounds_max.z, vertex.position.z);
            if (vertex.color_rgba8 != 0xffffffffu) {
                gpu.has_vertex_color = true;
            }
        }
        gpu.vertex_buffer = bgfx::createVertexBuffer(
            bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(GpuVertex))),
            GpuVertex::layout);
        gpu.index_buffer = bgfx::createIndexBuffer(
            bgfx::copy(mesh.indices.data(), static_cast<uint32_t>(mesh.indices.size() * sizeof(uint32_t))),
            BGFX_BUFFER_INDEX32);
        const uint32_t diffuse_sampler_flags = diffuseSamplerFlagsForMaterial(mesh.material);
        gpu.texture_sampler_flags = diffuse_sampler_flags;
        gpu.texture = loadTexture(mesh.material.diffuse_texture_path, diffuse_sampler_flags);
        if (!bgfx::isValid(gpu.texture)) {
            gpu.texture = mesh.material.diffuse_texture_path.empty() ? white_texture_ : missing_texture_;
        }
        std::string auxiliary_texture_path = mesh.material.dark_texture_path;
        TextureAddressMode auxiliary_texture_address = mesh.material.dark_texture_address;
        if (mesh.material.shader_family == LegacyShaderFamily::Metallic) {
            auxiliary_texture_path = mesh.material.detail_texture_path;
            auxiliary_texture_address = mesh.material.detail_texture_address;
            if (auxiliary_texture_path.empty() && !reflection_map_dir_.empty()) {
                auxiliary_texture_path = (reflection_map_dir_ / "metal_reflection_brushed_noise.dds").string();
                auxiliary_texture_address = {};
            }
        }
        const uint32_t auxiliary_sampler_flags = samplerFlagsForAddressMode(auxiliary_texture_address);
        gpu.dark_texture_sampler_flags = auxiliary_sampler_flags;
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
    rebuildEnvironmentProbeTexture();
    global_probe_capture_dirty_ = true;
    temporal_history_valid_ = false;
}

void BgfxRenderer::setEnvironment(const EnvironmentState& environment) {
    environment_ = environment;
    rebuildEnvironmentProbeTexture();
    global_probe_capture_dirty_ = true;
    temporal_history_valid_ = false;
}

void BgfxRenderer::setFeatureSettings(const RenderFeatureSettings& features) {
    const uint32_t old_reset = resetFlags();
    const std::string old_lut_path = features_.post.color_lut_path;
    const bool old_probe_enabled = features_.reflection_probe.enabled;
    const SurfaceModel old_lego_surface_model = features_.lego_surface_model;
    const PbrBrdfSettings old_pbr = features_.pbr;
    const bool old_taa_enabled = features_.post.taa_enabled;
    const float old_taa_feedback = features_.post.taa_feedback;
    const float old_taa_jitter = features_.post.taa_jitter;
    features_ = features;
    if (features_.reflection_probe.enabled != old_probe_enabled) {
        global_probe_capture_dirty_ = true;
    }
    if (features_.reflection_probe.enabled &&
        (features_.lego_surface_model != old_lego_surface_model ||
         features_.pbr.roughness != old_pbr.roughness ||
         features_.pbr.metallic != old_pbr.metallic ||
         features_.pbr.specular_intensity != old_pbr.specular_intensity)) {
        global_probe_capture_dirty_ = true;
    }
    if (features_.post.taa_enabled != old_taa_enabled ||
        features_.post.taa_feedback != old_taa_feedback ||
        features_.post.taa_jitter != old_taa_jitter) {
        temporal_history_valid_ = false;
    }
    if (initialized_ && features_.post.color_lut_path != old_lut_path) {
        if (bgfx::isValid(color_lut_texture_) && color_lut_texture_.idx != neutral_lut_texture_.idx) {
            bgfx::destroy(color_lut_texture_);
        }
        color_lut_texture_ = loadColorLutTexture(features_.post.color_lut_path);
        color_lut_path_ = features_.post.color_lut_path;
        if (!bgfx::isValid(color_lut_texture_)) {
            color_lut_texture_ = neutral_lut_texture_;
            color_lut_size_ = 16.0f;
            color_lut_horizontal_ = 1.0f;
        }
    }
    const uint32_t new_reset = resetFlags();
    if (initialized_ && old_reset != new_reset) {
        bgfx::reset(width_, height_, new_reset);
        bgfx::setViewRect(kViewShadow, 0, 0, kShadowMapSize, kShadowMapSize);
        bgfx::setViewRect(kViewWorld, 0, 0, width_, height_);
        bgfx::setViewRect(kViewSceneNormal, 0, 0, width_, height_);
        bgfx::setViewRect(kViewReflectionMask, 0, 0, width_, height_);
        bgfx::setViewRect(kViewBloomMask, 0, 0, width_, height_);
        bgfx::setViewRect(kViewGtaoRaw, 0, 0, width_, height_);
        bgfx::setViewRect(kViewGtaoDenoise, 0, 0, width_, height_);
        for (size_t i = 0; i < kBloomMipCount; ++i) {
            const bgfx::ViewId down_view = i == 0
                ? kViewBloomExtract
                : static_cast<bgfx::ViewId>(kViewBloomDownFirst + i - 1);
            bgfx::setViewClear(down_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
            if (i + 1 < kBloomMipCount) {
                const bgfx::ViewId up_view = static_cast<bgfx::ViewId>(kViewBloomUpFirst + i);
                bgfx::setViewClear(up_view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
            }
        }
        bgfx::setViewRect(kViewPost, 0, 0, width_, height_);
        bgfx::setViewRect(kViewTemporalPost, 0, 0, width_, height_);
        for (uint8_t face = 0; face < 6; ++face) {
            bgfx::setViewRect(static_cast<bgfx::ViewId>(kViewProbeFirst + face), 0, 0, kGlobalProbeSize, kGlobalProbeSize);
        }
        destroySceneTarget();
        destroySceneNormalTarget();
        destroyTemporalHistoryTargets();
        destroyReflectionMaskTarget();
        destroyBloomMaskTarget();
        destroyGtaoTargets();
        destroyBloomChain();
        global_probe_capture_dirty_ = true;
    }
}

void BgfxRenderer::render(const OrbitCamera& camera) {
    if (!initialized_) return;

    const auto now = std::chrono::steady_clock::now();
    const float effect_time = std::chrono::duration<float>(now - start_time_).count();
    captureGlobalReflectionProbe(effect_time);
    const bool post_enabled =
        (features_.post.vignette_enabled && features_.post.vignette_strength > 0.0f) ||
        (features_.post.color_lut_enabled && features_.post.color_lut_intensity > 0.0f) ||
        (features_.post.film_grain_enabled && features_.post.film_grain_strength > 0.0f) ||
        (features_.post.bloom_enabled && features_.post.bloom_intensity > 0.0f) ||
        (features_.post.dof_enabled && features_.post.dof_aperture > 0.0f) ||
        features_.post.taa_enabled ||
        (features_.screen_space.ssr_enabled && features_.screen_space.ssr_strength > 0.0f) ||
        (features_.screen_space.gtao_enabled && features_.screen_space.gtao_intensity > 0.0f);
    const bool reflection_mask_enabled =
        features_.screen_space.ssr_enabled && features_.screen_space.ssr_strength > 0.0f;
    const bool scene_normal_enabled = usesDepthPostEffects(features_);
    const bool bloom_mask_enabled =
        features_.post.bloom_enabled && features_.post.bloom_intensity > 0.0f;
    const bool directional_shadows_enabled =
        features_.shadows.directional_shadows_enabled && ensureShadowTarget() && bgfx::isValid(shadow_depth_program_);
    Vec3 lu_light_vec = normalize(environment_.sun.direction);

    Vec3 bounds_min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 bounds_max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool has_bounds = false;
    for (const auto& mesh : meshes_) {
        if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
        bounds_min.x = std::min(bounds_min.x, mesh.bounds_min.x);
        bounds_min.y = std::min(bounds_min.y, mesh.bounds_min.y);
        bounds_min.z = std::min(bounds_min.z, mesh.bounds_min.z);
        bounds_max.x = std::max(bounds_max.x, mesh.bounds_max.x);
        bounds_max.y = std::max(bounds_max.y, mesh.bounds_max.y);
        bounds_max.z = std::max(bounds_max.z, mesh.bounds_max.z);
        has_bounds = true;
    }
    if (!has_bounds) {
        bounds_min = {-5.0f, -5.0f, -5.0f};
        bounds_max = {5.0f, 5.0f, 5.0f};
    }
    const Vec3 world_center = {
        (bounds_min.x + bounds_max.x) * 0.5f,
        (bounds_min.y + bounds_max.y) * 0.5f,
        (bounds_min.z + bounds_max.z) * 0.5f
    };
    const Vec3 world_extent = {
        (bounds_max.x - bounds_min.x) * 0.5f,
        (bounds_max.y - bounds_min.y) * 0.5f,
        (bounds_max.z - bounds_min.z) * 0.5f
    };
    const float world_radius = std::max(4.0f, length(world_extent) * 1.35f);

    float light_view[16];
    float light_proj[16];
    float shadow_matrix[16];
    const bx::Vec3 light_eye{
        world_center.x + lu_light_vec.x * world_radius * 2.5f,
        world_center.y + lu_light_vec.y * world_radius * 2.5f,
        world_center.z + lu_light_vec.z * world_radius * 2.5f
    };
    const bx::Vec3 light_at{world_center.x, world_center.y, world_center.z};
    const bx::Vec3 light_up = std::abs(lu_light_vec.y) > 0.92f ? bx::Vec3{0.0f, 0.0f, 1.0f} : bx::Vec3{0.0f, 1.0f, 0.0f};
    bx::mtxLookAt(light_view, light_eye, light_at, light_up);
    bx::mtxOrtho(light_proj, -world_radius, world_radius, -world_radius, world_radius, 0.1f, world_radius * 5.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(shadow_matrix, light_view, light_proj);

    if (post_enabled && ensureSceneTarget()) {
        bgfx::setViewFrameBuffer(kViewWorld, scene_framebuffer_);
    } else {
        bgfx::setViewFrameBuffer(kViewWorld, BGFX_INVALID_HANDLE);
    }
    if (post_enabled && reflection_mask_enabled && ensureReflectionMaskTarget()) {
        bgfx::setViewFrameBuffer(kViewReflectionMask, reflection_mask_framebuffer_);
    } else {
        bgfx::setViewFrameBuffer(kViewReflectionMask, BGFX_INVALID_HANDLE);
    }
    if (post_enabled && scene_normal_enabled && ensureSceneNormalTarget()) {
        bgfx::setViewFrameBuffer(kViewSceneNormal, scene_normal_framebuffer_);
    } else {
        bgfx::setViewFrameBuffer(kViewSceneNormal, BGFX_INVALID_HANDLE);
    }
    if (post_enabled && bloom_mask_enabled && ensureBloomMaskTarget()) {
        bgfx::setViewFrameBuffer(kViewBloomMask, bloom_mask_framebuffer_);
    } else {
        bgfx::setViewFrameBuffer(kViewBloomMask, BGFX_INVALID_HANDLE);
    }
    bgfx::setViewFrameBuffer(kViewGtaoRaw, BGFX_INVALID_HANDLE);
    bgfx::setViewFrameBuffer(kViewGtaoDenoise, BGFX_INVALID_HANDLE);
    if (directional_shadows_enabled) {
        bgfx::setViewFrameBuffer(kViewShadow, shadow_framebuffer_);
    } else {
        bgfx::setViewFrameBuffer(kViewShadow, BGFX_INVALID_HANDLE);
    }

    float aspect = height_ == 0 ? 1.0f : static_cast<float>(width_) / static_cast<float>(height_);
    Vec3 eye = camera.position();
    Vec3 target = camera.target();
    if (features_.post.taa_enabled) {
        const bool moved =
            !last_temporal_camera_valid_ ||
            camera.mode() != last_temporal_camera_mode_ ||
            length(eye - last_temporal_eye_) > 0.0005f ||
            length(target - last_temporal_target_) > 0.0005f ||
            std::abs(camera.yaw() - last_temporal_yaw_) > 0.00005f ||
            std::abs(camera.pitch() - last_temporal_pitch_) > 0.00005f ||
            std::abs(camera.distance() - last_temporal_distance_) > 0.0005f;
        if (moved) {
            temporal_history_valid_ = false;
        }
        last_temporal_camera_mode_ = camera.mode();
        last_temporal_eye_ = eye;
        last_temporal_target_ = target;
        last_temporal_yaw_ = camera.yaw();
        last_temporal_pitch_ = camera.pitch();
        last_temporal_distance_ = camera.distance();
        last_temporal_camera_valid_ = true;
    } else {
        last_temporal_camera_valid_ = false;
    }
    float view[16];
    float proj[16];
    bx::mtxLookAt(view, bx::Vec3{eye.x, eye.y, eye.z}, bx::Vec3{target.x, target.y, target.z});
    constexpr float kVerticalFovDegrees = 60.0f;
    const float near_clip = std::max(0.02f, camera.distance() * 0.005f);
    const float far_clip = std::max(100.0f, camera.distance() * 20.0f);
    bx::mtxProj(proj, kVerticalFovDegrees, aspect, near_clip, far_clip, bgfx::getCaps()->homogeneousDepth);
    if (features_.post.taa_enabled && features_.post.taa_jitter > 0.0f && width_ > 0 && height_ > 0) {
        const uint64_t jitter_index = (frame_index_ % 8u) + 1u;
        const float jitter_scale = std::max(0.0f, features_.post.taa_jitter);
        const float jitter_x = ((halton(jitter_index, 2u) - 0.5f) * 2.0f * jitter_scale) / static_cast<float>(width_);
        const float jitter_y = ((halton(jitter_index, 3u) - 0.5f) * 2.0f * jitter_scale) / static_cast<float>(height_);
        proj[8] += jitter_x;
        proj[9] += jitter_y;
    }
    bgfx::setViewTransform(kViewWorld, view, proj);
    bgfx::setViewTransform(kViewSceneNormal, view, proj);
    bgfx::setViewTransform(kViewReflectionMask, view, proj);
    bgfx::setViewTransform(kViewBloomMask, view, proj);
    bgfx::setViewTransform(kViewShadow, light_view, light_proj);
    bgfx::touch(kViewWorld);
    if (directional_shadows_enabled) {
        bgfx::touch(kViewShadow);
    }
    if (post_enabled && reflection_mask_enabled && bgfx::isValid(reflection_mask_framebuffer_)) {
        bgfx::touch(kViewReflectionMask);
    }
    if (post_enabled && scene_normal_enabled && bgfx::isValid(scene_normal_framebuffer_)) {
        bgfx::touch(kViewSceneNormal);
    }
    if (post_enabled && bloom_mask_enabled && bgfx::isValid(bloom_mask_framebuffer_)) {
        bgfx::touch(kViewBloomMask);
    }

    drawGrid();

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
    const float identity_mtx[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    const float shadow_params_base[4] = {
        directional_shadows_enabled ? 1.0f : 0.0f,
        std::max(0.0f, features_.shadows.pcss_light_radius),
        std::max(0.0f, features_.shadows.pcss_bias),
        1.0f / static_cast<float>(kShadowMapSize)
    };
    const float shadow_bias_params[4] = {
        std::max(0.0f, features_.shadows.pcss_bias),
        std::max(0.0f, features_.shadows.pcss_normal_bias),
        std::max(0.0f, features_.shadows.pcss_slope_bias),
        0.25f
    };
    const float shadow_light_dir[4] = {
        lu_light_vec.x,
        lu_light_vec.y,
        lu_light_vec.z,
        0.0f
    };

    if (directional_shadows_enabled) {
        for (const auto& mesh : meshes_) {
            if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
            if (!mesh.material.depth_write || mesh.material.alpha_mode == RenderAlphaMode::AlphaBlend ||
                mesh.material.alpha_mode == RenderAlphaMode::Additive) {
                continue;
            }

            bgfx::setTransform(identity_mtx);
            bgfx::setVertexBuffer(0, mesh.vertex_buffer);
            bgfx::setIndexBuffer(mesh.index_buffer);
            const float shadow_diffuse[4] = {
                mesh.material.diffuse.x,
                mesh.material.diffuse.y,
                mesh.material.diffuse.z,
                mesh.material.diffuse.w
            };
            const std::array<float, 4> shadow_shader_flags = shaderFlagsForMaterial(mesh.material);
            const float shadow_effect_time[4] = {
                effect_time,
                mesh.material.lu_shader_uses_uv_animation ? 1.0f : 0.0f,
                mesh.material.lu_shader_uses_alpha_animation ? 1.0f : 0.0f,
                1.0f
            };
            const float shadow_uv_motion1[4] = {
                mesh.material.lu_uv_motion_layer1.x,
                mesh.material.lu_uv_motion_layer1.y,
                0.0f,
                0.0f
            };
            bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
            bgfx::setUniform(u_material_diffuse_, shadow_diffuse);
            bgfx::setUniform(u_lu_shader_flags_, shadow_shader_flags.data());
            bgfx::setUniform(u_lu_effect_time_, shadow_effect_time);
            bgfx::setUniform(u_lu_uv_motion1_, shadow_uv_motion1);
            uint64_t shadow_state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
            if (mesh.material.cull_mode == RenderCullMode::Clockwise) {
                shadow_state |= BGFX_STATE_CULL_CW;
            } else if (mesh.material.cull_mode == RenderCullMode::Backface ||
                       mesh.material.cull_mode == RenderCullMode::CounterClockwise) {
                shadow_state |= BGFX_STATE_CULL_CCW;
            }
            bgfx::setState(shadow_state);
            bgfx::submit(kViewShadow, shadow_depth_program_);
        }
    }

    std::vector<const GpuMesh*> visible_meshes;
    visible_meshes.reserve(meshes_.size());
    for (const auto& mesh : meshes_) {
        if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
        if (mesh.has_lod_range) {
            const float lod_distance = length(eye - mesh.lod_center);
            if (lod_distance < mesh.lod_near || lod_distance >= mesh.lod_far) {
                continue;
            }
        }
        visible_meshes.push_back(&mesh);
    }
    const auto bounds_center = [](const GpuMesh& mesh) {
        return Vec3{
            (mesh.bounds_min.x + mesh.bounds_max.x) * 0.5f,
            (mesh.bounds_min.y + mesh.bounds_max.y) * 0.5f,
            (mesh.bounds_min.z + mesh.bounds_max.z) * 0.5f
        };
    };
    std::stable_sort(visible_meshes.begin(), visible_meshes.end(), [&](const GpuMesh* a, const GpuMesh* b) {
        const bool a_transparent = isTransparentForwardMaterial(a->material);
        const bool b_transparent = isTransparentForwardMaterial(b->material);
        if (a_transparent != b_transparent) {
            return !a_transparent;
        }
        if (!a_transparent) {
            return false;
        }

        const Vec3 a_center = bounds_center(*a);
        const Vec3 b_center = bounds_center(*b);
        return length(a_center - eye) > length(b_center - eye);
    });

    const size_t active_meshes = visible_meshes.size();
    for (const GpuMesh* mesh_ptr : visible_meshes) {
        const GpuMesh& mesh = *mesh_ptr;

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
        const bool lego_family = isLegoppFamily(mesh.material.shader_family);
        const bool use_global_probe =
            features_.reflection_probe.enabled &&
            lego_family &&
            bgfx::isValid(global_probe_texture_);
        const bgfx::TextureHandle lu_env_texture =
            use_global_probe ? global_probe_texture_ : mesh.reflection_texture;
        bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
        bgfx::setTexture(1, s_lu_env_, lu_env_texture);
        bgfx::setTexture(2, s_dark_, mesh.dark_texture, mesh.dark_texture_sampler_flags);
        bgfx::setTexture(3, s_shadow_map_, directional_shadows_enabled ? shadow_depth_texture_ : white_texture_);
        bgfx::setUniform(u_shadow_matrix_, shadow_matrix);
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
        const std::array<float, 4> shader_flags = shaderFlagsForMaterial(mesh.material);
        bgfx::setUniform(u_lu_shader_flags_, shader_flags.data());
        float variant_flags[4] = {
            static_cast<float>(mesh.material.legopp_variant),
            usesLowLegoppSource(mesh.material) ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_specular ? 1.0f : 0.0f,
            mesh.material.lu_shader_uses_reflection ? 1.0f : 0.0f
        };
        bgfx::setUniform(u_lu_variant_flags_, variant_flags);
        const bool pbr_lego =
            features_.lego_surface_model == SurfaceModel::PBR &&
            lego_family;
        float pbr_params[4] = {
            pbr_lego ? 1.0f : 0.0f,
            std::clamp(features_.pbr.roughness, 0.04f, 1.0f),
            std::clamp(features_.pbr.metallic, 0.0f, 1.0f),
            std::max(0.0f, features_.pbr.specular_intensity)
        };
        bgfx::setUniform(u_lu_pbr_params_, pbr_params);
        float reflection_params[4] = {
            use_global_probe ? 1.0f : 0.0f,
            use_global_probe ? std::max(0.0f, features_.reflection_probe.intensity) : 1.0f,
            0.0f,
            0.0f
        };
        bgfx::setUniform(u_lu_reflection_params_, reflection_params);
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
        float shadow_params[4] = {
            mesh.material.lu_shader_uses_shadow_terrain ? shadow_params_base[0] : 0.0f,
            shadow_params_base[1],
            shadow_params_base[2],
            shadow_params_base[3]
        };
        bgfx::setUniform(u_shadow_params_, shadow_params);
        bgfx::setUniform(u_shadow_bias_params_, shadow_bias_params);
        bgfx::setUniform(u_shadow_light_dir_, shadow_light_dir);
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

        const bool depth_prepass_material = participatesInDepthPrepass(mesh.material);

        if (post_enabled && scene_normal_enabled && bgfx::isValid(scene_normal_framebuffer_) &&
            bgfx::isValid(view_normal_program_) && depth_prepass_material) {
            bgfx::setTransform(identity_mtx);
            bgfx::setVertexBuffer(0, mesh.vertex_buffer);
            bgfx::setIndexBuffer(mesh.index_buffer);
            bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
            bgfx::setUniform(u_material_diffuse_, diffuse);
            bgfx::setUniform(u_lu_shader_flags_, shader_flags.data());
            bgfx::setUniform(u_lu_effect_time_, effect_time_uniform);
            bgfx::setUniform(u_lu_uv_motion1_, uv_motion1);
            uint64_t normal_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                                    BGFX_STATE_DEPTH_TEST_LESS;
            if (mesh.material.cull_mode == RenderCullMode::Clockwise) {
                normal_state |= BGFX_STATE_CULL_CW;
            } else if (mesh.material.cull_mode == RenderCullMode::Backface ||
                       mesh.material.cull_mode == RenderCullMode::CounterClockwise) {
                normal_state |= BGFX_STATE_CULL_CCW;
            }
            bgfx::setState(normal_state);
            bgfx::submit(kViewSceneNormal, view_normal_program_);
        }

        uint64_t mask_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                              BGFX_STATE_DEPTH_TEST_LESS;
        if (mesh.material.cull_mode == RenderCullMode::Clockwise) {
            mask_state |= BGFX_STATE_CULL_CW;
        } else if (mesh.material.cull_mode == RenderCullMode::Backface ||
                   mesh.material.cull_mode == RenderCullMode::CounterClockwise) {
            mask_state |= BGFX_STATE_CULL_CCW;
        }

        if (post_enabled && reflection_mask_enabled && bgfx::isValid(reflection_mask_framebuffer_) &&
            bgfx::isValid(reflection_mask_program_) && depth_prepass_material) {
            bgfx::setTransform(identity_mtx);
            bgfx::setVertexBuffer(0, mesh.vertex_buffer);
            bgfx::setIndexBuffer(mesh.index_buffer);
            bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
            bgfx::setUniform(u_material_diffuse_, diffuse);
            bgfx::setUniform(u_lu_shader_flags_, shader_flags.data());
            bgfx::setUniform(u_lu_effect_time_, effect_time_uniform);
            bgfx::setUniform(u_lu_uv_motion1_, uv_motion1);
            const float reflection_mask_value[4] = {
                reflectionMaskForMaterial(mesh.material),
                0.0f,
                0.0f,
                0.0f
            };
            bgfx::setUniform(u_reflection_mask_value_, reflection_mask_value);
            bgfx::setState(mask_state);
            bgfx::submit(kViewReflectionMask, reflection_mask_program_);
        }

        const float bloom_mask = bloomMaskForMaterial(mesh.material, effect_time, mesh.name, source_asset_path_);
        const bool transparent_bloom_material = isTransparentForwardMaterial(mesh.material) && bloom_mask > 0.0f;
        if (post_enabled && bloom_mask_enabled && bgfx::isValid(bloom_mask_framebuffer_) &&
            bgfx::isValid(reflection_mask_program_) && (depth_prepass_material || transparent_bloom_material)) {
            bgfx::setTransform(identity_mtx);
            bgfx::setVertexBuffer(0, mesh.vertex_buffer);
            bgfx::setIndexBuffer(mesh.index_buffer);
            bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
            bgfx::setUniform(u_material_diffuse_, diffuse);
            bgfx::setUniform(u_lu_shader_flags_, shader_flags.data());
            bgfx::setUniform(u_lu_effect_time_, effect_time_uniform);
            bgfx::setUniform(u_lu_uv_motion1_, uv_motion1);
            const float bloom_mask_value[4] = {
                bloom_mask,
                0.0f,
                0.0f,
                0.0f
            };
            bgfx::setUniform(u_reflection_mask_value_, bloom_mask_value);
            uint64_t bloom_state = mask_state;
            if (transparent_bloom_material && !depth_prepass_material) {
                bloom_state &= ~BGFX_STATE_WRITE_Z;
                bloom_state |= mesh.material.alpha_mode == RenderAlphaMode::Additive
                    ? BGFX_STATE_BLEND_ADD
                    : BGFX_STATE_BLEND_ALPHA;
            }
            bgfx::setState(bloom_state);
            bgfx::submit(kViewBloomMask, reflection_mask_program_);
        }
    }

    if (post_enabled && bgfx::isValid(scene_framebuffer_)) {
        bgfx::TextureHandle bloom_texture = BGFX_INVALID_HANDLE;
        if (bloom_mask_enabled && bgfx::isValid(bloom_mask_framebuffer_)) {
            bloom_texture = buildBloomPyramid();
        }
        const float tan_half_fov_y = std::tan((kVerticalFovDegrees * 3.14159265358979323846f / 180.0f) * 0.5f);
        bgfx::TextureHandle gtao_texture = BGFX_INVALID_HANDLE;
        if (features_.screen_space.gtao_enabled && features_.screen_space.gtao_intensity > 0.0f) {
            gtao_texture = buildGtaoTexture(effect_time, near_clip, far_clip, tan_half_fov_y * aspect, tan_half_fov_y);
        }
        drawPostProcess(effect_time, near_clip, far_clip, tan_half_fov_y * aspect, tan_half_fov_y, bloom_texture, gtao_texture);
    }

    bgfx::dbgTextClear();
    bgfx::dbgTextPrintf(0, 0, 0x0f, "LU Renderer - %s", bgfx::getRendererName(bgfx::getRendererType()));
    bgfx::dbgTextPrintf(0, 1, 0x0f, "Asset: %s", source_asset_path_.empty() ? "<debug>" : source_asset_path_.c_str());
    bgfx::dbgTextPrintf(0, 2, 0x0f, "Meshes: %zu/%zu active", active_meshes, meshes_.size());
    bgfx::dbgTextPrintf(0, 3, 0x0f, "MSAA: %s x%u | TAA %s %.2f | SSR %s %.2f | GTAO %s %.2f | Bloom %s %.2f | LUT %s %.2f | Shadows %s %.3f | PBR %s | Probe %s %.2f",
        features_.msaa.enabled ? "on" : "off",
        static_cast<unsigned>(features_.msaa.samples),
        features_.post.taa_enabled ? "on" : "off",
        features_.post.taa_feedback,
        features_.screen_space.ssr_enabled ? "on" : "off",
        features_.screen_space.ssr_strength,
        features_.screen_space.gtao_enabled ? "on" : "off",
        features_.screen_space.gtao_intensity,
        features_.post.bloom_enabled ? "on" : "off",
        features_.post.bloom_intensity,
        features_.post.color_lut_enabled ? "on" : "off",
        features_.post.color_lut_intensity,
        features_.shadows.directional_shadows_enabled ? "on" : "off",
        features_.shadows.pcss_light_radius,
        features_.lego_surface_model == SurfaceModel::PBR ? "on" : "off",
        features_.reflection_probe.enabled ? "on" : "off",
        features_.reflection_probe.intensity);
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
    ++frame_index_;
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

bgfx::TextureHandle BgfxRenderer::loadTexture(const std::string& path, uint32_t sampler_flags) {
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

bgfx::TextureHandle BgfxRenderer::loadColorLutTexture(const std::string& path) {
    if (path.empty()) return BGFX_INVALID_HANDLE;

    std::vector<uint8_t> data = readFile(path);
    if (data.empty()) return BGFX_INVALID_HANDLE;

    try {
        auto dds = lu::assets::dds_parse_header(std::span<const uint8_t>(data.data(), data.size()));
        bgfx::TextureFormat::Enum format = ddsTextureFormat(dds);
        if (format == bgfx::TextureFormat::Unknown || dds.data_offset >= data.size()) {
            return BGFX_INVALID_HANDLE;
        }

        float lut_size = 0.0f;
        float horizontal = 1.0f;
        if (dds.width == dds.height * dds.height) {
            lut_size = static_cast<float>(dds.height);
            horizontal = 1.0f;
        } else if (dds.height == dds.width * dds.width) {
            lut_size = static_cast<float>(dds.width);
            horizontal = 0.0f;
        } else {
            return BGFX_INVALID_HANDLE;
        }

        const uint8_t* pixels = data.data() + dds.data_offset;
        uint32_t pixel_size = static_cast<uint32_t>(data.size() - dds.data_offset);
        const uint64_t sampler_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle handle = bgfx::createTexture2D(
            static_cast<uint16_t>(dds.width),
            static_cast<uint16_t>(dds.height),
            dds.mip_count > 1,
            1,
            format,
            sampler_flags,
            bgfx::copy(pixels, pixel_size));
        if (bgfx::isValid(handle)) {
            color_lut_size_ = lut_size;
            color_lut_horizontal_ = horizontal;
        }
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

bgfx::TextureHandle BgfxRenderer::createNeutralColorLutTexture() {
    constexpr uint32_t kLutSize = 16;
    constexpr uint32_t kWidth = kLutSize * kLutSize;
    constexpr uint32_t kHeight = kLutSize;
    std::array<uint32_t, kWidth * kHeight> pixels{};
    for (uint32_t b = 0; b < kLutSize; ++b) {
        for (uint32_t g = 0; g < kLutSize; ++g) {
            for (uint32_t r = 0; r < kLutSize; ++r) {
                const uint32_t x = r + b * kLutSize;
                const uint32_t y = g;
                pixels[y * kWidth + x] = packRgba8({
                    static_cast<float>(r) / static_cast<float>(kLutSize - 1),
                    static_cast<float>(g) / static_cast<float>(kLutSize - 1),
                    static_cast<float>(b) / static_cast<float>(kLutSize - 1)
                });
            }
        }
    }
    color_lut_size_ = static_cast<float>(kLutSize);
    color_lut_horizontal_ = 1.0f;
    return bgfx::createTexture2D(
        static_cast<uint16_t>(kWidth),
        static_cast<uint16_t>(kHeight),
        false,
        1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(uint32_t))));
}

bgfx::TextureHandle BgfxRenderer::createSolidCubeTexture(uint32_t rgba) {
    std::array<uint32_t, 6> pixels = {rgba, rgba, rgba, rgba, rgba, rgba};
    return bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, 0,
        bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(uint32_t))));
}

bgfx::TextureHandle BgfxRenderer::createEnvironmentProbeTexture() const {
    Vec3 side = environment_.specular * 0.45f + environment_.ambient * 0.35f + environment_.fog_color * 0.20f;
    Vec3 sun = environment_.sun.color * environment_.sun.intensity * 0.65f + environment_.upper_hemi * 0.35f;
    Vec3 upper = environment_.upper_hemi * 0.70f + environment_.fog_color * 0.30f;
    Vec3 lower = environment_.lower_hemi * 0.55f + environment_.ambient * 0.45f;
    Vec3 fog = environment_.fog_enabled ? environment_.fog_color : side;
    std::array<uint32_t, 6> pixels = {
        packRgba8(side),
        packRgba8(side * 0.85f + environment_.ambient * 0.15f),
        packRgba8(upper),
        packRgba8(lower),
        packRgba8(sun),
        packRgba8(fog)
    };
    return bgfx::createTextureCube(1, false, 1, bgfx::TextureFormat::RGBA8, 0,
        bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(uint32_t))));
}

void BgfxRenderer::rebuildEnvironmentProbeTexture() {
    if (!initialized_) return;
    destroyCapturedProbeTarget();
    if (bgfx::isValid(global_probe_texture_)) {
        bgfx::destroy(global_probe_texture_);
        global_probe_texture_ = BGFX_INVALID_HANDLE;
    }
    global_probe_texture_ = createEnvironmentProbeTexture();
}

void BgfxRenderer::destroyCapturedProbeTarget() {
    for (auto& framebuffer : global_probe_framebuffers_) {
        if (bgfx::isValid(framebuffer)) {
            bgfx::destroy(framebuffer);
            framebuffer = BGFX_INVALID_HANDLE;
        }
    }
    for (auto& depth_texture : global_probe_depth_textures_) {
        if (bgfx::isValid(depth_texture)) {
            bgfx::destroy(depth_texture);
            depth_texture = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(global_probe_texture_)) {
        bgfx::destroy(global_probe_texture_);
        global_probe_texture_ = BGFX_INVALID_HANDLE;
    }
}

bool BgfxRenderer::ensureCapturedProbeTarget() {
    if (bgfx::isValid(global_probe_texture_)) {
        bool all_faces_valid = true;
        for (const auto& framebuffer : global_probe_framebuffers_) {
            all_faces_valid = all_faces_valid && bgfx::isValid(framebuffer);
        }
        for (const auto& depth_texture : global_probe_depth_textures_) {
            all_faces_valid = all_faces_valid && bgfx::isValid(depth_texture);
        }
        if (all_faces_valid) return true;
    }

    destroyCapturedProbeTarget();
    const uint64_t probe_target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    global_probe_texture_ = bgfx::createTextureCube(
        kGlobalProbeSize,
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        probe_target_flags);
    if (!bgfx::isValid(global_probe_texture_)) {
        rebuildEnvironmentProbeTexture();
        return false;
    }

    for (uint8_t face = 0; face < 6; ++face) {
        global_probe_depth_textures_[face] = bgfx::createTexture2D(
            kGlobalProbeSize,
            kGlobalProbeSize,
            false,
            1,
            bgfx::TextureFormat::D16,
            probe_target_flags);
        if (!bgfx::isValid(global_probe_depth_textures_[face])) {
            rebuildEnvironmentProbeTexture();
            return false;
        }

        bgfx::Attachment color_attachment;
        color_attachment.init(global_probe_texture_, bgfx::Access::Write, face, 1, 0, BGFX_RESOLVE_NONE);
        bgfx::Attachment depth_attachment;
        depth_attachment.init(global_probe_depth_textures_[face], bgfx::Access::Write, 0, 1, 0, BGFX_RESOLVE_NONE);
        bgfx::Attachment attachments[] = {color_attachment, depth_attachment};
        global_probe_framebuffers_[face] = bgfx::createFrameBuffer(2, attachments, false);
        if (!bgfx::isValid(global_probe_framebuffers_[face])) {
            rebuildEnvironmentProbeTexture();
            return false;
        }
    }
    global_probe_capture_dirty_ = true;
    return true;
}

void BgfxRenderer::captureGlobalReflectionProbe(float effect_time) {
    if (!features_.reflection_probe.enabled || !global_probe_capture_dirty_ || meshes_.empty()) return;
    if (!ensureCapturedProbeTarget()) return;

    Vec3 bounds_min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 bounds_max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool has_bounds = false;
    for (const auto& mesh : meshes_) {
        if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
        bounds_min.x = std::min(bounds_min.x, mesh.bounds_min.x);
        bounds_min.y = std::min(bounds_min.y, mesh.bounds_min.y);
        bounds_min.z = std::min(bounds_min.z, mesh.bounds_min.z);
        bounds_max.x = std::max(bounds_max.x, mesh.bounds_max.x);
        bounds_max.y = std::max(bounds_max.y, mesh.bounds_max.y);
        bounds_max.z = std::max(bounds_max.z, mesh.bounds_max.z);
        has_bounds = true;
    }
    if (!has_bounds) return;

    const Vec3 probe_pos = {
        (bounds_min.x + bounds_max.x) * 0.5f,
        (bounds_min.y + bounds_max.y) * 0.5f,
        (bounds_min.z + bounds_max.z) * 0.5f
    };
    const Vec3 extent = {
        (bounds_max.x - bounds_min.x) * 0.5f,
        (bounds_max.y - bounds_min.y) * 0.5f,
        (bounds_max.z - bounds_min.z) * 0.5f
    };
    const float far_clip = std::max(16.0f, length(extent) * 4.0f);
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
    float lu_light_dir_fade[4] = {lu_light_vec.x, lu_light_vec.y, lu_light_vec.z, 1.0f};
    float lu_light_color_shadow[4] = {light_color[0], light_color[1], light_color[2], 1.0f};
    float lu_ambient[4] = {environment_.ambient.x, environment_.ambient.y, environment_.ambient.z, light_ambient[3]};
    float upper_hemi[4] = {environment_.upper_hemi.x, environment_.upper_hemi.y, environment_.upper_hemi.z, 1.0f};
    float lower_hemi[4] = {environment_.lower_hemi.x, environment_.lower_hemi.y, environment_.lower_hemi.z, 1.0f};
    float specular[4] = {environment_.specular.x, environment_.specular.y, environment_.specular.z, 1.0f};
    float camera_pos[4] = {probe_pos.x, probe_pos.y, probe_pos.z, 1.0f};
    float fog_color[4] = {environment_.fog_color.x, environment_.fog_color.y, environment_.fog_color.z, 1.0f};
    float fog_params[4] = {
        environment_.fog_near,
        environment_.fog_far,
        environment_.fog_enabled && environment_.fog_far > environment_.fog_near ? 1.0f : 0.0f,
        0.0f
    };
    float disabled_shadow_matrix[16];
    bx::mtxIdentity(disabled_shadow_matrix);
    float shadow_params[4] = {
        0.0f,
        std::max(0.0f, features_.shadows.pcss_light_radius),
        std::max(0.0f, features_.shadows.pcss_bias),
        1.0f / static_cast<float>(kShadowMapSize)
    };
    float shadow_bias_params[4] = {
        std::max(0.0f, features_.shadows.pcss_bias),
        std::max(0.0f, features_.shadows.pcss_normal_bias),
        std::max(0.0f, features_.shadows.pcss_slope_bias),
        0.25f
    };
    float shadow_light_dir[4] = {lu_light_vec.x, lu_light_vec.y, lu_light_vec.z, 0.0f};
    float reflection_params[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float bbb_light_dir1[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float bbb_light_dir2[4] = {-1.0f, -1.0f, -1.0f, 0.0f};
    float bbb_light_color1[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    float bbb_light_color2[4] = {1.0f, 0.0f, 0.0f, 1.0f};

    static const bx::Vec3 kFaceDir[6] = {
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f}
    };
    static const bx::Vec3 kFaceUp[6] = {
        {0.0f, -1.0f,  0.0f}, {0.0f, -1.0f,  0.0f},
        {0.0f,  0.0f,  1.0f}, {0.0f,  0.0f, -1.0f},
        {0.0f, -1.0f,  0.0f}, {0.0f, -1.0f,  0.0f}
    };

    for (uint8_t face = 0; face < 6; ++face) {
        const bgfx::ViewId view_id = static_cast<bgfx::ViewId>(kViewProbeFirst + face);
        if (!bgfx::isValid(global_probe_framebuffers_[face])) continue;

        bgfx::setViewFrameBuffer(view_id, global_probe_framebuffers_[face]);
        bgfx::setViewRect(view_id, 0, 0, kGlobalProbeSize, kGlobalProbeSize);
        float view[16];
        float proj[16];
        const bx::Vec3 eye{probe_pos.x, probe_pos.y, probe_pos.z};
        const bx::Vec3 at{probe_pos.x + kFaceDir[face].x, probe_pos.y + kFaceDir[face].y, probe_pos.z + kFaceDir[face].z};
        bx::mtxLookAt(view, eye, at, kFaceUp[face]);
        bx::mtxProj(proj, 90.0f, 1.0f, 0.05f, far_clip, bgfx::getCaps()->homogeneousDepth);
        bgfx::setViewTransform(view_id, view, proj);
        bgfx::touch(view_id);

        for (const auto& mesh : meshes_) {
            if (!bgfx::isValid(mesh.vertex_buffer) || !bgfx::isValid(mesh.index_buffer)) continue;
            if (!participatesInDepthPrepass(mesh.material)) continue;

            float identity_mtx[16];
            bx::mtxIdentity(identity_mtx);
            bgfx::setTransform(identity_mtx);
            bgfx::setVertexBuffer(0, mesh.vertex_buffer);
            bgfx::setIndexBuffer(mesh.index_buffer);
            bgfx::setTexture(0, s_diffuse_, mesh.texture, mesh.texture_sampler_flags);
            bgfx::setTexture(1, s_lu_env_, bgfx::isValid(mesh.reflection_texture) ? mesh.reflection_texture : neutral_env_texture_);
            bgfx::setTexture(2, s_dark_, mesh.dark_texture, mesh.dark_texture_sampler_flags);
            bgfx::setTexture(3, s_shadow_map_, white_texture_);

            const std::array<float, 4> shader_flags = shaderFlagsForMaterial(mesh.material);
            float variant_flags[4] = {
                static_cast<float>(mesh.material.legopp_variant),
                usesLowLegoppSource(mesh.material) ? 1.0f : 0.0f,
                mesh.material.lu_shader_uses_specular ? 1.0f : 0.0f,
                mesh.material.lu_shader_uses_reflection ? 1.0f : 0.0f
            };
            const bool pbr_lego =
                features_.lego_surface_model == SurfaceModel::PBR &&
                isLegoppFamily(mesh.material.shader_family);
            float pbr_params[4] = {
                pbr_lego ? 1.0f : 0.0f,
                std::clamp(features_.pbr.roughness, 0.04f, 1.0f),
                std::clamp(features_.pbr.metallic, 0.0f, 1.0f),
                std::max(0.0f, features_.pbr.specular_intensity)
            };
            Vec3 animated_emissive = materialEmissiveAtTime(mesh.material, effect_time);
            float material_diffuse[4] = {mesh.material.diffuse.x, mesh.material.diffuse.y, mesh.material.diffuse.z, mesh.material.diffuse.w};
            float material_emissive[4] = {animated_emissive.x, animated_emissive.y, animated_emissive.z, 1.0f};
            float effect_time_uniform[4] = {
                effect_time,
                mesh.material.lu_shader_uses_uv_animation ? 1.0f : 0.0f,
                mesh.material.lu_shader_uses_alpha_animation ? 1.0f : 0.0f,
                1.0f
            };
            float uv_motion1[4] = {mesh.material.lu_uv_motion_layer1.x, mesh.material.lu_uv_motion_layer1.y, 0.0f, 0.0f};
            float uv_motion2[4] = {mesh.material.lu_uv_motion_layer2.x, mesh.material.lu_uv_motion_layer2.y, 0.0f, 0.0f};
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
            float shiny_glint[4] = {mesh.material.lu_shiny_glint_height, mesh.material.lu_shiny_glint_size_power, 0.0f, 0.0f};
            float shiny_glint_color[4] = {
                mesh.material.lu_shiny_glint_color.x,
                mesh.material.lu_shiny_glint_color.y,
                mesh.material.lu_shiny_glint_color.z,
                mesh.material.lu_shiny_glint_color.w
            };

            bgfx::setUniform(u_shadow_matrix_, disabled_shadow_matrix);
            bgfx::setUniform(u_shadow_params_, shadow_params);
            bgfx::setUniform(u_shadow_bias_params_, shadow_bias_params);
            bgfx::setUniform(u_shadow_light_dir_, shadow_light_dir);
            bgfx::setUniform(u_material_diffuse_, material_diffuse);
            bgfx::setUniform(u_material_emissive_, material_emissive);
            bgfx::setUniform(u_light_dir_ambient_, light_ambient);
            bgfx::setUniform(u_light_color_, light_color);
            bgfx::setUniform(u_lu_light_dir_fade_, lu_light_dir_fade);
            bgfx::setUniform(u_lu_light_color_shadow_, lu_light_color_shadow);
            bgfx::setUniform(u_lu_ambient_, lu_ambient);
            bgfx::setUniform(u_lu_upper_hemi_, upper_hemi);
            bgfx::setUniform(u_lu_lower_hemi_, lower_hemi);
            bgfx::setUniform(u_lu_specular_, specular);
            bgfx::setUniform(u_lu_camera_pos_, camera_pos);
            bgfx::setUniform(u_lu_fog_color_, fog_color);
            bgfx::setUniform(u_lu_fog_params_, fog_params);
            bgfx::setUniform(u_lu_shader_flags_, shader_flags.data());
            bgfx::setUniform(u_lu_variant_flags_, variant_flags);
            bgfx::setUniform(u_lu_pbr_params_, pbr_params);
            bgfx::setUniform(u_lu_reflection_params_, reflection_params);
            bgfx::setUniform(u_lu_effect_time_, effect_time_uniform);
            bgfx::setUniform(u_lu_uv_motion1_, uv_motion1);
            bgfx::setUniform(u_lu_uv_motion2_, uv_motion2);
            bgfx::setUniform(u_lu_effect_params_, effect_params);
            bgfx::setUniform(u_lu_glow_color_, glow_color);
            bgfx::setUniform(u_lu_shiny_glint_, shiny_glint);
            bgfx::setUniform(u_lu_shiny_glint_color_, shiny_glint_color);
            bgfx::setUniform(u_lu_bbb_light_dir1_, bbb_light_dir1);
            bgfx::setUniform(u_lu_bbb_light_dir2_, bbb_light_dir2);
            bgfx::setUniform(u_lu_bbb_light_color1_, bbb_light_color1);
            bgfx::setUniform(u_lu_bbb_light_color2_, bbb_light_color2);
            bgfx::setState(renderStateForMaterial(mesh.material));
            bgfx::submit(view_id, programForMaterial(mesh.material));
        }
    }

    global_probe_capture_dirty_ = false;
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
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS;
    if (!isTransparentForwardMaterial(material) && material.depth_write) {
        state |= BGFX_STATE_WRITE_Z;
    }

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

void BgfxRenderer::destroySceneTarget() {
    if (bgfx::isValid(scene_framebuffer_)) {
        bgfx::destroy(scene_framebuffer_);
    } else {
        if (bgfx::isValid(scene_color_texture_)) bgfx::destroy(scene_color_texture_);
        if (bgfx::isValid(scene_depth_texture_)) bgfx::destroy(scene_depth_texture_);
    }
    scene_framebuffer_ = BGFX_INVALID_HANDLE;
    scene_color_texture_ = BGFX_INVALID_HANDLE;
    scene_depth_texture_ = BGFX_INVALID_HANDLE;
    scene_target_width_ = 0;
    scene_target_height_ = 0;
    scene_target_msaa_flags_ = 0;
    scene_target_depth_sampleable_ = true;
}

bool BgfxRenderer::ensureSceneTarget() {
    if (width_ == 0 || height_ == 0) return false;
    const uint64_t msaa_flags = features_.msaa.enabled
        ? msaaRenderTargetFlags(features_.msaa.samples)
        : 0;
    const bool depth_sampleable = msaa_flags == 0;
    if (bgfx::isValid(scene_framebuffer_) &&
        scene_target_width_ == width_ &&
        scene_target_height_ == height_ &&
        scene_target_msaa_flags_ == msaa_flags &&
        scene_target_depth_sampleable_ == depth_sampleable) {
        return true;
    }

    destroySceneTarget();

    const uint64_t sampler_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    const uint64_t color_target_flags = (msaa_flags != 0 ? msaa_flags : BGFX_TEXTURE_RT) | sampler_flags;
    const uint64_t depth_target_flags = depth_sampleable
        ? (BGFX_TEXTURE_RT | sampler_flags)
        : (BGFX_TEXTURE_RT_WRITE_ONLY | msaa_flags);
    scene_color_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        color_target_flags);
    scene_depth_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::D24S8,
        depth_target_flags);

    if (!bgfx::isValid(scene_color_texture_) || !bgfx::isValid(scene_depth_texture_)) {
        destroySceneTarget();
        return false;
    }

    bgfx::TextureHandle targets[] = {scene_color_texture_, scene_depth_texture_};
    scene_framebuffer_ = bgfx::createFrameBuffer(2, targets, true);
    if (!bgfx::isValid(scene_framebuffer_)) {
        destroySceneTarget();
        return false;
    }

    scene_target_width_ = width_;
    scene_target_height_ = height_;
    scene_target_msaa_flags_ = msaa_flags;
    scene_target_depth_sampleable_ = depth_sampleable;
    return true;
}

void BgfxRenderer::destroyShadowTarget() {
    if (bgfx::isValid(shadow_framebuffer_)) {
        bgfx::destroy(shadow_framebuffer_);
    } else if (bgfx::isValid(shadow_depth_texture_)) {
        bgfx::destroy(shadow_depth_texture_);
    }
    shadow_framebuffer_ = BGFX_INVALID_HANDLE;
    shadow_depth_texture_ = BGFX_INVALID_HANDLE;
}

bool BgfxRenderer::ensureShadowTarget() {
    if (bgfx::isValid(shadow_framebuffer_)) return true;

    const uint64_t target_flags = BGFX_TEXTURE_RT |
                                  BGFX_SAMPLER_U_CLAMP |
                                  BGFX_SAMPLER_V_CLAMP |
                                  BGFX_SAMPLER_MIN_POINT |
                                  BGFX_SAMPLER_MAG_POINT |
                                  BGFX_SAMPLER_MIP_POINT;
    shadow_depth_texture_ = bgfx::createTexture2D(
        kShadowMapSize,
        kShadowMapSize,
        false,
        1,
        bgfx::TextureFormat::D16,
        target_flags);
    if (!bgfx::isValid(shadow_depth_texture_)) {
        destroyShadowTarget();
        return false;
    }

    shadow_framebuffer_ = bgfx::createFrameBuffer(1, &shadow_depth_texture_, true);
    if (!bgfx::isValid(shadow_framebuffer_)) {
        destroyShadowTarget();
        return false;
    }
    return true;
}

void BgfxRenderer::destroySceneNormalTarget() {
    if (bgfx::isValid(scene_normal_framebuffer_)) {
        bgfx::destroy(scene_normal_framebuffer_);
    } else {
        if (bgfx::isValid(scene_normal_texture_)) bgfx::destroy(scene_normal_texture_);
        if (bgfx::isValid(scene_normal_depth_texture_)) bgfx::destroy(scene_normal_depth_texture_);
    }
    scene_normal_framebuffer_ = BGFX_INVALID_HANDLE;
    scene_normal_texture_ = BGFX_INVALID_HANDLE;
    scene_normal_depth_texture_ = BGFX_INVALID_HANDLE;
    scene_normal_target_width_ = 0;
    scene_normal_target_height_ = 0;
}

bool BgfxRenderer::ensureSceneNormalTarget() {
    if (width_ == 0 || height_ == 0) return false;
    if (bgfx::isValid(scene_normal_framebuffer_) &&
        scene_normal_target_width_ == width_ &&
        scene_normal_target_height_ == height_) {
        return true;
    }

    destroySceneNormalTarget();

    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    scene_normal_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        target_flags);
    scene_normal_depth_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::D24S8,
        target_flags);

    if (!bgfx::isValid(scene_normal_texture_) || !bgfx::isValid(scene_normal_depth_texture_)) {
        destroySceneNormalTarget();
        return false;
    }

    bgfx::TextureHandle targets[] = {scene_normal_texture_, scene_normal_depth_texture_};
    scene_normal_framebuffer_ = bgfx::createFrameBuffer(2, targets, true);
    if (!bgfx::isValid(scene_normal_framebuffer_)) {
        destroySceneNormalTarget();
        return false;
    }

    scene_normal_target_width_ = width_;
    scene_normal_target_height_ = height_;
    return true;
}

void BgfxRenderer::destroyTemporalHistoryTargets() {
    for (auto& framebuffer : temporal_history_framebuffers_) {
        if (bgfx::isValid(framebuffer)) {
            bgfx::destroy(framebuffer);
            framebuffer = BGFX_INVALID_HANDLE;
        }
    }
    for (auto& texture : temporal_history_textures_) {
        texture = BGFX_INVALID_HANDLE;
    }
    temporal_history_width_ = 0;
    temporal_history_height_ = 0;
    temporal_history_index_ = 0;
    temporal_history_valid_ = false;
    last_temporal_camera_valid_ = false;
}

bool BgfxRenderer::ensureTemporalHistoryTargets() {
    if (width_ == 0 || height_ == 0) return false;
    if (temporal_history_width_ == width_ && temporal_history_height_ == height_) {
        bool valid = true;
        for (const auto& framebuffer : temporal_history_framebuffers_) {
            valid = valid && bgfx::isValid(framebuffer);
        }
        if (valid) return true;
    }

    destroyTemporalHistoryTargets();

    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    for (size_t i = 0; i < temporal_history_framebuffers_.size(); ++i) {
        temporal_history_textures_[i] = bgfx::createTexture2D(
            static_cast<uint16_t>(width_),
            static_cast<uint16_t>(height_),
            false,
            1,
            bgfx::TextureFormat::BGRA8,
            target_flags);
        if (!bgfx::isValid(temporal_history_textures_[i])) {
            destroyTemporalHistoryTargets();
            return false;
        }
        temporal_history_framebuffers_[i] = bgfx::createFrameBuffer(1, &temporal_history_textures_[i], true);
        if (!bgfx::isValid(temporal_history_framebuffers_[i])) {
            destroyTemporalHistoryTargets();
            return false;
        }
    }

    temporal_history_width_ = width_;
    temporal_history_height_ = height_;
    temporal_history_valid_ = false;
    return true;
}

void BgfxRenderer::destroyReflectionMaskTarget() {
    if (bgfx::isValid(reflection_mask_framebuffer_)) {
        bgfx::destroy(reflection_mask_framebuffer_);
    } else {
        if (bgfx::isValid(reflection_mask_texture_)) bgfx::destroy(reflection_mask_texture_);
        if (bgfx::isValid(reflection_mask_depth_texture_)) bgfx::destroy(reflection_mask_depth_texture_);
    }
    reflection_mask_framebuffer_ = BGFX_INVALID_HANDLE;
    reflection_mask_texture_ = BGFX_INVALID_HANDLE;
    reflection_mask_depth_texture_ = BGFX_INVALID_HANDLE;
    reflection_mask_target_width_ = 0;
    reflection_mask_target_height_ = 0;
}

bool BgfxRenderer::ensureReflectionMaskTarget() {
    if (width_ == 0 || height_ == 0) return false;
    if (bgfx::isValid(reflection_mask_framebuffer_) &&
        reflection_mask_target_width_ == width_ &&
        reflection_mask_target_height_ == height_) {
        return true;
    }

    destroyReflectionMaskTarget();

    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    reflection_mask_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::R8,
        target_flags);
    reflection_mask_depth_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::D24S8,
        target_flags);

    if (!bgfx::isValid(reflection_mask_texture_) || !bgfx::isValid(reflection_mask_depth_texture_)) {
        destroyReflectionMaskTarget();
        return false;
    }

    bgfx::TextureHandle targets[] = {reflection_mask_texture_, reflection_mask_depth_texture_};
    reflection_mask_framebuffer_ = bgfx::createFrameBuffer(2, targets, true);
    if (!bgfx::isValid(reflection_mask_framebuffer_)) {
        destroyReflectionMaskTarget();
        return false;
    }

    reflection_mask_target_width_ = width_;
    reflection_mask_target_height_ = height_;
    return true;
}

void BgfxRenderer::destroyBloomMaskTarget() {
    if (bgfx::isValid(bloom_mask_framebuffer_)) {
        bgfx::destroy(bloom_mask_framebuffer_);
    } else {
        if (bgfx::isValid(bloom_mask_texture_)) bgfx::destroy(bloom_mask_texture_);
        if (bgfx::isValid(bloom_mask_depth_texture_)) bgfx::destroy(bloom_mask_depth_texture_);
    }
    bloom_mask_framebuffer_ = BGFX_INVALID_HANDLE;
    bloom_mask_texture_ = BGFX_INVALID_HANDLE;
    bloom_mask_depth_texture_ = BGFX_INVALID_HANDLE;
    bloom_mask_target_width_ = 0;
    bloom_mask_target_height_ = 0;
}

bool BgfxRenderer::ensureBloomMaskTarget() {
    if (width_ == 0 || height_ == 0) return false;
    if (bgfx::isValid(bloom_mask_framebuffer_) &&
        bloom_mask_target_width_ == width_ &&
        bloom_mask_target_height_ == height_) {
        return true;
    }

    destroyBloomMaskTarget();

    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bloom_mask_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::R8,
        target_flags);
    bloom_mask_depth_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::D24S8,
        target_flags);

    if (!bgfx::isValid(bloom_mask_texture_) || !bgfx::isValid(bloom_mask_depth_texture_)) {
        destroyBloomMaskTarget();
        return false;
    }

    bgfx::TextureHandle targets[] = {bloom_mask_texture_, bloom_mask_depth_texture_};
    bloom_mask_framebuffer_ = bgfx::createFrameBuffer(2, targets, true);
    if (!bgfx::isValid(bloom_mask_framebuffer_)) {
        destroyBloomMaskTarget();
        return false;
    }

    bloom_mask_target_width_ = width_;
    bloom_mask_target_height_ = height_;
    return true;
}

void BgfxRenderer::destroyGtaoTargets() {
    if (bgfx::isValid(gtao_raw_framebuffer_)) {
        bgfx::destroy(gtao_raw_framebuffer_);
    } else if (bgfx::isValid(gtao_raw_texture_)) {
        bgfx::destroy(gtao_raw_texture_);
    }
    if (bgfx::isValid(gtao_denoised_framebuffer_)) {
        bgfx::destroy(gtao_denoised_framebuffer_);
    } else if (bgfx::isValid(gtao_denoised_texture_)) {
        bgfx::destroy(gtao_denoised_texture_);
    }
    gtao_raw_framebuffer_ = BGFX_INVALID_HANDLE;
    gtao_raw_texture_ = BGFX_INVALID_HANDLE;
    gtao_denoised_framebuffer_ = BGFX_INVALID_HANDLE;
    gtao_denoised_texture_ = BGFX_INVALID_HANDLE;
    gtao_target_width_ = 0;
    gtao_target_height_ = 0;
}

bool BgfxRenderer::ensureGtaoTargets() {
    if (width_ == 0 || height_ == 0) return false;
    if (bgfx::isValid(gtao_raw_framebuffer_) &&
        bgfx::isValid(gtao_denoised_framebuffer_) &&
        gtao_target_width_ == width_ &&
        gtao_target_height_ == height_) {
        return true;
    }

    destroyGtaoTargets();

    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    gtao_raw_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::R8,
        target_flags);
    gtao_denoised_texture_ = bgfx::createTexture2D(
        static_cast<uint16_t>(width_),
        static_cast<uint16_t>(height_),
        false,
        1,
        bgfx::TextureFormat::R8,
        target_flags);

    if (!bgfx::isValid(gtao_raw_texture_) || !bgfx::isValid(gtao_denoised_texture_)) {
        destroyGtaoTargets();
        return false;
    }

    gtao_raw_framebuffer_ = bgfx::createFrameBuffer(1, &gtao_raw_texture_, true);
    gtao_denoised_framebuffer_ = bgfx::createFrameBuffer(1, &gtao_denoised_texture_, true);
    if (!bgfx::isValid(gtao_raw_framebuffer_) || !bgfx::isValid(gtao_denoised_framebuffer_)) {
        destroyGtaoTargets();
        return false;
    }

    gtao_target_width_ = width_;
    gtao_target_height_ = height_;
    return true;
}

bgfx::TextureHandle BgfxRenderer::buildGtaoTexture(
    float effect_time,
    float near_clip,
    float far_clip,
    float tan_half_fov_x,
    float tan_half_fov_y) {
    if (!bgfx::isValid(gtao_program_) ||
        !bgfx::isValid(gtao_denoise_program_) ||
        !bgfx::isValid(scene_normal_framebuffer_) ||
        !ensureGtaoTargets()) {
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::TextureHandle scene_depth = bgfx::getTexture(scene_normal_framebuffer_, 1);
    const bgfx::TextureHandle scene_normal = bgfx::getTexture(scene_normal_framebuffer_, 0);
    if (!bgfx::isValid(scene_depth) || !bgfx::isValid(scene_normal)) return BGFX_INVALID_HANDLE;

    const float screen_params[4] = {
        static_cast<float>(std::max<uint32_t>(width_, 1)),
        static_cast<float>(std::max<uint32_t>(height_, 1)),
        1.0f / static_cast<float>(std::max<uint32_t>(width_, 1)),
        1.0f / static_cast<float>(std::max<uint32_t>(height_, 1))
    };
    const float screen_space_params[4] = {
        features_.screen_space.gtao_enabled ? std::max(0.0f, features_.screen_space.gtao_intensity) : 0.0f,
        std::max(0.1f, features_.screen_space.gtao_radius),
        std::max(0.1f, features_.screen_space.ssr_max_distance),
        features_.screen_space.ssr_enabled ? std::max(0.0f, features_.screen_space.ssr_strength) : 0.0f
    };
    const float depth_linearize_mul = (far_clip * near_clip) / std::max(0.0001f, far_clip - near_clip);
    const float depth_linearize_add = far_clip / std::max(0.0001f, far_clip - near_clip);
    const float depth_params[4] = {
        depth_linearize_mul,
        depth_linearize_add,
        tan_half_fov_x,
        tan_half_fov_y
    };
    const float temporal_params[4] = {
        0.0f,
        0.0f,
        0.0f,
        static_cast<float>(frame_index_ & 1023u) + effect_time * 0.0f
    };

    bgfx::TransientVertexBuffer tvb;
    constexpr uint32_t vertex_count = 3;
    if (bgfx::getAvailTransientVertexBuffer(vertex_count, PostVertex::layout) < vertex_count) return BGFX_INVALID_HANDLE;
    bgfx::allocTransientVertexBuffer(&tvb, vertex_count, PostVertex::layout);
    auto* verts = reinterpret_cast<PostVertex*>(tvb.data);
    verts[0] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
    verts[1] = { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f};
    verts[2] = {-1.0f,  3.0f, 0.0f, 0.0f,-1.0f};

    bgfx::setViewFrameBuffer(kViewGtaoRaw, gtao_raw_framebuffer_);
    bgfx::setViewRect(kViewGtaoRaw, 0, 0, width_, height_);
    bgfx::touch(kViewGtaoRaw);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setTexture(0, s_scene_depth_, scene_depth);
    bgfx::setTexture(1, s_scene_normal_, scene_normal);
    bgfx::setUniform(u_screen_params_, screen_params);
    bgfx::setUniform(u_screen_space_params_, screen_space_params);
    bgfx::setUniform(u_depth_params_, depth_params);
    bgfx::setUniform(u_temporal_params_, temporal_params);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(kViewGtaoRaw, gtao_program_);

    if (bgfx::getAvailTransientVertexBuffer(vertex_count, PostVertex::layout) < vertex_count) return gtao_raw_texture_;
    bgfx::allocTransientVertexBuffer(&tvb, vertex_count, PostVertex::layout);
    verts = reinterpret_cast<PostVertex*>(tvb.data);
    verts[0] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
    verts[1] = { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f};
    verts[2] = {-1.0f,  3.0f, 0.0f, 0.0f,-1.0f};

    bgfx::setViewFrameBuffer(kViewGtaoDenoise, gtao_denoised_framebuffer_);
    bgfx::setViewRect(kViewGtaoDenoise, 0, 0, width_, height_);
    bgfx::touch(kViewGtaoDenoise);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setTexture(0, s_gtao_, gtao_raw_texture_);
    bgfx::setTexture(1, s_scene_depth_, scene_depth);
    bgfx::setUniform(u_screen_params_, screen_params);
    bgfx::setUniform(u_depth_params_, depth_params);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(kViewGtaoDenoise, gtao_denoise_program_);

    return gtao_denoised_texture_;
}

void BgfxRenderer::destroyBloomChain() {
    for (auto& framebuffer : bloom_framebuffers_) {
        if (bgfx::isValid(framebuffer)) {
            bgfx::destroy(framebuffer);
            framebuffer = BGFX_INVALID_HANDLE;
        }
    }
    for (auto& texture : bloom_textures_) {
        texture = BGFX_INVALID_HANDLE;
    }
    bloom_widths_.fill(0);
    bloom_heights_.fill(0);
    bloom_chain_target_width_ = 0;
    bloom_chain_target_height_ = 0;
}

bool BgfxRenderer::ensureBloomChain() {
    if (width_ == 0 || height_ == 0) return false;
    if (bloom_chain_target_width_ == width_ && bloom_chain_target_height_ == height_) {
        bool valid = true;
        for (const auto& framebuffer : bloom_framebuffers_) {
            valid = valid && bgfx::isValid(framebuffer);
        }
        if (valid) return true;
    }

    destroyBloomChain();

    uint32_t mip_width = std::max<uint32_t>(1, width_ / 2);
    uint32_t mip_height = std::max<uint32_t>(1, height_ / 2);
    const uint64_t target_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    for (size_t i = 0; i < kBloomMipCount; ++i) {
        bloom_widths_[i] = static_cast<uint16_t>(std::min<uint32_t>(mip_width, UINT16_MAX));
        bloom_heights_[i] = static_cast<uint16_t>(std::min<uint32_t>(mip_height, UINT16_MAX));
        bloom_textures_[i] = bgfx::createTexture2D(
            bloom_widths_[i],
            bloom_heights_[i],
            false,
            1,
            bgfx::TextureFormat::BGRA8,
            target_flags);
        if (!bgfx::isValid(bloom_textures_[i])) {
            destroyBloomChain();
            return false;
        }
        bloom_framebuffers_[i] = bgfx::createFrameBuffer(1, &bloom_textures_[i], true);
        if (!bgfx::isValid(bloom_framebuffers_[i])) {
            destroyBloomChain();
            return false;
        }
        mip_width = std::max<uint32_t>(1, mip_width / 2);
        mip_height = std::max<uint32_t>(1, mip_height / 2);
    }

    bloom_chain_target_width_ = width_;
    bloom_chain_target_height_ = height_;
    return true;
}

bgfx::TextureHandle BgfxRenderer::buildBloomPyramid() {
    if (!bgfx::isValid(bloom_program_) ||
        !bgfx::isValid(scene_framebuffer_) ||
        !bgfx::isValid(bloom_mask_framebuffer_) ||
        !ensureBloomChain()) {
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::TextureHandle scene_color = bgfx::getTexture(scene_framebuffer_, 0);
    const bgfx::TextureHandle bloom_mask = bgfx::getTexture(bloom_mask_framebuffer_, 0);
    if (!bgfx::isValid(scene_color) || !bgfx::isValid(bloom_mask)) return BGFX_INVALID_HANDLE;

    auto submitPass = [&](bgfx::ViewId view_id,
                          bgfx::FrameBufferHandle framebuffer,
                          uint16_t target_width,
                          uint16_t target_height,
                          bgfx::TextureHandle source,
                          bgfx::TextureHandle mask,
                          const float params[4],
                          uint64_t state) {
        bgfx::TransientVertexBuffer tvb;
        constexpr uint32_t vertex_count = 3;
        if (bgfx::getAvailTransientVertexBuffer(vertex_count, PostVertex::layout) < vertex_count) return false;
        bgfx::allocTransientVertexBuffer(&tvb, vertex_count, PostVertex::layout);

        auto* verts = reinterpret_cast<PostVertex*>(tvb.data);
        verts[0] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
        verts[1] = { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f};
        verts[2] = {-1.0f,  3.0f, 0.0f, 0.0f,-1.0f};

        bgfx::setViewFrameBuffer(view_id, framebuffer);
        bgfx::setViewRect(view_id, 0, 0, target_width, target_height);
        bgfx::touch(view_id);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setTexture(0, s_scene_color_, source);
        bgfx::setTexture(1, s_bloom_mask_, bgfx::isValid(mask) ? mask : black_texture_);
        bgfx::setUniform(u_bloom_params_, params);
        bgfx::setState(state);
        bgfx::submit(view_id, bloom_program_);
        return true;
    };

    const float threshold = std::clamp(features_.post.bloom_threshold, 0.0f, 4.0f);
    float params[4] = {
        0.0f,
        threshold,
        1.0f / static_cast<float>(std::max<uint32_t>(width_, 1)),
        1.0f / static_cast<float>(std::max<uint32_t>(height_, 1))
    };
    const uint64_t opaque_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    if (!submitPass(
            kViewBloomExtract,
            bloom_framebuffers_[0],
            bloom_widths_[0],
            bloom_heights_[0],
            scene_color,
            bloom_mask,
            params,
            opaque_state)) {
        return BGFX_INVALID_HANDLE;
    }

    for (size_t i = 1; i < kBloomMipCount; ++i) {
        params[0] = 1.0f;
        params[2] = 1.0f / static_cast<float>(std::max<uint16_t>(bloom_widths_[i - 1], 1));
        params[3] = 1.0f / static_cast<float>(std::max<uint16_t>(bloom_heights_[i - 1], 1));
        const bgfx::ViewId view_id = static_cast<bgfx::ViewId>(kViewBloomDownFirst + i - 1);
        if (!submitPass(
                view_id,
                bloom_framebuffers_[i],
                bloom_widths_[i],
                bloom_heights_[i],
                bloom_textures_[i - 1],
                black_texture_,
                params,
                opaque_state)) {
            return BGFX_INVALID_HANDLE;
        }
    }

    const uint64_t additive_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ADD;
    for (size_t source_level = kBloomMipCount - 1; source_level > 0; --source_level) {
        const size_t target_level = source_level - 1;
        params[0] = 2.0f;
        params[2] = 1.0f / static_cast<float>(std::max<uint16_t>(bloom_widths_[source_level], 1));
        params[3] = 1.0f / static_cast<float>(std::max<uint16_t>(bloom_heights_[source_level], 1));
        const bgfx::ViewId view_id = static_cast<bgfx::ViewId>(
            kViewBloomUpFirst + (kBloomMipCount - 1 - source_level));
        if (!submitPass(
                view_id,
                bloom_framebuffers_[target_level],
                bloom_widths_[target_level],
                bloom_heights_[target_level],
                bloom_textures_[source_level],
                black_texture_,
                params,
                additive_state)) {
            return BGFX_INVALID_HANDLE;
        }
    }

    return bloom_textures_[0];
}

void BgfxRenderer::drawFullscreenCopy(bgfx::TextureHandle texture) {
    if (!bgfx::isValid(fullscreen_copy_program_) || !bgfx::isValid(texture)) return;

    bgfx::TransientVertexBuffer tvb;
    constexpr uint32_t vertex_count = 3;
    if (bgfx::getAvailTransientVertexBuffer(vertex_count, PostVertex::layout) < vertex_count) return;
    bgfx::allocTransientVertexBuffer(&tvb, vertex_count, PostVertex::layout);

    auto* verts = reinterpret_cast<PostVertex*>(tvb.data);
    verts[0] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
    verts[1] = { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f};
    verts[2] = {-1.0f,  3.0f, 0.0f, 0.0f,-1.0f};

    bgfx::setViewFrameBuffer(kViewPost, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(kViewPost, 0, 0, width_, height_);
    bgfx::touch(kViewPost);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setTexture(0, s_scene_color_, texture);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(kViewPost, fullscreen_copy_program_);
}

void BgfxRenderer::drawPostProcess(
    float effect_time,
    float near_clip,
    float far_clip,
    float tan_half_fov_x,
    float tan_half_fov_y,
    bgfx::TextureHandle bloom_texture,
    bgfx::TextureHandle gtao_texture) {
    if (!bgfx::isValid(post_process_program_) || !bgfx::isValid(scene_framebuffer_)) return;
    const bgfx::TextureHandle scene_color = bgfx::getTexture(scene_framebuffer_, 0);
    const bgfx::TextureHandle scene_depth = bgfx::getTexture(scene_framebuffer_, 1);
    if (!bgfx::isValid(scene_color)) return;
    const bool depth_post_effects = usesDepthPostEffects(features_);
    bgfx::TextureHandle post_depth = scene_depth;
    if (depth_post_effects && bgfx::isValid(scene_normal_framebuffer_)) {
        post_depth = bgfx::getTexture(scene_normal_framebuffer_, 1);
    }
    if (depth_post_effects && !bgfx::isValid(post_depth)) return;
    if (!bgfx::isValid(post_depth)) {
        post_depth = white_texture_;
    }
    bgfx::TextureHandle reflection_mask = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(reflection_mask_framebuffer_)) {
        reflection_mask = bgfx::getTexture(reflection_mask_framebuffer_, 0);
    }
    bgfx::TextureHandle scene_normal = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(scene_normal_framebuffer_)) {
        scene_normal = bgfx::getTexture(scene_normal_framebuffer_, 0);
    }

    bgfx::TransientVertexBuffer tvb;
    constexpr uint32_t vertex_count = 3;
    if (bgfx::getAvailTransientVertexBuffer(vertex_count, PostVertex::layout) < vertex_count) return;
    bgfx::allocTransientVertexBuffer(&tvb, vertex_count, PostVertex::layout);

    auto* verts = reinterpret_cast<PostVertex*>(tvb.data);
    verts[0] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f};
    verts[1] = { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f};
    verts[2] = {-1.0f,  3.0f, 0.0f, 0.0f,-1.0f};

    const float post_params[4] = {
        features_.post.vignette_enabled ? std::max(0.0f, features_.post.vignette_strength) : 0.0f,
        features_.post.film_grain_enabled ? std::max(0.0f, features_.post.film_grain_strength) : 0.0f,
        effect_time,
        std::max(0.001f, features_.screen_space.ssr_thickness)
    };
    const float bloom_params[4] = {
        features_.post.bloom_enabled ? std::max(0.0f, features_.post.bloom_intensity) : 0.0f,
        std::clamp(features_.post.bloom_threshold, 0.0f, 4.0f),
        1.0f,
        0.0f
    };
    const float dof_aperture = features_.post.dof_enabled ? std::max(0.0f, features_.post.dof_aperture) : 0.0f;
    const float dof_params[4] = {
        dof_aperture,
        std::max(0.01f, features_.post.dof_focus_distance),
        std::clamp(dof_aperture * 96.0f, 0.0f, 28.0f),
        0.0f
    };
    const float color_lut_params[4] = {
        features_.post.color_lut_enabled ? std::max(0.0f, features_.post.color_lut_intensity) : 0.0f,
        std::max(2.0f, color_lut_size_),
        color_lut_horizontal_,
        0.0f
    };
    const float screen_params[4] = {
        static_cast<float>(std::max<uint32_t>(width_, 1)),
        static_cast<float>(std::max<uint32_t>(height_, 1)),
        1.0f / static_cast<float>(std::max<uint32_t>(width_, 1)),
        1.0f / static_cast<float>(std::max<uint32_t>(height_, 1))
    };
    const float screen_space_params[4] = {
        features_.screen_space.gtao_enabled ? std::max(0.0f, features_.screen_space.gtao_intensity) : 0.0f,
        std::max(0.1f, features_.screen_space.gtao_radius),
        std::max(0.1f, features_.screen_space.ssr_max_distance),
        features_.screen_space.ssr_enabled ? std::max(0.0f, features_.screen_space.ssr_strength) : 0.0f
    };
    const float depth_linearize_mul = (far_clip * near_clip) / std::max(0.0001f, far_clip - near_clip);
    const float depth_linearize_add = far_clip / std::max(0.0001f, far_clip - near_clip);
    const float depth_params[4] = {
        depth_linearize_mul,
        depth_linearize_add,
        tan_half_fov_x,
        tan_half_fov_y
    };
    const bool temporal_enabled =
        features_.post.taa_enabled &&
        features_.post.taa_feedback > 0.0f &&
        ensureTemporalHistoryTargets();
    const uint8_t history_read_index = temporal_history_index_;
    const uint8_t history_write_index = static_cast<uint8_t>(1u - temporal_history_index_);
    bgfx::TextureHandle history_read = scene_color;
    bgfx::FrameBufferHandle history_write_framebuffer = BGFX_INVALID_HANDLE;
    if (temporal_enabled) {
        history_write_framebuffer = temporal_history_framebuffers_[history_write_index];
        if (temporal_history_valid_ && bgfx::isValid(temporal_history_framebuffers_[history_read_index])) {
            history_read = bgfx::getTexture(temporal_history_framebuffers_[history_read_index], 0);
        }
    }
    const float temporal_params[4] = {
        temporal_enabled ? std::clamp(features_.post.taa_feedback, 0.0f, 0.98f) : 0.0f,
        temporal_enabled ? std::max(0.0f, features_.post.taa_jitter) : 0.0f,
        temporal_enabled && temporal_history_valid_ ? 1.0f : 0.0f,
        static_cast<float>(frame_index_ & 1023u)
    };

    const bgfx::ViewId post_view = temporal_enabled ? kViewTemporalPost : kViewPost;
    const bgfx::FrameBufferHandle post_framebuffer = temporal_enabled ? history_write_framebuffer : bgfx::FrameBufferHandle{bgfx::kInvalidHandle};
    bgfx::setViewFrameBuffer(post_view, post_framebuffer);
    bgfx::setViewRect(post_view, 0, 0, width_, height_);
    bgfx::touch(post_view);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setTexture(0, s_scene_color_, scene_color);
    bgfx::setTexture(1, s_scene_depth_, post_depth);
    if (bgfx::isValid(reflection_mask)) {
        bgfx::setTexture(2, s_reflection_mask_, reflection_mask);
    } else {
        bgfx::setTexture(2, s_reflection_mask_, white_texture_);
    }
    bgfx::setTexture(3, s_color_lut_, bgfx::isValid(color_lut_texture_) ? color_lut_texture_ : neutral_lut_texture_);
    bgfx::setTexture(4, s_bloom_mask_, bgfx::isValid(bloom_texture) ? bloom_texture : black_texture_);
    bgfx::setTexture(5, s_scene_normal_, bgfx::isValid(scene_normal) ? scene_normal : flat_normal_texture_);
    bgfx::setTexture(6, s_history_color_, bgfx::isValid(history_read) ? history_read : scene_color);
    bgfx::setTexture(7, s_gtao_, bgfx::isValid(gtao_texture) ? gtao_texture : white_texture_);
    bgfx::setUniform(u_post_params_, post_params);
    bgfx::setUniform(u_bloom_params_, bloom_params);
    bgfx::setUniform(u_dof_params_, dof_params);
    bgfx::setUniform(u_color_lut_params_, color_lut_params);
    bgfx::setUniform(u_screen_params_, screen_params);
    bgfx::setUniform(u_screen_space_params_, screen_space_params);
    bgfx::setUniform(u_depth_params_, depth_params);
    bgfx::setUniform(u_temporal_params_, temporal_params);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(post_view, post_process_program_);

    if (temporal_enabled) {
        const bgfx::TextureHandle history_write = bgfx::getTexture(history_write_framebuffer, 0);
        drawFullscreenCopy(history_write);
        temporal_history_index_ = history_write_index;
        temporal_history_valid_ = true;
    } else {
        temporal_history_valid_ = false;
    }
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
    uint64_t grid_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS |
                          BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA;
    if (features_.msaa.enabled) {
        grid_state |= BGFX_STATE_MSAA;
    }
    bgfx::setState(grid_state);
    bgfx::submit(kViewWorld, legacy_program_);
}

} // namespace lu::renderer::bgfx_backend
