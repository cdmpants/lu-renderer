add_library(lu_renderer_core STATIC
    camera.cpp
    include/lu/renderer/camera.h
    include/lu/renderer/math.h
    include/lu/renderer/render_types.h
)

target_compile_features(lu_renderer_core PUBLIC cxx_std_20)
target_include_directories(lu_renderer_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

