#pragma once
#include <dockyard/animation.hpp>
#include <dockyard/bindless_handle.hpp>
#include <dockyard/vfs_path.hpp>
#include <expected>
#include <optional>
#include <string>

namespace dy {

struct IAssetLoader {
  virtual auto load_mesh(const VFSPath &path)
      -> std::expected<MeshAssetHandle, std::string> = 0;
  virtual auto notify_material_overrides_added() -> void = 0;
  virtual auto make_animation_state(MeshAssetHandle handle, u32 skel_idx,
                                    u32 clip_idx)
      -> std::optional<AnimationState> = 0;
  virtual ~IAssetLoader() = default;
};

} // namespace dy
