#include <dockyard/mesh_cache.hpp>

#include <dockyard/animation.hpp>
#include <dockyard/binary_stream.hpp>
#include <dockyard/log.hpp>
#include <dockyard/vfs.hpp>

#include <zstd.h>

#include <cstring>
#include <format>
#include <type_traits>

namespace dy::mesh_cache {

static_assert(std::is_trivially_copyable_v<MeshLodGroup>);
static_assert(std::is_trivially_copyable_v<AnimationChannel>);

auto apply_texture_role(GPUMaterial &mat, TextureRole role, TextureHandle handle)
    -> void {
  switch (role) {
  case TextureRole::albedo:
    mat.albedo_index = handle.index();
    break;
  case TextureRole::normal:
    mat.normal_index = handle.index();
    break;
  case TextureRole::metallic_roughness:
    mat.metallic_roughness_index = handle.index();
    break;
  case TextureRole::occlusion:
    mat.occlusion_index = handle.index();
    break;
  case TextureRole::emissive:
    mat.emissive_index = handle.index();
    break;
  case TextureRole::specular_glossiness:
    mat.specular_glossiness_index = handle.index();
    break;
  case TextureRole::specular_color:
    mat.specular_color_index = handle.index();
    break;
  }
}

namespace {

[[nodiscard]] auto compress_zstd(std::span<const u8> src, int level)
    -> std::vector<u8> {
  const usize bound = ZSTD_compressBound(src.size());
  std::vector<u8> out(bound);
  const usize written =
      ZSTD_compress(out.data(), bound, src.data(), src.size(), level);
  if (ZSTD_isError(written)) {
    // src is a payload we just built ourselves — a compression failure here
    // means a programming error (e.g. corrupt bound calc), not bad input.
    error("mesh_cache: zstd compress error: {}", ZSTD_getErrorName(written));
    std::abort();
  }
  out.resize(written);
  return out;
}

[[nodiscard]] auto decompress_zstd(std::span<const u8> src, usize expected_size)
    -> std::expected<std::vector<u8>, std::string> {
  std::vector<u8> out(expected_size);
  const usize ret =
      ZSTD_decompress(out.data(), expected_size, src.data(), src.size());
  if (ZSTD_isError(ret))
    return std::unexpected(
        std::format("zstd decompress error: {}", ZSTD_getErrorName(ret)));
  if (ret != expected_size)
    return std::unexpected("zstd decompress size mismatch");
  return out;
}

auto write_aabb(BinaryWriter &w, const AABB &box) -> void {
  w.write_t(box.get_min());
  w.write_t(box.get_max());
}

[[nodiscard]] auto read_aabb(BinaryReader &r) -> AABB {
  const auto min = r.read_t<glm::vec3>();
  const auto max = r.read_t<glm::vec3>();
  return AABB::create(min, max);
}

[[nodiscard]] auto write_payload(const BinaryMesh &mesh) -> std::vector<u8> {
  OwningMemoryWriter w;

  w.write_pod_array<Vertex>(mesh.vertices);
  w.write_pod_array<u32>(mesh.indices);
  w.write_pod_array<SkinVertex>(mesh.skin_vertices);
  w.write_pod_array<GPUMaterial>(mesh.materials);
  w.write_pod_array<u32>(mesh.material_slots);

  w.write_t<u64>(mesh.meshes.size());
  for (const auto &groups : mesh.meshes)
    w.write_pod_array<MeshLodGroup>(groups);

  w.write_t<u64>(mesh.submesh_aabbs.size());
  for (const auto &aabbs : mesh.submesh_aabbs) {
    w.write_t<u64>(aabbs.size());
    for (const auto &box : aabbs)
      write_aabb(w, box);
  }
  write_aabb(w, mesh.mesh_aabb);

  w.write_t<u64>(mesh.nodes.size());
  for (const auto &node : mesh.nodes) {
    w.write_string(node.name);
    w.write_t(node.local_transform);
    w.write_t(node.parent_index);
    w.write_t(node.skin_index);
    w.write_t<u64>(node.primitives.size());
    for (const auto &prim : node.primitives) {
      w.write_t(prim.lod_group);
      w.write_t(prim.material_id);
      write_aabb(w, prim.aabb);
      w.write_t(prim.flat_index);
    }
  }
  w.write_pod_array<u32>(mesh.root_node_indices);

  w.write_t<u64>(mesh.skeletons.size());
  for (const auto &skel : mesh.skeletons) {
    w.write_t<u64>(skel.joints.size());
    for (const auto &joint : skel.joints) {
      w.write_t(joint.parent);
      w.write_t(joint.node_index);
      w.write_t(joint.inverse_bind);
      w.write_t(joint.bind_translation);
      w.write_t(joint.bind_rotation);
      w.write_t(joint.bind_scale);
      w.write_string(joint.name);
    }
  }

  w.write_t<u64>(mesh.animations.size());
  for (const auto &clip : mesh.animations) {
    w.write_string(clip.name);
    w.write_t(clip.duration);
    w.write_t<u64>(clip.samplers.size());
    for (const auto &sampler : clip.samplers) {
      w.write_pod_array<f32>(sampler.inputs);
      w.write_pod_array<glm::vec4>(sampler.outputs);
      w.write_t(sampler.interpolation);
    }
    w.write_pod_array<AnimationChannel>(clip.channels);
  }

  w.write_pod_array<PositionOnlyVertex>(mesh.collision_positions);
  w.write_pod_array<u32>(mesh.collision_indices);

  w.write_t<u64>(mesh.images.size());
  for (const auto &img : mesh.images) {
    w.write_pod_array<std::byte>(img.encoded_bytes);
    w.write_t(img.format);
    w.write_t(img.srgb);
    w.write_string(img.debug_name);
    w.write_string(img.cache_key);
    w.write_pod_array<MaterialTexturePatch>(img.patches);
  }

  return w.take();
}

[[nodiscard]] auto read_payload(const std::vector<u8> &bytes) -> BinaryMesh {
  MemoryReader r(bytes);
  BinaryMesh out;

  out.vertices = r.read_pod_vector<Vertex>();
  out.indices = r.read_pod_vector<u32>();
  out.skin_vertices = r.read_pod_vector<SkinVertex>();
  out.materials = r.read_pod_vector<GPUMaterial>();
  out.material_slots = r.read_pod_vector<u32>();

  out.meshes.resize(r.read_t<u64>());
  for (auto &groups : out.meshes)
    groups = r.read_pod_vector<MeshLodGroup>();

  out.submesh_aabbs.resize(r.read_t<u64>());
  for (auto &aabbs : out.submesh_aabbs) {
    aabbs.resize(r.read_t<u64>());
    for (auto &box : aabbs)
      box = read_aabb(r);
  }
  out.mesh_aabb = read_aabb(r);

  out.nodes.resize(r.read_t<u64>());
  for (auto &node : out.nodes) {
    node.name = r.read_string();
    node.local_transform = r.read_t<glm::mat4>();
    node.parent_index = r.read_t<i32>();
    node.skin_index = r.read_t<i32>();
    node.primitives.resize(r.read_t<u64>());
    for (auto &prim : node.primitives) {
      prim.lod_group = r.read_t<MeshLodGroup>();
      prim.material_id = r.read_t<u32>();
      prim.aabb = read_aabb(r);
      prim.flat_index = r.read_t<u32>();
    }
  }
  out.root_node_indices = r.read_pod_vector<u32>();

  out.skeletons.resize(r.read_t<u64>());
  for (auto &skel : out.skeletons) {
    skel.joints.resize(r.read_t<u64>());
    for (auto &joint : skel.joints) {
      joint.parent = r.read_t<i32>();
      joint.node_index = r.read_t<i32>();
      joint.inverse_bind = r.read_t<glm::mat4>();
      joint.bind_translation = r.read_t<glm::vec3>();
      joint.bind_rotation = r.read_t<glm::quat>();
      joint.bind_scale = r.read_t<glm::vec3>();
      joint.name = r.read_string();
    }
  }

  out.animations.resize(r.read_t<u64>());
  for (auto &clip : out.animations) {
    clip.name = r.read_string();
    clip.duration = r.read_t<f32>();
    clip.samplers.resize(r.read_t<u64>());
    for (auto &sampler : clip.samplers) {
      sampler.inputs = r.read_pod_vector<f32>();
      sampler.outputs = r.read_pod_vector<glm::vec4>();
      sampler.interpolation = r.read_t<AnimationInterpolation>();
    }
    clip.channels = r.read_pod_vector<AnimationChannel>();
  }

  out.collision_positions = r.read_pod_vector<PositionOnlyVertex>();
  out.collision_indices = r.read_pod_vector<u32>();

  out.images.resize(r.read_t<u64>());
  for (auto &img : out.images) {
    img.encoded_bytes = r.read_pod_vector<std::byte>();
    img.format = r.read_t<VkFormat>();
    img.srgb = r.read_t<bool>();
    img.debug_name = r.read_string();
    img.cache_key = r.read_string();
    img.patches = r.read_pod_vector<MaterialTexturePatch>();
  }

  return out;
}

} // namespace

auto serialize(const BinaryMesh &mesh, u64 source_hash) -> std::vector<u8> {
  const auto payload = write_payload(mesh);
  const u64 payload_hash = hash_bytes(payload.data(), payload.size());
  const auto compressed = compress_zstd(payload, mesh_cache_compression_level);

  const MeshCacheHeader header{
      .source_hash = source_hash,
      .payload_hash = payload_hash,
      .uncompressed_size = payload.size(),
      .compressed_size = compressed.size(),
  };

  std::vector<u8> out(sizeof(MeshCacheHeader) + compressed.size());
  std::memcpy(out.data(), &header, sizeof(header));
  std::memcpy(out.data() + sizeof(header), compressed.data(), compressed.size());
  return out;
}

auto deserialize(std::span<const u8> file_bytes, u64 expected_source_hash)
    -> std::expected<BinaryMesh, std::string> {
  if (file_bytes.size() < sizeof(MeshCacheHeader))
    return std::unexpected("mesh cache: file too small for header");

  MeshCacheHeader header;
  std::memcpy(&header, file_bytes.data(), sizeof(header));

  if (header.magic != mesh_cache_magic)
    return std::unexpected("mesh cache: bad magic");
  if (header.version != mesh_cache_version)
    return std::unexpected("mesh cache: version mismatch");
  if (header.source_hash != expected_source_hash)
    return std::unexpected("mesh cache: stale (source hash mismatch)");
  if (file_bytes.size() != sizeof(MeshCacheHeader) + header.compressed_size)
    return std::unexpected("mesh cache: truncated file");

  const auto compressed = file_bytes.subspan(sizeof(MeshCacheHeader));
  auto payload = decompress_zstd(compressed, header.uncompressed_size);
  if (!payload)
    return std::unexpected(payload.error());

  if (hash_bytes(payload->data(), payload->size()) != header.payload_hash)
    return std::unexpected("mesh cache: payload hash mismatch (corrupt)");

  return read_payload(*payload);
}

auto rebase(BinaryMesh &mesh, i64 vertex_delta, i64 index_delta, i64 skin_delta,
           i64 material_delta) -> void {
  auto rebase_lod_group = [&](MeshLodGroup &g) {
    g.vertex_offset = static_cast<i32>(g.vertex_offset + vertex_delta);
    if (g.is_skinned())
      g.skin_vertex_offset = static_cast<i32>(g.skin_vertex_offset + skin_delta);
    for (u8 i = 0; i < g.lod_count; ++i)
      g.lods[i].first_index = static_cast<u32>(
          static_cast<i64>(g.lods[i].first_index) + index_delta);
  };

  for (auto &groups : mesh.meshes)
    for (auto &g : groups)
      rebase_lod_group(g);

  for (auto &node : mesh.nodes) {
    for (auto &prim : node.primitives) {
      rebase_lod_group(prim.lod_group);
      prim.material_id =
          static_cast<u32>(static_cast<i64>(prim.material_id) + material_delta);
    }
  }

  for (auto &slot : mesh.material_slots)
    slot = static_cast<u32>(static_cast<i64>(slot) + material_delta);
}

auto hash_source(const VFSPath &source, const mesh::LoadOptions &opts) -> u64 {
  u64 h = 14695981039346656037ULL;

  if (auto bytes = VFS::get().read_bytes(source))
    h ^= hash_bytes(bytes->data(), bytes->size());
  h *= 1099511628211ULL;

  h ^= hash_bytes(&opts.lod_detection, sizeof(opts.lod_detection));
  h *= 1099511628211ULL;
  h ^= hash_bytes(&opts.retain_collision_geometry,
                  sizeof(opts.retain_collision_geometry));
  h *= 1099511628211ULL;
  if (opts.lod_base_name_filter)
    h ^= hash_bytes(opts.lod_base_name_filter->data(),
                    opts.lod_base_name_filter->size());
  h *= 1099511628211ULL;

  h ^= hash_bytes(&mesh_cache_version, sizeof(mesh_cache_version));
  h *= 1099511628211ULL;

  return h;
}

auto cache_path_for(const VFSPath &source, u64 source_hash) -> VFSPath {
  return VFSPath::create("binary://mesh_cache/{}_{:016x}.dymesh", source.stem(),
                         source_hash);
}

} // namespace dy::mesh_cache
