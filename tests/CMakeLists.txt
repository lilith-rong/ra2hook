cmake_minimum_required(VERSION 3.16)
project(IniPatchTests LANGUAGES CXX)

enable_testing()

add_executable(ini_patch_tests IniPatchTests.cpp ../src/IniPatch.cpp)
target_include_directories(ini_patch_tests PRIVATE ../src)

# Compile the production adapter against small fake engine/file APIs. These
# tests verify orchestration, not real game ABI or native Clear internals.
add_executable(startup_patch_tests
    StartupPatchTests.cpp
    ../src/StartupPatch.cpp
    ../src/IniPatch.cpp
)
target_include_directories(startup_patch_tests PRIVATE stubs ../src)

foreach(target ini_patch_tests startup_patch_tests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120)
endforeach()
