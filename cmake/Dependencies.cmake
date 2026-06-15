include(cmake/CPM.cmake)

# ---- Platform detection -------------------------------------------------------

if (WIN32)
  set(VOLK_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif (APPLE)
  set(VOLK_DEFINES VK_USE_PLATFORM_METAL_EXT)
elseif (UNIX)
  pkg_check_modules(WAYLAND wayland-client)
  if (WAYLAND_FOUND)
    set(VOLK_DEFINES VK_USE_PLATFORM_WAYLAND_KHR)
  else ()
    set(VOLK_DEFINES VK_USE_PLATFORM_XLIB_KHR)
  endif ()
endif ()

# ---- Vulkan ------------------------------------------------------------------

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
        OPTIONS "VK_BOOTSTRAP_POSITION_INDEPENDENT_CODE ON"
)

CPMAddPackage(
        NAME vma
        GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
        GIT_TAG master
        OPTIONS "VMA_ENABLE_INSTALL OFF"
)

CPMAddPackage(
        NAME ktx
        GITHUB_REPOSITORY KhronosGroup/KTX-Software
        GIT_TAG main
        OPTIONS
        "KTX_FEATURE_STATIC_LIBRARY ON"
        "KTX_FEATURE_TOOLS OFF"
        "KTX_FEATURE_DOCS OFF"
        "KTX_FEATURE_TESTS OFF"
)

# KTX-Software HEAD bundles an old vulkan_core.h inside dfdutils which
# shadows the real Vulkan SDK headers. Strip it from ktx's public interface.
foreach(_ktx_target IN ITEMS ktx ktx_read)
  if (TARGET ${_ktx_target})
    get_target_property(_ktx_iface_includes ${_ktx_target} INTERFACE_INCLUDE_DIRECTORIES)
    if (_ktx_iface_includes)
      list(FILTER _ktx_iface_includes EXCLUDE REGEX "dfdutils")
      set_target_properties(${_ktx_target} PROPERTIES
              INTERFACE_INCLUDE_DIRECTORIES "${_ktx_iface_includes}")
    endif ()
  endif ()
endforeach()

# ---- ECS / math / utility ----------------------------------------------------

CPMAddPackage(
        NAME entt
        GITHUB_REPOSITORY skypjack/entt
        GIT_TAG main
)

set(GLM_ENABLE_SIMD_SSE2  ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_SSE3  ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_SSSE3 ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_SSE4_1 ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_SSE4_2 ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_AVX   ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_AVX2  ON CACHE BOOL "" FORCE)
set(GLM_ENABLE_SIMD_NEON  ON CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY     OFF CACHE BOOL "" FORCE)

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
        "GLM_BUILD_LIBRARY=OFF"
)

CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG v1.x
        OPTIONS "SPDLOG_USE_STD_FORMAT ON"
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

# ---- Windowing / UI ----------------------------------------------------------

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

if (UNIX AND NOT APPLE)
  set(NFD_PORTAL ON CACHE BOOL "" FORCE)
endif ()

