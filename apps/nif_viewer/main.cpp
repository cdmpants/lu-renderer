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
#include "lu/renderer/lu_import/lvl_environment_importer.h"
#include "lu/renderer/lu_import/nif_importer.h"
#include "lu/renderer/render_types.h"
#include "lu/renderer_bgfx/bgfx_renderer.h"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(_WIN32)
#include <commdlg.h>
#include <windows.h>
#endif

#include <algorithm>
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
#include <string>

using lu::renderer::MeshAsset;
using lu::renderer::CameraMode;
using lu::renderer::EnvironmentState;
using lu::renderer::LegacyShaderFamily;
using lu::renderer::OrbitCamera;
using lu::renderer::RenderAlphaMode;
using lu::renderer::RenderCullMode;
using lu::renderer::RenderWorld;
using lu::renderer::Vec3;
using lu::renderer::Vertex;
using lu::renderer::bgfx_backend::BgfxRenderer;
using lu::renderer::bgfx_backend::RendererInit;

namespace {

struct Args {
    std::filesystem::path client_root;
    std::filesystem::path lvl_path;
    std::filesystem::path nif_path;
    std::filesystem::path screenshot_path;
    uint32_t exit_after_frames = 0;
    bool hidden = false;
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
    InputState input;
    std::filesystem::path pending_import;
    std::filesystem::path pending_lvl_import;
    bool import_requested = false;
    bool lvl_import_requested = false;
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
        } else if (arg == "--nif" && i + 1 < argc) {
            args.nif_path = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            args.screenshot_path = argv[++i];
        } else if ((arg == "--exit-after-frames" || arg == "--frames") && i + 1 < argc) {
            args.exit_after_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--hidden") {
            args.hidden = true;
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

void printShaderDiagnostics(const RenderWorld& world) {
    std::cout << "Asset key: " << (world.source_asset_path.empty() ? "<debug>" : world.source_asset_path) << "\n";
    const size_t count = std::min<size_t>(world.meshes.size(), 8);
    for (size_t i = 0; i < count; ++i) {
        const auto& mesh = world.meshes[i];
        const auto& material = mesh.material;
        std::cout
            << "  [" << i << "] " << mesh.name
            << " shader=" << material.lu_shader_id
            << " gameValue=" << material.lu_shader_game_value
            << " label=\"" << material.lu_shader_label << "\""
            << " program=" << shaderFamilyName(material.shader_family)
            << " port=" << portStatusName(material.lu_shader_port_status)
            << " fx=\"" << material.lu_shader_source_file << "\""
            << " technique=\"" << material.lu_shader_source_technique << "\""
            << " resolved=" << boolText(material.lu_shader_resolved)
            << " multishader=" << boolText(material.lu_shader_asset_is_multishader)
            << " prefix=" << material.lu_multishader_prefix_id
            << " vc=" << boolText(material.lu_shader_uses_vertex_color)
            << " meshVC=" << boolText(material.mesh_has_vertex_colors)
            << " shaderTex=" << boolText(material.lu_shader_uses_texture)
            << " matDiffuse=" << boolText(material.lu_shader_uses_material_diffuse)
            << " fog=" << boolText(material.lu_shader_uses_fog)
            << " spec=" << boolText(material.lu_shader_uses_specular)
            << " refl=" << boolText(material.lu_shader_uses_reflection)
            << " env=\"" << material.lu_shader_reflection_map << "\""
            << " envSemantic=\"" << material.lu_shader_reflection_semantic << "\""
            << " uvAnim=" << boolText(material.lu_shader_uses_uv_animation)
            << " alphaAnim=" << boolText(material.lu_shader_uses_alpha_animation)
            << " motion1=" << material.lu_uv_motion_layer1.x << "/" << material.lu_uv_motion_layer1.y
            << " motion2=" << material.lu_uv_motion_layer2.x << "/" << material.lu_uv_motion_layer2.y
            << " alpha=" << alphaModeName(material.alpha_mode)
            << " test=" << boolText(material.alpha_test)
            << " blend=" << boolText(material.alpha_blend)
            << " cull=" << cullModeName(material.cull_mode)
            << " emissive=" << std::max({material.emissive.x, material.emissive.y, material.emissive.z})
            << " diffuse=" << material.diffuse.x << "/" << material.diffuse.y << "/"
            << material.diffuse.z << "/" << material.diffuse.w
            << " texture=" << boolText(!material.diffuse_texture_path.empty())
            << " lod=" << boolText(mesh.has_lod_range);
        if (mesh.has_lod_range) {
            std::cout
                << " lodParent=" << mesh.lod_parent_block
                << " lodLevel=" << mesh.lod_level
                << " lodRange=" << mesh.lod_near << "/" << mesh.lod_far
                << " lodCenter=" << mesh.lod_center.x << "/" << mesh.lod_center.y << "/"
                << mesh.lod_center.z;
        }
        std::cout
            << "\n";
    }
    if (world.meshes.size() > count) {
        std::cout << "  ... " << (world.meshes.size() - count) << " more mesh(es)\n";
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
    cube.vertices = {
        {{-s,-s,-s}, { 0, 0,-1}, {0,1}, c}, {{ s,-s,-s}, { 0, 0,-1}, {1,1}, c}, {{ s, s,-s}, { 0, 0,-1}, {1,0}, c}, {{-s, s,-s}, { 0, 0,-1}, {0,0}, c},
        {{-s,-s, s}, { 0, 0, 1}, {0,1}, c}, {{ s,-s, s}, { 0, 0, 1}, {1,1}, c}, {{ s, s, s}, { 0, 0, 1}, {1,0}, c}, {{-s, s, s}, { 0, 0, 1}, {0,0}, c},
        {{-s,-s,-s}, {-1, 0, 0}, {0,1}, c}, {{-s, s,-s}, {-1, 0, 0}, {1,1}, c}, {{-s, s, s}, {-1, 0, 0}, {1,0}, c}, {{-s,-s, s}, {-1, 0, 0}, {0,0}, c},
        {{ s,-s,-s}, { 1, 0, 0}, {0,1}, c}, {{ s, s,-s}, { 1, 0, 0}, {1,1}, c}, {{ s, s, s}, { 1, 0, 0}, {1,0}, c}, {{ s,-s, s}, { 1, 0, 0}, {0,0}, c},
        {{-s,-s,-s}, { 0,-1, 0}, {0,1}, c}, {{-s,-s, s}, { 0,-1, 0}, {1,1}, c}, {{ s,-s, s}, { 0,-1, 0}, {1,0}, c}, {{ s,-s,-s}, { 0,-1, 0}, {0,0}, c},
        {{-s, s,-s}, { 0, 1, 0}, {0,1}, c}, {{-s, s, s}, { 0, 1, 0}, {1,1}, c}, {{ s, s, s}, { 0, 1, 0}, {1,0}, c}, {{ s, s,-s}, { 0, 1, 0}, {0,0}, c}
    };
    cube.indices = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7,
        8,10,9, 8,11,10, 12,13,14, 12,14,15,
        16,18,17, 16,19,18, 20,21,22, 20,22,23
    };
    world.meshes.push_back(std::move(cube));
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

void setViewerTitle(GLFWwindow* window, const std::filesystem::path& nif_path) {
    std::string title = "LU Renderer - NIF Viewer";
    if (!nif_path.empty()) {
        title += " - ";
        title += nif_path.filename().string();
    }
    glfwSetWindowTitle(window, title.c_str());
}

#if defined(_WIN32)
constexpr UINT_PTR kMenuFileImportNif = 1001;
constexpr UINT_PTR kMenuFileImportLvl = 1002;
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

void createLabel(HWND window, const wchar_t* text, int x, int y, int w, int h) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                    x, y, w, h, window, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void createEdit(HWND window, int id, int x, int y) {
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    x, y, 72, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
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
        CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        246, 300, 82, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLightingApply)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        338, 300, 82, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLightingClose)),
                        GetModuleHandleW(nullptr), nullptr);
        if (app && app->world) populateLightingControls(hwnd, app->world->environment);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kLightingApply) {
            if (app) applyLightingControls(hwnd, *app);
            return 0;
        }
        if (LOWORD(wparam) == kLightingClose) {
            DestroyWindow(hwnd);
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
    if (app.lighting_window) {
        if (app.world) populateLightingControls(app.lighting_window, app.world->environment);
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
        L"Lighting / Fog",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 450, 380,
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
    GLFWwindow* window = glfwCreateWindow(1280, 720, "LU Renderer - NIF Viewer", nullptr, nullptr);
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
    RendererInit init;
    init.native_window = nativeWindow(window);
    init.native_display = nativeDisplay();
    init.callback = &callback;
    init.width = static_cast<uint32_t>(width);
    init.height = static_cast<uint32_t>(height);
    if (!renderer.init(init)) {
        std::cerr << renderer.lastError() << "\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    RenderWorld world;
    if (!args.nif_path.empty()) {
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

    OrbitCamera camera;
    resetCameraToWorld(world, camera);
    renderer.loadWorld(world);
    setViewerTitle(window, args.nif_path);

    AppState app;
    app.args = &args;
    app.renderer = &renderer;
    app.world = &world;
    app.camera = &camera;
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
            if (loadNifWorld(app.pending_import, args, world)) {
                if (app.manual_environment_override) {
                    world.environment = previous_environment;
                } else if (!args.lvl_path.empty()) {
                    loadLvlEnvironment(args.lvl_path, world);
                }
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
