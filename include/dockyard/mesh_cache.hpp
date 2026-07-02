#pragma once

#include <dockyard/bindless_handle.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/mesh_loader.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace dy::mesh_cache {

// Which GPUMaterial texture-index field a texture patches. Mirrors the
// closures build_patch_list used to construct inline — pulled out into data
// so a patch list can be written to and read back from the binary cache.
enum class TextureRole : u8 {
  albedo,
  normal,
  metallic_roughness,
  occlusion,
  emissive,
  specular_glossiness,
  specular_color,
};

auto apply_texture_role(GPUMaterial &mat, TextureRole role, TextureHandle handle) -> void;

struct MaterialTexturePatch {
  // 0-based within the owning asset's own material list — rebased against
  // the live material_base_slot at load time, same convention as every other
  // offset in BinaryMesh.
  u32 material_local_index;
  TextureRole role;
};
static_assert(std::is_trivially_copyable_v<MaterialTexturePatch>);

struct ImageRecord {
  // Encoded bytes exactly as they would be handed to the KTX2/stb decoder —
  // KTX2 sidecar/embedded bytes, or raw PNG/JPG bytes. Self-contained: no
  // runtime dependency on the source glTF or its sidecar files.
  std::vector<std::byte> encoded_bytes;
  VkFormat format{VK_FORMAT_UNDEFINED};
  bool srgb{false};
  std::string debug_name;
  std::string cache_key;
  std::vector<MaterialTexturePatch> patches;
};

// Serializable mirror of the cacheable subset of MeshAsset. All offsets
// (vertex/index/skin positions inside MeshLodGroup, material indices) are
// asset-relative (0-based) — see rebase() — since a shared GeometryPool's
// base offsets differ between the run that wrote the cache and the run that
// reads it back.
struct BinaryMesh {
  std::vector<Vertex> vertices;
  std::vector<u32> indices;
  std::vector<SkinVertex> skin_vertices;
  std::vector<GPUMaterial> materials; // *_index fields are ignored on load

  std::vector<u32> material_slots;
  std::vector<std::vector<MeshLodGroup>> meshes;
  std::vector<std::vector<AABB>> submesh_aabbs;
  AABB mesh_aabb{AABB::create()};

  std::vector<MeshNodeDescription> nodes;
  std::vector<u32> root_node_indices;

  std::vector<Skeleton> skeletons;
  std::vector<AnimationClip> animations;

  std::vector<PositionOnlyVertex> collision_positions;
  std::vector<u32> collision_indices;

  std::vector<ImageRecord> images;
};

inline constexpr u32 mesh_cache_magic = 0x484D5944u; // "DYMH"
inline constexpr u32 mesh_cache_version = 1u;

struct MeshCacheHeader {
  u32 magic = mesh_cache_magic;
  u32 version = mesh_cache_version;
  u64 source_hash{};
  u64 payload_hash{};
  u64 uncompressed_size{};
  u64 compressed_size{};
};
static_assert(sizeof(MeshCacheHeader) == 40U);
static_assert(std::is_trivially_copyable_v<MeshCacheHeader>);

// zstd level for the one-time compile — decompress speed is level-independent.
// Embedded texture bytes (KTX2/BC7/UASTC) are already compressed and make up
// most of a typical payload, so a high level buys near-zero extra ratio on
// them while costing a lot of CPU; level 3 keeps the write fast without
// giving up much on the geometry/material section that actually compresses.
inline constexpr int mesh_cache_compression_level = 3;

[[nodiscard]] auto serialize(const BinaryMesh &mesh, u64 source_hash)
    -> std::vector<u8>;

[[nodiscard]] auto deserialize(std::span<const u8> file_bytes,
                               u64 expected_source_hash)
    -> std::expected<BinaryMesh, std::string>;

// Shifts every asset-relative offset in mesh by the given deltas, in place.
// Called with negative deltas when snapshotting a freshly-loaded MeshAsset
// down to asset-relative form, and with the live pool's current base offsets
// (positive) when expanding a cached BinaryMesh back into a shared pool.
auto rebase(BinaryMesh &mesh, i64 vertex_delta, i64 index_delta,
           i64 skin_delta, i64 material_delta) -> void;

// FNV-1a fold over the source file's bytes + LoadOptions + mesh_cache_version,
// so a version bump or an option change that affects the output both
// invalidate every existing cache with no migration code.
[[nodiscard]] auto hash_source(const VFSPath &source,
                               const mesh::LoadOptions &opts) -> u64;

[[nodiscard]] auto cache_path_for(const VFSPath &source, u64 source_hash)
    -> VFSPath;

} // namespace dy::mesh_cache
