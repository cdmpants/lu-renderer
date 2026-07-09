cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(lu-renderer
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Modern bgfx renderer for LU-Rebuilt"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LU_RENDERER_BUILD_VIEWER "Build the standalone NIF viewer" ON)
include(CTest)

set(LU_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lu-assets" CACHE PATH "Path to local lu-assets checkout")
if(NOT EXISTS "${LU_ASSETS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "LU_ASSETS_DIR must point to a lu-assets checkout")
endif()

set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/bgfx.cmake)
include(third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw)

add_subdirectory("${LU_ASSETS_DIR}" "${CMAKE_BINARY_DIR}/_deps/lu-assets")

add_subdirectory(src/renderer)
add_subdirectory(src/renderer_bgfx)
add_subdirectory(src/lu_import)

if(LU_RENDERER_BUILD_VIEWER)
    add_subdirectory(apps/nif_viewer)
    add_subdirectory(apps/shader_audit)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests/lu_import)
endif()
