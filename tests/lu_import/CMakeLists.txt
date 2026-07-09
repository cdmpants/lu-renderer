add_executable(lu_renderer_lu_import_tests
    test_shader_database.cpp
)

target_compile_features(lu_renderer_lu_import_tests PRIVATE cxx_std_20)
target_link_libraries(lu_renderer_lu_import_tests PRIVATE
    lu_renderer_lu_import
)

add_test(NAME lu_renderer_lu_import_tests COMMAND lu_renderer_lu_import_tests)
