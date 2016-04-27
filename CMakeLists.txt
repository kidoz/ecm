cmake_minimum_required(VERSION 3.3)
project(ECM)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")

add_executable(ecm "src/ecm.c")
add_executable(unecm "src/unecm.c")

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    function(add_clang_static_analysis target)
        get_target_property(SRCs ${target} SOURCES)
        add_library(${target}_analyze OBJECT EXCLUDE_FROM_ALL ${SRCs})
        set_target_properties(${target}_analyze PROPERTIES
                COMPILE_OPTIONS "--analyze"
                EXCLUDE_FROM_DEFAULT_BUILD true)
    endfunction()
endif ()