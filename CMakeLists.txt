cmake_minimum_required(VERSION 3.25)
project(DSPAASR VERSION 1.0.1 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DSPAAMod currently requires Windows x64 and MSVC.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(NGX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/ngx" CACHE PATH "Official NVIDIA DLSS SDK")
if(NOT EXISTS "${NGX_ROOT}/include/nvsdk_ngx.h" OR NOT EXISTS "${NGX_ROOT}/lib/nvsdk_ngx_d.lib")
    message(FATAL_ERROR "NGX SDK is missing. Run tools/fetch-ngx.ps1 first.")
endif()
add_library(ngx-sdk STATIC IMPORTED)
set_target_properties(ngx-sdk PROPERTIES
    IMPORTED_LOCATION "${NGX_ROOT}/lib/nvsdk_ngx_d.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${NGX_ROOT}/include")

add_library(dspaa-core STATIC native/core/preset-evidence.cpp)
target_include_directories(dspaa-core PUBLIC native)
target_compile_options(dspaa-core PRIVATE /W4 /permissive- /utf-8)
set_property(TARGET dspaa-core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_library(dspaa-ngx STATIC native/ngx/session.cpp)
target_include_directories(dspaa-ngx PUBLIC native)
target_link_libraries(dspaa-ngx PUBLIC ngx-sdk d3d11)
target_compile_definitions(dspaa-ngx PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(dspaa-ngx PRIVATE /W4 /permissive- /utf-8)
set_property(TARGET dspaa-ngx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_library(DSPAANative SHARED native/bridge/bridge.cpp)
target_link_libraries(DSPAANative PRIVATE dspaa-core dspaa-ngx)
target_compile_definitions(DSPAANative PRIVATE DSPAA_BRIDGE_EXPORTS)
target_compile_options(DSPAANative PRIVATE /W4 /permissive- /utf-8)
set_property(TARGET DSPAANative PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")


add_executable(ngx-probe native/probe/main.cpp)
target_link_libraries(ngx-probe PRIVATE dspaa-core dspaa-ngx DSPAANative dxgi)
target_compile_definitions(ngx-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(ngx-probe PRIVATE /W4 /permissive- /utf-8)
set_property(TARGET ngx-probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

option(DSPAA_BUILD_DRIVER_TOOL "Build explicit, manually authorized NVAPI probe-profile tool" OFF)
if(DSPAA_BUILD_DRIVER_TOOL)
    set(NVAPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/nvapi" CACHE PATH "Official NVAPI SDK")
    add_executable(driver-profile native/tools/driver-profile.cpp)
    target_include_directories(driver-profile SYSTEM PRIVATE "${NVAPI_ROOT}")
    target_link_libraries(driver-profile PRIVATE "${NVAPI_ROOT}/nvapi64.lib")
    target_compile_definitions(driver-profile PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(driver-profile PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET driver-profile PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()


include(CTest)
if(BUILD_TESTING)
    add_executable(preset-evidence-tests native/tests/preset-evidence.cpp)
    target_link_libraries(preset-evidence-tests PRIVATE dspaa-core)
    set_property(TARGET preset-evidence-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_test(NAME preset-evidence COMMAND preset-evidence-tests)
    add_executable(bridge-tests native/tests/bridge.cpp)
    target_include_directories(bridge-tests PRIVATE native)
    target_link_libraries(bridge-tests PRIVATE DSPAANative d3d11)
    target_compile_definitions(bridge-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    set_property(TARGET bridge-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_test(NAME bridge-lifetime-fallback COMMAND bridge-tests "${NGX_ROOT}/runtime/rel")
    add_test(NAME bridge-lifetime-fallback-typeless COMMAND bridge-tests "${NGX_ROOT}/runtime/rel" --typeless)
    set_tests_properties(bridge-lifetime-fallback bridge-lifetime-fallback-typeless PROPERTIES TIMEOUT 30)

    add_test(NAME probe-cli COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:ngx-probe>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/native/tests/probe-cli.cmake)
    set_tests_properties(preset-evidence probe-cli PROPERTIES TIMEOUT 10)
    find_program(POWERSHELL_EXECUTABLE pwsh REQUIRED)
    add_test(NAME dependency-no-clobber COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/test-download.ps1")
    set_tests_properties(dependency-no-clobber PROPERTIES TIMEOUT 15)
    add_test(NAME managed-policy-interop COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/test-managed.ps1")
    add_test(NAME game-sandbox-rollback COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/test-game-sandbox.ps1")
    set_tests_properties(game-sandbox-rollback PROPERTIES TIMEOUT 15)
    set_tests_properties(managed-policy-interop PROPERTIES TIMEOUT 60)
endif()
