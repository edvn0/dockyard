cmake_minimum_required(VERSION 3.25)

project(
  dockforge
  VERSION 0.1.0
  LANGUAGES CXX C
)

option(DOCKYARD_BUILD_TOOLS "Build Dockyard command line tools" ON)

if(UNIX AND NOT APPLE)
  find_program(MOLD_EXECUTABLE mold)
  if(MOLD_EXECUTABLE)
    add_link_options(-fuse-ld=mold)
    message(STATUS "Using mold linker: ${MOLD_EXECUTABLE}")
  endif()
endif()

include(cmake/DockyardRenderDoc.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

function(set_solution_folder folder_name)
  foreach(target_name IN LISTS ARGN)
    if(TARGET ${target_name})
      set_target_properties(${target_name} PROPERTIES
        FOLDER "${folder_name}"
      )
    endif()
  endforeach()
endfunction()

function(target_enable_native_arch target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2>
    )
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-march=native>
    )
  endif()
endfunction()

set(ASSETS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH
  "Path to game assets")
get_filename_component(ASSETS_ROOT_ABS "${ASSETS_ROOT}" ABSOLUTE)

find_package(Vulkan REQUIRED)

if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  find_package(X11 REQUIRED)
endif()

include(cmake/CPM.cmake)

if(WIN32)
  set(VOLK_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif(APPLE)
  set(VOLK_DEFINES VK_USE_PLATFORM_METAL_EXT)
elseif(UNIX)
  pkg_check_modules(WAYLAND wayland-client)

  if(WAYLAND_FOUND)
    set(VOLK_DEFINES VK_USE_PLATFORM_WAYLAND_KHR)
  else()
    set(VOLK_DEFINES VK_USE_PLATFORM_XLIB_KHR)
  endif()
endif()

CPMAddPackage(
  NAME volk
  GITHUB_REPOSITORY zeux/volk
  GIT_TAG master
  OPTIONS "VOLK_STATIC_DEFINES=${VOLK_DEFINES}"
)

CPMAddPackage(
  NAME vk-bootstrap
  GITHUB_REPOSITORY charles-lunarg/vk-bootstrap
  GIT_TAG main
)

CPMAddPackage(
  NAME entt
  GITHUB_REPOSITORY skypjack/entt
  GIT_TAG main
)

CPMAddPackage(
  NAME vma
  GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
  GIT_TAG master
  OPTIONS "VMA_ENABLE_INSTALL OFF"
)

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  GIT_TAG v1.x
  OPTIONS "SPDLOG_USE_STD_FORMAT ON"
)

CPMAddPackage(
  NAME glfw
  GITHUB_REPOSITORY glfw/glfw
  GIT_TAG master
  OPTIONS
    "GLFW_BUILD_DOCS OFF"
    "GLFW_BUILD_TESTS OFF"
    "GLFW_BUILD_EXAMPLES OFF"
    "GLFW_INSTALL OFF"
)

if(UNIX AND NOT APPLE)
  set(NFD_PORTAL ON CACHE BOOL "" FORCE)
endif()

CPMAddPackage(
  NAME nfd
  GITHUB_REPOSITORY btzy/nativefiledialog-extended
  GIT_TAG v1.2.1
)

CPMAddPackage(
  NAME ImGui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.92.5-docking
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME ImPlot
  GITHUB_REPOSITORY epezent/implot
  GIT_TAG bd99f8d8b9a80c493782ab8cb3335af102f61304
  GIT_SHALLOW NO
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME ImGuizmo
  GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
  GIT_TAG a15acd87a3f3241a29ea1363ceafc680dca3a96b
  GIT_SHALLOW NO
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  URI "gh:spnda/fastgltf@0.9.0"
  GIT_SHALLOW YES
  OPTIONS
    "FASTGLTF_COMPILE_AS_CPP20 ON"
    "FASTGLTF_USE_CUSTOM_SMALLVECTOR OFF"
    "FASTGLTF_DISABLE_CUSTOM_MEMORY_POOL ON"
)

CPMAddPackage(
  NAME efsw
  GITHUB_REPOSITORY SpartanJ/efsw
  GIT_TAG master
)

CPMAddPackage(
  URI "gh:zeux/meshoptimizer@1.0.1"
  GIT_SHALLOW YES
)

