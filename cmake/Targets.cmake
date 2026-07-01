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
        src/dockyard/archive.cpp
        src/dockyard/context.cpp
        src/dockyard/app.cpp
        src/dockyard/log.cpp
        src/dockyard/buffer.cpp
        src/dockyard/event_callbacks.cpp
        src/dockyard/vfs.cpp
        src/dockyard/device_geometry.cpp
        src/dockyard/animation.cpp
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
        src/dockyard/image_decoder.cpp
        src/dockyard/script_engine.cpp
        src/dockyard/physics_world.cpp
)

dockyard_configure_renderdoc(dockyard
        REAL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc.cpp"
        STUB_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/dockyard/renderdoc_stub.cpp"
        INCLUDE_CACHE_VARIABLE RENDERDOC_INCLUDE_PATH
)

if(ktx_SOURCE_DIR)
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
endif()

target_compile_options(dockyard PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
target_enable_native_arch(dockyard)
target_precompile_headers(dockyard PRIVATE include/PCH.hpp)
set_target_properties(dockyard PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 8)

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
target_include_directories(dockyard PRIVATE "${bullet3_SOURCE_DIR}/src")
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
        nfd
        ThirdPartySTB
        $<$<BOOL:${DOCKYARD_ENABLE_TRACY}>:TracyClient>
        ${CMAKE_DL_LIBS}
        PRIVATE
        fastgltf::fastgltf
        meshoptimizer
        efsw-static
        spdlog::spdlog
        volk::volk_headers
        ktx
        lua_static
        sol2::sol2
        miniz
        libzstd_static
        BulletDynamics
        BulletCollision
        LinearMath
)
target_compile_definitions(dockyard PRIVATE
        $<$<CONFIG:Debug>:SOL_ALL_SAFETIES_ON=1>
)
# sol2 headers are heavy template code; relax warnings for the one TU that includes them.
# Skip PCH to avoid the warning-level inconsistency MSVC raises when overriding /W4→/W3.
set_source_files_properties(src/dockyard/script_engine.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        SKIP_UNITY_BUILD_INCLUSION ON
)
# Bullet headers are heavy macro/template code; isolate to their own TU.
set_source_files_properties(src/dockyard/physics_world.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        SKIP_UNITY_BUILD_INCLUSION ON
)

# Crash reporter: platform-neutral ring buffer + OS-specific handler in separate TUs.
target_sources(dockyard PRIVATE src/dockyard/crash_reporter_common.cpp)
if (WIN32)
  target_sources(dockyard PRIVATE src/dockyard/crash_reporter_win32.cpp)
  target_link_libraries(dockyard PRIVATE DbgHelp)
else ()
  target_sources(dockyard PRIVATE src/dockyard/crash_reporter_linux.cpp)
endif ()
# Keep all three out of the unity batch and PCH: platform-specific headers are
# incompatible with batch compilation, and relative includes in unity files
# would resolve against the build-dir batch file, not the source directory.
set_source_files_properties(
        src/dockyard/archive.cpp
        src/dockyard/crash_reporter_common.cpp
        src/dockyard/crash_reporter_win32.cpp
        src/dockyard/crash_reporter_linux.cpp
        PROPERTIES
          SKIP_UNITY_BUILD_INCLUSION ON
          SKIP_PRECOMPILE_HEADERS ON
)
if (MSVC)
  set_source_files_properties(src/dockyard/script_engine.cpp PROPERTIES
          COMPILE_OPTIONS "/W3;/bigobj"
  )
else ()
  set_source_files_properties(src/dockyard/script_engine.cpp PROPERTIES
          COMPILE_OPTIONS "${DOCKYARD_SUPPRESSED_WARNINGS}"
  )
endif ()
target_set_warnings(dockyard)
set_target_properties(dockyard PROPERTIES FOLDER dockyard)

# ---- dockforge ---------------------------------------------------------------

add_executable(dockforge
        src/dockforge/main.cpp
        src/dockforge/app/dockforge_init.cpp
        src/dockforge/app/dockforge_ui.cpp
        src/dockforge/app/dockforge_sim.cpp
        src/dockforge/app/dockforge_update.cpp
        src/dockforge/app/dockforge_render.cpp
        src/dockforge/app/dockforge_window.cpp
        src/dockforge/free_camera_controller.cpp
        src/dockforge/path_camera_controller.cpp
        src/dockforge/first_person_controller.cpp
        src/dockforge/editor_camera.cpp
        src/dockforge/editor_utils.cpp
        src/dockforge/matrix_cache.cpp
        src/dockforge/scene_outliner_panel.cpp
        src/dockforge/inspector_panel.cpp
)
target_enable_native_arch(dockforge)
target_precompile_headers(dockforge PRIVATE include/dockforge/PCH.hpp)
target_link_libraries(dockforge PRIVATE dockyard imgui)
target_compile_options(dockforge PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MP>)
target_set_warnings(dockforge)
set_target_properties(dockforge PROPERTIES FOLDER apps)

if(WIN32)
  add_custom_command(TARGET dockforge POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:dockforge>
                $<TARGET_FILE_DIR:dockforge>
                COMMAND_EXPAND_LISTS
        )
endif()

# ---- Tools -------------------------------------------------------------------

if(DOCKYARD_BUILD_TOOLS)
  add_executable(gltf-imagemap tools/gltf-imagemap.cpp)
  target_compile_features(gltf-imagemap PRIVATE cxx_std_23)
  target_include_directories(gltf-imagemap PRIVATE ${cgltf_SOURCE_DIR})
  target_set_warnings(gltf-imagemap)
  set_target_properties(gltf-imagemap PROPERTIES FOLDER tools)
endif()

# ---- Tests -------------------------------------------------------------------

if(DOCKYARD_BUILD_TESTING)
  add_executable(dockyard-testing
                ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_scene_serialisation.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_sim_state.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_state_machine.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_skin_vertex.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_animation.cpp
                ${CMAKE_SOURCE_DIR}/tests/test_physics.cpp
        )
  target_enable_native_arch(dockyard-testing)
  target_precompile_headers(dockyard-testing REUSE_FROM dockyard)
  # The reused dockyard PCH is compiled with /utf-8 (inherited from a PRIVATE
  # dependency); MSVC raises C2855 unless the consuming TUs use it too.
  target_compile_options(dockyard-testing PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_set_warnings(dockyard-testing)
  if(DOCKYARD_BUILD_SMOKE_TESTS)
    target_compile_definitions(dockyard-testing PRIVATE RUN_SERIALISATION_SMOKE_TESTS)
  endif()
  target_link_libraries(dockyard-testing PRIVATE dockyard doctest::doctest)

  if(WIN32)
    add_custom_command(TARGET dockyard-testing POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:dockyard-testing>
                        $<TARGET_FILE_DIR:dockyard-testing>
                        COMMAND_EXPAND_LISTS
                )
  endif()

  include(doctest)
  doctest_discover_tests(dockyard-testing)

  set_solution_folder("tests" dockyard-testing)
endif()

# ---- Developer tooling -------------------------------------------------------

add_custom_target(install-hooks
        COMMAND git config core.hooksPath scripts/hooks
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Installing git hooks"
)
set_solution_folder("tools" install-hooks)
