# Dockyard — Agent Onboarding

## What this is

A modern C++23 Vulkan renderer / editor. Two binaries:

- **dockyard** — static library (core engine)
- **dockforge** — the editor executable, built on top of dockyard

## Build

CMake 3.25+, Ninja recommended. All third-party dependencies are fetched via CPM.
Slang must be installed separately (shader compiler).

```
cmake -S . -B build/user-ninja-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/user-ninja-debug
```

The executable is `build/user-ninja-debug/dockforge`.

## Project layout

```
include/dockyard/     Core engine headers
include/dockforge/    Editor headers
src/dockyard/         Core engine implementations
src/dockforge/        Editor implementation + main.cpp
assets/shaders/       Slang shader sources
assets/meshes/        glTF models
cmake/                CPM and RenderDoc helpers
3PP/                  Vendored single-header libs (stb, mikktspace, PerlinNoise)
```

## Key types

| Type | Header | Purpose |
|------|--------|---------|
| `VulkanContext` | context.hpp | Device, instance, allocator, queues, capability flags |
| `SceneRenderer` | scene_renderer.hpp | All render passes, resource pools |
| `App` | app.hpp | Abstract base: `init()`, `update()`, `render()` |
| `Texture` | texture.hpp | VkImage + views + VMA allocation |
| `Buffer` | buffer.hpp | VMA-backed GPU buffer |
| `BindlessSet` | bindless_descriptor.hpp | Single descriptor set; all resources indexed dynamically |
| `Pool<Tag,Impl>` | bindless_handle.hpp | Generic freelist with 20-bit index / 12-bit generation handles |
| `Scene` / `Entity` | scene.hpp | EnTT wrapper |
| `VFS` | vfs.hpp | Mount-based virtual filesystem (`shaders://`, `meshes://`, `env://`) |

## VulkanContext::Caps

Queried at init time. Check before using features:

```cpp
ctx.caps.maintenance5
ctx.caps.maintenance6
ctx.caps.smooth_lines
ctx.caps.stippled_smooth_lines
ctx.caps.push_descriptor
ctx.caps.present_wait
ctx.caps.unified_image_layouts
ctx.caps.executable_properties
ctx.caps.transient_attachments   // lazily-allocated memory exists (mobile/tiled GPUs)
```

## Texture creation patterns

```cpp
// Render target (non-MSAA)
Texture::create(ctx, "name", w, h, format, usage, aspect);

// MSAA attachment — transient + dedicated memory where supported
// (Texture::create gates VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT and
//  VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED on ctx.caps.transient_attachments)
Texture::create(ctx, "name", w, h, format, usage, aspect, VK_SAMPLE_COUNT_4_BIT, 1U, true);

// From pixel data
Texture::from_bytes(ctx, "name", {
    .bytes         = span,
    .width         = w,
    .height        = h,
    .format        = VK_FORMAT_R8G8B8A8_SRGB,
    .generate_mips = true,
    .dedicated_memory = false,   // opt-in for large/important textures
});

// HDR environment map (KTX2 BC6H)
Texture::load_ktx2_hdr_texture(ctx, "env://my_map.ktx2");
```

All textures are cleaned up via `DeletionQueue`; call `texture.destroy(ctx)`.

## Rendering architecture

Double-buffered (2 frames in flight), timeline semaphores for GPU sync.

Frame passes in order:

1. Depth prepass (compute culling → rasterize depth)
2. HiZ pyramid downsample (compute)
3. Forward occlusion culling (compute, refines with HiZ)
4. Shadow cascades (6 × graphics, CSM 2048²)
5. Forward pass (PBR, bindless, IBL, point lights)
6. Composite (tonemap → swapchain)
7. Canvas (debug lines/shapes)
8. ImGui

Everything is **GPU-driven**: indirect dispatch with GPU-generated command counts.
Culling is frustum + occlusion (HiZ).

## Bindless descriptor layout (set 0)

| Binding | Contents |
|---------|----------|
| 0 | Sampled images (textures) |
| 1 | Samplers |
| 2 | Comparison samplers (shadow) |
| 3 | Storage images |
| 4 | Cubemaps |
| 5 | Sub-images (mip layers) |

Push constants carry device addresses and frame data (`GpuPushConstants`).

## Shaders

Written in Slang, compiled at runtime via the `Compiler` class. Hot-reload is
supported — `shader_watcher` triggers recompilation on file change.
Shader sources live under `assets/shaders/`. Shared includes are in
`assets/shaders/include/`.

## Third-party highlights

- **volk** — dynamic Vulkan dispatch (no link to vulkan-1)
- **vk-bootstrap** — instance/device selection/creation
- **VMA** — all GPU memory (images, buffers)
- **EnTT** — ECS
- **glm** — math, SSE2/AVX2/NEON enabled, `GLM_FORCE_LEFT_HANDED`
- **fastgltf 0.9** — glTF 2.0 loading
- **KTX-Software v4.4.2** — KTX2 / BC6H textures
- **Tracy** — GPU profiling markers
- **spdlog** — logging via `dy::info()` / `dy::warn()` / `dy::error()`

## Conventions

- `u32`, `f32`, `i32`, etc. are typedefs from `types.hpp` — use them, not `uint32_t`
- Deferred GPU resource cleanup goes through `DeletionQueue::the().push(...)`
- Error handling: `std::expected<T, std::string>` for fallible operations;
  fatal GPU errors call `std::abort()` after logging
- VFS paths use the `"prefix://path"` convention (`VFSPath`)
- Test naming: no established test suite found — follow GWT form for any new tests
