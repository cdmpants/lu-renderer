add_executable(lu_nif_viewer
    main.cpp
)

find_package(Git QUIET)
set(LU_RENDERER_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE LU_RENDERER_GIT_RESULT
        OUTPUT_VARIABLE LU_RENDERER_GIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(LU_RENDERER_GIT_RESULT EQUAL 0 AND NOT LU_RENDERER_GIT_OUTPUT STREQUAL "")
        set(LU_RENDERER_GIT_COMMIT "${LU_RENDERER_GIT_OUTPUT}")
    endif()
endif()

add_dependencies(lu_nif_viewer lu_renderer_shaders)
target_compile_features(lu_nif_viewer PRIVATE cxx_std_20)
target_link_libraries(lu_nif_viewer PRIVATE
    lu_renderer_bgfx
    lu_renderer_lu_import
    glfw
)
target_compile_definitions(lu_nif_viewer PRIVATE
    GLFW_INCLUDE_NONE
    LU_RENDERER_BUILD_CONFIG="$<CONFIG>"
    LU_RENDERER_GIT_COMMIT="${LU_RENDERER_GIT_COMMIT}"
)

if(WIN32)
    target_link_libraries(lu_nif_viewer PRIVATE comctl32 comdlg32 user32)
endif()

if(WIN32)
    set(LU_RENDERER_CANONICAL_VIEWER "${CMAKE_SOURCE_DIR}/viewer/LU NIF Viewer.exe")
else()
    set(LU_RENDERER_CANONICAL_VIEWER "${CMAKE_SOURCE_DIR}/viewer/LU NIF Viewer")
endif()

# This explicit publish target is the only route to the friendly, canonical
# executable. The publish script rejects other build configurations so a local
# Debug build cannot silently replace the visual reference build.
add_custom_target(lu_nif_viewer_canonical
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=$<TARGET_FILE:lu_nif_viewer>"
        "-DOUTPUT=${LU_RENDERER_CANONICAL_VIEWER}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_SOURCE_DIR}/cmake/PublishCanonicalViewer.cmake"
    DEPENDS lu_nif_viewer
    VERBATIM
)
