#include <dockyard/mesh_loader.hpp>

#include <dockyard/archive.hpp>
#include <dockyard/log.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/mesh/animation_load.hpp>
#include <dockyard/mesh/load_types.hpp>
#include <dockyard/mesh/lod.hpp>
#include <dockyard/mesh/material.hpp>
#include <dockyard/mesh/primitive.hpp>
#include <dockyard/mesh/tangents.hpp>
#include <dockyard/mesh/texture_decode.hpp>
#include <dockyard/mesh_cache.hpp>
#include <dockyard/mesh_upload_pool.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <volk.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <glm/packing.hpp>

#include <algorithm>
#include <charconv>
#include <execution>
#include <expected>
#include <format>
#include <fstream>
#include <numeric>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dy {

using mesh::detail::build_gpu_material;
using mesh::detail::build_joint_lookup;
using mesh::detail::build_patch_list;
using mesh::detail::calculate_requirements;
using mesh::detail::classify_images;
using mesh::detail::decode_ktx2_bytes;
using mesh::detail::decode_ktx2_file;
using mesh::detail::DecodedImage;
using mesh::detail::detect_lod_groups;
using mesh::detail::ExplicitLodGroup;
using mesh::detail::extract_primitive;
using mesh::detail::generate_lods;
using mesh::detail::generate_mikktspace_tangents;
using mesh::detail::GeometryRequirements;
using mesh::detail::ImageColorSpace;
using mesh::detail::is_ktx2;
using mesh::detail::JointLookup;
using mesh::detail::KtxDecodeResult;
using mesh::detail::KtxMip;
using mesh::detail::load_animations;
using mesh::detail::load_skeletons;
using mesh::detail::node_local_matrix;
using mesh::detail::parse_lod_suffix;
using mesh::detail::PrimitiveData;
using mesh::detail::PrimitiveResult;
using mesh::detail::RawCollisionGeometry;
using mesh::detail::RawCollisionPrimitive;
using mesh::detail::remap_lod_indices_to_lod0;
using mesh::detail::RemapResult;

namespace {

// Templated so it can populate either a live MeshAsset (used by
// load_from_memory's simple path) or a mesh_cache::BinaryMesh (the CPU-only
// pipeline) — both share identical field types (nodes/meshes/submesh_aabbs/
// root_node_indices/material_slots/collision_positions/collision_indices),
// so the body needs no changes between the two.
template <typename MeshOut>
void flatten_nodes(
    const fastgltf::Asset &asset, std::span<const std::size_t> root_indices,
    MeshOut &out, const std::unordered_set<usize> &suppressed_mesh_indices,
    const std::unordered_map<usize, usize> &lod_to_primary_mesh,
    const RawCollisionGeometry &raw_collision) {
  struct Frame {
    usize node_idx;
    i32 parent_flat_idx;
    bool is_root;
  };

  // Parallel to out.nodes: world_transforms[i] is out.nodes[i]'s accumulated
  // world matrix. desc.parent_index is always < the node's own flat index
  // (this DFS visits a parent before any of its children), so a parent's
  // world transform is always already available when a child needs it.
  std::vector<glm::mat4> world_transforms;
  usize collision_prims_baked = 0;
  usize collision_prims_seen = 0;

  std::vector<Frame> dfs;
  dfs.reserve(root_indices.size());
  for (const auto idx : root_indices)
    dfs.push_back({.node_idx = idx, .parent_flat_idx = -1, .is_root = true});

  while (!dfs.empty()) {
    auto [node_idx, parent_flat, is_root] = dfs.back();
    dfs.pop_back();

    const auto &node = asset.nodes[node_idx];

    // Skip nodes that reference a LOD mesh (lod > 0); their geometry has
    // already been stitched into the LOD0 MeshLodGroup.
    if (node.meshIndex.has_value() &&
        suppressed_mesh_indices.contains(*node.meshIndex)) {
      // Still push children so the rest of the scene graph is intact —
      // some exporters parent accessories under a LOD1+ node.
      for (usize i = node.children.size(); i-- > 0;)
        dfs.push_back({
            .node_idx = node.children[i],
            .parent_flat_idx = parent_flat,
            .is_root = false,
        });
      continue;
    }

    const i32 flat_idx = static_cast<i32>(out.nodes.size());
    if (is_root)
      out.root_node_indices.push_back(static_cast<u32>(flat_idx));

    MeshNodeDescription desc;
    desc.name = node.name.empty() ? std::format("gltf_node_{}", node_idx)
                                  : std::string(node.name);
    desc.local_transform = node_local_matrix(node);
    desc.parent_index = parent_flat;
    if (node.skinIndex.has_value())
      desc.skin_index = static_cast<i32>(*node.skinIndex);

    const glm::mat4 world_transform =
        (parent_flat < 0)
            ? desc.local_transform
            : world_transforms[static_cast<usize>(parent_flat)] *
                  desc.local_transform;

    if (node.meshIndex.has_value()) {
      // Remap to primary mesh if this node holds a LOD0 that was merged.
      const usize mi = lod_to_primary_mesh.contains(*node.meshIndex)
                           ? lod_to_primary_mesh.at(*node.meshIndex)
                           : *node.meshIndex;
      const auto &lod_groups = out.meshes[mi];
      const auto &gltf_mesh = asset.meshes[mi];
      if (lod_groups.empty())
        warn("flatten_nodes: node '{}' references mesh {} but it has no "
             "uploaded lod_groups — all primitives failed extraction",
             desc.name, mi);
      desc.primitives.reserve(lod_groups.size());

      for (usize pi = 0; pi < lod_groups.size(); ++pi) {
        const u32 mat_id =
            gltf_mesh.primitives[pi].materialIndex.has_value()
                ? out.material_slots[*gltf_mesh.primitives[pi].materialIndex]
                : 0u;
        desc.primitives.push_back({
            .lod_group = lod_groups[pi],
            .material_id = mat_id,
            .aabb = out.submesh_aabbs[mi][pi],
        });

        // Bake this node's world transform into its share of the retained
        // collision geometry (see RawCollisionPrimitive) and flatten it into
        // the asset-wide collision buffers. mi < raw_collision.size() always
        // holds since raw_collision is sized like out.meshes; pi may exceed
        // raw_collision[mi].size() if this primitive failed extraction
        // (upload_geometry skips pushing a raw entry for it too).
        ++collision_prims_seen;
        if (mi < raw_collision.size() && pi < raw_collision[mi].size()) {
          ++collision_prims_baked;
          const auto &raw_prim = raw_collision[mi][pi];
          const usize vertex_base = out.collision_positions.size();

          out.collision_positions.reserve(out.collision_positions.size() +
                                          raw_prim.positions.size());
          for (const auto &vtx : raw_prim.positions) {
            const glm::vec3 local{vtx.position[0], vtx.position[1],
                                  vtx.position[2]};
            const glm::vec3 world{world_transform * glm::vec4(local, 1.0F)};
            out.collision_positions.push_back(
                {.position = {world.x, world.y, world.z}});
          }

          out.collision_indices.reserve(out.collision_indices.size() +
                                        raw_prim.indices.size());
          for (const u32 idx : raw_prim.indices)
            out.collision_indices.push_back(static_cast<u32>(vertex_base) +
                                            idx);
        }
      }
    }

    out.nodes.push_back(std::move(desc));
    world_transforms.push_back(world_transform);

    for (usize i = node.children.size(); i-- > 0;)
      dfs.push_back({
          .node_idx = node.children[i],
          .parent_flat_idx = flat_idx,
          .is_root = false,
      });
  }

  if (!raw_collision.empty())
    info("flatten_nodes: baked collision geometry for {}/{} primitive "
         "references ({} nodes total)",
         collision_prims_baked, collision_prims_seen, out.nodes.size());
}

} // namespace

