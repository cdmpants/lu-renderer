set(LU_RENDERER_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/runtime/shaders")
set(LU_RENDERER_SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders/legacy_lu")
set(LU_RENDERER_FRAGMENT_SHADER_INCLUDES
    "${LU_RENDERER_SHADER_DIR}/shadow_common.sh"
)

bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS
        "${LU_RENDERER_SHADER_DIR}/vs_legacy_mesh.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_lu_lit_mesh.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_lu_lit_mesh_no_ambient.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_metallic.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_terrain_rim.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_uv_scroll_alpha.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_ocean_distort.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_ocean_distort_unlit.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_ocean_distort_fx.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_fullscreen.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_shadow_depth.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_view_normal.sc"
        "${LU_RENDERER_SHADER_DIR}/vs_reflection_mask.sc"
    VARYING_DEF "${LU_RENDERER_SHADER_DIR}/varying.def.sc"
    OUTPUT_DIR "${LU_RENDERER_SHADER_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/src"
    OUT_FILES_VAR LU_RENDERER_VS_OUTPUTS
)

bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS
        "${LU_RENDERER_SHADER_DIR}/fs_legacy_mesh.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_basic.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_basic_lit.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_basic_two_layer.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_alpha_as_alpha.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_legopp_lighting.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_legopp_emissive.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_metallic_brushed.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_metallic_polished.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_terrain_rim.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_uv_scroll_alpha.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_ocean_distort.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_ocean_distort_fx.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_clear_plastic.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_reflection_mask.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_bloom.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_gtao.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_gtao_denoise.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_post_effects.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_fullscreen_copy.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_shadow_depth.sc"
        "${LU_RENDERER_SHADER_DIR}/fs_view_normal.sc"
    VARYING_DEF "${LU_RENDERER_SHADER_DIR}/varying.def.sc"
    OUTPUT_DIR "${LU_RENDERER_SHADER_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/src"
    DEPENDS ${LU_RENDERER_FRAGMENT_SHADER_INCLUDES}
    OUT_FILES_VAR LU_RENDERER_FS_OUTPUTS
)

add_custom_target(lu_renderer_shaders DEPENDS
    ${LU_RENDERER_VS_OUTPUTS}
    ${LU_RENDERER_FS_OUTPUTS}
)

add_library(lu_renderer_bgfx STATIC
    bgfx_renderer.cpp
    include/lu/renderer_bgfx/bgfx_renderer.h
)

add_dependencies(lu_renderer_bgfx lu_renderer_shaders)
target_compile_features(lu_renderer_bgfx PUBLIC cxx_std_20)
target_include_directories(lu_renderer_bgfx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_definitions(lu_renderer_bgfx PUBLIC
    LU_RENDERER_SHADER_DIR="${LU_RENDERER_SHADER_OUTPUT_DIR}"
    LU_RENDERER_REFLECTION_MAP_DIR="${CMAKE_SOURCE_DIR}/third_party/lu/reflection_maps"
)
target_link_libraries(lu_renderer_bgfx PUBLIC
    lu_renderer_core
    bgfx
    bx
    lu_assets
)
