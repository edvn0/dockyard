# Dockyard — Agent Onboarding

A modern **C++23 Vulkan renderer + editor**. GPU-driven, bindless, clustered
forward+. Two primary build artifacts:

- **dockyard** — static library: the core engine (rendering, ECS, assets, scripting).
- **dockforge** — the editor executable, built on top of dockyard.

> Read this whole file before contributing. The conventions below are not
> optional — match them exactly. When in doubt, read the neighbouring code first.

---

## Coding conventions (mandatory)

These override any habit or default. New code that does not follow them will be
rejected in review.

### Naming
- **`snake_case`** for everything: functions, variables, members, free
  functions, namespaces. No exceptions for "getters".
- **No prefixes or suffixes on members.** No `m_`, no `_` prefix, no trailing
  `_`. A member is just `position`, `frame_index`, `editor_scene`. (Private
  members read identically to locals — that is intended.)
- **No `k_` / `kConstant` prefix on constants.** Constants are `snake_case`
  too: `frames_in_flight`, `cluster_tiles_x`, `resize_debounce_delay`.
- **Types** (`struct` / `class` / `enum class`) are `PascalCase`:
  `VulkanContext`, `SceneRenderer`, `GizmoOp`.
- **ECS components** live in namespace `dy::Components` and are `PascalCase`
  structs: `Components::Transform`, `Components::Mesh`, `Components::PointLight`.
- **`enum class` enumerators**: match the surrounding enum. Newer code uses
  `PascalCase` (`GizmoMode::Local`); some older enums use `lower_case`
  (`FullscreenMode::borderless`). Do not mix styles within one enum.

### Types — always use the `dy` typedefs from `types.hpp`
- Use `u32` / `i32` / `u64` / `i64` / `u8` / `u16` / `usize` / `f32` / `f64`.
- **Never** write `uint32_t`, `std::uint32_t`, `size_t`, `int`, `float`,
  `double` in engine code. `u32` over `uint32_t`, `f32` over `float`, always.
- Float literals use the `F` suffix: `1.0F`, `0.5F`, `16.0F / 9.0F`.

### Style
- **Trailing return types, always**: `auto foo() -> void`,
  `[[nodiscard]] auto bar() const -> glm::mat4`. Even for `void`. (A few legacy
  spots use leading return types — do not copy them; write trailing.)
- `[[nodiscard]]` on any function whose result should not be discarded.
- Self-documenting code. Comment only for non-obvious intent or tricky
  algorithms — never restate what the code does.
- Named constants for all magic values (see `cluster_*` constants in
  `scene_renderer.hpp` for the pattern).

### Error handling
- `std::expected<T, std::string>` for fallible operations.
- Fatal GPU / invariant violations log via `dy::error(...)` then `std::abort()`.
- `vk_check.hpp` wraps `VkResult`; use it for every Vulkan call that returns one.

### Resource lifetime
- Deferred GPU cleanup goes through `DeletionQueue::the().push(...)` (per-frame)
  or `on_destroy(...)` (at app exit). Defined in `app.hpp`.
- VFS paths use the `"prefix://path"` convention via `VFSPath` / `NullableVFSPath`.

---

## Build

CMake **3.25+**, **Ninja** recommended, C++23 (no extensions). Dependencies are
fetched via **CPM** (see `cmake/Dependencies.cmake`). **Slang** must be installed
separately (it is the shader compiler, found via `find_package(slang CONFIG)`).

```sh
cmake -S . -B build/user-ninja-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/user-ninja-debug
```

The editor executable is `build/user-ninja-debug/dockforge`.

CMake options (`CMakeLists.txt`): `DOCKYARD_BUILD_TOOLS` (on),
`DOCKYARD_BUILD_TESTING` (on), `DOCKYARD_BUILD_SMOKE_TESTS` (off).
Tracy profiling is gated on `DOCKYARD_ENABLE_TRACY`.

### CMake targets (`cmake/Targets.cmake`)
| Target | Kind | Notes |
|--------|------|-------|
| `dockyard-allocator` | static lib | Isolated VMA translation unit |
| `dockyard-compiler`  | static lib | Isolated Slang compiler wrapper |
| `dockyard`           | static lib | Core engine. Unity build (batch 8) + PCH (`include/PCH.hpp`) |
| `dockforge`          | executable | Editor. PCH `include/dockforge/PCH.hpp` |
| `dockyard-testing`   | executable | doctest suite (gated on `DOCKYARD_BUILD_TESTING`) |
| `gltf-imagemap`      | executable | Tool (gated on `DOCKYARD_BUILD_TOOLS`) |

