add_library(lu_renderer_lu_import STATIC
    lvl_environment_importer.cpp
    include/lu/renderer/lu_import/lvl_environment_importer.h
    nif_importer.cpp
    include/lu/renderer/lu_import/nif_importer.h
    shader_database.cpp
    include/lu/renderer/lu_import/shader_database.h
)

target_compile_features(lu_renderer_lu_import PUBLIC cxx_std_20)
target_include_directories(lu_renderer_lu_import PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(lu_renderer_lu_import PUBLIC
    lu_renderer_core
    lu_assets::lu_assets
)