CPMAddPackage(
  NAME ThreadPool
  GITHUB_REPOSITORY bshoshany/thread-pool
  GIT_TAG v5.1.0
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME Tracy
  GITHUB_REPOSITORY wolfpld/tracy
  GIT_TAG 2c6adfb416004f38c57094a4e796206edf89f8ff
  OPTIONS
    "TRACY_ENABLE ON"
    "TRACY_ON_DEMAND ON"
    "TRACY_NO_CALLSTACK OFF"
    "TRACY_VK_USE_SYMBOL_TABLE ON"
)

CPMAddPackage(
  NAME freetype
  GITHUB_REPOSITORY freetype/freetype
  GIT_TAG VER-2-13-3
  OPTIONS
    "BUILD_SHARED_LIBS OFF"
    "FT_DISABLE_ZLIB ON"
    "FT_DISABLE_BZIP2 ON"
    "FT_DISABLE_PNG ON"
    "FT_DISABLE_HARFBUZZ ON"
    "FT_DISABLE_BROTLI ON"
)

CPMAddPackage(
  NAME ktx
  GITHUB_REPOSITORY KhronosGroup/KTX-Software
  GIT_TAG v4.4.2
  OPTIONS
    "KTX_FEATURE_STATIC_LIBRARY ON"
    "KTX_FEATURE_TOOLS OFF"
    "KTX_FEATURE_DOCS OFF"
    "KTX_FEATURE_TESTS OFF"
)

CPMAddPackage(
  NAME stb
  GITHUB_REPOSITORY nothings/stb
  GIT_TAG master
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME mikktspace
  GITHUB_REPOSITORY mmikk/MikkTSpace
  GIT_TAG master
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)

CPMAddPackage(
  NAME PerlinNoise
  GITHUB_REPOSITORY Reputeless/PerlinNoise
  GIT_TAG master
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)

set(GLM_ENABLE_SIMD_SSE2 ON CACHE BOOL "Enable SSE2 optimizations" FORCE)
set(GLM_ENABLE_SIMD_SSE3 ON CACHE BOOL "Enable SSE3 optimizations" FORCE)
set(GLM_ENABLE_SIMD_SSSE3 ON CACHE BOOL "Enable SSSE3 optimizations" FORCE)
set(GLM_ENABLE_SIMD_SSE4_1 ON CACHE BOOL "Enable SSE 4.1 optimizations" FORCE)
set(GLM_ENABLE_SIMD_SSE4_2 ON CACHE BOOL "Enable SSE 4.2 optimizations" FORCE)
set(GLM_ENABLE_SIMD_AVX ON CACHE BOOL "Enable AVX optimizations" FORCE)
set(GLM_ENABLE_SIMD_AVX2 ON CACHE BOOL "Enable AVX2 optimizations" FORCE)
set(GLM_ENABLE_SIMD_NEON ON CACHE BOOL "Enable ARM NEON optimizations" FORCE)

CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG master
  OPTIONS
    "GLM_ENABLE_SIMD_SSE2=ON"
    "GLM_ENABLE_SIMD_SSE3=ON"
    "GLM_ENABLE_SIMD_SSSE3=ON"
    "GLM_ENABLE_SIMD_SSE4_1=ON"
    "GLM_ENABLE_SIMD_SSE4_2=ON"
    "GLM_ENABLE_SIMD_AVX=ON"
    "GLM_ENABLE_SIMD_AVX2=ON"
    "GLM_ENABLE_SIMD_NEON=ON"
    "GLM_FORCE_PURE=ON"
)

if(DOCKYARD_BUILD_TOOLS)
  CPMAddPackage(
    NAME cgltf
    GITHUB_REPOSITORY jkuhlmann/cgltf
    GIT_TAG master
    GIT_SHALLOW YES
    DOWNLOAD_ONLY YES
  )
endif()