Some TUs are deliberately excluded from the unity build / PCH because they
include heavy or platform-specific headers: `script_engine.cpp` (sol2),
`archive.cpp`, and all three `crash_reporter_*` files.

---

## Tests

Framework: **doctest**. Sources in `tests/`. Run via CTest after building, or run
the `dockyard-testing` binary directly.

| File | Covers |
|------|--------|
| `test_main.cpp` | doctest entry point |
| `test_state_machine.cpp` | Compile-time state-machine transition validation |
| `test_sim_state.cpp` | Editor sim-state transitions (Editing/Playing/Paused) |
| `test_animation.cpp` | Joint hierarchy resolution + channel sampling |
| `test_scene_serialisation.cpp` | ECS round-trip serialization (smoke tests behind `RUN_SERIALISATION_SMOKE_TESTS`) |
| `test_skin_vertex.cpp` | Skin-vertex packing |

Test naming: match the existing doctest style in this suite. Use TDD for new
non-prototype code. Never skip/disable a test to make a change pass.

---

## Project layout

```
include/dockyard/     Core engine headers
include/dockforge/    Editor headers
src/dockyard/         Core engine implementations
src/dockforge/        Editor implementation (+ app/ split, main.cpp)
assets/shaders/       Slang shader sources (+ include/ shared, ibl/ precompute)
assets/scripts/       Lua game scripts + type defs (.d.lua, .luarc.json)
assets/meshes/        glTF models
assets/textures/      Textures, env/ HDR + KTX2 environment maps
assets/binary/        Serialized binary assets
assets/editor/        Editor config (Dockforge.ini, fonts)
cmake/                CPM, RenderDoc, warnings, target definitions
3PP/                  Vendored single-header libs (stb, mikktspace, PerlinNoise)
tests/                doctest suite + helpers
tools/                Standalone CLI tools (gltf-imagemap)
```

---

## Logical modules

### Rendering core
- **VulkanContext** (`context.hpp`) — device, instance, surface, VMA allocator,
  queues, swapchain, bindless set, and `Caps` feature flags. The hub everything
  borrows from.
- **SceneRenderer** (`scene_renderer.hpp`) — owns all render passes, GPU resource
  pools, push-constant layouts, thread pool, and the frame graph. The big one.
- **BindlessSet** (`bindless_descriptor.hpp`) — single descriptor set; all
  textures/samplers/storage images indexed dynamically (layout below).
- **Texture** (`texture.hpp`) — `VkImage` + views + VMA; mip generation, KTX2,
  BC6H HDR. **Buffer** (`buffer.hpp`) — VMA-backed GPU buffer with device address.
- **PipelineBuilder** (`pipeline_builder.hpp`) — Vulkan graphics/compute pipeline
  construction (blend modes, shader stages).
- **CanvasRenderer** (`canvas_renderer.hpp`) — immediate-mode 2D/3D debug
  line/shape rendering.
- **ImGuiRenderer** (`imgui_renderer.hpp`) — ImGui Vulkan backend + FreeType fonts.

### Resource handles & pools
- **`Handle<Tag,Impl>`** (`bindless_handle.hpp`) — 20-bit index / 12-bit
  generation handle. **`bindless_pool_entries.hpp`** defines the entry types.
- **FreelistPool** (`freelist_pool.hpp`) — generic slot-reuse allocator.
- **TextureUploadPool** (`texture_upload_pool.hpp`) — async, future-based GPU
  texture uploads with completion callbacks.
- **ThreadSafeMemoryCache** (`thread_safe_memory_cache.hpp`) — mutex-guarded
  get-or-create cache for deduplication.

### Geometry, meshes, animation
- **mesh.hpp / device_geometry.hpp** — geometry storage with LOD groups, AABBs,
  packed vertex formats, and sparse 4-bone skin data; auto-LOD via meshopt.
- **mesh_loader.hpp / .cpp** — fastgltf glTF 2.0 parse, KTX textures, mikktspace
  tangents, meshopt LOD stitching.
- **animation.hpp / .cpp** — skeletal animation: hierarchical joints, channel
  sampling (linear/step/cubic-spline), joint-palette computation for GPU skinning.

