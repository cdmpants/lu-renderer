#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif

#include "lu/renderer/camera.h"
#include "lu/renderer/lu_import/animation_importer.h"
#include "lu/renderer/lu_import/lvl_environment_importer.h"
#include "lu/renderer/lu_import/nif_importer.h"
#include "lu/renderer/render_types.h"
#include "lu/renderer_bgfx/bgfx_renderer.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(_WIN32)
#include <commdlg.h>
#include <commctrl.h>
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using lu::renderer::MeshAsset;
using lu::renderer::AnimationAsset;
using lu::renderer::CameraMode;
using lu::renderer::EnvironmentState;
using lu::renderer::LegacyShaderFamily;
using lu::renderer::OrbitCamera;
using lu::renderer::RenderAlphaMode;
using lu::renderer::RenderCullMode;
using lu::renderer::RenderFeatureSettings;
using lu::renderer::RenderWorld;
using lu::renderer::SurfaceModel;
using lu::renderer::Vec3;
using lu::renderer::Vertex;
using lu::renderer::bgfx_backend::BgfxRenderer;
using lu::renderer::bgfx_backend::RendererInit;

namespace {

#ifndef LU_RENDERER_BUILD_CONFIG
#define LU_RENDERER_BUILD_CONFIG "Unknown"
#endif

#ifndef LU_RENDERER_GIT_COMMIT
#define LU_RENDERER_GIT_COMMIT "unknown"
#endif

constexpr std::string_view kBuildConfig = LU_RENDERER_BUILD_CONFIG;
constexpr std::string_view kGitCommit = LU_RENDERER_GIT_COMMIT;
constexpr std::string_view kCanonicalBuildConfig = "RelWithDebInfo";

std::string viewerBuildIdentity() {
    std::string identity;
    if (kBuildConfig == kCanonicalBuildConfig) {
        identity = "Canonical";
    } else {
        identity = "Developer ";
        identity += kBuildConfig;
        identity += " build";
    }
    identity += " | ";
    identity += kGitCommit;
    return identity;
}

struct Args {
    std::filesystem::path client_root;
    std::filesystem::path lvl_path;
    std::filesystem::path animation_path;
    std::filesystem::path nif_path;
    std::filesystem::path screenshot_path;
    RenderFeatureSettings features;
    std::optional<float> camera_distance;
    std::optional<Vec3> camera_target;
    uint32_t exit_after_frames = 0;
    bool hidden = false;
    bool transparent_test_scene = false;
    bool shadow_test_scene = false;
    bool bgfx_device_debug = false;
};

struct InputState {
    bool left_down = false;
    bool middle_down = false;
    bool right_down = false;
    double last_x = 0.0;
    double last_y = 0.0;
    OrbitCamera* camera = nullptr;
};

struct AppState {
    Args* args = nullptr;
    BgfxRenderer* renderer = nullptr;
    RenderWorld* world = nullptr;
    OrbitCamera* camera = nullptr;
    RenderFeatureSettings features;
    InputState input;
    std::filesystem::path pending_import;
    std::filesystem::path pending_lvl_import;
    std::filesystem::path pending_animation_import;
    bool import_requested = false;
    bool lvl_import_requested = false;
    bool animation_import_requested = false;
    bool manual_environment_override = false;
#if defined(_WIN32)
    WNDPROC original_wnd_proc = nullptr;
    HMENU view_menu = nullptr;
    HWND lighting_window = nullptr;
#endif
};

Args parseArgs(int argc, char** argv) {
    Args args;
    if (const char* env = std::getenv("LU_CLIENT_ROOT")) {
        args.client_root = env;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--client-root" && i + 1 < argc) {
            args.client_root = argv[++i];
        } else if (arg == "--lvl" && i + 1 < argc) {
            args.lvl_path = argv[++i];
        } else if ((arg == "--kfm" || arg == "--kf" || arg == "--anim" || arg == "--animation") && i + 1 < argc) {
            args.animation_path = argv[++i];
        } else if (arg == "--nif" && i + 1 < argc) {
            args.nif_path = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            args.screenshot_path = argv[++i];
        } else if (arg == "--msaa" && i + 1 < argc) {
            args.features.msaa.enabled = true;
            args.features.msaa.samples = static_cast<uint8_t>(std::clamp(std::strtoul(argv[++i], nullptr, 10), 1ul, 16ul));
        } else if (arg == "--no-msaa") {
            args.features.msaa.enabled = false;
        } else if (arg == "--pbr") {
            args.features.lego_surface_model = SurfaceModel::PBR;
        } else if (arg == "--no-pbr") {
            args.features.lego_surface_model = SurfaceModel::LegacyLU;
        } else if (arg == "--pbr-roughness" && i + 1 < argc) {
            args.features.lego_surface_model = SurfaceModel::PBR;
            args.features.pbr.roughness = std::strtof(argv[++i], nullptr);
        } else if (arg == "--pbr-metallic" && i + 1 < argc) {
            args.features.lego_surface_model = SurfaceModel::PBR;
            args.features.pbr.metallic = std::strtof(argv[++i], nullptr);
        } else if (arg == "--pbr-specular" && i + 1 < argc) {
            args.features.lego_surface_model = SurfaceModel::PBR;
            args.features.pbr.specular_intensity = std::strtof(argv[++i], nullptr);
        } else if (arg == "--vignette" && i + 1 < argc) {
            args.features.post.vignette_enabled = true;
            args.features.post.vignette_strength = std::strtof(argv[++i], nullptr);
        } else if ((arg == "--color-lut" || arg == "--lut") && i + 1 < argc) {
            args.features.post.color_lut_enabled = true;
            args.features.post.color_lut_path = argv[++i];
        } else if (arg == "--no-color-lut" || arg == "--no-lut") {
            args.features.post.color_lut_enabled = false;
        } else if ((arg == "--color-lut-intensity" || arg == "--lut-intensity") && i + 1 < argc) {
            args.features.post.color_lut_enabled = true;
            args.features.post.color_lut_intensity = std::strtof(argv[++i], nullptr);
        } else if (arg == "--film-grain" && i + 1 < argc) {
            args.features.post.film_grain_enabled = true;
            args.features.post.film_grain_strength = std::strtof(argv[++i], nullptr);
        } else if (arg == "--taa") {
            args.features.post.taa_enabled = true;
        } else if (arg == "--no-taa") {
            args.features.post.taa_enabled = false;
        } else if (arg == "--taa-feedback" && i + 1 < argc) {
            args.features.post.taa_enabled = true;
            args.features.post.taa_feedback = std::strtof(argv[++i], nullptr);
        } else if (arg == "--taa-jitter" && i + 1 < argc) {
            args.features.post.taa_enabled = true;
            args.features.post.taa_jitter = std::strtof(argv[++i], nullptr);
        } else if (arg == "--ssr" && i + 1 < argc) {
            args.features.screen_space.ssr_enabled = true;
            args.features.screen_space.ssr_strength = std::strtof(argv[++i], nullptr);
        } else if (arg == "--ssr-max-distance" && i + 1 < argc) {
            args.features.screen_space.ssr_enabled = true;
            args.features.screen_space.ssr_max_distance = std::strtof(argv[++i], nullptr);
        } else if (arg == "--ssr-thickness" && i + 1 < argc) {
            args.features.screen_space.ssr_enabled = true;
            args.features.screen_space.ssr_thickness = std::strtof(argv[++i], nullptr);
        } else if (arg == "--bloom" && i + 1 < argc) {
            args.features.post.bloom_enabled = true;
            args.features.post.bloom_intensity = std::strtof(argv[++i], nullptr);
        } else if (arg == "--bloom-threshold" && i + 1 < argc) {
            args.features.post.bloom_enabled = true;
            args.features.post.bloom_threshold = std::strtof(argv[++i], nullptr);
        } else if ((arg == "--dof" || arg == "--dof-aperture") && i + 1 < argc) {
            args.features.post.dof_enabled = true;
            args.features.post.dof_aperture = std::strtof(argv[++i], nullptr);
        } else if (arg == "--dof-focus" && i + 1 < argc) {
            args.features.post.dof_enabled = true;
            args.features.post.dof_focus_distance = std::strtof(argv[++i], nullptr);
        } else if (arg == "--gtao" && i + 1 < argc) {
            args.features.screen_space.gtao_enabled = true;
            args.features.screen_space.gtao_intensity = std::strtof(argv[++i], nullptr);
        } else if (arg == "--gtao-radius" && i + 1 < argc) {
            args.features.screen_space.gtao_enabled = true;
            args.features.screen_space.gtao_radius = std::strtof(argv[++i], nullptr);
        } else if (arg == "--shadows") {
            args.features.shadows.directional_shadows_enabled = true;
        } else if (arg == "--no-shadows") {
            args.features.shadows.directional_shadows_enabled = false;
        } else if (arg == "--pcss-radius" && i + 1 < argc) {
            args.features.shadows.directional_shadows_enabled = true;
            args.features.shadows.pcss_light_radius = std::strtof(argv[++i], nullptr);
        } else if (arg == "--pcss-bias" && i + 1 < argc) {
            args.features.shadows.directional_shadows_enabled = true;
            args.features.shadows.pcss_bias = std::strtof(argv[++i], nullptr);
        } else if (arg == "--pcss-normal-bias" && i + 1 < argc) {
            args.features.shadows.directional_shadows_enabled = true;
            args.features.shadows.pcss_normal_bias = std::strtof(argv[++i], nullptr);
        } else if (arg == "--pcss-slope-bias" && i + 1 < argc) {
            args.features.shadows.directional_shadows_enabled = true;
            args.features.shadows.pcss_slope_bias = std::strtof(argv[++i], nullptr);
        } else if (arg == "--reflection-probe" || arg == "--probes") {
            args.features.reflection_probe.enabled = true;
        } else if (arg == "--no-reflection-probe" || arg == "--no-probes") {
            args.features.reflection_probe.enabled = false;
        } else if (arg == "--probe-intensity" && i + 1 < argc) {
            args.features.reflection_probe.enabled = true;
            args.features.reflection_probe.intensity = std::strtof(argv[++i], nullptr);
        } else if (arg == "--camera-distance" && i + 1 < argc) {
            args.camera_distance = std::max(0.1f, std::strtof(argv[++i], nullptr));
        } else if (arg == "--camera-target" && i + 3 < argc) {
            args.camera_target = Vec3{
                std::strtof(argv[++i], nullptr),
                std::strtof(argv[++i], nullptr),
                std::strtof(argv[++i], nullptr)
            };
        } else if ((arg == "--exit-after-frames" || arg == "--frames") && i + 1 < argc) {
            args.exit_after_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--hidden") {
            args.hidden = true;
        } else if (arg == "--transparent-test-scene") {
            args.transparent_test_scene = true;
        } else if (arg == "--shadow-test-scene") {
            args.shadow_test_scene = true;
        } else if (arg == "--bgfx-device-debug") {
            args.bgfx_device_debug = true;
        } else if (args.nif_path.empty()) {
            args.nif_path = arg;
        }
    }
    return args;
}

class ViewerBgfxCallback final : public bgfx::CallbackI {
public:
    bool screenshotWritten() const { return screenshot_written_.load(); }

    void fatal(const char* file_path, uint16_t line, bgfx::Fatal::Enum, const char* message) override {
        std::cerr << "bgfx fatal at " << file_path << ":" << line << ": " << message << "\n";
        std::abort();
    }

    void traceVargs(const char*, uint16_t, const char* format, va_list args) override {
        std::vfprintf(stderr, format, args);
    }

    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}

    void screenShot(const char* file_path, uint32_t width, uint32_t height, uint32_t pitch,
                    bgfx::TextureFormat::Enum, const void* data, uint32_t, bool y_flip) override {
        writeBmp(file_path, width, height, pitch, data, y_flip);
        screenshot_written_.store(true);
    }

    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}

