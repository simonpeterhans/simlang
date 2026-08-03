cmake_minimum_required(VERSION 3.16)

project(simlang VERSION 0.0.0 LANGUAGES C CXX)

add_library(simlang_options INTERFACE)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # gcc/clang: Enforce conversions, treat warnings as errors.
    target_compile_options(simlang_options INTERFACE
            -Wconversion
            -Wsign-conversion
            -Werror=conversion
            -Werror=sign-conversion
    )
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC: Enforce level-4 warnings, treat warnings as errors.
    target_compile_options(simlang_options INTERFACE
            /W4        # Level-4 warnings, already includes the commented warnings below.
            /WX        # Treat warnings as errors.
            # /we4244
            # /we4267
            # /we4389
    )
endif ()

add_subdirectory("src")
add_subdirectory("cli")
