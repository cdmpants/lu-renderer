if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Canonical viewer input does not exist: ${INPUT}")
endif()

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "Canonical viewer output path was not provided")
endif()

if(NOT CONFIG STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR
        "Refusing to publish ${CONFIG} as the canonical viewer. "
        "Use: cmake --build build --target lu_nif_viewer_canonical --config RelWithDebInfo"
    )
endif()

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(COPY_FILE "${INPUT}" "${OUTPUT}" ONLY_IF_DIFFERENT)
message(STATUS "Published canonical LU NIF Viewer: ${OUTPUT}")