private:
    static void writeU16(std::ofstream& out, uint16_t value) {
        out.put(static_cast<char>(value & 0xffu));
        out.put(static_cast<char>((value >> 8u) & 0xffu));
    }

    static void writeU32(std::ofstream& out, uint32_t value) {
        writeU16(out, static_cast<uint16_t>(value & 0xffffu));
        writeU16(out, static_cast<uint16_t>((value >> 16u) & 0xffffu));
    }

    static void writeI32(std::ofstream& out, int32_t value) {
        writeU32(out, static_cast<uint32_t>(value));
    }

    static void writeBmp(const char* file_path, uint32_t width, uint32_t height, uint32_t pitch,
                         const void* data, bool y_flip) {
        if (!file_path || !data || width == 0 || height == 0) return;
        const auto path = std::filesystem::path(file_path);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        constexpr uint32_t kFileHeaderSize = 14;
        constexpr uint32_t kInfoHeaderSize = 40;
        const uint32_t row_bytes = width * 4u;
        const uint32_t pixel_bytes = row_bytes * height;
        const uint32_t pixel_offset = kFileHeaderSize + kInfoHeaderSize;
        std::ofstream out(path, std::ios::binary);
        if (!out) return;

        out.put('B');
        out.put('M');
        writeU32(out, pixel_offset + pixel_bytes);
        writeU16(out, 0);
        writeU16(out, 0);
        writeU32(out, pixel_offset);

        writeU32(out, kInfoHeaderSize);
        writeI32(out, static_cast<int32_t>(width));
        writeI32(out, -static_cast<int32_t>(height));
        writeU16(out, 1);
        writeU16(out, 32);
        writeU32(out, 0);
        writeU32(out, pixel_bytes);
        writeI32(out, 2835);
        writeI32(out, 2835);
        writeU32(out, 0);
        writeU32(out, 0);

        const auto* bytes = static_cast<const uint8_t*>(data);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t src_y = y_flip ? (height - 1u - y) : y;
            out.write(reinterpret_cast<const char*>(bytes + src_y * pitch), row_bytes);
        }
    }

    std::atomic_bool screenshot_written_{false};
};

