# armyoftworecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(armyoftworecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

 set(REXSDK_VERSION "0.10.0.8-dev.g1406e1b")

# Fetch the ReXGlue SDK for this platform into rexglue/<platform> if not
# already present. Idempotent: existing SDK folders are left untouched.
# Version and platform are passed explicitly so the script stays self-contained.
if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(REXSDK_PLATFORM "win-amd64")
elseif(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^ARM64$")
    message(FATAL_ERROR "win-arm64 ReXGlue SDK release is not published")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(REXSDK_PLATFORM "mac-arm64")
elseif(APPLE)
    set(REXSDK_PLATFORM "mac-amd64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(REXSDK_PLATFORM "linux-arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(REXSDK_PLATFORM "linux-amd64")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
            "-DREXSDK_VERSION='${REXSDK_VERSION}'"
            "-DREXSDK_PLATFORM=${REXSDK_PLATFORM}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch-rexglue-sdk.cmake")

 set(REXSDK_VERSION "")

include(generated/rexglue.cmake)

# temporary disabled
#include(cmake/build_optimizations.cmake)

# CPU baseline for all code, recompiled guest code included: x86-64-v3 (AVX2,
# BMI2, MOVBE - Haswell / Zen and newer) turns every big-endian guest memory
# access into one MOVBE and the VMX emulation into AVX. No FMA contraction:
# float results stay bit-identical to the SSE build (co-op lockstep compares
# them between PCs). Empty = the compiler default.
set(AO2_CPU_ARCH "x86-64-v3" CACHE STRING "-march for the game (empty = compiler default)")
if(AO2_CPU_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    add_compile_options(-march=${AO2_CPU_ARCH} -ffp-contract=off)
endif()

# Profile-guided optimization: when pgo/ao2.profdata is there (build.cmd
# pgo-gen, play with that build, build.cmd pgo-merge) every build uses it -
# not the instrumented one, whose flags name a profile of their own. The
# guest's threads (game, rendering) spend 6-11% less time per frame. A profile
# older than the code only misses what changed since; float results do not
# change (no fast math, contraction stays off). Ninja does not see a new
# profile: build from scratch after one.
set(AO2_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/../pgo/ao2.profdata" CACHE FILEPATH
    "Clang instrumentation profile used for profile-guided optimization when it exists")
if(EXISTS "${AO2_PGO_PROFILE}" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "profile-instr")
    add_compile_options(-fprofile-instr-use=${AO2_PGO_PROFILE} -Wno-profile-instr-unprofiled
                        -Wno-profile-instr-out-of-date -Wno-backend-plugin)
    message(STATUS "PGO profile: ${AO2_PGO_PROFILE}")
endif()

set(ARMYOFTWORECOMP_SOURCES
    src/main.cpp
    src/midasm_hooks.cpp
    src/input_script.cpp
    src/mouse_aim.cpp
    src/cutscene_skip.cpp
    src/cheats.cpp
    src/aim_assist.cpp
    src/debug_menu.cpp
    src/dlc_folder.cpp
    src/perf_overlay.cpp
    src/autopilot.cpp
    src/console_cmd.cpp
    src/movie_trace.cpp
    src/control_movie_fix.cpp
    src/coop_input_delay.cpp
    src/frame_time.cpp
    src/decouple.cpp
    src/interp_camera.cpp
    src/interp_objects.cpp
    src/render_fence.cpp
    src/render_idle.cpp
    src/limiter_wait.cpp
    src/guest_fast.cpp
    src/perf_waits.cpp
    src/frame_spikes.cpp
    src/tick_profile.cpp
    src/hang_watchdog.cpp
    src/coop_link.cpp
    src/coop_cmd.cpp
    src/gpu/occlusion_wait.cpp
    src/pgo.cpp
    src/coop_debug.cpp
    src/coop_net_pump.cpp
    src/coop_sync_trace.cpp
    src/coop_state_hash.cpp
    src/ue3_reflect.cpp
    src/coop_lod.cpp
    src/coop_streaming.cpp
    src/audio_listener_guard.cpp
    src/aspect_ratio.cpp
    src/cyrillic_fonts.cpp
    src/game_language.cpp
    src/keyboard_prompts.cpp
    src/physx_worker.cpp
    src/coop_ragdoll_pool.cpp
    src/ea_server.cpp
    src/coop_invite.cpp
    src/ui_text.cpp
    src/ui_theme.cpp
    src/graphics_menu.cpp
    src/achievements.cpp
    src/menu_mouse.cpp
    src/gpu/d3d_trace.cpp
    src/gpu/native/native_graphics_system.cpp
    src/gpu/native/pm4.cpp
    src/gpu/native/hlsl_translator.cpp
    src/gpu/native/shader_cache.cpp
    src/gpu/native/renderer.cpp
    src/gpu/native/overlay.cpp
    src/gpu/native/texture_loader.cpp
    src/gpu/xenos/register_file.cpp
    src/gpu/xenos/registers.cpp
    src/gpu/xenos/shader.cpp
    src/gpu/xenos/translator.cpp
    src/gpu/xenos/translator_disasm.cpp
    src/gpu/xenos/ucode.cpp
)

if(WIN32)
    list(APPEND ARMYOFTWORECOMP_SOURCES
        src/armyoftworecomp.rc
    )
endif()

if(WIN32)
    add_executable(armyoftworecomp WIN32 ${ARMYOFTWORECOMP_SOURCES})
else()
    add_executable(armyoftworecomp ${ARMYOFTWORECOMP_SOURCES})
endif()

rexglue_setup_target(armyoftworecomp GPU_PLUGINS xenos)

# Cheaper stack accesses in the recompiled code (see src/recomp_memory.h);
# included after the generated precompiled header.
if(TARGET armyoftworecomp_recomp)
    target_precompile_headers(armyoftworecomp_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp_memory.h")
endif()

# Line tables for the port's own code too (profiling with tools/sampler.py
# --lines; no effect on the generated code).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(AO2_CXX_SOURCES ${ARMYOFTWORECOMP_SOURCES})
    list(FILTER AO2_CXX_SOURCES INCLUDE REGEX "\\.cpp$")
    set_source_files_properties(${AO2_CXX_SOURCES} PROPERTIES COMPILE_OPTIONS "-gline-tables-only")
endif()

if(WIN32)
    target_link_libraries(armyoftworecomp PRIVATE d3d12 dxgi d3dcompiler ws2_32 iphlpapi synchronization dbghelp pdh gdi32 winmm)
endif()

# Standalone co-op lobby server (the server the game runs with
# ao2_coop_server, as a console program; no ReXGlue runtime needed).
if(WIN32)
    add_executable(ao2_lobby_server src/lobby_server_main.cpp src/ea_server.cpp)
    target_compile_definitions(ao2_lobby_server PRIVATE AO2_STANDALONE_SERVER)
    target_compile_features(ao2_lobby_server PRIVATE cxx_std_20)
    target_link_libraries(ao2_lobby_server PRIVATE ws2_32 iphlpapi)
    set_property(TARGET ao2_lobby_server PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# SDL looks for this file (name set by settings/mapping.toml's
# hid_mappings_file) next to the exe at startup to fill in gamepads it
# doesn't already recognize. Sourced from
# https://github.com/mdqinc/SDL_GameControllerDB, see settings/README.md.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt")
    add_custom_command(TARGET armyoftworecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt"
                "$<TARGET_FILE_DIR:armyoftworecomp>/gamecontrollerdb.txt"
        COMMENT "Copying gamecontrollerdb.txt next to armyoftworecomp.exe")
endif()

# Offline shader translator check (see tools/xenos_hlsl_test.cpp).
if(WIN32)
    add_executable(xenos_hlsl_test
        tools/xenos_hlsl_test.cpp
        src/gpu/native/hlsl_translator.cpp
        src/gpu/native/shader_cache.cpp
        src/gpu/xenos/register_file.cpp
        src/gpu/xenos/shader.cpp
        src/gpu/xenos/translator.cpp
        src/gpu/xenos/translator_disasm.cpp
        src/gpu/xenos/ucode.cpp
    )
    target_link_libraries(xenos_hlsl_test PRIVATE rex::runtime d3dcompiler)

    # Texture conversion check and timing (see tools/texture_loader_test.cpp).
    add_executable(texture_loader_test
        tools/texture_loader_test.cpp
        src/gpu/native/texture_loader.cpp
    )
    target_link_libraries(texture_loader_test PRIVATE rex::runtime)

    # Reads the user's disc image and TU / DLC packages for the launcher's
    # install screen (see tools/ao2_install.cpp).
    add_executable(ao2_install tools/ao2_install.cpp)
    target_link_libraries(ao2_install PRIVATE rex::runtime)
endif()
