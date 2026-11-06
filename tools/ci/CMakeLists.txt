# What CI can build and run without the game's files: the renderer's offline tests and the lobby server, against the
# ReXGlue SDK built from source with sdk_changes applied and installed (CMAKE_PREFIX_PATH), as the game uses it. Not
# through add_subdirectory: the SDK then takes its version from this repository's tags and refuses to configure.
# The game itself needs code generated from the user's disc.
#   tools\ci\cpp_tests.cmd
cmake_minimum_required(VERSION 3.25)
project(ao2_ci LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(AO2 "${CMAKE_CURRENT_SOURCE_DIR}/../../aot" ABSOLUTE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(rexglue 0.10.0 REQUIRED CONFIG)

# The game's CPU baseline (aot/CMakeLists.txt).
add_compile_options(-march=x86-64-v3)

# The same lists as in aot/CMakeLists.txt.
add_executable(xenos_hlsl_test
    "${AO2}/tools/xenos_hlsl_test.cpp"
    "${AO2}/src/gpu/native/hlsl_translator.cpp"
    "${AO2}/src/gpu/native/shader_cache.cpp"
    "${AO2}/src/gpu/xenos/register_file.cpp"
    "${AO2}/src/gpu/xenos/shader.cpp"
    "${AO2}/src/gpu/xenos/translator.cpp"
    "${AO2}/src/gpu/xenos/translator_disasm.cpp"
    "${AO2}/src/gpu/xenos/ucode.cpp"
)
target_link_libraries(xenos_hlsl_test PRIVATE rex::runtime d3dcompiler)

add_executable(texture_loader_test
    "${AO2}/tools/texture_loader_test.cpp"
    "${AO2}/src/gpu/native/texture_loader.cpp"
)
target_link_libraries(texture_loader_test PRIVATE rex::runtime)

add_executable(ao2_lobby_server "${AO2}/src/lobby_server_main.cpp" "${AO2}/src/ea_server.cpp")
target_compile_definitions(ao2_lobby_server PRIVATE AO2_STANDALONE_SERVER)
target_link_libraries(ao2_lobby_server PRIVATE ws2_32 iphlpapi)

add_executable(invite_checks_test "${AO2}/tools/invite_checks_test.cpp")

add_custom_target(ci_all DEPENDS xenos_hlsl_test texture_loader_test ao2_lobby_server invite_checks_test)
