cmake_minimum_required(VERSION 3.6)
project(ECM C)

set(CMAKE_C_STANDARD 11)

include_directories(${ECM_SOURCE_DIR}/include)

add_executable(ecm "src/ecm.c")
add_executable(unecm "src/unecm.c")