uint32_t color(float r, float g, float b, float a = 1.0f) {
    auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(r) | (byte(g) << 8u) | (byte(b) << 16u) | (byte(a) << 24u);
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

const char* portStatusName(lu::renderer::ShaderPortStatus status) {
    switch (status) {
    case lu::renderer::ShaderPortStatus::Unported: return "unported";
    case lu::renderer::ShaderPortStatus::Placeholder: return "placeholder";
    case lu::renderer::ShaderPortStatus::Inferred: return "inferred";
    case lu::renderer::ShaderPortStatus::Verified: return "verified";
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

const char* legoppVariantName(lu::renderer::LegoppShaderVariant variant) {
    switch (variant) {
    case lu::renderer::LegoppShaderVariant::None: return "none";
    case lu::renderer::LegoppShaderVariant::Base: return "base";
    case lu::renderer::LegoppShaderVariant::NoAmbient: return "noambient";
    case lu::renderer::LegoppShaderVariant::Emissive: return "emissive";
    case lu::renderer::LegoppShaderVariant::SuperEmissive: return "superemissive";
    case lu::renderer::LegoppShaderVariant::Glow: return "glow";
    case lu::renderer::LegoppShaderVariant::GlowIgnoreVertAlpha: return "glow-ignore-va";
    case lu::renderer::LegoppShaderVariant::Grayscale: return "grayscale";
    case lu::renderer::LegoppShaderVariant::Darkling: return "darkling";
    case lu::renderer::LegoppShaderVariant::DarklingSpecular: return "darkling-spec";
    case lu::renderer::LegoppShaderVariant::DarklingStructure: return "darkling-structure";
    case lu::renderer::LegoppShaderVariant::DarklingShinyGlint: return "darkling-glint";
    case lu::renderer::LegoppShaderVariant::DarklingSpecularShinyGlint: return "darkling-spec-glint";
    case lu::renderer::LegoppShaderVariant::DarklingStructureShinyGlint: return "darkling-structure-glint";
    case lu::renderer::LegoppShaderVariant::Item: return "item";
    case lu::renderer::LegoppShaderVariant::ItemGlow: return "item-glow";
    case lu::renderer::LegoppShaderVariant::FrontEnd: return "frontend";
    case lu::renderer::LegoppShaderVariant::MaskedNonDecal: return "masked-nondecal";
    case lu::renderer::LegoppShaderVariant::Reveal: return "reveal";
    case lu::renderer::LegoppShaderVariant::FadeUp: return "fadeup";
    case lu::renderer::LegoppShaderVariant::AnimUv: return "animuv";
    case lu::renderer::LegoppShaderVariant::NoLight: return "nolight";
    case lu::renderer::LegoppShaderVariant::FaceCreate: return "facecreate";
    case lu::renderer::LegoppShaderVariant::PetTamingCloud: return "pet-cloud";
    case lu::renderer::LegoppShaderVariant::ThreeLight: return "3lights";
    case lu::renderer::LegoppShaderVariant::ShinyGlint: return "shiny-glint";
    }
    return "?";
}

const char* resolutionSourceName(lu::renderer::ShaderResolutionSource source) {
    switch (source) {
    case lu::renderer::ShaderResolutionSource::Unresolved: return "unresolved";
    case lu::renderer::ShaderResolutionSource::CdClientAsset: return "cdclient";
    case lu::renderer::ShaderResolutionSource::CdClientMultishaderPrefix: return "cdclient-prefix";
    case lu::renderer::ShaderResolutionSource::NifMultiShaderGameValue: return "nif-nimultishader";
    case lu::renderer::ShaderResolutionSource::NifMaterialName: return "nif-material";
    case lu::renderer::ShaderResolutionSource::NifFxShaderName: return "nif-fxshader";
    case lu::renderer::ShaderResolutionSource::Fallback: return "fallback";
    }
    return "?";
}

const char* alphaSemanticName(lu::renderer::ShaderAlphaSemantic semantic) {
    switch (semantic) {
    case lu::renderer::ShaderAlphaSemantic::Unknown: return "unknown";
    case lu::renderer::ShaderAlphaSemantic::OutputAlpha: return "output";
    case lu::renderer::ShaderAlphaSemantic::AlphaTest: return "test";
    case lu::renderer::ShaderAlphaSemantic::ControlGlow: return "control-glow";
    case lu::renderer::ShaderAlphaSemantic::ControlEmissive: return "control-emissive";
    case lu::renderer::ShaderAlphaSemantic::ControlDarkling: return "control-darkling";
    case lu::renderer::ShaderAlphaSemantic::Ignored: return "ignored";
    }
    return "?";
}

struct UvBounds {
    float min_u = std::numeric_limits<float>::max();
    float min_v = std::numeric_limits<float>::max();
    float max_u = std::numeric_limits<float>::lowest();
    float max_v = std::numeric_limits<float>::lowest();
    bool valid = false;
};

struct MeshBounds {
    Vec3 min_v{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    Vec3 max_v{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    bool valid = false;
};

MeshBounds computeMeshBounds(const MeshAsset& mesh) {
    MeshBounds bounds;
    for (const auto& vertex : mesh.vertices) {
        bounds.min_v.x = std::min(bounds.min_v.x, vertex.position.x);
        bounds.min_v.y = std::min(bounds.min_v.y, vertex.position.y);
        bounds.min_v.z = std::min(bounds.min_v.z, vertex.position.z);
        bounds.max_v.x = std::max(bounds.max_v.x, vertex.position.x);
        bounds.max_v.y = std::max(bounds.max_v.y, vertex.position.y);
        bounds.max_v.z = std::max(bounds.max_v.z, vertex.position.z);
        bounds.valid = true;
    }
    return bounds;
}

UvBounds computeUvBounds(const MeshAsset& mesh, bool use_second_uv) {
    UvBounds bounds;
    for (const auto& vertex : mesh.vertices) {
        const auto& uv = use_second_uv ? vertex.uv2 : vertex.uv;
        bounds.min_u = std::min(bounds.min_u, uv.x);
        bounds.min_v = std::min(bounds.min_v, uv.y);
        bounds.max_u = std::max(bounds.max_u, uv.x);
        bounds.max_v = std::max(bounds.max_v, uv.y);
        bounds.valid = true;
    }
    return bounds;
}

void printMeshBounds(std::ostream& stream, const MeshBounds& bounds) {
    stream << " bounds=";
    if (!bounds.valid) {
        stream << "<none>";
        return;
    }
    stream << bounds.min_v.x << "/" << bounds.min_v.y << "/" << bounds.min_v.z
           << ".." << bounds.max_v.x << "/" << bounds.max_v.y << "/" << bounds.max_v.z;
}

void printUvBounds(std::ostream& stream, const char* label, const UvBounds& bounds) {
    stream << " " << label << "=";
    if (!bounds.valid) {
        stream << "<none>";
        return;
    }
    stream << bounds.min_u << "/" << bounds.min_v << ".."
           << bounds.max_u << "/" << bounds.max_v;
}

void printTextureAddress(std::ostream& stream,
                         const char* label,
                         const lu::renderer::TextureAddressMode& address) {
    stream << " " << label << "="
           << (address.authored ? "authored" : "default")
           << ":u" << (address.wrap_u ? "Wrap" : "Clamp")
           << ":v" << (address.wrap_v ? "Wrap" : "Clamp");
}

void printShaderDiagnostics(const RenderWorld& world) {
    std::cout << "Asset key: " << (world.source_asset_path.empty() ? "<debug>" : world.source_asset_path) << "\n";
    const size_t count = std::min<size_t>(world.meshes.size(), 8);
    for (size_t i = 0; i < count; ++i) {
        const auto& mesh = world.meshes[i];
        const auto& material = mesh.material;
        const auto submitted_state = lu::renderer::currentRenderStateDiagnostic(material);
        std::cout
            << "  [" << i << "] " << mesh.name
            << " shader=" << material.lu_shader_id
            << " gameValue=" << material.lu_shader_game_value
            << " label=\"" << material.lu_shader_label << "\""
            << " variant=" << legoppVariantName(material.legopp_variant)
            << " program=" << shaderFamilyName(material.shader_family)
            << " port=" << portStatusName(material.lu_shader_port_status)
            << " fx=\"" << material.lu_shader_source_file << "\""
            << " technique=\"" << material.lu_shader_source_technique << "\""
            << " sourceNote=\"" << material.lu_shader_source_status_note << "\""
            << " validationNote=\"" << material.lu_shader_validation_status_note << "\""
            << " resolved=" << boolText(material.lu_shader_resolved)
            << " source=" << resolutionSourceName(material.lu_shader_resolution_source)
            << " metadata=\"" << material.lu_shader_metadata << "\""
            << " multishader=" << boolText(material.lu_shader_asset_is_multishader)
            << " prefix=" << material.lu_multishader_prefix_id
            << " vc=" << boolText(material.lu_shader_uses_vertex_color)
            << " meshVC=" << boolText(material.mesh_has_vertex_colors)
            << " nifVCUse=" << boolText(material.nif_vertex_colors_effective)
            << " flat=" << boolText(material.nif_flat_shading_effective)
            << " shaderTex=" << boolText(material.lu_shader_uses_texture)
            << " matDiffuse=" << boolText(material.lu_shader_uses_material_diffuse)
            << " fog=" << boolText(material.lu_shader_uses_fog)
            << " spec=" << boolText(material.lu_shader_uses_specular)
            << " refl=" << boolText(material.lu_shader_uses_reflection)
            << " shadowTerrain=" << boolText(material.lu_shader_uses_shadow_terrain)
            << " env=\"" << material.lu_shader_reflection_map << "\""
            << " envSemantic=\"" << material.lu_shader_reflection_semantic << "\""
            << " uvAnim=" << boolText(material.lu_shader_uses_uv_animation)
            << " alphaAnim=" << boolText(material.lu_shader_uses_alpha_animation)
            << " matCtrl=" << boolText(material.nif_has_material_color_controller)
            << " emCtrl=" << boolText(material.material_emissive_controller)
            << " emKeys=" << material.material_emissive_controller_keys.size()
            << " emRange=" << material.material_emissive_controller_start << "/"
            << material.material_emissive_controller_stop
            << " motion1=" << material.lu_uv_motion_layer1.x << "/" << material.lu_uv_motion_layer1.y
            << " motion2=" << material.lu_uv_motion_layer2.x << "/" << material.lu_uv_motion_layer2.y
            << " alpha=" << alphaModeName(material.alpha_mode)
            << " alphaSemantic=" << alphaSemanticName(material.lu_shader_alpha_semantic)
            << " test=" << boolText(material.alpha_test)
            << " blend=" << boolText(material.alpha_blend)
            << " usesNi=" << boolText(material.lu_shader_uses_ni_render_state)
            << " blendFunc=" << static_cast<int>(material.source_blend) << "/"
            << static_cast<int>(material.destination_blend)
            << " alphaFunc=" << static_cast<int>(material.alpha_test_function)
            << " zwrite=" << boolText(material.depth_write)
            << " ztest=" << boolText(material.depth_test)
            << ":" << static_cast<int>(material.depth_test_function)
            << " submittedZ=" << boolText(submitted_state.submitted_depth_write)
            << " noSort=" << boolText(material.disable_transparent_sort)
            << " stencil=" << boolText(material.stencil_enabled)
            << ":" << static_cast<int>(material.stencil_test_function)
            << ":" << static_cast<int>(material.stencil_reference)
            << ":" << static_cast<int>(material.stencil_read_mask)
            << ":" << static_cast<int>(material.stencil_fail_action)
            << "/" << static_cast<int>(material.stencil_z_fail_action)
            << "/" << static_cast<int>(material.stencil_pass_action)
            << " cull=" << cullModeName(material.cull_mode)
            << " nifAlpha=" << material.nif_resolved_state.alpha.raw_flags
            << ":blend" << boolText(material.nif_resolved_state.alpha.blend_enabled)
            << ":test" << boolText(material.nif_resolved_state.alpha.test_enabled)
            << " nifZ=" << material.nif_resolved_state.z_buffer.raw_flags
            << ":test" << boolText(material.nif_resolved_state.z_buffer.test_enabled)
            << ":write" << boolText(material.nif_resolved_state.z_buffer.write_enabled)
            << " nifVC=" << material.nif_resolved_state.vertex_color.raw_flags
            << ":source" << static_cast<int>(material.nif_resolved_state.vertex_color.source_vertex_mode)
            << ":light" << static_cast<int>(material.nif_resolved_state.vertex_color.lighting_mode)
            << " nifSpec=" << boolText(material.nif_resolved_state.specular_enabled)
            << " nifSort=" << material.nif_resolved_state.sorting_mode
            << " emissive=" << std::max({material.emissive.x, material.emissive.y, material.emissive.z})
            << " diffuse=" << material.diffuse.x << "/" << material.diffuse.y << "/"
            << material.diffuse.z << "/" << material.diffuse.w
            << " texture=" << boolText(!material.diffuse_texture_path.empty())
            << " darkTex=" << boolText(!material.dark_texture_path.empty())
            << " lod=" << boolText(mesh.has_lod_range)
            << " skin=" << boolText(mesh.is_skinned);
        printMeshBounds(std::cout, computeMeshBounds(mesh));
        printUvBounds(std::cout, "uv0", computeUvBounds(mesh, false));
        printUvBounds(std::cout, "uv1", computeUvBounds(mesh, true));
        if (!material.diffuse_texture_path.empty()) {
            printTextureAddress(std::cout, "diffuseAddr", material.diffuse_texture_address);
            std::cout << " diffuseFile=\""
                      << std::filesystem::path(material.diffuse_texture_path).filename().string()
                      << "\"";
        }
        if (!material.dark_texture_path.empty()) {
            printTextureAddress(std::cout, "darkAddr", material.dark_texture_address);
            std::cout << " darkFile=\""
                      << std::filesystem::path(material.dark_texture_path).filename().string()
                      << "\"";
        }
        if (mesh.has_lod_range) {
            std::cout
                << " lodParent=" << mesh.lod_parent_block
                << " lodLevel=" << mesh.lod_level
                << " lodRange=" << mesh.lod_near << "/" << mesh.lod_far
                << " lodCenter=" << mesh.lod_center.x << "/" << mesh.lod_center.y << "/"
                << mesh.lod_center.z;
        }
        if (mesh.is_skinned) {
            size_t weighted_vertices = 0;
            for (const auto& vertex : mesh.vertices) {
                if (vertex.bone_weights[0] > 0.0f) ++weighted_vertices;
            }
            std::cout
                << " skinInst=" << mesh.skin_instance_block
                << " skeletonRoot=" << mesh.skeleton_root_block
                << " bones=" << mesh.skin_bone_node_blocks.size()
                << " weightedVerts=" << weighted_vertices << "/" << mesh.vertices.size();
            if (!mesh.skin_bone_names.empty()) {
                std::cout << " firstBone=\"" << mesh.skin_bone_names.front() << "\"";
            }
        }
        std::cout
            << "\n";
    }
    if (world.meshes.size() > count) {
        std::cout << "  ... " << (world.meshes.size() - count) << " more mesh(es)\n";
    }
}

void printMeshOrientationDiagnostics(const RenderWorld& world) {
    size_t total_triangles = 0;
    size_t aligned_triangles = 0;
    size_t opposed_triangles = 0;
    size_t degenerate_triangles = 0;
    float dot_sum = 0.0f;

    struct SuspiciousMesh {
        std::string name;
        size_t aligned = 0;
        size_t opposed = 0;
        size_t degenerate = 0;
        float average_dot = 0.0f;
    };
    std::vector<SuspiciousMesh> suspicious;

    for (const auto& mesh : world.meshes) {
        size_t mesh_triangles = 0;
        size_t mesh_aligned = 0;
        size_t mesh_opposed = 0;
        size_t mesh_degenerate = 0;
        float mesh_dot_sum = 0.0f;

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t i0 = mesh.indices[i + 0];
            const uint32_t i1 = mesh.indices[i + 1];
            const uint32_t i2 = mesh.indices[i + 2];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) {
                ++mesh_degenerate;
                continue;
            }

            const auto& v0 = mesh.vertices[i0];
            const auto& v1 = mesh.vertices[i1];
            const auto& v2 = mesh.vertices[i2];
            const Vec3 e1 = v1.position - v0.position;
            const Vec3 e2 = v2.position - v0.position;
            const Vec3 face_normal = lu::renderer::normalize(lu::renderer::cross(e1, e2));
            const Vec3 vertex_normal = lu::renderer::normalize(v0.normal + v1.normal + v2.normal);
            const float d = lu::renderer::dot(face_normal, vertex_normal);
            if (std::abs(d) < 0.001f) {
                ++mesh_degenerate;
                continue;
            }

            ++mesh_triangles;
            mesh_dot_sum += d;
            if (d >= 0.0f) {
                ++mesh_aligned;
            } else {
                ++mesh_opposed;
            }
        }

        if (mesh_triangles > 0) {
            total_triangles += mesh_triangles;
            aligned_triangles += mesh_aligned;
            opposed_triangles += mesh_opposed;
            degenerate_triangles += mesh_degenerate;
            dot_sum += mesh_dot_sum;

            const float opposed_ratio = static_cast<float>(mesh_opposed) / static_cast<float>(mesh_triangles);
            if (opposed_ratio > 0.20f && suspicious.size() < 8) {
                suspicious.push_back({
                    mesh.name,
                    mesh_aligned,
                    mesh_opposed,
                    mesh_degenerate,
                    mesh_dot_sum / static_cast<float>(mesh_triangles)
                });
            }
        } else {
            degenerate_triangles += mesh_degenerate;
        }
    }

    if (total_triangles == 0) return;

    const float aligned_pct = 100.0f * static_cast<float>(aligned_triangles) / static_cast<float>(total_triangles);
    const float opposed_pct = 100.0f * static_cast<float>(opposed_triangles) / static_cast<float>(total_triangles);
    std::cout
        << "Mesh orientation: triangles=" << total_triangles
        << " aligned=" << aligned_triangles << " (" << aligned_pct << "%)"
        << " opposed=" << opposed_triangles << " (" << opposed_pct << "%)"
        << " degenerate=" << degenerate_triangles
        << " avgDot=" << (dot_sum / static_cast<float>(total_triangles))
        << "\n";

    for (const auto& mesh : suspicious) {
        std::cout
            << "  orientation warning: " << mesh.name
            << " aligned=" << mesh.aligned
            << " opposed=" << mesh.opposed
            << " degenerate=" << mesh.degenerate
            << " avgDot=" << mesh.average_dot
            << "\n";
    }
}

void printAnimationDiagnostics(const RenderWorld& world) {
    const auto& animation = world.animation;
    if (animation.source_path.empty()) return;

    std::cout
        << "Animation: " << animation.source_path
        << " clips=" << animation.clips.size()
        << " model=\"" << animation.model_path << "\""
        << " root=\"" << animation.model_root << "\""
        << "\n";

    const size_t count = std::min<size_t>(animation.clips.size(), 8);
    for (size_t i = 0; i < count; ++i) {
        const auto& clip = animation.clips[i];
        std::cout
            << "  [" << i << "] "
            << "seqId=" << clip.sequence_id
            << " animIndex=" << clip.anim_index
            << " name=\"" << clip.name << "\""
            << " source=\"" << std::filesystem::path(clip.source_path).filename().string() << "\""
            << " time=" << clip.start_time << "/" << clip.stop_time
            << " freq=" << clip.frequency
            << " cycle=" << clip.cycle_type
            << " controlled=" << clip.controlled_blocks.size()
            << " textKeys=" << clip.text_keys.size();
        if (!clip.controlled_blocks.empty()) {
            std::cout << " firstTarget=\"" << clip.controlled_blocks.front().node_name << "\"";
        }
        if (!clip.text_keys.empty()) {
            std::cout << " firstKey=\"" << clip.text_keys.front().text << "\"";
        }
        std::cout << "\n";
    }
    if (animation.clips.size() > count) {
        std::cout << "  ... " << (animation.clips.size() - count) << " more clip(s)\n";
    }
}

RenderWorld makeCubeWorld() {
    RenderWorld world;
    MeshAsset cube;
    cube.name = "Fallback Cube";
    cube.material.name = "Legacy Debug";
    cube.material.diffuse = {0.55f, 0.65f, 0.95f, 1.0f};

    constexpr float s = 1.0f;
    const uint32_t c = color(1, 1, 1);
    auto vertex = [](Vec3 position, Vec3 normal, lu::renderer::Vec2 uv, uint32_t color_rgba) {
        Vertex v;
        v.position = position;
        v.normal = normal;
        v.uv = uv;
        v.uv2 = uv;
        v.color_rgba8 = color_rgba;
        return v;
    };
    cube.vertices = {
        vertex({-s,-s,-s}, { 0, 0,-1}, {0,1}, c), vertex({ s,-s,-s}, { 0, 0,-1}, {1,1}, c), vertex({ s, s,-s}, { 0, 0,-1}, {1,0}, c), vertex({-s, s,-s}, { 0, 0,-1}, {0,0}, c),
        vertex({-s,-s, s}, { 0, 0, 1}, {0,1}, c), vertex({ s,-s, s}, { 0, 0, 1}, {1,1}, c), vertex({ s, s, s}, { 0, 0, 1}, {1,0}, c), vertex({-s, s, s}, { 0, 0, 1}, {0,0}, c),
        vertex({-s,-s,-s}, {-1, 0, 0}, {0,1}, c), vertex({-s, s,-s}, {-1, 0, 0}, {1,1}, c), vertex({-s, s, s}, {-1, 0, 0}, {1,0}, c), vertex({-s,-s, s}, {-1, 0, 0}, {0,0}, c),
        vertex({ s,-s,-s}, { 1, 0, 0}, {0,1}, c), vertex({ s, s,-s}, { 1, 0, 0}, {1,1}, c), vertex({ s, s, s}, { 1, 0, 0}, {1,0}, c), vertex({ s,-s, s}, { 1, 0, 0}, {0,0}, c),
        vertex({-s,-s,-s}, { 0,-1, 0}, {0,1}, c), vertex({-s,-s, s}, { 0,-1, 0}, {1,1}, c), vertex({ s,-s, s}, { 0,-1, 0}, {1,0}, c), vertex({ s,-s,-s}, { 0,-1, 0}, {0,0}, c),
        vertex({-s, s,-s}, { 0, 1, 0}, {0,1}, c), vertex({-s, s, s}, { 0, 1, 0}, {1,1}, c), vertex({ s, s, s}, { 0, 1, 0}, {1,0}, c), vertex({ s, s,-s}, { 0, 1, 0}, {0,0}, c)
    };
    cube.indices = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7,
        8,10,9, 8,11,10, 12,13,14, 12,14,15,
        16,18,17, 16,19,18, 20,21,22, 20,22,23
    };
    world.meshes.push_back(std::move(cube));
    return world;
}

RenderWorld makeTransparentTestWorld() {
    RenderWorld world = makeCubeWorld();
    world.source_asset_path = "<transparent-test-scene>";
    world.meshes.front().name = "Opaque Depth Reference";
    world.meshes.front().material.diffuse = {0.38f, 0.42f, 0.48f, 1.0f};

    auto vertex = [](Vec3 position, Vec3 normal, lu::renderer::Vec2 uv, uint32_t color_rgba) {
        Vertex v;
        v.position = position;
        v.normal = normal;
        v.uv = uv;
        v.uv2 = uv;
        v.color_rgba8 = color_rgba;
        return v;
    };
    auto makePane = [&](const char* name, float z, uint32_t color_rgba) {
        MeshAsset pane;
        pane.name = name;
        pane.material.name = name;
        pane.material.shader_family = LegacyShaderFamily::AlphaAsAlpha;
        pane.material.alpha_mode = RenderAlphaMode::AlphaBlend;
        pane.material.alpha_blend = true;
        pane.material.depth_write = false;
        pane.material.cull_mode = RenderCullMode::TwoSided;
        pane.material.diffuse = {1.0f, 1.0f, 1.0f, 0.55f};
        pane.material.lu_shader_uses_texture = false;
        pane.material.lu_shader_uses_vertex_color = true;
        pane.material.lu_shader_uses_material_diffuse = false;
        pane.material.lu_shader_uses_lighting = false;
        pane.material.lu_shader_uses_fog = false;
        pane.material.lu_shader_uses_specular = false;
        pane.material.lu_shader_uses_reflection = false;
        pane.material.lu_shader_port_status = lu::renderer::ShaderPortStatus::Verified;
        pane.vertices = {
            vertex({-1.25f, -0.95f, z}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, color_rgba),
            vertex({ 1.25f, -0.95f, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, color_rgba),
            vertex({ 1.25f,  0.95f, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, color_rgba),
            vertex({-1.25f,  0.95f, z}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, color_rgba)
        };
        pane.indices = {0, 2, 1, 0, 3, 2};
        return pane;
    };

    world.meshes.push_back(makePane("Far Transparent Blue", 0.35f, color(0.15f, 0.45f, 1.0f, 0.55f)));
    world.meshes.push_back(makePane("Near Transparent Amber", -0.35f, color(1.0f, 0.55f, 0.12f, 0.55f)));
    return world;
}

RenderWorld makeShadowTestWorld() {
    RenderWorld world;
    world.source_asset_path = "<shadow-test-scene>";
    world.environment.ambient = {0.045f, 0.047f, 0.052f};
    world.environment.sun.direction = lu::renderer::normalize(Vec3{0.82f, 0.42f, 0.34f});
    world.environment.sun.position = world.environment.sun.direction;
    world.environment.sun.color = {1.0f, 0.96f, 0.88f};
    world.environment.sun.intensity = 1.25f;

    auto material = [](const char* name, Vec3 diffuse) {
        lu::renderer::MaterialAsset mat;
        mat.name = name;
        mat.shader_family = LegacyShaderFamily::BasicLit;
        mat.diffuse = {diffuse.x, diffuse.y, diffuse.z, 1.0f};
        mat.alpha_mode = RenderAlphaMode::Opaque;
        mat.depth_write = true;
        mat.cull_mode = RenderCullMode::Backface;
        mat.lu_shader_uses_texture = false;
        mat.lu_shader_uses_vertex_color = false;
        mat.lu_shader_uses_material_diffuse = true;
        mat.lu_shader_uses_lighting = true;
        mat.lu_shader_uses_fog = false;
        mat.lu_shader_uses_shadow_terrain = true;
        mat.lu_shader_port_status = lu::renderer::ShaderPortStatus::Verified;
        return mat;
    };
    auto vertex = [](Vec3 position, Vec3 normal, lu::renderer::Vec2 uv) {
        Vertex v;
        v.position = position;
        v.normal = normal;
        v.uv = uv;
        v.uv2 = uv;
        v.color_rgba8 = color(1, 1, 1);
        return v;
    };
    auto addPlane = [&]() {
        MeshAsset plane;
        plane.name = "Shadow Receiver";
        plane.material = material("Shadow Receiver", {0.62f, 0.64f, 0.60f});
        plane.vertices = {
            vertex({-5.0f, -1.0f, -4.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}),
            vertex({ 5.0f, -1.0f, -4.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}),
            vertex({ 5.0f, -1.0f,  4.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}),
            vertex({-5.0f, -1.0f,  4.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f})
        };
        plane.indices = {0, 2, 1, 0, 3, 2};
        world.meshes.push_back(std::move(plane));
    };
    auto addBox = [&](const char* name, Vec3 min_v, Vec3 max_v, Vec3 diffuse) {
        MeshAsset box;
        box.name = name;
        box.material = material(name, diffuse);
        box.vertices = {
            vertex({min_v.x,min_v.y,min_v.z}, { 0, 0,-1}, {0,1}), vertex({max_v.x,min_v.y,min_v.z}, { 0, 0,-1}, {1,1}), vertex({max_v.x,max_v.y,min_v.z}, { 0, 0,-1}, {1,0}), vertex({min_v.x,max_v.y,min_v.z}, { 0, 0,-1}, {0,0}),
            vertex({min_v.x,min_v.y,max_v.z}, { 0, 0, 1}, {0,1}), vertex({max_v.x,min_v.y,max_v.z}, { 0, 0, 1}, {1,1}), vertex({max_v.x,max_v.y,max_v.z}, { 0, 0, 1}, {1,0}), vertex({min_v.x,max_v.y,max_v.z}, { 0, 0, 1}, {0,0}),
            vertex({min_v.x,min_v.y,min_v.z}, {-1, 0, 0}, {0,1}), vertex({min_v.x,max_v.y,min_v.z}, {-1, 0, 0}, {1,1}), vertex({min_v.x,max_v.y,max_v.z}, {-1, 0, 0}, {1,0}), vertex({min_v.x,min_v.y,max_v.z}, {-1, 0, 0}, {0,0}),
            vertex({max_v.x,min_v.y,min_v.z}, { 1, 0, 0}, {0,1}), vertex({max_v.x,max_v.y,min_v.z}, { 1, 0, 0}, {1,1}), vertex({max_v.x,max_v.y,max_v.z}, { 1, 0, 0}, {1,0}), vertex({max_v.x,min_v.y,max_v.z}, { 1, 0, 0}, {0,0}),
            vertex({min_v.x,min_v.y,min_v.z}, { 0,-1, 0}, {0,1}), vertex({min_v.x,min_v.y,max_v.z}, { 0,-1, 0}, {1,1}), vertex({max_v.x,min_v.y,max_v.z}, { 0,-1, 0}, {1,0}), vertex({max_v.x,min_v.y,min_v.z}, { 0,-1, 0}, {0,0}),
            vertex({min_v.x,max_v.y,min_v.z}, { 0, 1, 0}, {0,1}), vertex({min_v.x,max_v.y,max_v.z}, { 0, 1, 0}, {1,1}), vertex({max_v.x,max_v.y,max_v.z}, { 0, 1, 0}, {1,0}), vertex({max_v.x,max_v.y,min_v.z}, { 0, 1, 0}, {0,0})
        };
        box.indices = {
            0,2,1, 0,3,2, 4,5,6, 4,6,7,
            8,10,9, 8,11,10, 12,13,14, 12,14,15,
            16,18,17, 16,19,18, 20,21,22, 20,22,23
        };
        world.meshes.push_back(std::move(box));
    };

    addPlane();
    addBox("Near Shadow Caster", {-1.2f, -0.85f, -0.6f}, {-0.35f, 0.55f, 0.25f}, {0.86f, 0.48f, 0.32f});
    addBox("Tall Shadow Caster", {0.45f, -0.85f, -0.45f}, {1.25f, 1.65f, 0.35f}, {0.34f, 0.55f, 0.86f});
    addBox("Floating Shadow Bar", {-2.05f, 0.65f, -0.30f}, {-0.15f, 1.00f, 0.15f}, {0.28f, 0.70f, 0.48f});
    return world;
}

void frameWorld(const RenderWorld& world, OrbitCamera& camera) {
    Vec3 min_v{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    Vec3 max_v{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    bool any = false;
    for (const auto& mesh : world.meshes) {
        for (const auto& v : mesh.vertices) {
            min_v.x = std::min(min_v.x, v.position.x);
            min_v.y = std::min(min_v.y, v.position.y);
            min_v.z = std::min(min_v.z, v.position.z);
            max_v.x = std::max(max_v.x, v.position.x);
            max_v.y = std::max(max_v.y, v.position.y);
            max_v.z = std::max(max_v.z, v.position.z);
            any = true;
        }
    }
    if (!any) return;

    Vec3 center = (min_v + max_v) * 0.5f;
    Vec3 extent = max_v - min_v;
    float radius = std::max({extent.x, extent.y, extent.z, 1.0f}) * 0.5f;
    camera.setTarget(center);
    camera.setDistance(radius * 3.0f);
}

void resetCameraToWorld(const RenderWorld& world, OrbitCamera& camera) {
    frameWorld(world, camera);
    camera.syncFlyToOrbit();
}

std::filesystem::path inferClientRoot(const std::filesystem::path& nif_path) {
    for (auto current = nif_path.parent_path(); !current.empty(); current = current.parent_path()) {
        if (current.filename() == "res") {
            return current;
        }
        if (current == current.parent_path()) break;
    }
    return {};
}

bool loadNifWorld(const std::filesystem::path& nif_path, Args& args, RenderWorld& world) {
    if (nif_path.empty()) return false;
    if (args.client_root.empty()) {
        args.client_root = inferClientRoot(nif_path);
    }

    lu::renderer::lu_import::NifImportOptions options;
    options.client_root = args.client_root;
    options.nif_path = nif_path;
    auto imported = lu::renderer::lu_import::importNif(options);
    if (!imported.error.empty()) {
        std::cerr << "NIF import failed: " << imported.error << "\n";
        return false;
    }

    world = std::move(imported.world);
    args.nif_path = nif_path;
    std::cout << "Loaded " << world.meshes.size() << " mesh(es) from " << nif_path << "\n";
    printShaderDiagnostics(world);
    printMeshOrientationDiagnostics(world);
    return true;
}

bool loadLvlEnvironment(const std::filesystem::path& lvl_path, RenderWorld& world) {
    if (lvl_path.empty()) return false;
    auto imported = lu::renderer::lu_import::importLvlEnvironment(lvl_path);
    if (!imported.error.empty()) {
        std::cerr << "LVL environment import failed: " << imported.error << "\n";
        return false;
    }
    if (!imported.has_environment) {
        std::cerr << "LVL has no environment chunk: " << lvl_path << "\n";
        return false;
    }

    world.environment = imported.environment;
    const auto& e = world.environment;
    std::cout
        << "Loaded LVL environment from " << lvl_path
        << " ambient=" << e.ambient.x << "/" << e.ambient.y << "/" << e.ambient.z
        << " dirLight=" << e.sun.color.x << "/" << e.sun.color.y << "/" << e.sun.color.z
        << " lightPos=" << e.sun.position.x << "/" << e.sun.position.y << "/" << e.sun.position.z
        << " upperHemi=" << e.upper_hemi.x << "/" << e.upper_hemi.y << "/" << e.upper_hemi.z
        << " fog=" << boolText(e.fog_enabled)
        << " fogRange=" << e.fog_near << "/" << e.fog_far
        << " fogColor=" << e.fog_color.x << "/" << e.fog_color.y << "/" << e.fog_color.z
        << "\n";
    return true;
}

bool loadAnimation(const std::filesystem::path& animation_path, Args& args, RenderWorld& world) {
    if (animation_path.empty()) return false;
    lu::renderer::lu_import::AnimationImportOptions options;
    options.path = animation_path;
    auto imported = lu::renderer::lu_import::importAnimation(options);
    if (!imported.error.empty()) {
        std::cerr << "Animation import failed: " << imported.error << "\n";
        return false;
    }

    world.animation = std::move(imported.animation);
    args.animation_path = animation_path;
    printAnimationDiagnostics(world);
    return true;
}

void setViewerTitle(GLFWwindow* window, const std::filesystem::path& nif_path) {
    std::string title = "LU NIF Viewer [";
    title += viewerBuildIdentity();
    title += "]";
    if (!nif_path.empty()) {
        title += " - ";
        title += nif_path.filename().string();
    }
    glfwSetWindowTitle(window, title.c_str());
}

#if defined(_WIN32)
constexpr UINT_PTR kMenuFileImportNif = 1001;
constexpr UINT_PTR kMenuFileImportLvl = 1002;
constexpr UINT_PTR kMenuFileImportAnimation = 1003;
constexpr UINT_PTR kMenuViewOrbitCamera = 2001;
constexpr UINT_PTR kMenuViewFlyCamera = 2002;
constexpr UINT_PTR kMenuViewResetCamera = 2003;
constexpr UINT_PTR kMenuViewLightingFog = 2004;
constexpr wchar_t kAppStateProperty[] = L"LuRendererNifViewerAppState";

constexpr int kLightingApply = 3001;
constexpr int kLightingClose = 3002;
constexpr int kLightingFogEnabled = 3003;
constexpr int kEditAmbientR = 3100;
constexpr int kEditAmbientG = 3101;
constexpr int kEditAmbientB = 3102;
constexpr int kEditLightR = 3110;
constexpr int kEditLightG = 3111;
constexpr int kEditLightB = 3112;
constexpr int kEditLightX = 3120;
constexpr int kEditLightY = 3121;
constexpr int kEditLightZ = 3122;
constexpr int kEditSpecularR = 3130;
constexpr int kEditSpecularG = 3131;
constexpr int kEditSpecularB = 3132;
constexpr int kEditUpperR = 3140;
constexpr int kEditUpperG = 3141;
constexpr int kEditUpperB = 3142;
constexpr int kEditLowerR = 3150;
constexpr int kEditLowerG = 3151;
constexpr int kEditLowerB = 3152;
constexpr int kEditFogR = 3160;
constexpr int kEditFogG = 3161;
constexpr int kEditFogB = 3162;
constexpr int kEditFogNear = 3170;
constexpr int kEditFogFar = 3171;
constexpr int kGraphicsMsaaEnabled = 3180;
constexpr int kEditMsaaSamples = 3181;
constexpr int kGraphicsPbrEnabled = 3182;
constexpr int kGraphicsSsrEnabled = 3183;
constexpr int kEditSsrStrength = 3184;
constexpr int kGraphicsGtaoEnabled = 3185;
constexpr int kEditGtaoRadius = 3186;
constexpr int kEditGtaoIntensity = 3187;
constexpr int kGraphicsBloomEnabled = 3188;
constexpr int kEditBloomThreshold = 3189;
constexpr int kEditBloomIntensity = 3190;
constexpr int kGraphicsVignetteEnabled = 3191;
constexpr int kEditVignetteStrength = 3192;
constexpr int kGraphicsDofEnabled = 3193;
constexpr int kEditDofFocus = 3194;
constexpr int kEditDofAperture = 3195;
constexpr int kGraphicsFilmGrainEnabled = 3196;
constexpr int kEditFilmGrainStrength = 3197;
constexpr int kGraphicsShadowsEnabled = 3198;
constexpr int kEditPcssRadius = 3199;
constexpr int kEditPcssBias = 3200;
constexpr int kGraphicsProbesEnabled = 3201;
constexpr int kEditProbeIntensity = 3202;
constexpr int kEditPbrRoughness = 3203;
constexpr int kEditPbrMetallic = 3204;
constexpr int kEditPbrSpecular = 3205;
constexpr int kGraphicsColorLutEnabled = 3206;
constexpr int kEditColorLutIntensity = 3207;
constexpr int kEditColorLutPath = 3208;
constexpr int kEditSsrMaxDistance = 3209;
constexpr int kEditSsrThickness = 3210;
constexpr int kGraphicsBrowseColorLut = 3211;
constexpr int kGraphicsTaaEnabled = 3212;
constexpr int kEditTaaFeedback = 3213;
constexpr int kEditTaaJitter = 3214;
constexpr int kEditPcssNormalBias = 3215;
constexpr int kEditPcssSlopeBias = 3216;
constexpr int kSliderPbrRoughness = 3300;
constexpr int kSliderPbrMetallic = 3301;
constexpr int kSliderPbrSpecular = 3302;
constexpr int kSliderSsrStrength = 3303;
constexpr int kSliderGtaoRadius = 3304;
constexpr int kSliderGtaoIntensity = 3305;
constexpr int kSliderBloomThreshold = 3306;
constexpr int kSliderBloomIntensity = 3307;
constexpr int kSliderColorLutIntensity = 3308;
constexpr int kSliderVignetteStrength = 3309;
constexpr int kSliderDofFocus = 3310;
constexpr int kSliderDofAperture = 3311;
constexpr int kSliderFilmGrainStrength = 3312;
constexpr int kSliderPcssRadius = 3313;
constexpr int kSliderPcssBias = 3314;
constexpr int kSliderProbeIntensity = 3315;
constexpr int kSliderSsrMaxDistance = 3316;
constexpr int kSliderSsrThickness = 3317;
constexpr int kSliderTaaFeedback = 3318;
constexpr int kSliderTaaJitter = 3319;
constexpr int kSliderPcssNormalBias = 3320;
constexpr int kSliderPcssSlopeBias = 3321;

struct SliderBinding {
    int slider_id;
    int edit_id;
    float min_value;
    float max_value;
};

constexpr std::array<SliderBinding, 22> kGraphicsSliders = {{
    {kSliderPbrRoughness, kEditPbrRoughness, 0.04f, 1.0f},
    {kSliderPbrMetallic, kEditPbrMetallic, 0.0f, 1.0f},
    {kSliderPbrSpecular, kEditPbrSpecular, 0.0f, 3.0f},
    {kSliderSsrStrength, kEditSsrStrength, 0.0f, 1.5f},
    {kSliderSsrMaxDistance, kEditSsrMaxDistance, 1.0f, 100.0f},
    {kSliderSsrThickness, kEditSsrThickness, 0.001f, 0.12f},
    {kSliderGtaoRadius, kEditGtaoRadius, 0.1f, 5.0f},
    {kSliderGtaoIntensity, kEditGtaoIntensity, 0.0f, 3.0f},
    {kSliderBloomThreshold, kEditBloomThreshold, 0.0f, 2.0f},
    {kSliderBloomIntensity, kEditBloomIntensity, 0.0f, 3.0f},
    {kSliderColorLutIntensity, kEditColorLutIntensity, 0.0f, 1.0f},
    {kSliderVignetteStrength, kEditVignetteStrength, 0.0f, 1.0f},
    {kSliderDofFocus, kEditDofFocus, 0.1f, 100.0f},
    {kSliderDofAperture, kEditDofAperture, 0.0f, 1.0f},
    {kSliderFilmGrainStrength, kEditFilmGrainStrength, 0.0f, 0.15f},
    {kSliderTaaFeedback, kEditTaaFeedback, 0.0f, 0.98f},
    {kSliderTaaJitter, kEditTaaJitter, 0.0f, 2.0f},
    {kSliderPcssRadius, kEditPcssRadius, 0.0f, 0.2f},
    {kSliderPcssBias, kEditPcssBias, 0.0f, 0.01f},
    {kSliderPcssNormalBias, kEditPcssNormalBias, 0.0f, 8.0f},
    {kSliderPcssSlopeBias, kEditPcssSlopeBias, 0.0f, 12.0f},
    {kSliderProbeIntensity, kEditProbeIntensity, 0.0f, 3.0f}
}};

constexpr int kSliderSteps = 1000;

std::filesystem::path openFileDialog(HWND owner, const Args& args, const wchar_t* filter,
                                     const wchar_t* title, const std::filesystem::path& preferred_path) {
    wchar_t file_name[32768] = {};
    std::wstring initial_dir;
    if (!preferred_path.empty()) {
        initial_dir = preferred_path.parent_path().wstring();
    } else if (!args.nif_path.empty()) {
        initial_dir = args.nif_path.parent_path().wstring();
    } else if (!args.client_root.empty()) {
        initial_dir = args.client_root.wstring();
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_name;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file_name));
    ofn.lpstrInitialDir = initial_dir.empty() ? nullptr : initial_dir.c_str();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    return std::filesystem::path(file_name);
}

std::filesystem::path openNifDialog(HWND owner, const Args& args) {
    return openFileDialog(owner, args, L"NIF files (*.nif)\0*.nif\0All files (*.*)\0*.*\0\0",
                          L"Import NIF", args.nif_path);
}

std::filesystem::path openLvlDialog(HWND owner, const Args& args) {
    return openFileDialog(owner, args, L"LVL files (*.lvl)\0*.lvl\0All files (*.*)\0*.*\0\0",
                          L"Import LVL Environment", args.lvl_path);
}

std::filesystem::path openAnimationDialog(HWND owner, const Args& args) {
    return openFileDialog(owner, args,
                          L"Animation files (*.kfm;*.kf)\0*.kfm;*.kf\0KFM files (*.kfm)\0*.kfm\0KF files (*.kf)\0*.kf\0All files (*.*)\0*.*\0\0",
                          L"Import Animation", args.animation_path);
}

std::filesystem::path openColorLutDialog(HWND owner, const Args& args, const std::string& current_path) {
    std::filesystem::path preferred_path = current_path.empty() ? args.client_root : std::filesystem::path(current_path);
    return openFileDialog(owner, args,
                          L"DDS LUT files (*.dds)\0*.dds\0DDS files (*.DDS)\0*.DDS\0All files (*.*)\0*.*\0\0",
                          L"Select Color LUT", preferred_path);
}

void setEditFloat(HWND window, int id, float value) {
    wchar_t text[64] = {};
    std::swprintf(text, std::size(text), L"%.6g", static_cast<double>(value));
    SetDlgItemTextW(window, id, text);
}

float getEditFloat(HWND window, int id, float fallback) {
    wchar_t text[128] = {};
    if (GetDlgItemTextW(window, id, text, static_cast<int>(std::size(text))) <= 0) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float value = std::wcstof(text, &end);
    return end == text ? fallback : value;
}

void setEditUInt(HWND window, int id, uint32_t value) {
    wchar_t text[64] = {};
    std::swprintf(text, std::size(text), L"%u", value);
    SetDlgItemTextW(window, id, text);
}

uint32_t getEditUInt(HWND window, int id, uint32_t fallback) {
    wchar_t text[128] = {};
    if (GetDlgItemTextW(window, id, text, static_cast<int>(std::size(text))) <= 0) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    return end == text ? fallback : static_cast<uint32_t>(value);
}

void setEditString(HWND window, int id, const std::string& value) {
    SetDlgItemTextW(window, id, std::filesystem::path(value).wstring().c_str());
}

std::string getEditString(HWND window, int id, const std::string& fallback) {
    wchar_t text[1024] = {};
    if (GetDlgItemTextW(window, id, text, static_cast<int>(std::size(text))) <= 0) {
        return fallback;
    }
    return std::filesystem::path(text).string();
}

void setCheckbox(HWND window, int id, bool enabled) {
    SendDlgItemMessageW(window, id, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool getCheckbox(HWND window, int id) {
    return SendDlgItemMessageW(window, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void createLabel(HWND window, const wchar_t* text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                    x, y, w, h, window, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void createEdit(HWND window, int id, int x, int y) {
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    x, y, 72, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
}

void createEditWide(HWND window, int id, int x, int y, int w) {
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    x, y, w, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
}

const SliderBinding* findSliderBindingBySlider(int slider_id) {
    for (const SliderBinding& binding : kGraphicsSliders) {
        if (binding.slider_id == slider_id) {
            return &binding;
        }
    }
    return nullptr;
}

int sliderPositionFromValue(const SliderBinding& binding, float value) {
    const float t = (std::clamp(value, binding.min_value, binding.max_value) - binding.min_value) /
                    std::max(binding.max_value - binding.min_value, 0.0001f);
    return static_cast<int>(std::lround(t * static_cast<float>(kSliderSteps)));
}

float sliderValueFromPosition(const SliderBinding& binding, int position) {
    const float t = std::clamp(static_cast<float>(position) / static_cast<float>(kSliderSteps), 0.0f, 1.0f);
    return binding.min_value + (binding.max_value - binding.min_value) * t;
}

void setSliderFloat(HWND window, int slider_id, float value) {
    const SliderBinding* binding = findSliderBindingBySlider(slider_id);
    if (!binding) return;
    SendDlgItemMessageW(window, slider_id, TBM_SETPOS, TRUE, sliderPositionFromValue(*binding, value));
}

void setEditAndSliderFloat(HWND window, int edit_id, int slider_id, float value) {
    setEditFloat(window, edit_id, value);
    setSliderFloat(window, slider_id, value);
}

void createSlider(HWND window, int id, int x, int y, int w) {
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS,
                                  x, y, w, 28, window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderSteps));
    SendMessageW(slider, TBM_SETTICFREQ, 100, 0);
}

void createCheckbox(HWND window, int id, const wchar_t* text, int x, int y, int w = 150) {
    CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    x, y, w, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
}

void createVec3Row(HWND window, const wchar_t* label, int y, int r_id, int g_id, int b_id,
                   const wchar_t* a = L"R", const wchar_t* b = L"G", const wchar_t* c = L"B") {
    createLabel(window, label, 12, y + 3, 110, 18);
    createLabel(window, a, 128, y + 3, 16, 18);
    createEdit(window, r_id, 146, y);
    createLabel(window, b, 226, y + 3, 16, 18);
    createEdit(window, g_id, 244, y);
    createLabel(window, c, 324, y + 3, 16, 18);
    createEdit(window, b_id, 342, y);
}

void populateLightingControls(HWND window, const EnvironmentState& env) {
    setEditFloat(window, kEditAmbientR, env.ambient.x);
    setEditFloat(window, kEditAmbientG, env.ambient.y);
    setEditFloat(window, kEditAmbientB, env.ambient.z);
    setEditFloat(window, kEditLightR, env.sun.color.x);
    setEditFloat(window, kEditLightG, env.sun.color.y);
    setEditFloat(window, kEditLightB, env.sun.color.z);
    setEditFloat(window, kEditLightX, env.sun.position.x);
    setEditFloat(window, kEditLightY, env.sun.position.y);
    setEditFloat(window, kEditLightZ, env.sun.position.z);
    setEditFloat(window, kEditSpecularR, env.specular.x);
    setEditFloat(window, kEditSpecularG, env.specular.y);
    setEditFloat(window, kEditSpecularB, env.specular.z);
    setEditFloat(window, kEditUpperR, env.upper_hemi.x);
    setEditFloat(window, kEditUpperG, env.upper_hemi.y);
    setEditFloat(window, kEditUpperB, env.upper_hemi.z);
    setEditFloat(window, kEditLowerR, env.lower_hemi.x);
    setEditFloat(window, kEditLowerG, env.lower_hemi.y);
    setEditFloat(window, kEditLowerB, env.lower_hemi.z);
    setEditFloat(window, kEditFogR, env.fog_color.x);
    setEditFloat(window, kEditFogG, env.fog_color.y);
    setEditFloat(window, kEditFogB, env.fog_color.z);
    setEditFloat(window, kEditFogNear, env.fog_near);
    setEditFloat(window, kEditFogFar, env.fog_far);
    SendDlgItemMessageW(window, kLightingFogEnabled, BM_SETCHECK, env.fog_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

void populateGraphicsControls(HWND window, const RenderFeatureSettings& features) {
    setCheckbox(window, kGraphicsMsaaEnabled, features.msaa.enabled);
    setEditUInt(window, kEditMsaaSamples, features.msaa.samples);
    setCheckbox(window, kGraphicsPbrEnabled, features.lego_surface_model == SurfaceModel::PBR);
    setEditAndSliderFloat(window, kEditPbrRoughness, kSliderPbrRoughness, features.pbr.roughness);
    setEditAndSliderFloat(window, kEditPbrMetallic, kSliderPbrMetallic, features.pbr.metallic);
    setEditAndSliderFloat(window, kEditPbrSpecular, kSliderPbrSpecular, features.pbr.specular_intensity);
    setCheckbox(window, kGraphicsSsrEnabled, features.screen_space.ssr_enabled);
    setEditAndSliderFloat(window, kEditSsrStrength, kSliderSsrStrength, features.screen_space.ssr_strength);
    setEditAndSliderFloat(window, kEditSsrMaxDistance, kSliderSsrMaxDistance, features.screen_space.ssr_max_distance);
    setEditAndSliderFloat(window, kEditSsrThickness, kSliderSsrThickness, features.screen_space.ssr_thickness);
    setCheckbox(window, kGraphicsGtaoEnabled, features.screen_space.gtao_enabled);
    setEditAndSliderFloat(window, kEditGtaoRadius, kSliderGtaoRadius, features.screen_space.gtao_radius);
    setEditAndSliderFloat(window, kEditGtaoIntensity, kSliderGtaoIntensity, features.screen_space.gtao_intensity);
    setCheckbox(window, kGraphicsBloomEnabled, features.post.bloom_enabled);
    setEditAndSliderFloat(window, kEditBloomThreshold, kSliderBloomThreshold, features.post.bloom_threshold);
    setEditAndSliderFloat(window, kEditBloomIntensity, kSliderBloomIntensity, features.post.bloom_intensity);
    setCheckbox(window, kGraphicsColorLutEnabled, features.post.color_lut_enabled);
    setEditAndSliderFloat(window, kEditColorLutIntensity, kSliderColorLutIntensity, features.post.color_lut_intensity);
    setEditString(window, kEditColorLutPath, features.post.color_lut_path);
    setCheckbox(window, kGraphicsVignetteEnabled, features.post.vignette_enabled);
    setEditAndSliderFloat(window, kEditVignetteStrength, kSliderVignetteStrength, features.post.vignette_strength);
    setCheckbox(window, kGraphicsDofEnabled, features.post.dof_enabled);
    setEditAndSliderFloat(window, kEditDofFocus, kSliderDofFocus, features.post.dof_focus_distance);
    setEditAndSliderFloat(window, kEditDofAperture, kSliderDofAperture, features.post.dof_aperture);
    setCheckbox(window, kGraphicsFilmGrainEnabled, features.post.film_grain_enabled);
    setEditAndSliderFloat(window, kEditFilmGrainStrength, kSliderFilmGrainStrength, features.post.film_grain_strength);
    setCheckbox(window, kGraphicsTaaEnabled, features.post.taa_enabled);
    setEditAndSliderFloat(window, kEditTaaFeedback, kSliderTaaFeedback, features.post.taa_feedback);
    setEditAndSliderFloat(window, kEditTaaJitter, kSliderTaaJitter, features.post.taa_jitter);
    setCheckbox(window, kGraphicsShadowsEnabled, features.shadows.directional_shadows_enabled);
    setEditAndSliderFloat(window, kEditPcssRadius, kSliderPcssRadius, features.shadows.pcss_light_radius);
    setEditAndSliderFloat(window, kEditPcssBias, kSliderPcssBias, features.shadows.pcss_bias);
    setEditAndSliderFloat(window, kEditPcssNormalBias, kSliderPcssNormalBias, features.shadows.pcss_normal_bias);
    setEditAndSliderFloat(window, kEditPcssSlopeBias, kSliderPcssSlopeBias, features.shadows.pcss_slope_bias);
    setCheckbox(window, kGraphicsProbesEnabled, features.reflection_probe.enabled);
    setEditAndSliderFloat(window, kEditProbeIntensity, kSliderProbeIntensity, features.reflection_probe.intensity);
}

void applyLightingControls(HWND window, AppState& app) {
    if (!app.world || !app.renderer) return;
    EnvironmentState env = app.world->environment;
    env.ambient = {
        getEditFloat(window, kEditAmbientR, env.ambient.x),
        getEditFloat(window, kEditAmbientG, env.ambient.y),
        getEditFloat(window, kEditAmbientB, env.ambient.z)
    };
    env.sun.color = {
        getEditFloat(window, kEditLightR, env.sun.color.x),
        getEditFloat(window, kEditLightG, env.sun.color.y),
        getEditFloat(window, kEditLightB, env.sun.color.z)
    };
    env.sun.position = {
        getEditFloat(window, kEditLightX, env.sun.position.x),
        getEditFloat(window, kEditLightY, env.sun.position.y),
        getEditFloat(window, kEditLightZ, env.sun.position.z)
    };
    env.sun.direction = lu::renderer::normalize(env.sun.position);
    env.specular = {
        getEditFloat(window, kEditSpecularR, env.specular.x),
        getEditFloat(window, kEditSpecularG, env.specular.y),
        getEditFloat(window, kEditSpecularB, env.specular.z)
    };
    env.upper_hemi = {
        getEditFloat(window, kEditUpperR, env.upper_hemi.x),
        getEditFloat(window, kEditUpperG, env.upper_hemi.y),
        getEditFloat(window, kEditUpperB, env.upper_hemi.z)
    };
    env.lower_hemi = {
        getEditFloat(window, kEditLowerR, env.lower_hemi.x),
        getEditFloat(window, kEditLowerG, env.lower_hemi.y),
        getEditFloat(window, kEditLowerB, env.lower_hemi.z)
    };
    env.fog_color = {
        getEditFloat(window, kEditFogR, env.fog_color.x),
        getEditFloat(window, kEditFogG, env.fog_color.y),
        getEditFloat(window, kEditFogB, env.fog_color.z)
    };
    env.fog_near = getEditFloat(window, kEditFogNear, env.fog_near);
    env.fog_far = getEditFloat(window, kEditFogFar, env.fog_far);
    env.fog_enabled = SendDlgItemMessageW(window, kLightingFogEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;

    app.world->environment = env;
    app.renderer->setEnvironment(env);
    app.manual_environment_override = true;
}

void applyGraphicsControls(HWND window, AppState& app, bool refresh_controls = true) {
    RenderFeatureSettings features = app.features;
    features.msaa.enabled = getCheckbox(window, kGraphicsMsaaEnabled);
    features.msaa.samples = static_cast<uint8_t>(std::clamp(getEditUInt(window, kEditMsaaSamples, features.msaa.samples), 1u, 16u));
    features.lego_surface_model = getCheckbox(window, kGraphicsPbrEnabled) ? SurfaceModel::PBR : SurfaceModel::LegacyLU;
    features.pbr.roughness = std::clamp(getEditFloat(window, kEditPbrRoughness, features.pbr.roughness), 0.04f, 1.0f);
    features.pbr.metallic = std::clamp(getEditFloat(window, kEditPbrMetallic, features.pbr.metallic), 0.0f, 1.0f);
    features.pbr.specular_intensity = std::max(0.0f, getEditFloat(window, kEditPbrSpecular, features.pbr.specular_intensity));
    features.screen_space.ssr_enabled = getCheckbox(window, kGraphicsSsrEnabled);
    features.screen_space.ssr_strength = getEditFloat(window, kEditSsrStrength, features.screen_space.ssr_strength);
    features.screen_space.ssr_max_distance = std::max(0.1f, getEditFloat(window, kEditSsrMaxDistance, features.screen_space.ssr_max_distance));
    features.screen_space.ssr_thickness = std::max(0.001f, getEditFloat(window, kEditSsrThickness, features.screen_space.ssr_thickness));
    features.screen_space.gtao_enabled = getCheckbox(window, kGraphicsGtaoEnabled);
    features.screen_space.gtao_radius = getEditFloat(window, kEditGtaoRadius, features.screen_space.gtao_radius);
    features.screen_space.gtao_intensity = getEditFloat(window, kEditGtaoIntensity, features.screen_space.gtao_intensity);
    features.post.bloom_enabled = getCheckbox(window, kGraphicsBloomEnabled);
    features.post.bloom_threshold = getEditFloat(window, kEditBloomThreshold, features.post.bloom_threshold);
    features.post.bloom_intensity = getEditFloat(window, kEditBloomIntensity, features.post.bloom_intensity);
    features.post.color_lut_enabled = getCheckbox(window, kGraphicsColorLutEnabled);
    features.post.color_lut_intensity = std::max(0.0f, getEditFloat(window, kEditColorLutIntensity, features.post.color_lut_intensity));
    features.post.color_lut_path = getEditString(window, kEditColorLutPath, features.post.color_lut_path);
    features.post.vignette_enabled = getCheckbox(window, kGraphicsVignetteEnabled);
    features.post.vignette_strength = getEditFloat(window, kEditVignetteStrength, features.post.vignette_strength);
    features.post.dof_enabled = getCheckbox(window, kGraphicsDofEnabled);
    features.post.dof_focus_distance = getEditFloat(window, kEditDofFocus, features.post.dof_focus_distance);
    features.post.dof_aperture = getEditFloat(window, kEditDofAperture, features.post.dof_aperture);
    features.post.film_grain_enabled = getCheckbox(window, kGraphicsFilmGrainEnabled);
    features.post.film_grain_strength = getEditFloat(window, kEditFilmGrainStrength, features.post.film_grain_strength);
    features.post.taa_enabled = getCheckbox(window, kGraphicsTaaEnabled);
    features.post.taa_feedback = std::clamp(getEditFloat(window, kEditTaaFeedback, features.post.taa_feedback), 0.0f, 0.98f);
    features.post.taa_jitter = std::clamp(getEditFloat(window, kEditTaaJitter, features.post.taa_jitter), 0.0f, 2.0f);
    features.shadows.directional_shadows_enabled = getCheckbox(window, kGraphicsShadowsEnabled);
    features.shadows.pcss_light_radius = std::max(0.0f, getEditFloat(window, kEditPcssRadius, features.shadows.pcss_light_radius));
    features.shadows.pcss_bias = std::clamp(getEditFloat(window, kEditPcssBias, features.shadows.pcss_bias), 0.0f, 0.01f);
    features.shadows.pcss_normal_bias = std::clamp(getEditFloat(window, kEditPcssNormalBias, features.shadows.pcss_normal_bias), 0.0f, 8.0f);
    features.shadows.pcss_slope_bias = std::clamp(getEditFloat(window, kEditPcssSlopeBias, features.shadows.pcss_slope_bias), 0.0f, 12.0f);
    features.reflection_probe.enabled = getCheckbox(window, kGraphicsProbesEnabled);
    features.reflection_probe.intensity = getEditFloat(window, kEditProbeIntensity, features.reflection_probe.intensity);

    app.features = features;
    if (app.renderer) {
        app.renderer->setFeatureSettings(app.features);
    }
    if (refresh_controls) {
        populateGraphicsControls(window, app.features);
    }
}

void applySliderControl(HWND window, AppState& app, HWND slider_window) {
    const int slider_id = GetDlgCtrlID(slider_window);
    const SliderBinding* binding = findSliderBindingBySlider(slider_id);
    if (!binding) return;

    const int position = static_cast<int>(SendMessageW(slider_window, TBM_GETPOS, 0, 0));
    setEditFloat(window, binding->edit_id, sliderValueFromPosition(*binding, position));
    applyGraphicsControls(window, app, false);
}

LRESULT CALLBACK lightingWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

        createVec3Row(hwnd, L"Ambient", 14, kEditAmbientR, kEditAmbientG, kEditAmbientB);
        createVec3Row(hwnd, L"Light Color", 44, kEditLightR, kEditLightG, kEditLightB);
        createVec3Row(hwnd, L"Light Position", 74, kEditLightX, kEditLightY, kEditLightZ, L"X", L"Y", L"Z");
        createVec3Row(hwnd, L"Specular", 104, kEditSpecularR, kEditSpecularG, kEditSpecularB);
        createVec3Row(hwnd, L"Upper Hemi", 134, kEditUpperR, kEditUpperG, kEditUpperB);
        createVec3Row(hwnd, L"Lower Hemi", 164, kEditLowerR, kEditLowerG, kEditLowerB);
        CreateWindowExW(0, L"BUTTON", L"Fog Enabled", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                        12, 198, 120, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLightingFogEnabled)),
                        GetModuleHandleW(nullptr), nullptr);
        createVec3Row(hwnd, L"Fog Color", 228, kEditFogR, kEditFogG, kEditFogB);
        createLabel(hwnd, L"Fog Range", 12, 261, 110, 18);
        createLabel(hwnd, L"Near", 128, 261, 34, 18);
        createEdit(hwnd, kEditFogNear, 166, 258);
        createLabel(hwnd, L"Far", 250, 261, 28, 18);
        createEdit(hwnd, kEditFogFar, 284, 258);
        createLabel(hwnd, L"Graphics", 12, 338, 110, 18);
        createCheckbox(hwnd, kGraphicsMsaaEnabled, L"MSAA", 12, 366, 75);
        createLabel(hwnd, L"Samples", 128, 369, 56, 18);
        createEdit(hwnd, kEditMsaaSamples, 190, 366);
        createCheckbox(hwnd, kGraphicsPbrEnabled, L"PBR Lego BRDF", 282, 366, 140);
        createLabel(hwnd, L"Rough", 12, 399, 80, 18);
        createEdit(hwnd, kEditPbrRoughness, 128, 396);
        createSlider(hwnd, kSliderPbrRoughness, 210, 393, 250);
        createLabel(hwnd, L"Metal", 12, 429, 80, 18);
        createEdit(hwnd, kEditPbrMetallic, 128, 426);
        createSlider(hwnd, kSliderPbrMetallic, 210, 423, 250);
        createLabel(hwnd, L"Spec", 12, 459, 80, 18);
        createEdit(hwnd, kEditPbrSpecular, 128, 456);
        createSlider(hwnd, kSliderPbrSpecular, 210, 453, 250);
        createCheckbox(hwnd, kGraphicsSsrEnabled, L"SSR", 12, 486, 75);
        createLabel(hwnd, L"Strength", 128, 489, 72, 18);
        createEdit(hwnd, kEditSsrStrength, 210, 486);
        createSlider(hwnd, kSliderSsrStrength, 292, 483, 250);
        createLabel(hwnd, L"Max Dist", 128, 519, 72, 18);
        createEdit(hwnd, kEditSsrMaxDistance, 210, 516);
        createSlider(hwnd, kSliderSsrMaxDistance, 292, 513, 250);
        createLabel(hwnd, L"Thickness", 128, 549, 72, 18);
        createEdit(hwnd, kEditSsrThickness, 210, 546);
        createSlider(hwnd, kSliderSsrThickness, 292, 543, 250);
        createCheckbox(hwnd, kGraphicsGtaoEnabled, L"GTAO", 12, 576, 75);
        createLabel(hwnd, L"Radius", 128, 579, 72, 18);
        createEdit(hwnd, kEditGtaoRadius, 210, 576);
        createSlider(hwnd, kSliderGtaoRadius, 292, 573, 250);
        createLabel(hwnd, L"AO Int", 128, 609, 72, 18);
        createEdit(hwnd, kEditGtaoIntensity, 210, 606);
        createSlider(hwnd, kSliderGtaoIntensity, 292, 603, 250);
        createCheckbox(hwnd, kGraphicsBloomEnabled, L"Bloom", 12, 636, 75);
        createLabel(hwnd, L"Threshold", 128, 639, 72, 18);
        createEdit(hwnd, kEditBloomThreshold, 210, 636);
        createSlider(hwnd, kSliderBloomThreshold, 292, 633, 250);
        createLabel(hwnd, L"Intensity", 128, 669, 72, 18);
        createEdit(hwnd, kEditBloomIntensity, 210, 666);
        createSlider(hwnd, kSliderBloomIntensity, 292, 663, 250);
        createCheckbox(hwnd, kGraphicsColorLutEnabled, L"LUT", 12, 696, 60);
        createLabel(hwnd, L"Intensity", 128, 699, 72, 18);
        createEdit(hwnd, kEditColorLutIntensity, 210, 696);
        createSlider(hwnd, kSliderColorLutIntensity, 292, 693, 250);
        createLabel(hwnd, L"LUT Path", 128, 729, 72, 18);
        createEditWide(hwnd, kEditColorLutPath, 210, 726, 252);
        CreateWindowExW(0, L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        470, 725, 72, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGraphicsBrowseColorLut)),
                        GetModuleHandleW(nullptr), nullptr);
        createCheckbox(hwnd, kGraphicsVignetteEnabled, L"Vignette", 12, 756, 100);
        createLabel(hwnd, L"Strength", 128, 759, 72, 18);
        createEdit(hwnd, kEditVignetteStrength, 210, 756);
        createSlider(hwnd, kSliderVignetteStrength, 292, 753, 250);
        createCheckbox(hwnd, kGraphicsDofEnabled, L"DoF", 12, 786, 60);
        createLabel(hwnd, L"Focus", 128, 789, 72, 18);
        createEdit(hwnd, kEditDofFocus, 210, 786);
        createSlider(hwnd, kSliderDofFocus, 292, 783, 250);
        createLabel(hwnd, L"Aperture", 128, 819, 72, 18);
        createEdit(hwnd, kEditDofAperture, 210, 816);
        createSlider(hwnd, kSliderDofAperture, 292, 813, 250);
        createCheckbox(hwnd, kGraphicsFilmGrainEnabled, L"Film Grain", 12, 846, 100);
        createLabel(hwnd, L"Strength", 128, 849, 72, 18);
        createEdit(hwnd, kEditFilmGrainStrength, 210, 846);
        createSlider(hwnd, kSliderFilmGrainStrength, 292, 843, 250);
        createCheckbox(hwnd, kGraphicsTaaEnabled, L"TAA", 12, 876, 75);
        createLabel(hwnd, L"Feedback", 128, 879, 72, 18);
        createEdit(hwnd, kEditTaaFeedback, 210, 876);
        createSlider(hwnd, kSliderTaaFeedback, 292, 873, 250);
        createLabel(hwnd, L"Jitter", 128, 909, 72, 18);
        createEdit(hwnd, kEditTaaJitter, 210, 906);
        createSlider(hwnd, kSliderTaaJitter, 292, 903, 250);
        createCheckbox(hwnd, kGraphicsShadowsEnabled, L"PCSS Shadows", 12, 936, 120);
        createLabel(hwnd, L"Radius", 128, 939, 72, 18);
        createEdit(hwnd, kEditPcssRadius, 210, 936);
        createSlider(hwnd, kSliderPcssRadius, 292, 933, 250);
        createLabel(hwnd, L"Bias", 128, 969, 72, 18);
        createEdit(hwnd, kEditPcssBias, 210, 966);
        createSlider(hwnd, kSliderPcssBias, 292, 963, 250);
        createLabel(hwnd, L"Normal Bias", 128, 999, 90, 18);
        createEdit(hwnd, kEditPcssNormalBias, 210, 996);
        createSlider(hwnd, kSliderPcssNormalBias, 292, 993, 250);
        createLabel(hwnd, L"Slope Bias", 128, 1029, 90, 18);
        createEdit(hwnd, kEditPcssSlopeBias, 210, 1026);
        createSlider(hwnd, kSliderPcssSlopeBias, 292, 1023, 250);
        createCheckbox(hwnd, kGraphicsProbesEnabled, L"Reflection Probe", 12, 1056, 135);
        createLabel(hwnd, L"Intensity", 166, 1059, 72, 18);
        createEdit(hwnd, kEditProbeIntensity, 248, 1056);
        createSlider(hwnd, kSliderProbeIntensity, 330, 1053, 212);
        CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        368, 1096, 82, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLightingApply)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        460, 1096, 82, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLightingClose)),
                        GetModuleHandleW(nullptr), nullptr);
        if (app && app->world) populateLightingControls(hwnd, app->world->environment);
        if (app) populateGraphicsControls(hwnd, app->features);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kLightingApply) {
            if (app) {
                applyLightingControls(hwnd, *app);
                applyGraphicsControls(hwnd, *app);
            }
            return 0;
        }
        if (LOWORD(wparam) == kLightingClose) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == kGraphicsBrowseColorLut) {
            if (app && app->args) {
                std::filesystem::path selected = openColorLutDialog(hwnd, *app->args, app->features.post.color_lut_path);
                if (!selected.empty()) {
                    setCheckbox(hwnd, kGraphicsColorLutEnabled, true);
                    setEditString(hwnd, kEditColorLutPath, selected.string());
                    applyGraphicsControls(hwnd, *app, false);
                }
            }
            return 0;
        }
        if (app && HIWORD(wparam) == BN_CLICKED) {
            switch (LOWORD(wparam)) {
            case kGraphicsMsaaEnabled:
            case kGraphicsPbrEnabled:
            case kGraphicsSsrEnabled:
            case kGraphicsGtaoEnabled:
            case kGraphicsBloomEnabled:
            case kGraphicsColorLutEnabled:
            case kGraphicsVignetteEnabled:
            case kGraphicsDofEnabled:
            case kGraphicsFilmGrainEnabled:
            case kGraphicsTaaEnabled:
            case kGraphicsShadowsEnabled:
            case kGraphicsProbesEnabled:
                applyGraphicsControls(hwnd, *app, false);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_HSCROLL:
        if (app && reinterpret_cast<HWND>(lparam)) {
            applySliderControl(hwnd, *app, reinterpret_cast<HWND>(lparam));
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app && app->lighting_window == hwnd) app->lighting_window = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void showLightingWindow(HWND owner, AppState& app) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    if (app.lighting_window) {
        if (app.world) populateLightingControls(app.lighting_window, app.world->environment);
        populateGraphicsControls(app.lighting_window, app.features);
        ShowWindow(app.lighting_window, SW_SHOWNORMAL);
        SetForegroundWindow(app.lighting_window);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = lightingWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LuRendererLightingFogWindow";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    app.lighting_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"LuRendererLightingFogWindow",
        L"Lighting / Fog / Graphics",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 1180,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        &app);
    if (app.lighting_window) {
        ShowWindow(app.lighting_window, SW_SHOWNORMAL);
    }
}

void updateViewMenuChecks(AppState& app) {
    if (!app.view_menu || !app.camera) return;
    const UINT checked = app.camera->mode() == CameraMode::Fly
        ? kMenuViewFlyCamera
        : kMenuViewOrbitCamera;
    CheckMenuRadioItem(
        app.view_menu,
        kMenuViewOrbitCamera,
        kMenuViewFlyCamera,
        checked,
        MF_BYCOMMAND);
}

LRESULT CALLBACK viewerWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = static_cast<AppState*>(GetPropW(hwnd, kAppStateProperty));
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case kMenuFileImportNif:
            if (app && app->args) {
                std::filesystem::path selected = openNifDialog(hwnd, *app->args);
                if (!selected.empty()) {
                    app->pending_import = selected;
                    app->import_requested = true;
                }
            }
            return 0;
        case kMenuFileImportLvl:
            if (app && app->args) {
                std::filesystem::path selected = openLvlDialog(hwnd, *app->args);
                if (!selected.empty()) {
                    app->pending_lvl_import = selected;
                    app->lvl_import_requested = true;
                }
            }
            return 0;
        case kMenuFileImportAnimation:
            if (app && app->args) {
                std::filesystem::path selected = openAnimationDialog(hwnd, *app->args);
                if (!selected.empty()) {
                    app->pending_animation_import = selected;
                    app->animation_import_requested = true;
                }
            }
            return 0;
        case kMenuViewOrbitCamera:
            if (app && app->camera) {
                app->camera->setMode(CameraMode::Orbit);
                updateViewMenuChecks(*app);
            }
            return 0;
        case kMenuViewFlyCamera:
            if (app && app->camera) {
                app->camera->setMode(CameraMode::Fly);
                updateViewMenuChecks(*app);
            }
            return 0;
        case kMenuViewResetCamera:
            if (app && app->world && app->camera) {
                resetCameraToWorld(*app->world, *app->camera);
            }
            return 0;
        case kMenuViewLightingFog:
            if (app) showLightingWindow(hwnd, *app);
            return 0;
        default:
            break;
        }
    }

    if (app && app->original_wnd_proc) {
        return CallWindowProcW(app->original_wnd_proc, hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void installNativeMenu(GLFWwindow* window, AppState& app) {
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;

    HMENU menu_bar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    app.view_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, kMenuFileImportNif, L"Import NIF...");
    AppendMenuW(file_menu, MF_STRING, kMenuFileImportLvl, L"Import LVL Environment...");
    AppendMenuW(file_menu, MF_STRING, kMenuFileImportAnimation, L"Import Animation...");
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"File");
    AppendMenuW(app.view_menu, MF_STRING, kMenuViewOrbitCamera, L"Orbit Camera");
    AppendMenuW(app.view_menu, MF_STRING, kMenuViewFlyCamera, L"Fly Camera");
    AppendMenuW(app.view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(app.view_menu, MF_STRING, kMenuViewResetCamera, L"Reset Camera");
    AppendMenuW(app.view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(app.view_menu, MF_STRING, kMenuViewLightingFog, L"Lighting / Fog...");
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(app.view_menu), L"View");
    SetMenu(hwnd, menu_bar);
    updateViewMenuChecks(app);

    SetPropW(hwnd, kAppStateProperty, &app);
    app.original_wnd_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(viewerWndProc)));
}

