cmake_minimum_required(VERSION 3.22)

# The oldest macOS the app runs on. Must be set BEFORE project(). Without it
# CMake stamps the BUILD HOST's OS version into the binary as the minimum —
# the v0.9.0 DMG built on a macOS 26 runner refused to launch anywhere older
# (issue #11, the crossed-out icon in Finder). 11.0 = Big Sur, JUCE 8's
# supported floor and the first arm64 macOS, so one target covers both slices
# of the universal binary.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

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
project(LooperCat VERSION 0.9.4 LANGUAGES C CXX)   # C: juce_graphics compiles Sheenbidi (.c)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE

# Static CRT on Windows: the default /MD binary needs the VC++ redistributable
# installed, and a clean user machine greets that with "VCRUNTIME140.dll not
# found" — the Windows cousin of the deployment-target trap (#11). One
# standalone exe, no install-time dependencies. Applies to every target in
# the build (JUCE included) — mixing runtimes is a link-time mess.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshorten-64-to-32;-Wimplicit-float-conversion;-Wconditional-uninitialized;-Wconstant-conversion;-Wbool-conversion>
        # GCC's -Wextra still fires -Wmissing-field-initializers on C++20
        # DESIGNATED initializers, where omitting a member is the whole point
        # and the standard value-initializes it. The suites use them heavily
        # ({ .trashRoot = ..., .write = ... }), so GCC 13 rejected the core
        # tier outright with 56 -Werror hits while Clang and MSVC saw nothing.
        # Suppressed for GNU only: the aggregate case Clang does warn about
        # stays covered by the macOS and Windows lanes.
        #
        # GCC 12 at -O3 emits a bogus -Wrestrict/-Wstringop-overflow deep in
        # libstdc++'s std::string (char_traits.h memcpy, "accessing
        # 9223372036854775810 bytes") — a well-known false positive with no
        # call site in our code. It fires only at release optimisation, which
        # is why the -O0 CI lanes never saw it and the tagged release build
        # did. Demoted from error to warning for GNU ONLY, so the signal stays
        # visible without a compiler bug blocking a build our own code is clean
        # in; Clang and MSVC keep it as an error.
        $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers;-Wno-error=restrict;-Wno-error=stringop-overflow>)
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
loopercat_core_test(quit_tests)
loopercat_core_test(rc0_tests)
loopercat_core_test(wav_tests)
loopercat_core_test(volume_tests)
loopercat_core_test(lifecycle_tests)
loopercat_core_test(sysex_tests)
loopercat_core_test(catalog_tests)
loopercat_core_test(commands_tests)
loopercat_core_test(downmix_tests)
loopercat_core_test(usecase_count_in_tests)
# Parsing and decisions only — no syscalls, no Linux headers — so the
# contracts behind the Linux DeviceWatcher are gated on every platform's
# lane, not just on a machine that happens to have /proc and /sys.
loopercat_core_test(mount_table_tests)
loopercat_core_test(linux_device_rules_tests)

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
# Provenance has to be PROVEN, never assumed. execute_process overwrites its
# OUTPUT_VARIABLE with an empty string when the command fails, so the
# "unknown" default below was unreachable: a build with no reachable .git
# ended up with an empty hash and a zero commit count — which is exactly the
# shape of a clean release build. A source tarball, a "Download ZIP", a
# distro packager's tree or a container that cannot see the worktree's .git
# would all present themselves as the binary users downloaded. Caught on the
# Linux lane. Hence RESULT_VARIABLE on every query, and one flag that says
# whether we know anything at all.
find_package(Git QUIET)
set(LOOPERCAT_GIT_HASH "unknown")
set(LOOPERCAT_GIT_DIRTY 0)
set(LOOPERCAT_GIT_KNOWN 0)
set(LOOPERCAT_BUILD_COUNT 0)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD
        RESULT_VARIABLE _loopercat_rc OUTPUT_VARIABLE _loopercat_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_loopercat_rc EQUAL 0 AND NOT _loopercat_hash STREQUAL "")
        set(LOOPERCAT_GIT_HASH "${_loopercat_hash}")
        set(LOOPERCAT_GIT_KNOWN 1)
    endif()