### Scene / ECS
- **Scene / Entity** (`scene.hpp`) — thin EnTT wrapper. `Entity::get<T>()` aborts
  on a missing component; use `try_get<T>()` when absence is valid.
- **components.hpp** — `dy::Components`: `Transform` (dirty-tracked
  position/rotation/scale via `mut()`/`get()` accessors), `Camera`, `Mesh`,
  `MaterialOverride`, `PointLight`, `ParentOf`, `Tag`, `LocalToWorld`, debug
  shapes (`DebugLine`/`DebugBox`/`DebugPlane`/`DebugFrustum`), animation state.
- **component_traits.hpp** — compile-time per-component config: serializability
  and UI inspectability, plus fixup dispatch.

### Assets, serialization, persistence
- **asset_loader.hpp** — `IAssetLoader` interface (mesh loading, animation-state
  creation); the renderer implements it.
- **scene_serialiser.hpp** — entt-snapshot ECS serialization with parallel
  component batch writes and post-load GPU-handle fixup.
- **binary_stream.hpp** — binary readers/writers over memory or file, borrowed or
  owning buffers.
- **archive.hpp / .cpp** — ZIP / tar.gz / tar.zst extraction (miniz + zstd), disk
  or in-memory (mmap, ~12 GiB budget).
- **image_decoder.hpp** — stb_image decode via the VFS.

### Scripting (Lua)
- **script_engine.hpp / .cpp** — sol2 Lua bindings with efsw hot-reload. Exposes
  scene/asset/entity/component APIs and lifecycle hooks: `on_scene_load`,
  `begin_play`, `tick`, `end_play`, `on_scene_unload`. Scripts in `assets/scripts/`
  (`sandbox.lua`; `dockyard.d.lua` + `.luarc.json` for editor tooling).

### Shaders
- **compiler.hpp / .cpp** — Slang compilation, caching, error reporting (runtime).
- **shader_watcher.hpp / .cpp** — debounced file watch → hot recompilation.

### Infrastructure
- **VFS** (`vfs.hpp`, `vfs_path.hpp`) — mount-based virtual filesystem; `"prefix://path"`.
- **state_machine.hpp** — compile-time-validated state machine (variant of states
  + constexpr transition rules). **sim_state.hpp** (dockforge) instantiates the
  editor's `Editing ↔ Playing ↔ Paused` machine.
- **crash_reporter.hpp** + `crash_reporter_common.cpp` (lock-free breadcrumb ring
  buffer + frame counter), `crash_reporter_win32.cpp` (SEH + minidump),
  `crash_reporter_linux.cpp` (signals).
- **renderdoc.hpp** — RenderDoc capture integration (real + stub TU).
- **log.hpp** — `dy::info()` / `dy::warn()` / `dy::error()` / `dy::trace()` (spdlog).
- **events.hpp / event_callbacks.hpp** — GLFW event types + wiring.
- **types.hpp** — the `u32`/`f32`/… typedefs, `Badge`, `FrameArray`,
  `MAKE_BITFIELD`, `StringMap`, `PROFILE_SCOPE`, `hash_bytes`.

### Editor (dockforge)
- **Dockforge** (`dockforge.hpp`) — the `App` subclass. Owns editor+runtime
  scenes, camera, renderer, script engine, panels, gizmos, sim state.
  Implementation is split across `src/dockforge/app/dockforge_{init,update,sim,
  render,ui,window}.cpp`.
- **Panels** (`ipanel.hpp` + `inspector_panel.hpp`, `scene_outliner_panel.hpp`,
  `renderer_settings_panel.hpp`) — dockable editor UI.
- **Camera** — `editor_camera.hpp` orchestrates `free_camera_controller.hpp`
  (WASD + mouse-look) and `path_camera_controller.hpp` (keyframe playback).
- **editor_state.hpp / editor_actions.hpp** — selection + entity create/duplicate/
  destroy. **editor_utils.hpp** — screen-to-ray picking. **matrix_cache.hpp** —
  cached transform lookups.

---

## Rendering architecture

Double-buffered (`frames_in_flight = 2`), timeline-semaphore GPU sync.
Everything is **GPU-driven**: indirect dispatch with GPU-generated command
counts. Culling is frustum + occlusion (HiZ). Lighting is **clustered forward+**
(`16 × 9 × 24` cluster grid — keep in sync with `clustering.slang`).