void uninstallNativeMenu(GLFWwindow* window, AppState& app) {
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;
    if (app.lighting_window) {
        DestroyWindow(app.lighting_window);
        app.lighting_window = nullptr;
    }
    if (app.original_wnd_proc) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(app.original_wnd_proc));
        app.original_wnd_proc = nullptr;
    }
    RemovePropW(hwnd, kAppStateProperty);
    app.view_menu = nullptr;
}
#else
void installNativeMenu(GLFWwindow*, AppState&) {}
void uninstallNativeMenu(GLFWwindow*, AppState&) {}
#endif

void* nativeWindow(GLFWwindow* window) {
#if defined(_WIN32)
    return glfwGetWin32Window(window);
#elif defined(__APPLE__)
    return glfwGetCocoaWindow(window);
#else
    return reinterpret_cast<void*>(glfwGetX11Window(window));
#endif
}

void* nativeDisplay() {
#if defined(__linux__)
    return glfwGetX11Display();
#else
    return nullptr;
#endif
}

void cursorPosCallback(GLFWwindow* window, double x, double y) {
    auto* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app || !app->input.camera) return;
    InputState& input = app->input;
    double dx = x - input.last_x;
    double dy = y - input.last_y;
    input.last_x = x;
    input.last_y = y;

    if (input.camera->mode() == CameraMode::Fly) {
        if (input.right_down) input.camera->flyLook(static_cast<float>(dx), static_cast<float>(dy));
        return;
    }

    if (input.right_down) input.camera->orbit(static_cast<float>(dx), static_cast<float>(dy));
    if (input.middle_down) input.camera->pan(static_cast<float>(dx), static_cast<float>(dy));
}

