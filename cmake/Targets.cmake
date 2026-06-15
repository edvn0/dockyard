find_package(slang REQUIRED CONFIG)

# ---- dockyard-allocator ------------------------------------------------------

add_library(dockyard-allocator STATIC src/dockyard/allocator.cpp)

target_compile_options(dockyard-allocator PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:${DOCKYARD_SUPPRESSED_WARNINGS}>
        $<$<CXX_COMPILER_ID:Clang>:${DOCKYARD_SUPPRESSED_WARNINGS}>
)
target_link_libraries(dockyard-allocator
        PUBLIC VulkanMemoryAllocator
        PRIVATE volk
)
set_target_properties(dockyard-allocator PROPERTIES FOLDER dockyard)

# ---- dockyard-compiler -------------------------------------------------------

add_library(dockyard-compiler STATIC src/dockyard/compiler.cpp)

target_link_libraries(dockyard-compiler PRIVATE slang::slang)
target_compile_definitions(dockyard-compiler PRIVATE
        ${DOCKYARD_PUBLIC_DEFINES}
        ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
        $<$<CONFIG:Debug>:DOCKYARD_DEBUG>
)
target_include_directories(dockyard-compiler PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
set_target_properties(dockyard-compiler PROPERTIES FOLDER dockyard)

# ---- dockyard ----------------------------------------------------------------

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
        REAL_SOURCE  "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc.cpp"
        STUB_SOURCE  "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc_stub.cpp"
        INCLUDE_CACHE_VARIABLE RENDERDOC_INCLUDE_PATH
)

if (ktx_SOURCE_DIR)
  target_include_directories(dockyard PRIVATE
          "${ktx_SOURCE_DIR}/external/basis_universal/encoder"
          "${ktx_SOURCE_DIR}/external/basis_universal/transcoder"
  )
  # dfdutils is stripped from ktx's public interface (see Dependencies.cmake).
  # ktx.h still needs <KHR/khr_df.h>, so expose it via a stub directory.
  set(_khr_stub_dir "${CMAKE_CURRENT_BINARY_DIR}/khr_stub")
  file(MAKE_DIRECTORY "${_khr_stub_dir}/KHR")
  file(WRITE "${_khr_stub_dir}/KHR/khr_df.h"
          "#pragma once\n#include \"${ktx_SOURCE_DIR}/external/dfdutils/KHR/khr_df.h\"\n"
  )
  target_include_directories(dockyard PRIVATE "${_khr_stub_dir}")
endif ()

