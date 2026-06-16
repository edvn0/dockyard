#pragma once
#include <dockyard/bindless_handle.hpp>
#include <dockyard/vfs_path.hpp>
#include <expected>
#include <string>

namespace dy {

struct IAssetLoader {
  virtual auto load_mesh(const VFSPath &path)
      -> std::expected<MeshAssetHandle, std::string> = 0;
  virtual ~IAssetLoader() = default;
};

} // namespace dy
