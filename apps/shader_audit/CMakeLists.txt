add_executable(lu_shader_audit
    main.cpp
)

target_compile_features(lu_shader_audit PRIVATE cxx_std_20)
target_link_libraries(lu_shader_audit PRIVATE
    lu_renderer_lu_import
)
