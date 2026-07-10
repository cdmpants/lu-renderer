add_executable(lu_nif_viewer
    main.cpp
)

add_dependencies(lu_nif_viewer lu_renderer_shaders)
target_compile_features(lu_nif_viewer PRIVATE cxx_std_20)
target_link_libraries(lu_nif_viewer PRIVATE
    lu_renderer_bgfx
    lu_renderer_lu_import
    glfw
)
target_compile_definitions(lu_nif_viewer PRIVATE
    GLFW_INCLUDE_NONE
)

if(WIN32)
    target_link_libraries(lu_nif_viewer PRIVATE comctl32 comdlg32 user32)
endif()

set_target_properties(lu_nif_viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/viewer"
    OUTPUT_NAME_RELWITHDEBINFO "LU NIF Viewer"
)