void updateCursorCapture(GLFWwindow* window, const InputState& input) {
    const bool dragging_view = input.right_down || input.middle_down;
    glfwSetInputMode(window, GLFW_CURSOR, dragging_view ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    auto* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app) return;
    InputState& input = app->input;
    if (button == GLFW_MOUSE_BUTTON_LEFT) input.left_down = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) input.middle_down = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) input.right_down = action == GLFW_PRESS;
    glfwGetCursorPos(window, &input.last_x, &input.last_y);
    updateCursorCapture(window, input);
}

void scrollCallback(GLFWwindow* window, double, double yoffset) {
    auto* app = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!app || !app->input.camera) return;
    if (app->input.camera->mode() == CameraMode::Fly) {
        const float amount = static_cast<float>(yoffset) * std::max(1.0f, app->input.camera->distance() * 0.08f);
        app->input.camera->flyMove({0.0f, 0.0f, amount});
    } else {
        app->input.camera->zoom(static_cast<float>(yoffset));
    }
}

bool keyDown(GLFWwindow* window, int key) {
    return glfwGetKey(window, key) == GLFW_PRESS;
}

void updateFlyCamera(GLFWwindow* window, OrbitCamera& camera, float dt) {
    if (camera.mode() != CameraMode::Fly) return;

    Vec3 local_delta{};
    if (keyDown(window, GLFW_KEY_A)) local_delta.x += 1.0f;
    if (keyDown(window, GLFW_KEY_D)) local_delta.x -= 1.0f;
    if (keyDown(window, GLFW_KEY_Q)) local_delta.y -= 1.0f;
    if (keyDown(window, GLFW_KEY_E)) local_delta.y += 1.0f;
    if (keyDown(window, GLFW_KEY_W)) local_delta.z += 1.0f;
    if (keyDown(window, GLFW_KEY_S)) local_delta.z -= 1.0f;

    if (local_delta.x == 0.0f && local_delta.y == 0.0f && local_delta.z == 0.0f) return;
    local_delta = lu::renderer::normalize(local_delta);

    float speed = std::max(1.0f, camera.distance() * 0.7f);
    if (keyDown(window, GLFW_KEY_LEFT_SHIFT) || keyDown(window, GLFW_KEY_RIGHT_SHIFT)) {
        speed *= 4.0f;
    }
    camera.flyMove(local_delta * (speed * dt));
}

} // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (args.hidden) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    const std::string initial_title = "LU NIF Viewer [" + viewerBuildIdentity() + "]";
    GLFWwindow* window = glfwCreateWindow(1280, 720, initial_title.c_str(), nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return 1;
    }

    int width = 1280;
    int height = 720;
    glfwGetFramebufferSize(window, &width, &height);

    ViewerBgfxCallback callback;
    BgfxRenderer renderer;
    RenderFeatureSettings features = args.features;
    RendererInit init;
    init.native_window = nativeWindow(window);
    init.native_display = nativeDisplay();
    init.callback = &callback;
    init.width = static_cast<uint32_t>(width);
    init.height = static_cast<uint32_t>(height);
    init.bgfx_device_debug = args.bgfx_device_debug;
    init.features = features;
    if (!renderer.init(init)) {
        std::cerr << renderer.lastError() << "\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    RenderWorld world;
    if (args.shadow_test_scene) {
        std::cout << "Showing synthetic shadow test scene.\n";
        world = makeShadowTestWorld();
    } else if (args.transparent_test_scene) {
        std::cout << "Showing synthetic transparent test scene.\n";
        world = makeTransparentTestWorld();
    } else if (!args.nif_path.empty()) {
        if (!loadNifWorld(args.nif_path, args, world)) {
            world = makeCubeWorld();
        }
    } else {
        std::cout << "No --nif supplied; showing fallback cube.\n";
        world = makeCubeWorld();
    }
    if (!args.lvl_path.empty()) {
        loadLvlEnvironment(args.lvl_path, world);
    }
    if (!args.animation_path.empty()) {
        loadAnimation(args.animation_path, args, world);
    }

    OrbitCamera camera;
    resetCameraToWorld(world, camera);
    if (args.camera_target) {
        camera.setTarget(*args.camera_target);
    }
    if (args.camera_distance) {
        camera.setDistance(*args.camera_distance);
        camera.syncFlyToOrbit();
    }
    renderer.loadWorld(world);
    setViewerTitle(window, args.nif_path);

    AppState app;
    app.args = &args;
    app.renderer = &renderer;
    app.world = &world;
    app.camera = &camera;
    app.features = features;
    app.input.camera = &camera;
    glfwSetWindowUserPointer(window, &app);
    if (!args.hidden) {
        installNativeMenu(window, app);
    }
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetScrollCallback(window, scrollCallback);

    uint32_t rendered_frames = 0;
    bool screenshot_requested = false;
    const std::string screenshot_path = args.screenshot_path.string();
    auto last_frame_time = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last_frame_time).count();
        last_frame_time = now;
        updateFlyCamera(window, camera, std::min(dt, 0.1f));

        int current_width = 0;
        int current_height = 0;
        glfwGetFramebufferSize(window, &current_width, &current_height);
        if (current_width != width || current_height != height) {
            width = std::max(current_width, 1);
            height = std::max(current_height, 1);
            renderer.resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
        if (app.import_requested) {
            app.import_requested = false;
            const EnvironmentState previous_environment = world.environment;
            const AnimationAsset previous_animation = world.animation;
            if (loadNifWorld(app.pending_import, args, world)) {
                if (app.manual_environment_override) {
                    world.environment = previous_environment;
                } else if (!args.lvl_path.empty()) {
                    loadLvlEnvironment(args.lvl_path, world);
                }
                world.animation = previous_animation;
                resetCameraToWorld(world, camera);
                renderer.loadWorld(world);
                setViewerTitle(window, args.nif_path);
                if (app.lighting_window) populateLightingControls(app.lighting_window, world.environment);
            }
            app.pending_import.clear();
        }
        if (app.lvl_import_requested) {
            app.lvl_import_requested = false;
            if (loadLvlEnvironment(app.pending_lvl_import, world)) {
                args.lvl_path = app.pending_lvl_import;
                app.manual_environment_override = false;
                renderer.setEnvironment(world.environment);
                if (app.lighting_window) populateLightingControls(app.lighting_window, world.environment);
            }
            app.pending_lvl_import.clear();
        }
        if (app.animation_import_requested) {
            app.animation_import_requested = false;
            loadAnimation(app.pending_animation_import, args, world);
            app.pending_animation_import.clear();
        }
        renderer.render(camera);
        ++rendered_frames;
        if (!screenshot_path.empty() && !screenshot_requested && rendered_frames >= 3) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, screenshot_path.c_str());
            screenshot_requested = true;
        }
        if (screenshot_requested && callback.screenshotWritten()) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (args.exit_after_frames > 0 && rendered_frames >= args.exit_after_frames) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }

    uninstallNativeMenu(window, app);
    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
