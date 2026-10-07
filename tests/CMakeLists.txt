cmake_minimum_required(VERSION 3.16)
project(IniRemovalTests LANGUAGES CXX)

add_executable(ini_removal_tests
    IniRemovalTests.cpp
    ../src/IniRemoval.cpp
)
target_include_directories(ini_removal_tests PRIVATE ../src)
target_compile_features(ini_removal_tests PRIVATE cxx_std_20)
set_target_properties(ini_removal_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(ini_removal_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(ini_removal_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME ini_removal COMMAND ini_removal_tests)
set_tests_properties(ini_removal PROPERTIES TIMEOUT 120)

# Compile the production adapter unchanged against small fake engine/file APIs.
# This tests its orchestration, not the game's ABI or native Clear internals.
add_executable(rules_removal_tests
    RulesRemovalTests.cpp
    ../src/RulesRemoval.cpp
    ../src/IniRemoval.cpp
)
target_include_directories(rules_removal_tests PRIVATE stubs ../src)
target_compile_features(rules_removal_tests PRIVATE cxx_std_20)
set_target_properties(rules_removal_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(rules_removal_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(rules_removal_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME rules_removal_adapter COMMAND rules_removal_tests)
set_tests_properties(rules_removal_adapter PROPERTIES TIMEOUT 120)
