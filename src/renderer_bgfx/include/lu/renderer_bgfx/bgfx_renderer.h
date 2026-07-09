#pragma once

#include "lu/renderer/camera.h"
#include "lu/renderer/render_types.h"

#include <bgfx/bgfx.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace lu::renderer::bgfx_backend {

struct RendererInit {
    void* native_window = nullptr;
    void* native_display = nullptr;
    bgfx::CallbackI* callback = nullptr;
    bgfx::NativeWindowHandleType::Enum native_window_type = bgfx::NativeWindowHandleType::Default;
    uint32_t width = 1280;
    uint32_t height = 720;
    RenderFeatureSettings features;
    std::filesystem::path shader_dir = LU_RENDERER_SHADER_DIR;
    std::filesystem::path reflection_map_dir = LU_RENDERER_REFLECTION_MAP_DIR;
};

class BgfxRenderer {
public:
    BgfxRenderer() = default;
    ~BgfxRenderer();

    BgfxRenderer(const BgfxRenderer&) = delete;
    BgfxRenderer& operator=(const BgfxRenderer&) = delete;

    bool init(const RendererInit& init);
    void shutdown();
    void resize(uint32_t width, uint32_t height);
    void loadWorld(const RenderWorld& world);
    void setEnvironment(const EnvironmentState& environment);
    void setFeatureSettings(const RenderFeatureSettings& features);
    const RenderFeatureSettings& featureSettings() const { return features_; }
    void render(const OrbitCamera& camera);
    void clearWorld();

    bool isValid() const { return initialized_; }
    std::string lastError() const { return last_error_; }

private:
    struct GpuMesh {
        bgfx::VertexBufferHandle vertex_buffer = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle index_buffer = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle dark_texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle reflection_texture = BGFX_INVALID_HANDLE;
        std::string name;
        MaterialAsset material;
        uint32_t index_count = 0;
        bool has_vertex_color = false;
        bool has_lod_range = false;
        uint32_t lod_parent_block = 0;
        uint32_t lod_level = 0;
        float lod_near = 0.0f;
        float lod_far = 0.0f;
        Vec3 lod_center = {0.0f, 0.0f, 0.0f};
    };

    bgfx::ShaderHandle loadShader(const char* name);
    bgfx::ProgramHandle loadProgram(const char* vs_name, const char* fs_name);
    bgfx::TextureHandle loadTexture(const std::string& path, uint64_t sampler_flags);
    bgfx::TextureHandle loadReflectionCubeTexture(const std::string& name);
    bgfx::TextureHandle loadCubeTextureDds(const std::filesystem::path& path, uint64_t sampler_flags);
    bgfx::TextureHandle createSolidTexture(uint32_t rgba);
    bgfx::TextureHandle createSolidCubeTexture(uint32_t rgba);
    bgfx::ProgramHandle programForMaterial(const MaterialAsset& material) const;
    uint64_t renderStateForMaterial(const MaterialAsset& material) const;
    uint32_t resetFlags() const;
    void destroyTextureCache();
    void destroyMesh(GpuMesh& mesh);
    void drawGrid();

    std::filesystem::path shader_dir_;
    std::filesystem::path reflection_map_dir_;
    std::string source_asset_path_;
    std::vector<GpuMesh> meshes_;
    std::unordered_map<std::string, bgfx::TextureHandle> texture_cache_;
    std::unordered_map<std::string, bgfx::TextureHandle> cube_texture_cache_;
    EnvironmentState environment_;
    RenderFeatureSettings features_;
    bgfx::ProgramHandle legacy_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle basic_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle basic_lit_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle basic_two_layer_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle alpha_as_alpha_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle alpha_uv_scroll_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle legopp_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle legopp_no_ambient_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle legopp_emissive_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle metallic_brushed_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle metallic_polished_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle terrain_rim_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle ocean_distort_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle ocean_distort_directional_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle ocean_distort_unlit_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle ocean_distort_fx_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle clear_plastic_program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_diffuse_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_dark_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_lu_env_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_material_diffuse_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_material_emissive_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_light_dir_ambient_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_light_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_light_dir_fade_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_light_color_shadow_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_ambient_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_upper_hemi_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_lower_hemi_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_specular_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_camera_pos_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_fog_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_fog_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_shader_flags_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_variant_flags_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_effect_time_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_uv_motion1_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_uv_motion2_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_effect_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_glow_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_shiny_glint_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_shiny_glint_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_bbb_light_dir1_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_bbb_light_dir2_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_bbb_light_color1_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_bbb_light_color2_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle missing_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle neutral_env_texture_ = BGFX_INVALID_HANDLE;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    std::chrono::steady_clock::time_point start_time_{};
    bool initialized_ = false;
    std::string last_error_;
};

} // namespace lu::renderer::bgfx_backend
