#pragma once
#include <dockyard/animation.hpp>
#include <dockyard/bindless_handle.hpp>
#include <dockyard/vfs_path.hpp>
#include <expected>
#include <optional>
#include <string>

namespace dy {

struct IAssetLoader {
  // retain_collision_geometry requests LOD0 CPU geometry be kept on the
  // returned MeshAsset for building a Bullet mesh collider (see
  // mesh::LoadOptions::retain_collision_geometry). Off by default.
  virtual auto load_mesh(const VFSPath &path,
                        bool retain_collision_geometry = false)
      -> std::expected<MeshAssetHandle, std::string> = 0;
  virtual auto notify_material_overrides_added() -> void = 0;
  virtual auto make_animation_state(MeshAssetHandle handle, u32 skel_idx,
                                    u32 clip_idx)
      -> std::optional<AnimationState> = 0;
  virtual ~IAssetLoader() = default;
};

} // namespace dy
