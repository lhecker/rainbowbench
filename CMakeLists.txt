cmake_minimum_required(VERSION 3.21)
project(rainbowbench)

add_executable(rainbowbench main.cpp)
target_compile_features(rainbowbench PRIVATE cxx_std_20)

if (MSVC)
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(rainbowbench PRIVATE /OPT:REF /OPT:ICF)
    endif ()
endif ()
