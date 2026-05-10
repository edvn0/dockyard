cmake_minimum_required(VERSION 3.25)

project(
  dockforge
  VERSION 0.1.0
  LANGUAGES CXX
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
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG docking
  DOWNLOAD_ONLY YES
)
CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG master
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
)

# ─── imgui ────────────────────────────────────────────────────────────────────

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
  IMGUI_IMPL_VULKAN_USE_VOLK
  ${DOCKYARD_PUBLIC_DEFINES}
)
target_link_libraries(imgui PUBLIC glfw volk)
set_target_properties(imgui PROPERTIES FOLDER external)

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

# ─── dockyard ─────────────────────────────────────────────────────────────────

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
    imgui
    glfw
    volk
    vk-bootstrap::vk-bootstrap
    EnTT::EnTT
    glm::glm
    ${CMAKE_DL_LIBS}
  PRIVATE
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

add_executable(dockforge src/dockforge/main.cpp)
target_link_libraries(dockforge PRIVATE dockyard)
target_set_warnings(dockforge)
set_target_properties(dockforge PROPERTIES FOLDER apps)