namespace mesh {

using MaterialTexturePatch = mesh_cache::MaterialTexturePatch;

struct ImageSource {
  std::variant<std::filesystem::path, std::vector<std::byte>> data;
  std::optional<std::filesystem::path> ktx_sidecar_path;
  std::string debug_name;
  std::string cache_key;
  VkFormat format;
  bool srgb{false};
  usize image_idx;
};

struct PendingUpload {
  std::future<pool::CpuTextureData> fut;
  usize image_idx;
  std::stop_source stop_src;
  std::vector<MaterialTexturePatch> patches;
};

struct PrimWork {
  usize mesh_idx;
  usize prim_idx;
  const fastgltf::Primitive *ptr;
  // If this primitive is part of an explicit LOD chain:
  //   lod_slot == 0  → primary (LOD0); owns the MeshLodGroup
  //   lod_slot >= 1  → secondary; indices are remapped into LOD0's vertices
  //   primary_mesh_idx → which mesh_idx holds the MeshLodGroup for
  //   lod_slot>0
  u32 lod_slot{0};
  usize primary_mesh_idx{0}; // only meaningful when lod_slot > 0
  bool is_explicit_lod{false};
};

struct PrimLods {
  // meshopt-generated extra LODs (only populated when
  // is_explicit_lod==false)
  std::vector<std::vector<u32>> extra;
};

// A fresh Parser per call (rather than a shared instance) because
// fastgltf::Parser is not safe to call concurrently from multiple threads —
// the async mesh-load path (parse_to_binary_mesh) may run several parses in
// parallel on different worker threads.
[[nodiscard]] static auto make_gltf_parser() -> fastgltf::Parser {
  return fastgltf::Parser{
      fastgltf::Extensions::KHR_lights_punctual |
          fastgltf::Extensions::KHR_materials_specular |
          fastgltf::Extensions::KHR_materials_pbrSpecularGlossiness,
  };
}

static auto parse_gltf_file(const std::filesystem::path &fs_path,
                            const std::filesystem::path &gltf_dir)
    -> std::expected<fastgltf::Asset, std::string> {
  auto data = fastgltf::GltfDataBuffer::FromPath(fs_path);
  if (!data)
    return std::unexpected(std::format("GltfDataBuffer from path failed: {}",
                                       fastgltf::getErrorMessage(data.error())));
  auto parser = make_gltf_parser();
  auto result = parser.loadGltf(data.get(), gltf_dir,
                                fastgltf::Options::GenerateMeshIndices);
  if (!result)
    return std::unexpected(std::format("glTF parse error: {}",
                                       fastgltf::getErrorMessage(result.error())));
  return std::move(result.get());
}

static auto parse_gltf_memory(std::span<const std::byte> bytes)
    -> std::expected<fastgltf::Asset, std::string> {
  auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
  if (!data)
    return std::unexpected(
        std::format("GltfDataBuffer from memory failed: {}",
                    fastgltf::getErrorMessage(data.error())));
  auto parser = make_gltf_parser();
  auto result =
      parser.loadGltf(data.get(), {}, fastgltf::Options::GenerateMeshIndices);
  if (!result)
    return std::unexpected(std::format(
        "glTF parse error: {}", fastgltf::getErrorMessage(result.error())));
  return std::move(result.get());
}

[[nodiscard]] static auto
sidecar_path_for(const std::filesystem::path &gltf_dir,
                 const std::string &image_name, usize image_idx)
    -> std::optional<std::filesystem::path> {
  if (!image_name.empty()) {
    auto candidate = gltf_dir / "ktx2" / (image_name + ".ktx2");
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  auto indexed = gltf_dir / "ktx2" / std::format("img{}.ktx2", image_idx);
  if (std::filesystem::exists(indexed)) {
    return indexed;
  }

  return std::nullopt;
}

static auto
collect_image_sources(const fastgltf::Asset &asset,
                      const std::filesystem::path &gltf_dir,
                      const std::filesystem::path &fs_path,
                      const std::vector<ImageColorSpace> &color_spaces)
    -> std::vector<ImageSource> {

  std::vector<ImageSource> sources;
  sources.reserve(asset.images.size());

  for (usize i = 0; i < asset.images.size(); ++i) {
    const bool srgb = (color_spaces[i] == ImageColorSpace::srgb);
    const VkFormat fmt =
        srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const std::string fmt_suffix = srgb ? ":srgb" : ":linear";
    const std::string image_name = std::string(asset.images[i].name);
    const std::string debug_name =
        image_name.empty()
            ? std::format("{}#img{}", fs_path.filename().string(), i)
            : image_name;

    auto sidecar = sidecar_path_for(gltf_dir, image_name, i);

    std::visit(
        fastgltf::visitor{
            [&](const fastgltf::sources::URI &uri) {
              const auto full = gltf_dir / uri.uri.fspath();
              const std::string cache_key =
                  sidecar ? (sidecar->string() + fmt_suffix)
                          : (full.string() + fmt_suffix);
              sources.push_back({
                  .data = full,
                  .ktx_sidecar_path = sidecar,
                  .debug_name = debug_name,
                  .cache_key = cache_key,
                  .format = fmt,
                  .srgb = srgb,
                  .image_idx = i,
              });
            },
            [&](const fastgltf::sources::Array &arr) {
              std::vector<std::byte> buf(arr.bytes.size());
              std::memcpy(buf.data(), arr.bytes.data(), arr.bytes.size());
              sources.push_back({
                  .data = std::move(buf),
                  .ktx_sidecar_path = sidecar,
                  .debug_name = debug_name,
                  .cache_key = {},
                  .format = fmt,
                  .srgb = srgb,
                  .image_idx = i,
              });
            },
            [&](const fastgltf::sources::BufferView &bv) {
              const auto &view = asset.bufferViews[bv.bufferViewIndex];
              const auto &buf = asset.buffers[view.bufferIndex];
              std::visit(fastgltf::visitor{
                             [&](const fastgltf::sources::Array &arr) {
                               std::vector<std::byte> copy(view.byteLength);
                               std::memcpy(copy.data(),
                                           arr.bytes.data() + view.byteOffset,
                                           view.byteLength);
                               sources.push_back({
                                   .data = std::move(copy),
                                   .ktx_sidecar_path = sidecar,
                                   .debug_name = debug_name,
                                   .cache_key = {},
                                   .format = fmt,
                                   .srgb = srgb,
                                   .image_idx = i,
                               });
                             },
                             [&](const auto &) {
                               warn("Unsupported buffer backing for image '{}'",
                                    debug_name);
                             },
                         },
                         buf.data);
            },
            [&](const auto &) {
              warn("Unsupported image source for '{}'", debug_name);
            },
        },
        asset.images[i].data);
  }

  return sources;
}

static auto launch_texture_futures(std::vector<ImageSource> &sources,
                                   SceneRenderer &renderer, MeshAsset &result)
    -> std::vector<PendingUpload> {
  PROFILE_SCOPE("Launch deferred texture futures");

  std::vector<PendingUpload> pending;
  pending.reserve(sources.size());

  for (auto &src : sources) {
    if (auto cached = renderer.texture_cache.get(src.cache_key)) {
      result.texture_handles[src.image_idx] = *cached;
      continue;
    }

    auto stop_src = std::stop_source{};
    auto token = stop_src.get_token();

    auto fut = renderer.thread_pool.submit_task(
        [src = std::move(src), token]() mutable -> pool::CpuTextureData {
          if (token.stop_requested())
            return {};

          if (src.ktx_sidecar_path) {
            auto ktx_result =
                decode_ktx2_file(*src.ktx_sidecar_path, src.format, src.srgb);
            if (ktx_result) {
              auto &kr = *ktx_result;
              std::vector<pool::MipData> mips;
              mips.reserve(kr.mips.size());
              for (auto &m : kr.mips)
                mips.push_back({
                    .pixels = std::move(m.data),
                    .width = m.width,
                    .height = m.height,
                });
              return pool::CpuTextureData{
                  .mips = std::move(mips),
                  .name = std::move(src.debug_name),
                  .cache_key = src.cache_key,
                  .width = kr.base_width,
                  .height = kr.base_height,
                  .format = kr.vk_format,
                  .generate_mips = false, // already have all mips
              };
            }
            // Sidecar failed — fall through to embedded data
            warn("KTX2 sidecar decode failed for '{}', falling back to "
                 "embedded data",
                 src.debug_name);
          }

          if (auto *buf = std::get_if<std::vector<std::byte>>(&src.data)) {
            if (is_ktx2(*buf)) {
              auto ktx_result = decode_ktx2_bytes(*buf, src.format, src.srgb);
              if (ktx_result) {
                auto &kr = *ktx_result;
                std::vector<pool::MipData> mips;
                mips.reserve(kr.mips.size());
                for (auto &m : kr.mips)
                  mips.push_back({
                      .pixels = std::move(m.data),
                      .width = m.width,
                      .height = m.height,
                  });
                return pool::CpuTextureData{
                    .mips = std::move(mips),
                    .name = std::move(src.debug_name),
                    .cache_key = src.cache_key,
                    .width = kr.base_width,
                    .height = kr.base_height,
                    .format = kr.vk_format,
                    .generate_mips = false,
                };
              }
            }
          }

          int w{};
          int h{};
          int ch{};
          stbi_uc *raw = nullptr;

          if (auto *file_path = std::get_if<std::filesystem::path>(&src.data)) {
            raw = stbi_load(file_path->string().c_str(), &w, &h, &ch,
                            STBI_rgb_alpha);
          } else {
            auto &raw_buf = std::get<std::vector<std::byte>>(src.data);
            raw = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc *>(raw_buf.data()),
                static_cast<int>(raw_buf.size()), &w, &h, &ch, STBI_rgb_alpha);
          }

          if (!raw) {
            warn("stbi failed for '{}': {}", src.debug_name,
                 stbi_failure_reason());
            return {};
          }

          if (token.stop_requested()) {
            stbi_image_free(raw);
            return {};
          }

          std::vector<std::byte> pixels(static_cast<usize>(w * h * 4));
          std::memcpy(pixels.data(), raw, pixels.size());
          stbi_image_free(raw);

          const std::string cache_key =
              src.cache_key.empty()
                  ? std::format("hash:{:016x}{}",
                                hash_bytes(pixels.data(), pixels.size()),
                                src.format == VK_FORMAT_R8G8B8A8_SRGB
                                    ? ":srgb"
                                    : ":linear")
                  : src.cache_key;

          // Generate all mips on CPU with alpha-weighted (premultiplied) filtering
          // to prevent transparent-texel colors from bleeding into leaf/foliage edges.
          const u32 level_count = static_cast<u32>(
              std::bit_width(static_cast<u32>(std::max(w, h))));
          const bool is_srgb = (src.format == VK_FORMAT_R8G8B8A8_SRGB);

          std::vector<pool::MipData> mips;
          mips.reserve(level_count);
          mips.push_back({
              .pixels = std::move(pixels),
              .width = static_cast<u32>(w),
              .height = static_cast<u32>(h),
          });

          for (u32 level = 1; level < level_count; ++level) {
            const auto &prev = mips.back();
            const u32 nw = std::max(1u, prev.width / 2);
            const u32 nh = std::max(1u, prev.height / 2);
            std::vector<std::byte> dst(nw * nh * 4);

            if (is_srgb) {
              stbir_resize_uint8_srgb(
                  reinterpret_cast<const unsigned char *>(prev.pixels.data()),
                  static_cast<int>(prev.width), static_cast<int>(prev.height),
                  0,
                  reinterpret_cast<unsigned char *>(dst.data()),
                  static_cast<int>(nw), static_cast<int>(nh), 0, STBIR_RGBA);
            } else {
              stbir_resize_uint8_linear(
                  reinterpret_cast<const unsigned char *>(prev.pixels.data()),
                  static_cast<int>(prev.width), static_cast<int>(prev.height),
                  0,
                  reinterpret_cast<unsigned char *>(dst.data()),
                  static_cast<int>(nw), static_cast<int>(nh), 0, STBIR_RGBA);
            }

            mips.push_back({.pixels = std::move(dst), .width = nw, .height = nh});
          }

          return pool::CpuTextureData{
              .mips = std::move(mips),
              .name = std::move(src.debug_name),
              .cache_key = std::move(cache_key),
              .width = static_cast<u32>(w),
              .height = static_cast<u32>(h),
              .format = src.format,
              .generate_mips = false,
          };
        });

    pending.push_back(PendingUpload{
        .fut = std::move(fut),
        .image_idx = src.image_idx,
        .stop_src = std::move(stop_src),
        .patches = {},
    });
  }

  return pending;
}

static auto
extract_primitives_parallel(const fastgltf::Asset &asset,
                            const std::vector<PrimWork> &prim_work_list,
                            BS::priority_thread_pool &thread_pool)
    -> std::vector<std::expected<PrimitiveResult, std::string>> {
  PROFILE_SCOPE("Extract primitives");

  const auto n = prim_work_list.size();
  std::vector<std::expected<PrimitiveResult, std::string>> results(
      n, std::unexpected<std::string>("Could not extract"));

  thread_pool
      .submit_blocks(usize{0}, n,
                     [&](usize begin, usize end) {
                       for (usize i = begin; i < end; ++i)
                         results[i] =
                             extract_primitive(asset, *prim_work_list[i].ptr);
                     })
      .wait();

  return results;
}

// This runs after extract_primitives_parallel so we have both LOD0 and LODn
// PrimitiveData available.
static void remap_explicit_lod_indices(
    const std::vector<PrimWork> &prim_work_list,
    std::vector<std::expected<PrimitiveResult, std::string>> &extracted_prims,
    BS::priority_thread_pool &thread_pool) {
  PROFILE_SCOPE("Remap explicit LOD indices");

  // Build a lookup: (primary_mesh_idx, prim_idx) → index into
  // extracted_prims for that LOD0 entry.
  std::unordered_map<usize, usize> lod0_prim_map; // mesh_idx → work list index
  for (usize i = 0; i < prim_work_list.size(); ++i) {
    const auto &w = prim_work_list[i];
    if (w.is_explicit_lod && w.lod_slot == 0)
      lod0_prim_map[w.mesh_idx] = i;
    else if (!w.is_explicit_lod)
      lod0_prim_map[w.mesh_idx] = i;
  }

  const auto n = prim_work_list.size();
  thread_pool
      .submit_blocks(
          usize{0}, n,
          [&](usize begin, usize end) {
            for (usize i = begin; i < end; ++i) {
              const auto &w = prim_work_list[i];
              if (!w.is_explicit_lod || w.lod_slot == 0)
                continue;
              if (!extracted_prims[i])
                continue;

              // Find the corresponding LOD0 entry.
              const auto it = lod0_prim_map.find(w.primary_mesh_idx);
              if (it == lod0_prim_map.end() || !extracted_prims[it->second])
                continue;

              const auto &lod0_data = extracted_prims[it->second]->data;
              auto &lod_n_data = extracted_prims[i]->data;

              auto remap = remap_lod_indices_to_lod0(lod0_data, lod_n_data);

              if (!remap.exact) {
                warn("LOD{} for mesh_idx {} required nearest-"
                     "neighbour vertex remap (positions differ "
                     "from LOD0)",
                     w.lod_slot, w.mesh_idx);
              }

              // Replace the indices; vertices are no longer needed
              // (they won't be uploaded).
              lod_n_data.indices = std::move(remap.indices);
            }
          })
      .wait();
}

static auto generate_lods_parallel(
    const std::vector<PrimWork> &prim_work_list,
    const std::vector<std::expected<PrimitiveResult, std::string>>
        &extracted_prims,
    BS::priority_thread_pool &thread_pool)
    -> std::pair<std::vector<PrimLods>, usize> {
  PROFILE_SCOPE("Generate LODs (Parallel)");

  const auto n = prim_work_list.size();
  std::vector<PrimLods> prim_lods(n);
  std::vector<usize> indices_per_prim(n, 0);

  thread_pool
      .submit_blocks(usize{0}, n,
                     [&](usize begin, usize end) {
                       for (usize i = begin; i < end; ++i) {
                         // Never generate meshopt LODs for explicit LOD
                         // meshes — either they're the primary (lod0) with
                         // pre-authored siblings, or they're a secondary
                         // that we don't even upload vertices for.
                         if (prim_work_list[i].is_explicit_lod)
                           continue;
                         if (!extracted_prims[i])
                           continue;
                         const auto &[pdata, aabb] = *extracted_prims[i];
                         if (!should_generate_lods(pdata))
                           continue;
                         prim_lods[i].extra = generate_lods(pdata);
                         for (const auto &lod_indices : prim_lods[i].extra)
                           indices_per_prim[i] += lod_indices.size();
                       }
                     })
      .wait();

  usize total_lod_indices = 0;
  for (usize c : indices_per_prim)
    total_lod_indices += c;

  return {std::move(prim_lods), total_lod_indices};
}

// CPU-only twin of the old pool-writing allocate_materials: builds the
// GPUMaterial list and asset-relative (0-based) material_slots into a
// BinaryMesh, using a placeholder texture handle for every slot (textures
// haven't been decoded/uploaded yet at this point). GeometryPool::allocate_
// materials + mesh_cache::rebase apply the live pool's base slot at commit
// time — see build_from_cache.
static void assemble_materials_cpu(const fastgltf::Asset &asset,
                                   TextureHandle dummy_texture_handle,
                                   mesh_cache::BinaryMesh &out) {
  if (asset.materials.empty())
    return;
  PROFILE_SCOPE("Assemble materials (CPU)");

  const std::vector<TextureHandle> placeholder_handles(asset.images.size(),
                                                        dummy_texture_handle);

  out.materials.reserve(asset.materials.size());
  for (const auto &mat : asset.materials)
    out.materials.push_back(build_gpu_material(mat, asset, placeholder_handles));

  out.material_slots.resize(asset.materials.size());
  for (usize i = 0; i < asset.materials.size(); ++i)
    out.material_slots[i] = static_cast<u32>(i);
}

// CPU-only twin of the old pool-writing upload_geometry: assembles
// vertices/indices/skin_vertices and MeshLodGroup records into a BinaryMesh
// at asset-relative (0-based) offsets — no GeometryPool/renderer access, so
// this is safe to run on a worker thread. Position-only shadow vertices are
// deliberately not produced here: GeometryPool::allocate derives them from
// vertex positions at commit time, so BinaryMesh never needs to carry them.
static void
assemble_geometry_cpu(const std::vector<PrimWork> &prim_work_list,
                      const std::vector<std::expected<PrimitiveResult, std::string>>
                          &extracted_prims,
                      const std::vector<PrimLods> &prim_lods,
                      usize total_lod_indices, mesh_cache::BinaryMesh &out,
                      bool retain_collision_geometry,
                      RawCollisionGeometry &raw_collision_out) {
  PROFILE_SCOPE("Assemble geometry (CPU)");

  // Only count vertices for LOD0 (or non-LOD) entries. Higher LOD meshes
  // share LOD0's vertex range — their vertices are never duplicated.
  usize total_v = 0;
  usize total_i = 0;
  usize total_skin = 0;
  for (usize i = 0; i < prim_work_list.size(); ++i) {
    if (!extracted_prims[i])
      continue;
    const auto &w = prim_work_list[i];
    const bool uploads_vertices = !w.is_explicit_lod || w.lod_slot == 0;
    if (uploads_vertices) {
      total_v += extracted_prims[i]->data.vertices.size();
      total_skin += extracted_prims[i]->data.skin.size();
    }
    total_i += extracted_prims[i]->data.indices.size();
  }
  out.vertices.reserve(total_v);
  out.indices.reserve(total_i + total_lod_indices);
  if (total_skin > 0)
    out.skin_vertices.reserve(total_skin);

  if (retain_collision_geometry)
    raw_collision_out.resize(out.meshes.size());

  for (usize i = 0; i < prim_work_list.size(); ++i) {
    const auto &w = prim_work_list[i];
    const auto &res = extracted_prims[i];

    // For LOD meshes with slot > 0 we only need to write their (remapped)
    // index data — their vertices live in LOD0's range.
    const bool uploads_vertices = !w.is_explicit_lod || w.lod_slot == 0;

    // Determine which mesh owns the MeshLodGroup for this primitive.
    // For LOD0 / non-LOD prims it's w.mesh_idx itself.
    // For LODn (n>0) it's w.primary_mesh_idx.
    const usize owning_mesh_idx =
        (w.is_explicit_lod && w.lod_slot > 0) ? w.primary_mesh_idx : w.mesh_idx;

    if (!res) {
      warn("assemble_geometry_cpu: skipping primitive (mesh={}, prim={}): {}",
           w.mesh_idx, w.prim_idx, res.error());
      if (uploads_vertices)
        out.submesh_aabbs[owning_mesh_idx].push_back(AABB::create());
      continue;
    }

    const auto &[pdata, aabb] = *res;

    // Index upload always happens; vertex upload is skipped for LODn > 0
    // (patched into the LOD0 MeshLodGroup below instead).
    const usize index_offset = out.indices.size();
    usize vertex_offset = 0;
    if (uploads_vertices) {
      vertex_offset = out.vertices.size();
      out.vertices.insert(out.vertices.end(), pdata.vertices.begin(),
                          pdata.vertices.end());
    }
    out.indices.insert(out.indices.end(), pdata.indices.begin(),
                       pdata.indices.end());

    if (uploads_vertices) {
      // LOD0 / standalone: create the group entry.
      MeshLodGroup lod_group;
      lod_group.vertex_offset = static_cast<i32>(vertex_offset);
      lod_group.lods[0].first_index = static_cast<u32>(index_offset);
      lod_group.lods[0].index_count = static_cast<u32>(pdata.indices.size());
      lod_group.vertex_count = static_cast<u32>(pdata.vertices.size());
      lod_group.lod_count = 1;

      // Skin vertices (when present) are written densely, parallel to this
      // LOD0's vertices; higher LODs reuse this offset since they share
      // LOD0's vertex range.
      if (!pdata.skin.empty()) {
        const usize skin_idx = out.skin_vertices.size();
        out.skin_vertices.insert(out.skin_vertices.end(), pdata.skin.begin(),
                                 pdata.skin.end());
        lod_group.skin_vertex_offset = static_cast<i32>(skin_idx);
      }

      // Append meshopt-generated LODs (only for non-explicit-lod prims).
      if (!w.is_explicit_lod) {
        for (const auto &lod_indices : prim_lods[i].extra) {
          auto &lod = lod_group.lods[lod_group.lod_count++];
          lod.first_index = static_cast<u32>(out.indices.size());
          lod.index_count = static_cast<u32>(lod_indices.size());
          out.indices.insert(out.indices.end(), lod_indices.begin(),
                             lod_indices.end());
        }
      }

      out.meshes[owning_mesh_idx].push_back(lod_group);
      out.submesh_aabbs[owning_mesh_idx].push_back(aabb);
      out.mesh_aabb.merge(aabb);

      if (retain_collision_geometry) {
        // Use the coarsest generated LOD's indices, not LOD0's — collision
        // doesn't need render-quality detail, and a large environment mesh
        // at full LOD0 density (millions of triangles) is both far too
        // slow for real-time collision and large enough to blow past
        // Bullet's BVH-build limits. LODn indices reference the same LOD0
        // vertex range (see generate_lods), so vertices are unaffected.
        const auto &collision_source_indices = prim_lods[i].extra.empty()
                                                   ? pdata.indices
                                                   : prim_lods[i].extra.back();

        // Stored in this primitive's own local space, indices untouched —
        // flatten_nodes applies each owning node's world transform and
        // flattens these into collision_positions/collision_indices once
        // the node hierarchy (and thus per-node transforms) is known.
        RawCollisionPrimitive raw_prim;
        raw_prim.positions.reserve(pdata.vertices.size());
        for (const auto &vtx : pdata.vertices)
          raw_prim.positions.push_back(
              {.position = {vtx.position[0], vtx.position[1], vtx.position[2]}});
        raw_prim.indices.assign(collision_source_indices.begin(),
                                collision_source_indices.end());
        raw_collision_out[owning_mesh_idx].push_back(std::move(raw_prim));
      }
    } else {
      // LODn > 0: patch the lod_slot into the existing MeshLodGroup.
      auto &lod_group = out.meshes[owning_mesh_idx][w.prim_idx];
      if (w.lod_slot < static_cast<u32>(max_lods)) {
        auto &lod_entry = lod_group.lods[w.lod_slot];
        lod_entry.first_index = static_cast<u32>(index_offset);
        lod_entry.index_count = static_cast<u32>(pdata.indices.size());
        lod_group.lod_count =
            static_cast<u8>(glm::max(static_cast<u32>(lod_group.lod_count),
                                     static_cast<u32>(w.lod_slot + 1)));
      }
    }
  }
}

static void submit_texture_uploads(std::vector<PendingUpload> &pending,
                                   SceneRenderer &renderer,
                                   MeshAssetHandle handle,
                                   u32 material_base_slot) {
  for (auto &pu : pending) {
    renderer.texture_upload_pool->submit(
        std::move(pu.fut), std::move(pu.stop_src),
        [&renderer, mesh_handle = handle, image_idx = pu.image_idx,
         patches = std::move(pu.patches), material_base_slot](
            TextureHandle h) mutable {
          auto *mesh_asset = renderer.resolve_mut(mesh_handle);
          mesh_asset->texture_handles[image_idx] = h;
          for (const auto &[local_index, role] : patches) {
            const u32 slot = material_base_slot + local_index;
            auto &mat = renderer.geometry_pool->get_material(slot);
            mesh_cache::apply_texture_role(mat, role, h);
            renderer.geometry_pool->update_material(slot, mat);
          }
        });
  }
}

auto load_from_memory(SceneRenderer &renderer, std::span<const Vertex> vertices,
                      std::span<const u32> indices, NullableVFSPath source_path)
    -> std::expected<MeshAssetHandle, std::string> {
  auto &pool = *renderer.geometry_pool;

  MeshAsset result{.mesh_aabb = AABB::create()};
  result.source_path.path = source_path.path;
  result.material_base_slot = 0u;
  result.material_count = 1u;
  result.material_slots = {0u};
  result.texture_handles = {};
  result.vertex_base_offset = pool.vertex_offset;
  result.shadow_vertex_base_offset = pool.shadow_vertex_offset;
  result.index_base_offset = pool.index_offset;

  auto &&[v_off, sv_off, i_off] = pool.allocate(vertices, indices);

  MeshLodGroup lod_group;
  lod_group.vertex_offset = static_cast<i32>(v_off / sizeof(Vertex));
  lod_group.lods[0].index_count = static_cast<u32>(indices.size());
  lod_group.lods[0].first_index = static_cast<u32>(i_off / sizeof(u32));
  lod_group.lod_count = 1;

  auto aabb = AABB::create();
  for (const auto &v : vertices)
    aabb.update({v.position[0], v.position[1], v.position[2]});

  result.meshes = {{lod_group}};
  result.submesh_aabbs = {{aabb}};
  result.mesh_aabb = aabb;

  MeshNodeDescription desc{};
  desc.name = "mesh_from_memory";
  desc.local_transform = glm::mat4{1.f};
  desc.parent_index = -1;
  desc.primitives = {{
      .lod_group = lod_group,
      .material_id = 0u,
      .aabb = aabb,
  }};

  result.nodes = {std::move(desc)};
  result.root_node_indices = {0u};

  return renderer.register_gltf(std::move(result));
}

// Reads the bytes an ImageSource would hand to the KTX2/stb decoder, even
// when they live on disk (URI or KTX2 sidecar) rather than already embedded
// in the glTF — used only when building a mesh-cache snapshot, so the cache
// stays self-contained and has no runtime dependency on the source glTF.
[[nodiscard]] static auto read_file_bytes(const std::filesystem::path &path)
    -> std::vector<std::byte> {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return {};
  const auto size = in.tellg();
  std::vector<std::byte> bytes(static_cast<usize>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(bytes.data()), size);
  return bytes;
}

[[nodiscard]] static auto resolve_image_source_bytes(const ImageSource &src)
    -> std::vector<std::byte> {
  if (src.ktx_sidecar_path)
    return read_file_bytes(*src.ktx_sidecar_path);
  if (const auto *path = std::get_if<std::filesystem::path>(&src.data))
    return read_file_bytes(*path);
  return std::get<std::vector<std::byte>>(src.data);
}

// Fully CPU-side glTF → BinaryMesh pipeline, shared by the file-based
// (parse_to_binary_mesh) and archive-based (load_from_compressed) loaders.
// Touches no SceneRenderer/GPU state beyond a plain TextureHandle value and
// the shared worker pool used for fan-out, so it's safe to run entirely off
// the main thread. The result is committed to the live pool by
// build_from_cache, exactly like a warm mesh-cache hit — cold and warm loads
// share the same GPU-writing code path.
// gltf_dir  — directory used to resolve sidecar KTX2 files and external image
// URIs; pass an empty path for GLB loaded from memory (no external files).
// debug_path — used only in log messages.
static auto assemble_binary_mesh(fastgltf::Asset &asset,
                                 const std::filesystem::path &gltf_dir,
                                 const std::filesystem::path &debug_path,
                                 TextureHandle dummy_texture_handle,
                                 const LoadOptions &opts,
                                 BS::priority_thread_pool &thread_pool)
    -> mesh_cache::BinaryMesh {
  mesh_cache::BinaryMesh out;
  out.mesh_aabb = AABB::create();
  out.meshes.resize(asset.meshes.size());
  out.submesh_aabbs.resize(asset.meshes.size());

  const auto color_spaces = classify_images(asset);
  auto image_sources =
      collect_image_sources(asset, gltf_dir, debug_path, color_spaces);

  // Encoded bytes are captured up front (rather than deferred to async
  // decode, as the live-render path does) so BinaryMesh stays self-contained
  // — no runtime dependency on the source glTF / sidecar files.
  out.images.reserve(image_sources.size());
  for (const auto &src : image_sources) {
    out.images.push_back({
        .encoded_bytes = resolve_image_source_bytes(src),
        .format = src.format,
        .srgb = src.srgb,
        .debug_name = src.debug_name,
        .cache_key = src.cache_key,
    });
  }
  for (usize image_idx = 0; image_idx < out.images.size(); ++image_idx)
    out.images[image_idx].patches = build_patch_list(asset, image_idx);

  // -------------------------------------------------------------------------
  // Detect explicit LOD groups from mesh names (e.g. _lod0 … _lod5)
  // -------------------------------------------------------------------------
  std::vector<ExplicitLodGroup> lod_groups;
  if (opts.lod_detection != LodDetection::none) {
    lod_groups = detect_lod_groups(asset, opts.lod_base_name_filter);
    if (!lod_groups.empty()) {
      info("load_gltf: '{}' — detected {} explicit LOD group(s):",
           debug_path.filename().string(), lod_groups.size());
      for (const auto &g : lod_groups) {
        info("  '{}': {} LOD level(s) ({} → {})", g.base_name, g.members.size(),
             g.members.front().first, g.members.back().first);
      }
    }
  }

  // Build fast lookup tables from the detected groups.
  //
  // lod_membership[mesh_idx] = { lod_slot, primary_mesh_idx }
  // suppressed_mesh_indices  = mesh indices for lod_slot > 0
  //   (these get no scene-graph node; their geometry is merged into LOD0)
  struct LodMembership {
    u32 lod_slot;
    usize primary_mesh_idx;
  };
  std::unordered_map<usize, LodMembership> lod_membership;
  std::unordered_set<usize> suppressed_mesh_indices;
  // lod_to_primary_mesh: used by flatten_nodes to remap node→mesh
  // references
  std::unordered_map<usize, usize> lod_to_primary_mesh;

  for (const auto &g : lod_groups) {
    const usize primary_mi = g.primary_mesh_idx();
    for (const auto &[slot, mi] : g.members) {
      lod_membership[mi] = {.lod_slot = slot, .primary_mesh_idx = primary_mi};
      lod_to_primary_mesh[mi] = primary_mi;
      if (slot > 0)
        suppressed_mesh_indices.insert(mi);
    }
  }

  // -------------------------------------------------------------------------
  // Build the flat work list
  //
  // Key ordering requirement: LOD0 primitives MUST appear before LOD1+
  // primitives for the same group, because assemble_geometry_cpu processes
  // them serially and LODn patching reads from the already-written
  // MeshLodGroup.
  //
  // We achieve this by emitting all LOD0 (and non-LOD) entries first, then
  // the higher LOD entries in lod_slot order.
  // -------------------------------------------------------------------------
  std::vector<PrimWork> prim_work_list;
  prim_work_list.reserve(std::accumulate(
      asset.meshes.begin(), asset.meshes.end(), usize{0},
      [](usize acc, const auto &m) { return acc + m.primitives.size(); }));

  // Pass 1: non-LOD and LOD0 entries
  for (auto &&[mi, m] : std::views::enumerate(asset.meshes)) {
    const usize mesh_idx = static_cast<usize>(mi);
    const auto it = lod_membership.find(mesh_idx);
    const bool is_lod_member = (it != lod_membership.end());
    const bool is_higher_lod = is_lod_member && it->second.lod_slot > 0;
    if (is_higher_lod)
      continue; // deferred to pass 2

    for (auto &&[pi, p] : std::views::enumerate(m.primitives)) {
      prim_work_list.push_back({
          .mesh_idx = mesh_idx,
          .prim_idx = static_cast<usize>(pi),
          .ptr = &p,
          .lod_slot = is_lod_member ? it->second.lod_slot : 0u,
          .primary_mesh_idx =
              is_lod_member ? it->second.primary_mesh_idx : mesh_idx,
          .is_explicit_lod = is_lod_member,
      });
    }
  }

  // Pass 2: LOD1+ entries, sorted by lod_slot so they arrive in order
  {
    std::vector<PrimWork> higher_lods;
    for (auto &&[mi, m] : std::views::enumerate(asset.meshes)) {
      const usize mesh_idx = static_cast<usize>(mi);
      const auto it = lod_membership.find(mesh_idx);
      if (it == lod_membership.end() || it->second.lod_slot == 0)
        continue;

      for (auto &&[pi, p] : std::views::enumerate(m.primitives)) {
        higher_lods.push_back({
            .mesh_idx = mesh_idx,
            .prim_idx = static_cast<usize>(pi),
            .ptr = &p,
            .lod_slot = it->second.lod_slot,
            .primary_mesh_idx = it->second.primary_mesh_idx,
            .is_explicit_lod = true,
        });
      }
    }
    std::ranges::sort(higher_lods, {}, &PrimWork::lod_slot);
    for (auto &&pw : higher_lods)
      prim_work_list.push_back(std::move(pw));
  }

  // -------------------------------------------------------------------------
  // Parallel extract → remap → LOD generation → CPU geometry/material
  // assembly (all pure CPU — thread_pool is used only for fan-out here)
  // -------------------------------------------------------------------------
  auto extracted_prims =
      extract_primitives_parallel(asset, prim_work_list, thread_pool);

  // Remap LODn indices to point into LOD0's vertex range.
  if (!lod_groups.empty())
    remap_explicit_lod_indices(prim_work_list, extracted_prims, thread_pool);

  auto [prim_lods, total_lod_indices] =
      generate_lods_parallel(prim_work_list, extracted_prims, thread_pool);

  assemble_materials_cpu(asset, dummy_texture_handle, out);

  RawCollisionGeometry raw_collision;
  assemble_geometry_cpu(prim_work_list, extracted_prims, prim_lods,
                        total_lod_indices, out, opts.retain_collision_geometry,
                        raw_collision);

  {
    PROFILE_SCOPE("Load skeletons & animations");
    out.skeletons = load_skeletons(asset);
    const auto joint_lookup = build_joint_lookup(out.skeletons);
    out.animations = load_animations(asset, joint_lookup);
  }

  using Def = decltype(asset.scenes[0].nodeIndices);
  const auto scene_roots = asset.defaultScene.has_value()
                               ? asset.scenes[*asset.defaultScene].nodeIndices
                               : Def{};
  if (!scene_roots.empty()) {
    PROFILE_SCOPE("Iterate nodes");
    flatten_nodes(asset, scene_roots, out, suppressed_mesh_indices,
                  lod_to_primary_mesh, raw_collision);
  }

  // Count effective LOD levels for the log message.
  u32 max_lod_count = 0;
  for (const auto &mesh_lod_groups : out.meshes)
    for (const auto &lg : mesh_lod_groups)
      max_lod_count = glm::max(max_lod_count, static_cast<u32>(lg.lod_count));

  info("load_gltf: '{}' - {} image(s), {} material(s), {} mesh(es), {} "
       "node(s), {} LOD level(s)",
       debug_path.filename().string(), asset.images.size(),
       asset.materials.size(), asset.meshes.size(), out.nodes.size(),
       max_lod_count);

  return out;
}

// File-based CPU pipeline entry point: parses the source and hands off to
// assemble_binary_mesh. Used by the async loader (parse_to_binary_mesh runs
// entirely on a worker thread) and by the synchronous load_from_path.
static auto
parse_to_binary_mesh(const std::filesystem::path &fs_path,
                     const std::filesystem::path &gltf_dir,
                     TextureHandle dummy_texture_handle,
                     const LoadOptions &opts,
                     BS::priority_thread_pool &thread_pool)
    -> std::expected<mesh_cache::BinaryMesh, std::string> {
  auto asset = parse_gltf_file(fs_path, gltf_dir);
  if (!asset)
    return std::unexpected(asset.error());
  return assemble_binary_mesh(*asset, gltf_dir, fs_path, dummy_texture_handle,
                              opts, thread_pool);
}

// Serializes and writes a mesh cache entry on a background task, without
// blocking the caller. Takes the BinaryMesh by value (copied at the call
// site) so the caller's own copy — about to be handed to build_from_cache —
// is never touched concurrently by the background write. Takes the thread
// pool directly (rather than SceneRenderer&) so it never dereferences any
// renderer state from a worker thread beyond the pool itself, which is safe
// by construction — a thread pool's destructor cannot run while one of its
// own tasks is still executing.
static void write_mesh_cache_async(BS::priority_thread_pool &thread_pool,
                                   mesh_cache::BinaryMesh mesh,
                                   VFSPath cache_path, u64 source_hash) {
  auto snapshot = std::make_shared<mesh_cache::BinaryMesh>(std::move(mesh));
  [[maybe_unused]] auto write_task = thread_pool.submit_task(
      [snapshot, cache_path, source_hash]() {
        try {
          info("mesh cache: background write starting for '{}'",
               cache_path.view());
          auto bytes = mesh_cache::serialize(*snapshot, source_hash);
          std::filesystem::create_directories(
              VFS::get().resolve(cache_path).parent_path());
          auto stream = VFS::get().resolve_to_output_stream(cache_path);
          if (!stream) {
            warn("mesh cache write failed for '{}': {}", cache_path.view(),
                 stream.error());
            return;
          }
          stream->write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
          info("mesh cache: wrote '{}' ({} bytes)", cache_path.view(),
               bytes.size());
        } catch (const std::exception &e) {
          warn("mesh cache write threw for '{}': {}", cache_path.view(),
               e.what());
        }
      });
}

// Expands a cached BinaryMesh back into the live GeometryPool with a single
// memcpy per bulk section (vertices/indices via GeometryPool::allocate,
// materials via GeometryPool::allocate_materials) instead of re-running the
// glTF/meshopt/mikktspace pipeline. Textures still decode asynchronously
// through the normal texture_upload_pool — only the CPU mesh-build pipeline
// is skipped.
[[nodiscard]] static auto build_from_cache(mesh_cache::BinaryMesh &&cached,
                                           SceneRenderer &renderer,
                                           NullableVFSPath source_path)
    -> std::expected<MeshAssetHandle, std::string> {
  auto &pool = *renderer.geometry_pool;

  auto result =
      std::make_unique<MeshAsset>(MeshAsset{.mesh_aabb = AABB::create()});
  result->source_path = source_path;

  const auto offs = pool.allocate(cached.vertices, cached.indices);
  result->vertex_base_offset = offs.vertex_offset;
  result->shadow_vertex_base_offset = offs.shadow_vertex_offset;
  result->index_base_offset = offs.index_offset;

  usize skin_base = 0;
  if (!cached.skin_vertices.empty()) {
    pool.ensure_skin_capacity(cached.skin_vertices.size());
    skin_base = pool.skin_vertex_offset;
    std::memcpy(pool.skin_mapped_pointer(skin_base), cached.skin_vertices.data(),
               cached.skin_vertices.size() * sizeof(SkinVertex));
    pool.flush_skin_range(skin_base, cached.skin_vertices.size());
    pool.skin_vertex_offset += cached.skin_vertices.size();
  }

  const auto mat_offset = pool.allocate_materials(
      std::span<const GPUMaterial>(cached.materials));
  result->material_base_slot = mat_offset.start_index;
  result->material_count = static_cast<u32>(cached.materials.size());

  mesh_cache::rebase(cached,
                     static_cast<i64>(offs.vertex_offset / sizeof(Vertex)),
                     static_cast<i64>(offs.index_offset / sizeof(u32)),
                     static_cast<i64>(skin_base),
                     static_cast<i64>(result->material_base_slot));

  result->material_slots = std::move(cached.material_slots);
  result->meshes = std::move(cached.meshes);
  result->submesh_aabbs = std::move(cached.submesh_aabbs);
  result->mesh_aabb = cached.mesh_aabb;
  result->nodes = std::move(cached.nodes);
  result->root_node_indices = std::move(cached.root_node_indices);
  result->skeletons = std::move(cached.skeletons);
  result->animations = std::move(cached.animations);
  result->collision_positions = std::move(cached.collision_positions);
  result->collision_indices = std::move(cached.collision_indices);

  result->texture_handles.resize(cached.images.size(),
                                 renderer.dummy_texture_handle);

  std::vector<ImageSource> image_sources;
  image_sources.reserve(cached.images.size());
  for (usize i = 0; i < cached.images.size(); ++i) {
    auto &img = cached.images[i];
    image_sources.push_back({
        .data = std::move(img.encoded_bytes),
        .ktx_sidecar_path = std::nullopt,
        .debug_name = img.debug_name,
        .cache_key = img.cache_key,
        .format = img.format,
        .srgb = img.srgb,
        .image_idx = i,
    });
  }

  auto pending = launch_texture_futures(image_sources, renderer, *result);
  for (auto &pu : pending)
    pu.patches = std::move(cached.images[pu.image_idx].patches);

  const u32 material_base_slot = result->material_base_slot;
  auto handle = renderer.register_gltf(std::move(*result));
  submit_texture_uploads(pending, renderer, handle, material_base_slot);
  return handle;
}

auto load_from_path(const VFSPath &path, SceneRenderer &renderer,
                    const LoadOptions &opts)
    -> std::expected<MeshAssetHandle, std::string> {
  const u64 source_hash = mesh_cache::hash_source(path, opts);
  const auto cache_path = mesh_cache::cache_path_for(path, source_hash);

  if (auto cache_bytes = VFS::get().read_bytes(cache_path)) {
    if (auto cached = mesh_cache::deserialize(*cache_bytes, source_hash)) {
      info("load_gltf: '{}' — loaded from mesh cache '{}'", path.view(),
           cache_path.view());
      return build_from_cache(std::move(*cached), renderer,
                              NullableVFSPath{path});
    } else {
      info("load_gltf: '{}' — mesh cache miss ({}), doing a cold load",
           path.view(), cached.error());
    }
  }

  const auto fs_path = VFS::get().resolve(path);
  const auto gltf_dir = fs_path.parent_path();

  if (!std::filesystem::exists(fs_path))
    return std::unexpected("File not found");

  auto binary_mesh = parse_to_binary_mesh(
      fs_path, gltf_dir, renderer.dummy_texture_handle, opts,
      renderer.thread_pool);
  if (!binary_mesh)
    return std::unexpected(binary_mesh.error());

  // Compress + write in the background — never blocks the caller on a cache
  // miss, and a write failure is not fatal (next cold load just retries it).
  write_mesh_cache_async(renderer.thread_pool, *binary_mesh, cache_path,
                        source_hash);

  return build_from_cache(std::move(*binary_mesh), renderer,
                          NullableVFSPath{path});
}

// Async counterpart to load_from_path. The whole cache-check/parse/LOD
// pipeline runs as a task on renderer.thread_pool (not a separately-owned
// thread) — the same pool extract_primitives_parallel/generate_lods_parallel
// fan out onto internally, and the same pool every other background mesh/
// texture task already uses, so its lifetime is governed by the exact
// mechanism SceneRenderer's teardown already relies on. This makes the
// dispatch a nested submit-and-wait (a pool task waiting on more work
// submitted to the same pool); safe in practice since mesh loads are rare,
// UI-driven, one-at-a-time events relative to the pool's thread count — a
// hypothetical future bulk-import path issuing hardware_concurrency()+
// simultaneous loads could stall until a worker frees up.
// on_complete fires on the main thread once SceneRenderer::prepare drains
// mesh_upload_pool (see SceneRenderer::prepare's "Poll Registries" step).
auto load_from_path_async(const VFSPath &path, SceneRenderer &renderer,
                          std::function<void(MeshAssetHandle)> on_complete,
                          const LoadOptions &opts) -> void {
  auto stop_src = std::stop_source{};
  auto token = stop_src.get_token();
  const TextureHandle dummy_texture_handle = renderer.dummy_texture_handle;

  auto fut = renderer.thread_pool.submit_task(
      [&thread_pool = renderer.thread_pool, path, opts, dummy_texture_handle,
       token]() mutable -> pool::MeshLoadResult {
        if (token.stop_requested())
          return {};

        const u64 source_hash = mesh_cache::hash_source(path, opts);
        const auto cache_path = mesh_cache::cache_path_for(path, source_hash);

        if (auto cache_bytes = VFS::get().read_bytes(cache_path)) {
          if (auto cached =
                  mesh_cache::deserialize(*cache_bytes, source_hash)) {
            info("load_gltf: '{}' — loaded from mesh cache '{}' (async)",
                path.view(), cache_path.view());
            return {.mesh = std::move(*cached),
                    .source_path = NullableVFSPath{path}};
          } else {
            info("load_gltf: '{}' — mesh cache miss ({}), doing a cold load "
                "(async)",
                path.view(), cached.error());
          }
        }

        const auto fs_path = VFS::get().resolve(path);
        const auto gltf_dir = fs_path.parent_path();
        if (!std::filesystem::exists(fs_path))
          return {.mesh = std::unexpected("File not found"),
                  .source_path = NullableVFSPath{path}};

        auto binary_mesh = parse_to_binary_mesh(
            fs_path, gltf_dir, dummy_texture_handle, opts, thread_pool);
        if (binary_mesh)
          write_mesh_cache_async(thread_pool, *binary_mesh, cache_path,
                                source_hash);

        return {.mesh = std::move(binary_mesh),
                .source_path = NullableVFSPath{path}};
      });

  renderer.mesh_upload_pool->submit(std::move(fut), std::move(stop_src),
                                    std::move(on_complete));
}

auto load_from_compressed(const VFSPath &archive_path, SceneRenderer &renderer,
                          const LoadOptions &opts)
    -> std::expected<MeshAssetHandle, std::string> {
  auto bundle = archive::extract_to_memory(archive_path);
  if (!bundle)
    return std::unexpected(
        std::format("archive extraction failed: {}", bundle.error()));

  // Find the first .glb entry in the bundle.
  const std::string *glb_key = nullptr;
  for (const auto &[key, _] : *bundle) {
    if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".glb") == 0) {
      glb_key = &key;
      break;
    }
  }
  if (!glb_key)
    return std::unexpected(
        "no .glb found in archive — repack as GLB (self-contained) or use "
        "archive::extract() + load_from_path() for .gltf with external files");

  auto asset = parse_gltf_memory(bundle->at(*glb_key));
  if (!asset)
    return std::unexpected(asset.error());

  const std::filesystem::path debug_path =
      std::format("{}/{}", archive_path.view(), *glb_key);
  const auto gltf_dir = VFS::get().resolve(archive_path).parent_path();
  auto binary_mesh =
      assemble_binary_mesh(*asset, gltf_dir, debug_path,
                          renderer.dummy_texture_handle, opts,
                          renderer.thread_pool);
  return build_from_cache(std::move(binary_mesh), renderer,
                          NullableVFSPath{archive_path});
}

} // namespace mesh

// Defined here (rather than in a standalone mesh_upload_pool.cpp) because the
// commit step is exactly build_from_cache — a function private to this file.
auto pool::MeshUploadPool::poll_n(SceneRenderer &renderer, usize n) -> void {
  auto batch = take_ready(n);
  if (batch.empty())
    return;

  for (auto &entry : batch) {
    auto result = entry.work.get();
    if (!result.mesh) {
      warn("MeshUploadPool: load failed for '{}': {}",
          result.source_path.valid() ? result.source_path.view()
                                     : std::string_view{"<unknown>"},
          result.mesh.error());
      entry.on_complete(MeshAssetHandle{});
      continue;
    }

    auto handle = mesh::build_from_cache(std::move(*result.mesh), renderer,
                                         result.source_path);
    if (!handle) {
      warn("MeshUploadPool: commit failed for '{}': {}",
          result.source_path.valid() ? result.source_path.view()
                                     : std::string_view{"<unknown>"},
          handle.error());
      entry.on_complete(MeshAssetHandle{});
      continue;
    }
    entry.on_complete(*handle);
  }

  note_completed(batch.size());
}

} // namespace dy
