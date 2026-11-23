cmake_minimum_required(VERSION 3.22.1)
project(invar_android C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(INVAR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(invar_android SHARED
    "${INVAR_ROOT}/packages/host/native/android_host.c"
    "${INVAR_ROOT}/packages/host/native/film_none.c"
    "${INVAR_ROOT}/packages/host/compat/pion_mb_compat.c"
    "${INVAR_ROOT}/packages/hostapi/native/wake_bridge.c"
)

target_include_directories(invar_android PRIVATE
    "${INVAR_ROOT}/packages/host/compat"
    "${INVAR_ROOT}/packages/host/native"
)

target_compile_options(invar_android PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
)

target_link_libraries(invar_android
    GLESv3
    EGL
    android
    log
    z
    m
)
