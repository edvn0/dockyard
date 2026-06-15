cmake_minimum_required(VERSION 3.25)

project(
        dockforge
        VERSION 0.1.0
        LANGUAGES CXX C
)

# ---- Options -----------------------------------------------------------------

option(DOCKYARD_BUILD_TOOLS       "Build Dockyard command line tools" ON)
option(DOCKYARD_BUILD_TESTING     "Build Dockyard test suite"         ON)
option(DOCKYARD_BUILD_SMOKE_TESTS "Build Dockyard smoke tests"        OFF)

# ---- Global settings ---------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (MSVC)
  add_compile_options(/EHsc)
endif ()

if (UNIX AND NOT APPLE)
  find_program(MOLD_EXECUTABLE mold)
  if (MOLD_EXECUTABLE)
    add_link_options(-fuse-ld=mold)
    message(STATUS "Using mold linker: ${MOLD_EXECUTABLE}")
  endif ()
endif ()

# ---- Paths -------------------------------------------------------------------

set(ASSETS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Path to game assets")
get_filename_component(ASSETS_ROOT_ABS "${ASSETS_ROOT}" ABSOLUTE)

# ---- Compiler flag sets (consumed by Dependencies and Targets) ---------------

set(DOCKYARD_PUBLIC_DEFINES
        VK_NO_PROTOTYPES
        GLFW_INCLUDE_VULKAN
        ${VOLK_DEFINES}
)
if (WIN32)
  list(APPEND DOCKYARD_PUBLIC_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX)
endif ()

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

# ---- System packages ---------------------------------------------------------

find_package(Vulkan REQUIRED)

if (UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  find_package(X11 REQUIRED)
endif ()

# ---- Sub-files ---------------------------------------------------------------

include(cmake/DockyardRenderDoc.cmake)
include(cmake/Utils.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Targets.cmake)
