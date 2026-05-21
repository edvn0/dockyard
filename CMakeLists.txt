cmake_minimum_required(VERSION 3.25)

project(
  dockforge
  VERSION 0.1.0
  LANGUAGES CXX C
)

# ─── Global settings ──────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ─── Options ──────────────────────────────────────────────────────────────────

set(RENDERDOC_INCLUDE_PATH "" CACHE PATH
  "Path to directory containing renderdoc_app.h")
set(ASSETS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH
  "Path to game assets")

# ─── Dependencies ─────────────────────────────────────────────────────────────

find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED) 

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
CPMAddPackage(
  NAME ImGui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.92.5-docking
  GIT_SHALLOW YES
  DOWNLOAD_ONLY YES
)
set(TF_BUILD_CUDA CACHE FORCE "Build CUDA" OFF)
set(TF_BUILD_TESTS CACHE FORCE "" OFF)
set(TF_BUILD_EXAMPLES CACHE FORCE "" OFF)
CPMAddPackage(
  NAME taskflow
  GITHUB_REPOSITORY taskflow/taskflow
  GIT_TAG master
  GIT_SHALLOW YES
  OPTIONS
    "TF_BUILD_CUDA OFF"
    "TF_BUILD_TESTS OFF"
    "TF_BUILD_EXAMPLES OFF"
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
  OPTIONS "FASTGLTF_COMPILE_AS_CPP20 ON FASTGLTF_USE_CUSTOM_SMALLVECTOR ON FASTGLTF_DISABLE_CUSTOM_MEMORY_POOL ON"
)
CPMAddPackage(
  NAME efsw
  GITHUB_REPOSITORY SpartanJ/efsw
  GIT_TAG master  
)

set(GLM_ENABLE_SIMD_SSE2 CACHE FORCE "Enable SSE2 optimizations" ON)
set(GLM_ENABLE_SIMD_SSE3 CACHE FORCE "Enable SSE3 optimizations" ON)
set(GLM_ENABLE_SIMD_SSSE3 CACHE FORCE "Enable SSSE3 optimizations" ON)
set(GLM_ENABLE_SIMD_SSE4_1 CACHE FORCE "Enable SSE 4.1 optimizations" ON)
set(GLM_ENABLE_SIMD_SSE4_2 CACHE FORCE "Enable SSE 4.2 optimizations" ON)
set(GLM_ENABLE_SIMD_AVX CACHE FORCE "Enable AVX optimizations" ON)
set(GLM_ENABLE_SIMD_AVX2 CACHE FORCE "Enable AVX2 optimizations" ON)
set(GLM_ENABLE_SIMD_NEON CACHE FORCE "Enable ARM NEON optimizations" ON)
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

# ─── Helpers ──────────────────────────────────────────────────────────────────

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
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

# Warnings that are false positives or irrelevant for GCC/Clang in this project
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
  add_library(imgui STATIC ${IMGUI_SRCS} ${IMPLOT_SRCS} ${IMGUIZMO_SRCS})

  target_include_directories(imgui PUBLIC
    ${ImGui_SOURCE_DIR}
    ${ImGui_SOURCE_DIR}/backends
    ${ImPlot_SOURCE_DIR}
    ${ImGuizmo_SOURCE_DIR}
  )

  find_package(Freetype REQUIRED)

  target_link_libraries(imgui PUBLIC
    glfw
    volk
    volk::volk_headers
    X11::X11
    Freetype::Freetype
  )

  target_compile_definitions(imgui
    PUBLIC
    GLFW_INCLUDE_NONE
    IMGUI_IMPL_VULKAN_USE_VOLK
    IMGUI_ENABLE_FREETYPE
  )
endif()

# ─── dockyard-allocator ───────────────────────────────────────────────────────

add_library(dockyard-allocator STATIC src/dockyard/allocator.cpp)
target_compile_options(dockyard-allocator PUBLIC
  $<$<CXX_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
  $<$<CXX_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
)
target_link_libraries(dockyard-allocator
  PUBLIC VulkanMemoryAllocator
  PRIVATE volk
)

# dockyard-compiler

find_package(slang REQUIRED CONFIG)

add_library(dockyard-compiler STATIC src/dockyard/compiler.cpp)
target_link_libraries(dockyard-compiler PRIVATE slang::slang)
target_compile_definitions(dockyard-compiler PRIVATE
    ${DOCKYARD_PUBLIC_DEFINES}
    ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
    $<$<CONFIG:Debug>:DOCKYARD_DEBUG>)
target_include_directories(dockyard-compiler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(ThirdPartySTB STATIC "3PP/stb.c" "3PP/mikktspace.c")
target_compile_definitions(ThirdPartySTB PRIVATE
  STB_IMAGE_IMPLEMENTATION
  STB_IMAGE_RESIZE_IMPLEMENTATION)
target_include_directories(ThirdPartySTB PUBLIC "3PP")


get_filename_component(ASSETS_ROOT_ABS "${ASSETS_ROOT}" ABSOLUTE)

add_library(dockyard STATIC
  src/dockyard/context.cpp
  src/dockyard/app.cpp
  src/dockyard/log.cpp
  src/dockyard/buffer.cpp
  src/dockyard/event_callbacks.cpp
  src/dockyard/vfs.cpp
  src/dockyard/renderdoc.cpp
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
)
target_precompile_headers(dockyard PRIVATE include/PCH.hpp)
target_include_directories(dockyard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(dockyard
  PUBLIC
    ${DOCKYARD_PUBLIC_DEFINES}
    ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
    $<$<CONFIG:Debug>:DOCKYARD_DEBUG>
    GLM_FORCE_LEFT_HANDED
    GLM_FORCE_DEPTH_ZERO_TO_ONE
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
    fastgltf::fastgltf
    Taskflow::Taskflow
    ${CMAKE_DL_LIBS}
  PRIVATE
    efsw-static
    spdlog::spdlog
    Vulkan::Headers
)
target_set_warnings(dockyard)
set_target_properties(dockyard PROPERTIES FOLDER dockyard)

if(RENDERDOC_INCLUDE_PATH)
  if(NOT EXISTS "${RENDERDOC_INCLUDE_PATH}/renderdoc_app.h")
    message(FATAL_ERROR
      "RENDERDOC_INCLUDE_PATH is '${RENDERDOC_INCLUDE_PATH}' "
      "but renderdoc_app.h was not found there.")
  endif()
  target_include_directories(dockyard PRIVATE "${RENDERDOC_INCLUDE_PATH}")
  message(STATUS "RenderDoc: using header from ${RENDERDOC_INCLUDE_PATH}")
else()
  message(STATUS "RenderDoc: using renderdoc_app.h from default include paths")
endif()

# ─── dockforge ────────────────────────────────────────────────────────────────

add_executable(dockforge src/dockforge/main.cpp
  src/dockforge/dockforge.cpp
  src/dockforge/free_camera_controller.cpp
  src/dockforge/path_camera_controller.cpp
  src/dockforge/editor_camera.cpp
  src/dockforge/editor_utils.cpp

  src/dockforge/matrix_cache.cpp)
target_link_libraries(dockforge PRIVATE dockyard imgui)
target_set_warnings(dockforge)
set_target_properties(dockforge PROPERTIES FOLDER apps)