CPMAddPackage(
        NAME nfd
        GITHUB_REPOSITORY btzy/nativefiledialog-extended
        GIT_TAG v1.2.1
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

# ---- Asset loading -----------------------------------------------------------

CPMAddPackage(
        URI "gh:spnda/fastgltf@0.9.0"
        GIT_SHALLOW YES
        OPTIONS
        "FASTGLTF_COMPILE_AS_CPP20 ON"
        "FASTGLTF_USE_CUSTOM_SMALLVECTOR OFF"
        "FASTGLTF_DISABLE_CUSTOM_MEMORY_POOL ON"
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

# ---- Profiling ---------------------------------------------------------------

set(DOCKYARD_ENABLE_TRACY ON CACHE BOOL "Enable Tracy profiling")
if (CMAKE_BUILD_TYPE STREQUAL "Release")
  set(DOCKYARD_ENABLE_TRACY OFF CACHE BOOL "Enable Tracy profiling" FORCE)
endif ()

if (DOCKYARD_ENABLE_TRACY)
  CPMAddPackage(
          NAME Tracy
          GITHUB_REPOSITORY wolfpld/tracy
          GIT_TAG 05cceee
          OPTIONS
          "TRACY_ENABLE ON"
          "TRACY_ON_DEMAND ON"
          "TRACY_NO_CALLSTACK ON"
          "TRACY_VK_USE_SYMBOL_TABLE ON"
          "CMAKE_POSITION_INDEPENDENT_CODE ON"
          "TRACY_STATIC OFF"
  )
endif ()

# ---- Tools / testing (optional) ---------------------------------------------

if (DOCKYARD_BUILD_TOOLS)
  CPMAddPackage(
          NAME cgltf
          GITHUB_REPOSITORY jkuhlmann/cgltf
          GIT_TAG master
          GIT_SHALLOW YES
          DOWNLOAD_ONLY YES
  )
endif ()

if (DOCKYARD_BUILD_TESTING)
  CPMAddPackage(
          NAME doctest
          GITHUB_REPOSITORY doctest/doctest
          GIT_TAG v2.5.2
  )
  enable_testing()
  if (doctest_ADDED)
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
  endif ()
endif ()

# ---- Assembled third-party targets ------------------------------------------

if (ImGui_SOURCE_DIR AND ImPlot_SOURCE_DIR AND ImGuizmo_SOURCE_DIR)
  add_library(imgui STATIC
          ${ImGui_SOURCE_DIR}/imgui.cpp
          ${ImGui_SOURCE_DIR}/imgui_demo.cpp
          ${ImGui_SOURCE_DIR}/imgui_draw.cpp
          ${ImGui_SOURCE_DIR}/imgui_tables.cpp
          ${ImGui_SOURCE_DIR}/imgui_widgets.cpp
          ${ImGui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
          ${ImGui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
          ${ImGui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
          ${ImPlot_SOURCE_DIR}/implot.cpp
          ${ImPlot_SOURCE_DIR}/implot_items.cpp
          ${ImGuizmo_SOURCE_DIR}/ImGuizmo.cpp
  )
  target_include_directories(imgui PUBLIC
          ${ImGui_SOURCE_DIR}
          ${ImGui_SOURCE_DIR}/backends
          ${ImPlot_SOURCE_DIR}
          ${ImGuizmo_SOURCE_DIR}
  )
  target_link_libraries(imgui PUBLIC glfw volk volk::volk_headers freetype)
  target_compile_definitions(imgui PUBLIC
          GLFW_INCLUDE_NONE
          IMGUI_IMPL_VULKAN_USE_VOLK
          IMGUI_ENABLE_FREETYPE
  )
  if (UNIX AND NOT APPLE)
    target_link_libraries(imgui PUBLIC X11::X11)
  endif ()
endif ()

set(THIRD_PARTY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party")
file(MAKE_DIRECTORY "${THIRD_PARTY_GENERATED_DIR}")
file(WRITE "${THIRD_PARTY_GENERATED_DIR}/stb_impl.cpp"
        "#define STB_IMAGE_IMPLEMENTATION\n#define STB_IMAGE_RESIZE_IMPLEMENTATION\n#include <stb_image.h>\n#include <stb_image_resize2.h>\n"
)

add_library(ThirdPartySTB STATIC
        "${THIRD_PARTY_GENERATED_DIR}/stb_impl.cpp"
        "${mikktspace_SOURCE_DIR}/mikktspace.c"
)
target_include_directories(ThirdPartySTB PUBLIC
        "${stb_SOURCE_DIR}"
        "${mikktspace_SOURCE_DIR}"
        "${PerlinNoise_SOURCE_DIR}"
)
target_compile_options(ThirdPartySTB PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
        $<$<CXX_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
        $<$<C_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
        $<$<C_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
)

# ---- Solution folders --------------------------------------------------------

set_solution_folder("third_party/windowing" glfw nfd)
set_solution_folder("third_party/vulkan"
        volk volk_headers vk-bootstrap VulkanMemoryAllocator
        ktx ktx_read ktx_version ktx_basisu
        astcenc-avx2-static astcenc-sse4.1-static astcenc-static
)
set_solution_folder("third_party/ui" imgui)
set_solution_folder("third_party/utility"
        EnTT spdlog efsw-static meshoptimizer freetype ThirdPartySTB glm
)
if (DOCKYARD_ENABLE_TRACY)
  set_solution_folder("third_party/utility" TracyClient)
endif ()
