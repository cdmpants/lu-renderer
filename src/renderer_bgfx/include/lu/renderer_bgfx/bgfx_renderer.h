#pragma once

#include "lu/renderer/camera.h"
#include "lu/renderer/render_types.h"

#include <bgfx/bgfx.h>

#include <array>
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
    bool bgfx_device_debug = false;
    RenderFeatureSettings features;
    std::filesystem::path shader_dir = LU_RENDERER_SHADER_DIR;
    std::filesystem::path reflection_map_dir = LU_RENDERER_REFLECTION_MAP_DIR;
};

class BgfxRenderer {
public:
    BgfxRenderer();
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
    static constexpr size_t kBloomMipCount = 6;

    struct GpuMesh {
        bgfx::VertexBufferHandle vertex_buffer = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle index_buffer = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle dark_texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle reflection_texture = BGFX_INVALID_HANDLE;
        uint32_t texture_sampler_flags = 0;
        uint32_t dark_texture_sampler_flags = 0;
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
        Vec3 bounds_min = {0.0f, 0.0f, 0.0f};
        Vec3 bounds_max = {0.0f, 0.0f, 0.0f};
    };

    bgfx::ShaderHandle loadShader(const char* name);
    bgfx::ProgramHandle loadProgram(const char* vs_name, const char* fs_name);
    bgfx::TextureHandle loadTexture(const std::string& path, uint32_t sampler_flags);
    bgfx::TextureHandle loadColorLutTexture(const std::string& path);
    bgfx::TextureHandle loadReflectionCubeTexture(const std::string& name);
    bgfx::TextureHandle loadCubeTextureDds(const std::filesystem::path& path, uint64_t sampler_flags);
    bgfx::TextureHandle createSolidTexture(uint32_t rgba);
    bgfx::TextureHandle createNeutralColorLutTexture();
    bgfx::TextureHandle createSolidCubeTexture(uint32_t rgba);
    bgfx::TextureHandle createEnvironmentProbeTexture() const;
    void rebuildEnvironmentProbeTexture();
    void destroyCapturedProbeTarget();
    bool ensureCapturedProbeTarget();
    void captureGlobalReflectionProbe(float effect_time);
    bgfx::ProgramHandle programForMaterial(const MaterialAsset& material) const;
    uint64_t renderStateForMaterial(const MaterialAsset& material) const;
    uint32_t resetFlags() const;
    void destroySceneTarget();
    bool ensureSceneTarget();
    void destroyTextureCache();
    void destroyMesh(GpuMesh& mesh);
    void drawGrid();
    void destroyShadowTarget();
    bool ensureShadowTarget();
    void destroyReflectionMaskTarget();
    bool ensureReflectionMaskTarget();
    void destroyBloomMaskTarget();
    bool ensureBloomMaskTarget();
    void destroyGtaoTargets();
    bool ensureGtaoTargets();
    bgfx::TextureHandle buildGtaoTexture(
        float effect_time,
        float near_clip,
        float far_clip,
        float tan_half_fov_x,
        float tan_half_fov_y);
    void destroyBloomChain();
    bool ensureBloomChain();
    bgfx::TextureHandle buildBloomPyramid();
    void destroySceneNormalTarget();
    bool ensureSceneNormalTarget();
    void destroyTemporalHistoryTargets();
    bool ensureTemporalHistoryTargets();
    void drawPostProcess(
        float effect_time,
        float near_clip,
        float far_clip,
        float tan_half_fov_x,
        float tan_half_fov_y,
        bgfx::TextureHandle bloom_texture,
        bgfx::TextureHandle gtao_texture);
    void drawFullscreenCopy(bgfx::TextureHandle texture);

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
    bgfx::ProgramHandle shadow_depth_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle post_process_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle bloom_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gtao_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gtao_denoise_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle fullscreen_copy_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle reflection_mask_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle view_normal_program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_diffuse_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_dark_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_lu_env_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_scene_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_scene_depth_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_scene_normal_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_history_color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_reflection_mask_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_bloom_mask_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_gtao_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_color_lut_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_shadow_map_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_shadow_matrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_shadow_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_shadow_bias_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_shadow_light_dir_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_post_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_bloom_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_dof_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_color_lut_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_screen_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_screen_space_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_depth_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_temporal_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_reflection_mask_value_ = BGFX_INVALID_HANDLE;
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
    bgfx::UniformHandle u_lu_alpha_test_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_variant_flags_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_pbr_params_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lu_reflection_params_ = BGFX_INVALID_HANDLE;
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
    bgfx::TextureHandle black_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle missing_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle flat_normal_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle neutral_lut_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color_lut_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle neutral_env_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle global_probe_texture_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::TextureHandle, 6> global_probe_depth_textures_{};
    std::array<bgfx::FrameBufferHandle, 6> global_probe_framebuffers_{};
    bool global_probe_capture_dirty_ = true;
    bgfx::TextureHandle scene_color_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle scene_depth_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle scene_framebuffer_ = BGFX_INVALID_HANDLE;
    uint32_t scene_target_width_ = 0;
    uint32_t scene_target_height_ = 0;
    uint64_t scene_target_msaa_flags_ = 0;
    bool scene_target_depth_sampleable_ = true;
    bgfx::TextureHandle scene_normal_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle scene_normal_depth_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle scene_normal_framebuffer_ = BGFX_INVALID_HANDLE;
    uint32_t scene_normal_target_width_ = 0;
    uint32_t scene_normal_target_height_ = 0;
    std::array<bgfx::TextureHandle, 2> temporal_history_textures_{};
    std::array<bgfx::FrameBufferHandle, 2> temporal_history_framebuffers_{};
    uint32_t temporal_history_width_ = 0;
    uint32_t temporal_history_height_ = 0;
    uint8_t temporal_history_index_ = 0;
    bool temporal_history_valid_ = false;
    uint64_t frame_index_ = 0;
    CameraMode last_temporal_camera_mode_ = CameraMode::Orbit;
    Vec3 last_temporal_eye_ = {0.0f, 0.0f, 0.0f};
    Vec3 last_temporal_target_ = {0.0f, 0.0f, 0.0f};
    float last_temporal_yaw_ = 0.0f;
    float last_temporal_pitch_ = 0.0f;
    float last_temporal_distance_ = 0.0f;
    bool last_temporal_camera_valid_ = false;
    bgfx::TextureHandle shadow_depth_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle shadow_framebuffer_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle reflection_mask_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle reflection_mask_depth_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle reflection_mask_framebuffer_ = BGFX_INVALID_HANDLE;
    uint32_t reflection_mask_target_width_ = 0;
    uint32_t reflection_mask_target_height_ = 0;
    bgfx::TextureHandle bloom_mask_texture_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle bloom_mask_depth_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle bloom_mask_framebuffer_ = BGFX_INVALID_HANDLE;
    uint32_t bloom_mask_target_width_ = 0;
    uint32_t bloom_mask_target_height_ = 0;
    bgfx::TextureHandle gtao_raw_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle gtao_raw_framebuffer_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle gtao_denoised_texture_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle gtao_denoised_framebuffer_ = BGFX_INVALID_HANDLE;
    uint32_t gtao_target_width_ = 0;
    uint32_t gtao_target_height_ = 0;
    std::array<bgfx::TextureHandle, kBloomMipCount> bloom_textures_{};
    std::array<bgfx::FrameBufferHandle, kBloomMipCount> bloom_framebuffers_{};
    std::array<uint16_t, kBloomMipCount> bloom_widths_{};
    std::array<uint16_t, kBloomMipCount> bloom_heights_{};
    uint32_t bloom_chain_target_width_ = 0;
    uint32_t bloom_chain_target_height_ = 0;
    std::string color_lut_path_;
    float color_lut_size_ = 16.0f;
    float color_lut_horizontal_ = 1.0f;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    std::chrono::steady_clock::time_point start_time_{};
    bool initialized_ = false;
    std::string last_error_;
};

} // namespace lu::renderer::bgfx_backend
