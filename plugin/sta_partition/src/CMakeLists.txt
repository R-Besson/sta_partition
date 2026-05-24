include(GitRevision)

find_package(Yosys)

git_rev_parse(YOSYS_SYN_REVISION ${CMAKE_SOURCE_DIR})

add_library(sta_partition SHARED
    sta_partition.cc
)
target_include_directories(sta_partition
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(sta_partition
    PRIVATE
        yosys::yosys
)
target_compile_definitions(sta_partition
    PRIVATE
        SYN_SHARE_DIR=\"plugins/${OUTPUT_NAME}\"
)

set_target_properties(sta_partition PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    # Yosys assumes `.so` to be the suffix on all platforms, so the following is required to make
    # `yosys -m sta_partition` to work after installation on any platform.
    PREFIX ""
    OUTPUT_NAME ${OUTPUT_NAME}
    SUFFIX ".so"
)

set(SHARE_DIRECTORIES
)

if (WIN32)
    # install .dll only
    install(TARGETS sta_partition RUNTIME DESTINATION ${YOSYS_DATDIR}/plugins)
else()
    # install .so/.dylib only
    install(TARGETS sta_partition LIBRARY DESTINATION ${YOSYS_DATDIR}/plugins)
endif()
install(DIRECTORY ${SHARE_DIRECTORIES} DESTINATION ${YOSYS_SHARE_DIR})

add_custom_target(
    uninstall
    COMMAND xargs rm < ${CMAKE_BINARY_DIR}/install_manifest.txt
)