target_compile_options(dockyard PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
target_enable_native_arch(dockyard)
target_precompile_headers(dockyard PRIVATE include/PCH.hpp)

target_include_directories(dockyard PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ThreadPool_SOURCE_DIR}/include
)
target_compile_definitions(dockyard PUBLIC
        ${DOCKYARD_PUBLIC_DEFINES}
        ASSETS_ROOT_PATH="${ASSETS_ROOT_ABS}"
        $<$<CONFIG:Debug>:DOCKYARD_DEBUG>
        GLM_FORCE_LEFT_HANDED
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        $<$<BOOL:${DOCKYARD_ENABLE_TRACY}>:TRACY_ENABLE>
        $<$<BOOL:${DOCKYARD_ENABLE_TRACY}>:TRACY_VK_USE_SYMBOL_TABLE>
        $<$<BOOL:${DOCKYARD_ENABLE_TRACY}>:TRACY_IMPORTS>
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
        $<$<BOOL:${DOCKYARD_ENABLE_TRACY}>:TracyClient>
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
set_target_properties(dockyard PROPERTIES FOLDER dockyard)

# ---- dockforge ---------------------------------------------------------------

add_executable(dockforge
        src/dockforge/main.cpp
        src/dockforge/dockforge.cpp
        src/dockforge/free_camera_controller.cpp
        src/dockforge/path_camera_controller.cpp
        src/dockforge/editor_camera.cpp
        src/dockforge/editor_utils.cpp
        src/dockforge/matrix_cache.cpp
        src/dockforge/scene_outliner_panel.cpp
        src/dockforge/inspector_panel.cpp
)
target_enable_native_arch(dockforge)
target_link_libraries(dockforge PRIVATE dockyard imgui nfd)
target_compile_options(dockforge PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
target_set_warnings(dockforge)
set_target_properties(dockforge PROPERTIES FOLDER apps)

add_custom_command(TARGET dockforge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:dockforge>
            $<TARGET_FILE_DIR:dockforge>
        COMMAND_EXPAND_LISTS
)

# ---- sandbox -----------------------------------------------------------------

add_library(sandbox SHARED src/sandbox/sandbox.cpp)
target_link_libraries(sandbox PRIVATE dockyard)
target_enable_native_arch(sandbox)
target_compile_options(sandbox PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
set_target_properties(sandbox PROPERTIES
        FOLDER apps
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY                "${ASSETS_ROOT_ABS}/binary"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${ASSETS_ROOT_ABS}/binary"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${ASSETS_ROOT_ABS}/binary"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASSETS_ROOT_ABS}/binary"
        LIBRARY_OUTPUT_DIRECTORY                "${ASSETS_ROOT_ABS}/binary"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${ASSETS_ROOT_ABS}/binary"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${ASSETS_ROOT_ABS}/binary"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASSETS_ROOT_ABS}/binary"
)

# ---- Tools -------------------------------------------------------------------

if (DOCKYARD_BUILD_TOOLS)
  add_executable(gltf-imagemap tools/gltf-imagemap.cpp)
  target_compile_features(gltf-imagemap PRIVATE cxx_std_23)
  target_include_directories(gltf-imagemap PRIVATE ${cgltf_SOURCE_DIR})
  target_set_warnings(gltf-imagemap)
  set_target_properties(gltf-imagemap PROPERTIES FOLDER tools)
endif ()

# ---- Tests -------------------------------------------------------------------

if (DOCKYARD_BUILD_TESTING)
  set(TEST_DLL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_dlls")

  foreach(_dll IN ITEMS test-dll-valid test-dll-no-symbol test-dll-null-factory)
    string(REPLACE "test-dll-" "" _stem "${_dll}")
    string(REPLACE "-" "_" _stem "${_stem}")
    add_library(${_dll} SHARED "${CMAKE_SOURCE_DIR}/tests/helpers/dll_${_stem}.cpp")
    set_target_properties(${_dll} PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY                "${TEST_DLL_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${TEST_DLL_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${TEST_DLL_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TEST_DLL_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY                "${TEST_DLL_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${TEST_DLL_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${TEST_DLL_OUTPUT_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TEST_DLL_OUTPUT_DIR}"
            FOLDER tests/helpers
    )
  endforeach()

  target_include_directories(test-dll-valid        PRIVATE include)
  target_include_directories(test-dll-null-factory PRIVATE include)

  add_executable(dockyard-testing
          ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
          ${CMAKE_SOURCE_DIR}/tests/test_scene_serialisation.cpp
          ${CMAKE_SOURCE_DIR}/tests/test_game_dll.cpp
  )
  add_dependencies(dockyard-testing
          test-dll-valid test-dll-no-symbol test-dll-null-factory)
  target_enable_native_arch(dockyard-testing)
  target_set_warnings(dockyard-testing)
  target_compile_definitions(dockyard-testing PRIVATE
          "TEST_DLL_DIR=\"${TEST_DLL_OUTPUT_DIR}\""
          "TEST_DLL_VALID_NAME=\"$<TARGET_FILE_NAME:test-dll-valid>\""
          "TEST_DLL_NO_SYMBOL_NAME=\"$<TARGET_FILE_NAME:test-dll-no-symbol>\""
          "TEST_DLL_NULL_FACTORY_NAME=\"$<TARGET_FILE_NAME:test-dll-null-factory>\""
          "TEST_DLL_SUFFIX=\"${CMAKE_SHARED_LIBRARY_SUFFIX}\""
  )
  if (DOCKYARD_BUILD_SMOKE_TESTS)
    target_compile_definitions(dockyard-testing PRIVATE RUN_SERIALISATION_SMOKE_TESTS)
  endif ()
  target_link_libraries(dockyard-testing PRIVATE dockyard doctest::doctest)

  add_custom_command(TARGET dockyard-testing POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:dockyard-testing>
              $<TARGET_FILE_DIR:dockyard-testing>
          COMMAND_EXPAND_LISTS
  )

  include(doctest)
  doctest_discover_tests(dockyard-testing)

  set_solution_folder("tests"
          dockyard-testing test-dll-valid test-dll-no-symbol test-dll-null-factory)
endif ()

# ---- Developer tooling -------------------------------------------------------

add_custom_target(install-hooks
    COMMAND git config core.hooksPath scripts/hooks
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Installing git hooks"
)
set_solution_folder("tools" install-hooks)
