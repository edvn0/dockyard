#pragma once

#include <dockyard/app.hpp>
#include <dockyard/bindless_handle.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/vfs_path.hpp>

#include <glm/glm.hpp>

#include <expected>
#include <string>
#include <vector>

namespace dy::mesh {

enum class LodDetection : u8 {
  // Scan all mesh names for _lod{N} suffixes and stitch automatically.
  // This is the default. A GLB whose meshes are named "foo", "bar" (no suffix)
  // is unaffected — it behaves exactly as before.
  automatic,

  // Treat every mesh independently; run meshopt simplification as usual.
  // Use this when _lod{N} in a name is coincidental and not a LOD chain.
  none,
};

struct LoadOptions {
  LodDetection lod_detection = LodDetection::automatic;

  // When set, only meshes whose base name matches this string are treated as
  // an explicit LOD chain. All other meshes load normally.
  //
  // Example: filter = "damaged_helmet"
  //   loads damaged_helmet_lod0 … _lod5 as one chain,
  //   ignores any other meshes in the same file.
  std::optional<std::string> lod_base_name_filter;
};

// Unchanged signature for the no-options case — zero cost, zero noise.
auto load_from_path(const VFSPath &path, SceneRenderer &renderer,
                    const LoadOptions &opts = {})
    -> std::expected<MeshAssetHandle, std::string>;

auto load_from_memory(SceneRenderer &renderer, std::span<const Vertex> vertices,
                      std::span<const u32> indices)
    -> std::expected<MeshAssetHandle, std::string>;

} // namespace dy