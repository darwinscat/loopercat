cmake_minimum_required(VERSION 3.22)

# ----------------------------------------------------------------------------
# Looper Cat — companion app for hardware loopers (v1: BOSS RC-5). Felitronics
# line, by Darwin's Cat.
#
# core/ is a header-only, JUCE-free C++20 model of the pedal's on-disk format
# (memory files, boot-indexing formula, WAV shapes, volume layout), pinned to
# fixtures/golden.json — values captured from real hardware, shared with the
# rc5cat JS and Swift suites. `-DLOOPERCAT_BUILD_APP=OFF` configures core-only
# (no JUCE fetch) and runs the conformance suites via `ctest`.
# ----------------------------------------------------------------------------
project(LooperCat VERSION 0.1.0 LANGUAGES C CXX)   # C: juce_graphics compiles Sheenbidi (.c)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build archs" FORCE)
endif()

option(LOOPERCAT_BUILD_APP   "Build the JUCE desktop app"                     ON)
option(LOOPERCAT_BUILD_TESTS "Build the JUCE-free core conformance suites"    ON)

# ----------------------------------------------------------------------------
# The core — header-only, JUCE-free. An INTERFACE target: include path and the
# C++20 requirement, nothing else.
# ----------------------------------------------------------------------------
add_library(loopercat_core INTERFACE)
add_library(loopercat::core ALIAS loopercat_core)
target_include_directories(loopercat_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_features(loopercat_core INTERFACE cxx_std_20)

enable_testing()

if(LOOPERCAT_BUILD_TESTS)

# ----------------------------------------------------------------------------
# Conformance tests: JUCE-free, framework-free (a tiny harness in
# tests/support.hpp), each suite an executable under ctest. golden.json is
# consumed as the actual shared fixture file — nlohmann/json (fetched, pinned,
# test-tier only) parses it; the app never links it.
# ----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# The same downstream-grade strict set the family gates its headers with
# (felitronics-appkit/-core convention): the core must be warning-clean under
# a consumer's harshest flags.
set(LOOPERCAT_STRICT_FLAGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(LOOPERCAT_STRICT_FLAGS
        -Werror -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wfloat-equal -Wshadow -Wcast-align
        -Wzero-as-null-pointer-constant -Wunreachable-code -Wswitch-enum -Wextra-semi
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshorten-64-to-32;-Wimplicit-float-conversion;-Wconditional-uninitialized;-Wconstant-conversion;-Wbool-conversion>)
endif()

function(loopercat_core_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE loopercat::core nlohmann_json::nlohmann_json)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE ${LOOPERCAT_STRICT_FLAGS})
    target_compile_definitions(${name} PRIVATE
        LOOPERCAT_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden.json")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

loopercat_core_test(params_tests)

endif()  # LOOPERCAT_BUILD_TESTS
