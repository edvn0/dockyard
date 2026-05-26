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

namespace dy {

namespace mesh {

auto load_from_memory(SceneRenderer &, std::span<const Vertex>,
                      std::span<const u32>)
    -> std::expected<MeshAssetHandle, std::string>;
auto load_from_path(const VFSPath &, SceneRenderer &)
    -> std::expected<MeshAssetHandle, std::string>;
} // namespace mesh

} // namespace dy