endif()
if(LOOPERCAT_GIT_KNOWN)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE _loopercat_status ERROR_QUIET)
    if(NOT _loopercat_status STREQUAL "")
        set(LOOPERCAT_GIT_DIRTY 1)
    endif()
    # Commits since the last release tag: 0 means this build IS a release
    # build, anything else means it is ahead of what users can download. The
    # window says so out loud — a dev build must never pass for the shipped
    # one. Only trustworthy when the tag query itself succeeded.
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --tags --abbrev=0
        RESULT_VARIABLE _loopercat_tag_rc OUTPUT_VARIABLE _loopercat_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_loopercat_tag_rc EQUAL 0 AND _loopercat_tag)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-list "${_loopercat_tag}..HEAD" --count
            RESULT_VARIABLE _loopercat_count_rc OUTPUT_VARIABLE _loopercat_count
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_loopercat_count_rc EQUAL 0 AND NOT _loopercat_count STREQUAL "")
            set(LOOPERCAT_BUILD_COUNT "${_loopercat_count}")
        endif()
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
    ICON_SMALL   "${CMAKE_CURRENT_SOURCE_DIR}/art/icon/loopercat-icon-32.png"
    # The Mach-O minos gates the loader; this plist key is what Finder reads
    # to say "requires macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}" in words instead
    # of a crossed-out icon (issue #11).
    PLIST_TO_MERGE "<plist><dict><key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string></dict></plist>")

target_sources(LooperCat PRIVATE
    app/Main.cpp
    app/MainComponent.cpp
    app/PlayerPane.cpp
    app/SlotInspector.cpp
    app/SlotTable.cpp
    app/WavImport.cpp)

# JUCE's two-entry icns renders as noise in Sequoia's Launchpad (see
# art/icon/make-icns.sh); overwrite it with a full iconutil-built set.
# Runs at build time, so it lands before the release workflow signs the app.
if(APPLE)
    add_custom_command(TARGET LooperCat POST_BUILD
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/art/icon/make-icns.sh"
                "${CMAKE_CURRENT_SOURCE_DIR}/art/icon/loopercat-icon-1024.png"
                "$<TARGET_BUNDLE_CONTENT_DIR:LooperCat>/Resources/AppIcon.icns"
        COMMENT "Rebuilding AppIcon.icns with the full iconset")
endif()

# One DeviceWatcher interface, one backend per platform (issue #5): IOKit +
# DiskArbitration on macOS; WM_DEVICECHANGE + uncached probe + DeviceIoControl
# eject on Windows; udev + mountinfo + O_DIRECT + udisks2 on Linux. A new
# platform must bring a backend — there is deliberately no portable fallback
# to compile against.
if(APPLE)
    target_sources(LooperCat PRIVATE app/DeviceWatcherMac.cpp)
elseif(WIN32)
    target_sources(LooperCat PRIVATE app/DeviceWatcherWin.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(LooperCat PRIVATE app/DeviceWatcherLinux.cpp)
else()
    message(FATAL_ERROR "no DeviceWatcher backend for this platform")
endif()

# The storage-mode sender, likewise one backend per platform. JUCE's
# MidiOutput carries macOS and Windows; on Linux it delivers only sometimes,
# silently, on every JUCE version tried, so that platform writes to ALSA
# rawmidi itself — PedalLinkLinux.cpp records the measurements.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(LooperCat PRIVATE app/PedalLinkLinux.cpp)
else()
    target_sources(LooperCat PRIVATE app/PedalLinkJuce.cpp)
endif()

target_compile_definitions(LooperCat PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1        # the --snapshot path pumps the loop while a waveform builds
    LOOPERCAT_VERSION="${PROJECT_VERSION}"
    LOOPERCAT_GIT_HASH="${LOOPERCAT_GIT_HASH}"
    LOOPERCAT_GIT_DIRTY=${LOOPERCAT_GIT_DIRTY}
    LOOPERCAT_GIT_KNOWN=${LOOPERCAT_GIT_KNOWN}
    LOOPERCAT_BUILD_NUMBER=${LOOPERCAT_BUILD_NUMBER}
    LOOPERCAT_BUILD_COUNT=${LOOPERCAT_BUILD_COUNT}
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # DeviceWatcher: the udev netlink monitor behind the detach push. REQUIRED
    # on purpose — a Linux build without it would have no device truth at all,
    # and libudev ships on every systemd and eudev distribution, so a missing
    # one means the build machine lacks the -dev package, not that the app
    # should quietly degrade. Unmount and eject go through the `udisksctl`
    # binary at runtime, so they add no link-time dependency here.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
    # ALSA for the rawmidi sender. JUCE links it too, but this backend uses
    # the API directly and must say so rather than inherit it by luck.
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    target_link_libraries(LooperCat PRIVATE PkgConfig::UDEV PkgConfig::ALSA)
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
# Import conversion (issue #20): theory tests for wavimport::prepare — the
# pedal-ready pass-through, the DAW extensible-header case, resampling, mono
# duplication, and the refusals.
# ----------------------------------------------------------------------------
juce_add_console_app(wav_import_tests PRODUCT_NAME "wav_import_tests")
target_sources(wav_import_tests PRIVATE tests/wav_import_tests.cpp app/WavImport.cpp)
target_compile_definitions(wav_import_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    LOOPERCAT_GOLDEN_JSON="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden.json")
target_link_libraries(wav_import_tests PRIVATE
    loopercat::core
    nlohmann_json::nlohmann_json
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_test(NAME wav_import_tests COMMAND wav_import_tests)

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
