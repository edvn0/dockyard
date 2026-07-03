#pragma once

#include <dockyard/mesh_cache.hpp>
#include <dockyard/upload_pool.hpp>

#include <dockyard/forward.hpp>

#include <expected>
#include <string>

namespace dy::pool {

// Result of the CPU-only parse phase (see dy::mesh::load_from_path_async):
// either a ready-to-commit BinaryMesh or the error that aborted the parse,
// plus the source path the eventual MeshAsset should be tagged with.
struct MeshLoadResult {
  std::expected<mesh_cache::BinaryMesh, std::string> mesh;
  NullableVFSPath source_path;
};

class MeshUploadPool : public UploadPool<MeshLoadResult, MeshAssetHandle> {
public:
  auto poll_n(SceneRenderer &renderer, usize) -> void;
};

} // namespace dy::pool
