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
project(LooperCat VERSION 0.9.0 LANGUAGES C CXX)   # C: juce_graphics compiles Sheenbidi (.c)

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
elseif(MSVC)
    # /W4 without /WX for now: the Windows gate (issue #5) starts by proving
    # the suites build and pass; warning-parity with the Clang set comes once
    # the first green run shows what MSVC actually flags.
    set(LOOPERCAT_STRICT_FLAGS /W4 /permissive-)
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

loopercat_core_test(banner_tests)
loopercat_core_test(connect_tests)
loopercat_core_test(params_tests)
loopercat_core_test(rc0_tests)
loopercat_core_test(wav_tests)
loopercat_core_test(volume_tests)
loopercat_core_test(lifecycle_tests)
loopercat_core_test(sysex_tests)
loopercat_core_test(catalog_tests)
loopercat_core_test(commands_tests)

endif()  # LOOPERCAT_BUILD_TESTS

if(LOOPERCAT_BUILD_APP)

# ----------------------------------------------------------------------------
# felitronics-appkit — the family's shared app-side JUCE infrastructure
# (brand, BrandHeader, VersionBadge, UpdateChecker). HYBRID consumption, the
# family convention: a local sibling checkout wins for co-development,
# otherwise the pinned public release is fetched — a fresh clone / CI is
# self-contained.
# ----------------------------------------------------------------------------
include(FetchContent)
set(LOOPERCAT_APPKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../felitronics-appkit" CACHE PATH
    "Local felitronics-appkit checkout (used if present); else the pinned release is fetched")
set(LOOPERCAT_APPKIT_TAG "v0.7.0" CACHE STRING
    "felitronics-appkit git tag to fetch when no local checkout is present")
if(LOOPERCAT_APPKIT_DIR AND EXISTS "${LOOPERCAT_APPKIT_DIR}/CMakeLists.txt")
    message(STATUS "felitronics-appkit: local checkout — ${LOOPERCAT_APPKIT_DIR}")
    FetchContent_Declare(felitronics_appkit SOURCE_DIR "${LOOPERCAT_APPKIT_DIR}")
else()
    message(STATUS "felitronics-appkit: fetching ${LOOPERCAT_APPKIT_TAG} from GitHub")
    FetchContent_Declare(felitronics_appkit
        GIT_REPOSITORY https://github.com/darwinscat/felitronics-appkit.git
        GIT_TAG        ${LOOPERCAT_APPKIT_TAG}
        GIT_SHALLOW    TRUE)
endif()
FetchContent_MakeAvailable(felitronics_appkit)

set(LOOPERCAT_JUCE_TAG "8.0.14" CACHE STRING "JUCE git tag")   # the family's pinned JUCE
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${LOOPERCAT_JUCE_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(juce)

# Brand assets ship with felitronics-appkit; embedding is the consumer's job
# (the appkit target stays include-path-only). BinaryData::catlogo_svg + the
# Michroma wordmark font (OFL — license text lives next to the font upstream).
juce_add_binary_data(LooperCatAssets
    SOURCES
        "${felitronics_appkit_SOURCE_DIR}/assets/catlogo.svg"
        "${felitronics_appkit_SOURCE_DIR}/assets/Michroma-Regular.ttf")

# ----------------------------------------------------------------------------
# Configure-time build stamp for the version badge: short hash + dirty flag +
# a UTC number. Deliberately simple for now — the always-fresh per-build
# generator (the TabbyEQ pattern) arrives with packaging in Sprint 5.
# ----------------------------------------------------------------------------
find_package(Git QUIET)
set(LOOPERCAT_GIT_HASH "unknown")
set(LOOPERCAT_GIT_DIRTY 0)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE LOOPERCAT_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE _loopercat_status ERROR_QUIET)
    if(NOT _loopercat_status STREQUAL "")
        set(LOOPERCAT_GIT_DIRTY 1)
    endif()
endif()
string(TIMESTAMP LOOPERCAT_BUILD_NUMBER "%Y%m%d%H%M%S" UTC)