Frame passes, in order:
1. Depth prepass (compute culling → rasterize depth)
2. HiZ pyramid downsample (compute)
3. Forward occlusion culling (compute, refines with HiZ)
4. Light clustering (compute)
5. Shadow cascades (graphics, CSM 2048²)
6. Forward pass (PBR, bindless, IBL, point lights)
7. Bloom (downsample → blur → upsample)
8. Composite (tonemap + bloom → swapchain)
9. Canvas (debug lines/shapes)
10. ImGui

### Bindless descriptor layout (set 0)
| Binding | Contents |
|---------|----------|
| 0 | Sampled images (textures) |
| 1 | Samplers |
| 2 | Comparison samplers (shadow) |
| 3 | Storage images |
| 4 | Cubemaps |
| 5 | Sub-images (mip layers) |

Push constants carry device addresses + frame data (`GpuPushConstants`, 96 bytes —
asserted). Other passes have their own push-constant structs (bloom, composite).

### VulkanContext::Caps
Queried at init. Check before using the feature:
```cpp
ctx.caps.maintenance5 / maintenance6
ctx.caps.smooth_lines / stippled_smooth_lines
ctx.caps.push_descriptor / present_wait
ctx.caps.unified_image_layouts / executable_properties
ctx.caps.transient_attachments   // lazily-allocated memory (mobile/tiled GPUs)
ctx.caps.device_fault            // VK_EXT_device_fault
```

### Texture creation patterns
```cpp
// Render target (non-MSAA)
Texture::create(ctx, "name", w, h, format, usage, aspect);

// MSAA attachment — transient + dedicated memory where supported
Texture::create(ctx, "name", w, h, format, usage, aspect, VK_SAMPLE_COUNT_4_BIT, 1U, true);

// From pixel data
Texture::from_bytes(ctx, "name", {
    .bytes            = span,
    .width            = w,
    .height           = h,
    .format           = VK_FORMAT_R8G8B8A8_SRGB,
    .generate_mips    = true,
    .dedicated_memory = false,   // opt-in for large/important textures
});

// HDR environment map (KTX2 BC6H)
Texture::load_ktx2_hdr_texture(ctx, "env://my_map.ktx2");
```
All textures are cleaned up via `DeletionQueue`; call `texture.destroy(ctx)`.

---

## VFS mounts

Registered in `src/dockyard/vfs.cpp` against the assets root:

| Scheme | Path |
|--------|------|
| `res://` | assets root |
| `shaders://` | assets/shaders |
| `scripts://` | assets/scripts |
| `fonts://` | assets/fonts |
| `editor://` | assets/editor |
| `textures://` | assets/textures |
| `meshes://` | assets/meshes |
| `binary://` | assets/binary |

Runtime mounts (`mount_file`, `mount_file_scoped` + `ScopedMount` RAII) are used
for picked scripts and HDR files. The shader `Compiler` mounts an adapter for
Slang includes.

---

## Third-party dependencies (`cmake/Dependencies.cmake`)
- **volk** — dynamic Vulkan dispatch (`VK_NO_PROTOTYPES`; no link to vulkan-1)
- **vk-bootstrap** — instance/device selection + creation
- **VMA** — all GPU memory (images, buffers)
- **EnTT** — ECS
- **glm** — math; SSE2/AVX2/NEON; `GLM_FORCE_LEFT_HANDED`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`
- **fastgltf** — glTF 2.0 loading
- **meshoptimizer** — LOD generation / mesh optimization
- **KTX-Software** — KTX2 / BC6H textures
- **sol2 + Lua** — scripting
- **efsw** — filesystem watching (shaders, scripts)
- **miniz + zstd** — archive extraction
- **GLFW** — windowing/input · **ImGui** (+ FreeType) — editor UI · **nfd** — native file dialogs
- **Tracy** — GPU/CPU profiling (opt-in) · **spdlog** — logging
- **BS::thread_pool** — task parallelism · **doctest** — tests
- **3PP/**: stb_image, stb_image_resize2, mikktspace, PerlinNoise

---

## Source control
- Never commit directly to `main`; all changes go through feature branches + PRs.
- Never merge locally into `main`. Human review is mandatory before integration.
- Commit messages explain the *why*, not just the *what*.