function(target_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

set(DOCKYARD_PUBLIC_DEFINES
  VK_NO_PROTOTYPES
  GLFW_INCLUDE_VULKAN
  ${VOLK_DEFINES}
)

if(WIN32)
  list(APPEND DOCKYARD_PUBLIC_DEFINES
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
endif()

set(DOCKYARD_SUPPRESSED_WARNINGS
  -Wno-nullability-completeness
  -Wno-nullability-extension
  -Wno-c2y-extensions
  -Wno-free-nonheap-object
  -Wno-missing-designated-field-initializers
  -Wno-sign-conversion
  -Wno-implicit-int-float-conversion
  -Wno-shorten-64-to-32
)

if(ImGui_SOURCE_DIR AND ImPlot_SOURCE_DIR AND ImGuizmo_SOURCE_DIR)
  set(IMGUI_SRCS
    ${ImGui_SOURCE_DIR}/imgui.cpp
    ${ImGui_SOURCE_DIR}/imgui_demo.cpp
    ${ImGui_SOURCE_DIR}/imgui_draw.cpp
    ${ImGui_SOURCE_DIR}/imgui_tables.cpp
    ${ImGui_SOURCE_DIR}/imgui_widgets.cpp
    ${ImGui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${ImGui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${ImGui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
  )

  set(IMPLOT_SRCS
    ${ImPlot_SOURCE_DIR}/implot.cpp
    ${ImPlot_SOURCE_DIR}/implot_items.cpp
  )

  set(IMGUIZMO_SRCS
    ${ImGuizmo_SOURCE_DIR}/ImGuizmo.cpp
  )

  add_library(imgui STATIC
    ${IMGUI_SRCS}
    ${IMPLOT_SRCS}
    ${IMGUIZMO_SRCS}
  )

  target_include_directories(imgui PUBLIC
    ${ImGui_SOURCE_DIR}
    ${ImGui_SOURCE_DIR}/backends
    ${ImPlot_SOURCE_DIR}
    ${ImGuizmo_SOURCE_DIR}
  )

  target_link_libraries(imgui PUBLIC
    glfw
    volk
    volk::volk_headers
    freetype
  )

  if(UNIX AND NOT APPLE)
    target_link_libraries(imgui PUBLIC X11::X11)
  endif()

  target_compile_definitions(imgui
    PUBLIC
      GLFW_INCLUDE_NONE
      IMGUI_IMPL_VULKAN_USE_VOLK
      IMGUI_ENABLE_FREETYPE
  )
endif()

set(THIRD_PARTY_GENERATED_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party"
)

file(MAKE_DIRECTORY "${THIRD_PARTY_GENERATED_DIR}")

file(WRITE "${THIRD_PARTY_GENERATED_DIR}/stb_impl.cpp"
"#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
"
)

add_library(ThirdPartySTB STATIC
  "${THIRD_PARTY_GENERATED_DIR}/stb_impl.cpp"
  "${mikktspace_SOURCE_DIR}/mikktspace.c"
)

target_include_directories(ThirdPartySTB
  PUBLIC
    "${stb_SOURCE_DIR}"
    "${mikktspace_SOURCE_DIR}"
    "${PerlinNoise_SOURCE_DIR}"
)

target_compile_options(ThirdPartySTB
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
    $<$<CXX_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
    $<$<C_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
    $<$<C_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
)

set_solution_folder("third_party/windowing"
  glfw
  nfd
)

set_solution_folder("third_party/vulkan"
  volk
  volk_headers
  vk-bootstrap
  VulkanMemoryAllocator
  ktx
  ktx_read
  ktx_version
  ktx_basisu
  astcenc-avx2-static
  astcenc-sse4.1-static
  astcenc-static
)

set_solution_folder("third_party/ui"
  imgui
)

set_solution_folder("third_party/utility"
  EnTT
  spdlog
  efsw-static
  meshoptimizer
  TracyClient
  freetype
  ThirdPartySTB
  glm
)

add_library(dockyard-allocator STATIC
  src/dockyard/allocator.cpp
)

target_compile_options(dockyard-allocator PUBLIC
  $<$<CXX_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
  $<$<CXX_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
)

target_link_libraries(dockyard-allocator
  PUBLIC
    VulkanMemoryAllocator
  PRIVATE
    volk
)

find_package(slang REQUIRED CONFIG)

add_library(dockyard-compiler STATIC
  src/dockyard/compiler.cpp
)

target_link_libraries(dockyard-compiler PRIVATE
  slang::slang
)

target_compile_definitions(dockyard-compiler PRIVATE
  ${DOCKYARD_PUBLIC_DEFINES}
  ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
  $<$<CONFIG:Debug>:DOCKYARD_DEBUG>
)

target_include_directories(dockyard-compiler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(dockyard STATIC
  src/dockyard/context.cpp
  src/dockyard/app.cpp
  src/dockyard/log.cpp
  src/dockyard/buffer.cpp
  src/dockyard/event_callbacks.cpp
  src/dockyard/vfs.cpp
  src/dockyard/device_geometry.cpp
  src/dockyard/bindless_descriptor.cpp
  src/dockyard/texture.cpp
  src/dockyard/scene_renderer.cpp
  src/dockyard/imgui_renderer.cpp
  src/dockyard/pipeline_builder.cpp
  src/dockyard/mesh_loader.cpp
  src/dockyard/scene.cpp
  src/dockyard/shader_watcher.cpp
  src/dockyard/canvas_renderer.cpp
  src/dockyard/components.cpp
  src/dockyard/component_traits.cpp
  src/dockyard/bindless_handle.cpp
  src/dockyard/texture_upload_pool.cpp
  src/dockyard/game_memory.cpp
  src/dockyard/game_dll.cpp
)

dockyard_configure_renderdoc(dockyard
  REAL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc.cpp"
  STUB_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc_stub.cpp"
  INCLUDE_CACHE_VARIABLE
    RENDERDOC_INCLUDE_PATH
)

target_compile_options(dockyard PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/MP>
)

target_enable_native_arch(dockyard)
target_precompile_headers(dockyard PRIVATE include/PCH.hpp)

target_include_directories(dockyard PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${ThreadPool_SOURCE_DIR}/include
)

target_compile_definitions(dockyard
  PUBLIC
    ${DOCKYARD_PUBLIC_DEFINES}
    ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
    $<$<CONFIG:Debug>:DOCKYARD_DEBUG>
    GLM_FORCE_LEFT_HANDED
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    TRACY_ENABLE
    TRACY_VK_USE_SYMBOL_TABLE
)

target_link_libraries(dockyard
  PUBLIC
    dockyard-allocator
    dockyard-compiler
    imgui
    glfw
    volk
    vk-bootstrap::vk-bootstrap
    EnTT::EnTT
    glm::glm
    ThirdPartySTB
    TracyClient
    ${CMAKE_DL_LIBS}
  PRIVATE
    fastgltf::fastgltf
    meshoptimizer
    efsw-static
    spdlog::spdlog
    Vulkan::Headers
    ktx
)

target_set_warnings(dockyard)

set_target_properties(dockyard PROPERTIES
  FOLDER dockyard
)

set_target_properties(dockyard-allocator PROPERTIES
  FOLDER dockyard
)

set_target_properties(dockyard-compiler PROPERTIES
  FOLDER dockyard
)

add_executable(dockforge
  src/dockforge/main.cpp
  src/dockforge/dockforge.cpp
  src/dockforge/free_camera_controller.cpp
  src/dockforge/path_camera_controller.cpp
  src/dockforge/editor_camera.cpp
  src/dockforge/editor_utils.cpp
  src/dockforge/matrix_cache.cpp
)

target_enable_native_arch(dockforge)

target_link_libraries(dockforge PRIVATE
  dockyard
  imgui
  nfd
)

target_compile_options(dockforge PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/MP>
)

target_set_warnings(dockforge)

set_target_properties(dockforge PROPERTIES
  FOLDER apps
)

add_library(sandbox SHARED
  src/sandbox/sandbox.cpp
)

target_link_libraries(sandbox PRIVATE dockyard)
target_enable_native_arch(sandbox)

target_compile_options(sandbox PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/MP>
)

set_target_properties(sandbox PROPERTIES
  FOLDER apps
  PREFIX ""
)

if(DOCKYARD_BUILD_TOOLS)
  add_executable(gltf-imagemap
    tools/gltf-imagemap.cpp
  )

  target_compile_features(gltf-imagemap PRIVATE cxx_std_23)

  target_include_directories(gltf-imagemap PRIVATE
    ${cgltf_SOURCE_DIR}
  )

  target_set_warnings(gltf-imagemap)

  set_target_properties(gltf-imagemap PROPERTIES
    FOLDER tools
  )
endif()