set(LOOPERCAT_BUILD_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT LOOPERCAT_BUILD_ARCH)
    set(LOOPERCAT_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(REPLACE ";" "+" LOOPERCAT_BUILD_ARCH "${LOOPERCAT_BUILD_ARCH}")   # "arm64;x86_64" -> one token

# Who built this binary, shown as the version-badge flavor. Defaults to "dev"
# (a workstation build); the release workflow stamps "release" so a shipped
# DMG never wears the dev badge (hardware QA 2026-08-01, setup note).
set(LOOPERCAT_BUILDER "dev" CACHE STRING "Builder flavor shown in the version badge")

# The app icon — the "ears" mark (art/icon/loopercat-mark.svg is the geometry
# source; the PNGs are its renders). JUCE builds the .icns/.ico from these.
juce_add_gui_app(LooperCat
    PRODUCT_NAME "LooperCat"
    COMPANY_NAME "Darwin's Cat"
    BUNDLE_ID    "com.darwinscat.loopercat"
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/art/icon/loopercat-icon-1024.png"
    ICON_SMALL   "${CMAKE_CURRENT_SOURCE_DIR}/art/icon/loopercat-icon-32.png")

target_sources(LooperCat PRIVATE
    app/DeviceWatcher.cpp
    app/Main.cpp
    app/MainComponent.cpp
    app/PlayerPane.cpp
    app/SlotTable.cpp)

target_compile_definitions(LooperCat PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1        # the --snapshot path pumps the loop while a waveform builds
    LOOPERCAT_VERSION="${PROJECT_VERSION}"
    LOOPERCAT_GIT_HASH="${LOOPERCAT_GIT_HASH}"
    LOOPERCAT_GIT_DIRTY=${LOOPERCAT_GIT_DIRTY}
    LOOPERCAT_BUILD_NUMBER=${LOOPERCAT_BUILD_NUMBER}
    LOOPERCAT_BUILD_OS="${CMAKE_SYSTEM_NAME}"
    LOOPERCAT_BUILD_ARCH="${LOOPERCAT_BUILD_ARCH}"
    LOOPERCAT_BUILDER="${LOOPERCAT_BUILDER}")

target_link_libraries(LooperCat PRIVATE
    loopercat::core
    felitronics::appkit                 # BrandHeader / VersionBadge / UpdateChecker / AudioSettingsPanel
    LooperCatAssets                     # embedded brand assets (cat logo + Michroma)
    juce::juce_audio_utils              # playback chain, AudioThumbnail, device selector
    juce::juce_gui_basics
    juce::juce_events
    juce::juce_data_structures
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
    # DeviceWatcher: IORegistry liveness + DiskArbitration unmount/eject.
    target_link_libraries(LooperCat PRIVATE
        "-framework DiskArbitration"
        "-framework IOKit")
endif()

if(LOOPERCAT_BUILD_TESTS)

# ----------------------------------------------------------------------------
# The live-refresh harness: the REAL PedalMonitor (worker thread + async
# delivery) against a scratch volume that mounts, edits and unmounts under
# it — the headless proof behind "rows follow the filesystem". Needs JUCE,
# so it lives in the app tier, registered under the same ctest.
# ----------------------------------------------------------------------------
juce_add_console_app(pedal_monitor_harness PRODUCT_NAME "pedal_monitor_harness")
target_sources(pedal_monitor_harness PRIVATE tests/pedal_monitor_harness.cpp)
target_compile_definitions(pedal_monitor_harness PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1        # runDispatchLoopUntil pumps the delivery loop
    LOOPERCAT_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden.json")
target_link_libraries(pedal_monitor_harness PRIVATE
    loopercat::core
    nlohmann_json::nlohmann_json
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME pedal_monitor_harness COMMAND pedal_monitor_harness)

# ----------------------------------------------------------------------------
# The playback harness: the REAL AudioEngine pumped through its device
# callback with a fake device — samples must match the frame-index ramp, so
# it proves where the engine plays from (load, seek, loop wrap, run-dry).
# ----------------------------------------------------------------------------
juce_add_console_app(audio_engine_harness PRODUCT_NAME "audio_engine_harness")
target_sources(audio_engine_harness PRIVATE tests/audio_engine_harness.cpp)
target_compile_definitions(audio_engine_harness PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    LOOPERCAT_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden.json")
target_link_libraries(audio_engine_harness PRIVATE
    loopercat::core
    nlohmann_json::nlohmann_json
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME audio_engine_harness COMMAND audio_engine_harness)

# ----------------------------------------------------------------------------
# The brand-mark contract: the product mark must cover appkit's drawOrbit
# completely (LooperBrandHeader overpaints it), touch nothing outside its
# disc, and actually be the ears — pixel-level, against rendered images.
# ----------------------------------------------------------------------------
juce_add_console_app(brand_mark_tests PRODUCT_NAME "brand_mark_tests")
target_sources(brand_mark_tests PRIVATE tests/brand_mark_tests.cpp)
target_compile_definitions(brand_mark_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    LOOPERCAT_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden.json")
target_link_libraries(brand_mark_tests PRIVATE
    loopercat::core
    nlohmann_json::nlohmann_json
    felitronics::appkit
    LooperCatAssets
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME brand_mark_tests COMMAND brand_mark_tests)

endif()  # LOOPERCAT_BUILD_TESTS

endif()  # LOOPERCAT_BUILD_APP
