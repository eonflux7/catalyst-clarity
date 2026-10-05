# Catalyst Clarity: DLSS for Mirror's Edge Catalyst. Built on Windows with MSVC (build.cmd).
# Output is version.dll: a proxy for System32\version.dll that the game loads from its own folder.
cmake_minimum_required(VERSION 3.24)
project(catalyst_clarity VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: nothing extra to install next to the game.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

include(FetchContent)
# SOURCE_SUBDIR points at a missing dir so FetchContent only downloads; targets are defined below.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
    SOURCE_SUBDIR _no_cmake)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.tar.gz
    SOURCE_SUBDIR _no_cmake)
FetchContent_MakeAvailable(imgui minhook)

# NVIDIA DLSS SDK (NGX): only the x64 files we need, straight from the tagged repo. Not committed and
# not redistributed on its own (LICENSE.txt in the SDK); nvngx_dlss.dll is deployed next to the game.
set(DLSS_SDK_TAG v310.9.1)
set(DLSS_SDK_DIR ${CMAKE_BINARY_DIR}/dlss-sdk-${DLSS_SDK_TAG})
set(DLSS_SDK_FILES
    LICENSE.txt
    lib/Windows_x86_64/x64/nvsdk_ngx_s.lib
    lib/Windows_x86_64/rel/nvngx_dlss.dll
    lib/Windows_x86_64/dev/nvngx_dlss.dll)
foreach(h nvsdk_ngx nvsdk_ngx_defs nvsdk_ngx_defs_dlssd nvsdk_ngx_defs_dlssg nvsdk_ngx_defs_vk nvsdk_ngx_helpers
          nvsdk_ngx_helpers_cuda nvsdk_ngx_helpers_d3d nvsdk_ngx_helpers_dlssd nvsdk_ngx_helpers_dlssd_cuda
          nvsdk_ngx_helpers_dlssd_d3d nvsdk_ngx_helpers_dlssd_vk nvsdk_ngx_helpers_dlssg nvsdk_ngx_helpers_dlssg_d3d
          nvsdk_ngx_helpers_dlssg_vk nvsdk_ngx_helpers_vk nvsdk_ngx_loader nvsdk_ngx_params nvsdk_ngx_params_dlssd
          nvsdk_ngx_params_dlssg nvsdk_ngx_standalone_common nvsdk_ngx_standalone_cuda nvsdk_ngx_vk)
    list(APPEND DLSS_SDK_FILES include/${h}.h)
endforeach()
foreach(f ${DLSS_SDK_FILES})
    if(NOT EXISTS ${DLSS_SDK_DIR}/${f})
        message(STATUS "DLSS SDK: downloading ${f}")
        file(DOWNLOAD https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_SDK_TAG}/${f} ${DLSS_SDK_DIR}/${f}.part
             STATUS st TLS_VERIFY ON)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE ${DLSS_SDK_DIR}/${f}.part)
            message(FATAL_ERROR "DLSS SDK download failed for ${f}: ${st}")
        endif()
        file(RENAME ${DLSS_SDK_DIR}/${f}.part ${DLSS_SDK_DIR}/${f})
    endif()
endforeach()
# Copied next to version.dll: rel ships in the release zip, dev has the DLSS debug overlay.
file(COPY ${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll DESTINATION ${CMAKE_BINARY_DIR}/ngx/rel)
file(COPY ${DLSS_SDK_DIR}/lib/Windows_x86_64/dev/nvngx_dlss.dll DESTINATION ${CMAKE_BINARY_DIR}/ngx/dev)

file(GLOB_RECURSE MINHOOK_SOURCES ${minhook_SOURCE_DIR}/src/*.c)
add_library(minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

add_library(catalyst_clarity SHARED
    src/core/dllmain.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/crash.cpp
    src/core/d3d_debug.cpp
    src/core/proxy_version.cpp
    src/core/proxy_version.asm
    src/core/version.def
    src/hooks/present.cpp
    src/renderer/dlss_pass.cpp
    src/renderer/gpu.cpp
    src/renderer/jitter.cpp
    src/renderer/mip_bias.cpp
    src/renderer/post.cpp
    src/renderer/render_scale.cpp
    src/renderer/taa.cpp
    src/upscaler/dlss.cpp
    src/ui/overlay.cpp)
set_target_properties(catalyst_clarity PROPERTIES OUTPUT_NAME version)
target_include_directories(catalyst_clarity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${DLSS_SDK_DIR}/include)
target_compile_definitions(catalyst_clarity PRIVATE
    CS_VERSION="${PROJECT_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(catalyst_clarity PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->")
# No static import of d3d11/dxgi: the loader waits for the game to load d3d11.dll itself.
# d3dcompiler (ImGui DX11 backend, depth-copy shader) is delay-loaded so the proxy pulls in nothing at process start.
target_link_libraries(catalyst_clarity PRIVATE minhook imgui delayimp d3dcompiler
    ${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib)
target_link_options(catalyst_clarity PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll /IGNORE:4199
    /PDBALTPATH:%_PDB%)  # only the PDB file name in the DLL, not the build path
