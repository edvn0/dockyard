#include <dockyard/mesh_loader.hpp>

#include <dockyard/log.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <mikktspace.h>

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/packing.hpp>

#include <expected>
#include <format>
#include <string>
#include <vector>

#include <taskflow/algorithm/for_each.hpp>
#include <taskflow/taskflow.hpp>

namespace dy {

struct PrimitiveData {
  std::vector<Vertex> vertices;
  std::vector<u32> indices;
};
struct PrimitiveResult {
  PrimitiveData data;
  AABB aabb;
};

} // namespace dy

namespace mikkt {
struct MikkContext {
  dy::PrimitiveData *prim = nullptr;
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<glm::vec2> uvs;
};

static auto mikk_get_num_faces(const SMikkTSpaceContext *ctx) -> int {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  return static_cast<int>(c->prim->indices.size() / 3);
}

static auto mikk_get_num_verts_of_face(const SMikkTSpaceContext *, int) -> int {
  return 3;
}

static void mikk_get_position(const SMikkTSpaceContext *ctx, float out[],
                              int face, int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  dy::u32 idx = c->prim->indices[static_cast<dy::u32>(face * 3 + vert)];
  auto &p = c->positions[idx];
  out[0] = p.x;
  out[1] = p.y;
  out[2] = p.z;
}

static void mikk_get_normal(const SMikkTSpaceContext *ctx, float out[],
                            int face, int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  dy::u32 idx = c->prim->indices[static_cast<dy::u32>((face * 3) + vert)];
  auto &n = c->normals[idx];
  out[0] = n.x;
  out[1] = n.y;
  out[2] = n.z;
}

static void mikk_get_uv(const SMikkTSpaceContext *ctx, float out[], int face,
                        int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  dy::u32 idx = c->prim->indices[static_cast<dy::u32>((face * 3) + vert)];
  auto &uv = c->uvs[idx];
  out[0] = uv.x;
  out[1] = uv.y;
}

static void mikk_set_tspace(const SMikkTSpaceContext *ctx,
                            const float tangent[], float sign, int face,
                            int vert) {
  auto *c = static_cast<MikkContext *>(ctx->m_pUserData);
  dy::u32 idx = c->prim->indices[static_cast<dy::u32>(face * 3 + vert)];

  c->prim->vertices[idx].tangent =
      glm::packSnorm4x8(glm::vec4{tangent[0], tangent[1], tangent[2], sign});

  glm::vec3 n = c->normals[idx];
  glm::vec3 t{tangent[0], tangent[1], tangent[2]};
  glm::vec3 bt = glm::normalize(glm::cross(n, t)) * sign;
  c->prim->vertices[idx].bitangent = glm::packSnorm4x8(glm::vec4{bt, 0.0f});
}

static auto generate_mikktspace_tangents(dy::PrimitiveData &prim)
    -> std::expected<void, std::string> {
  MikkContext mctx;
  mctx.prim = &prim;

  // Unpack once up front — cheaper than re-unpacking per mikk callback
  const dy::usize vtx_count = prim.vertices.size();
  mctx.positions.resize(vtx_count);
  mctx.normals.resize(vtx_count);
  mctx.uvs.resize(vtx_count);

  for (dy::usize i = 0; i < vtx_count; ++i) {
    auto &v = prim.vertices[i];
    mctx.positions[i] = {v.position[0], v.position[1], v.position[2]};
    mctx.normals[i] = glm::unpackSnorm4x8(v.normal);
    mctx.uvs[i] = glm::unpackHalf2x16(v.uvs);
  }

  SMikkTSpaceInterface iface{};
  iface.m_getNumFaces = mikk_get_num_faces;
  iface.m_getNumVerticesOfFace = mikk_get_num_verts_of_face;
  iface.m_getPosition = mikk_get_position;
  iface.m_getNormal = mikk_get_normal;
  iface.m_getTexCoord = mikk_get_uv;
  iface.m_setTSpaceBasic = mikk_set_tspace;

  SMikkTSpaceContext ctx{};
  ctx.m_pInterface = &iface;
  ctx.m_pUserData = &mctx;

  if (genTangSpaceDefault(&ctx) == 0)
    return std::unexpected("mikktspace tangent generation failed");

  return {};
}
} // namespace mikkt

namespace dy {
namespace {

struct DecodedImage {
  std::vector<u32> pixels;
  u32 width{};
  u32 height{};
};

[[nodiscard]] auto decode_stbi(const stbi_uc *data, int len,
                               std::string_view debug_name)
    -> std::expected<DecodedImage, std::string> {
  int w{};
  int h{};
  int ch{};
  stbi_uc *raw = stbi_load_from_memory(data, len, &w, &h, &ch, STBI_rgb_alpha);
  if (!raw)
    return std::unexpected(
        std::format("stbi '{}': {}", debug_name, stbi_failure_reason()));

  DecodedImage img;
  img.width = static_cast<u32>(w);
  img.height = static_cast<u32>(h);
  img.pixels.resize(static_cast<usize>(w * h));
  std::memcpy(img.pixels.data(), raw, img.pixels.size() * sizeof(u32));
  stbi_image_free(raw);
  return std::expected<DecodedImage, std::string>{
      std::in_place,
      std::move(img),
  };
}

[[nodiscard]] auto load_image(const fastgltf::Asset &asset,
                              const fastgltf::Image &image,
                              const std::filesystem::path &gltf_dir)
    -> std::expected<DecodedImage, std::string> {
  const std::string_view name = image.name;

  return std::visit(
      fastgltf::visitor{
          [&](const fastgltf::sources::URI &uri)
              -> std::expected<DecodedImage, std::string> {
            const auto full = gltf_dir / uri.uri.fspath();
            int w{};
            int h{};
            int ch{};
            stbi_uc *raw =
                stbi_load(full.string().c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!raw)
              return std::unexpected(std::format("stbi '{}': {}", full.string(),
                                                 stbi_failure_reason()));
            DecodedImage img;
            img.width = static_cast<u32>(w);
            img.height = static_cast<u32>(h);
            img.pixels.resize(static_cast<usize>(w * h));
            std::memcpy(img.pixels.data(), raw,
                        img.pixels.size() * sizeof(u32));
            stbi_image_free(raw);
            return img;
          },
          [&](const fastgltf::sources::Array &arr)
              -> std::expected<DecodedImage, std::string> {
            return decode_stbi(
                reinterpret_cast<const stbi_uc *>(arr.bytes.data()),
                static_cast<int>(arr.bytes.size()), name);
          },
          [&](const fastgltf::sources::BufferView &bv)
              -> std::expected<DecodedImage, std::string> {
            const auto &view = asset.bufferViews[bv.bufferViewIndex];
            const auto &buf = asset.buffers[view.bufferIndex];
            return std::visit(
                fastgltf::visitor{
                    [&](const fastgltf::sources::Array &arr)
                        -> std::expected<DecodedImage, std::string> {
                      return decode_stbi(
                          reinterpret_cast<const stbi_uc *>(arr.bytes.data() +
                                                            view.byteOffset),
                          static_cast<int>(view.byteLength), name);
                    },
                    [&](const auto &)
                        -> std::expected<DecodedImage, std::string> {
                      return std::unexpected(std::format(
                          "Unsupported buffer backing for image '{}'", name));
                    },
                },
                buf.data);
          },
          [&](const auto &) -> std::expected<DecodedImage, std::string> {
            return std::unexpected(
                std::format("Unsupported image source type for '{}'", name));
          },
      },
      image.data);
}

// ─────────────────────────────────────────────────────────────────────────────
// sRGB classification — scan materials before uploading so format is known
// ─────────────────────────────────────────────────────────────────────────────

enum class ImageColorSpace : u8 { linear, srgb };

[[nodiscard]] auto classify_images(const fastgltf::Asset &asset)
    -> std::vector<ImageColorSpace> {
  std::vector<ImageColorSpace> cs(asset.images.size(), ImageColorSpace::linear);

  auto mark_srgb = [&](const fastgltf::Optional<fastgltf::TextureInfo> &info) {
    if (!info.has_value())
      return;
    const auto &tex = asset.textures[info->textureIndex];
    if (tex.imageIndex.has_value())
      cs[*tex.imageIndex] = ImageColorSpace::srgb;
  };

  for (const auto &mat : asset.materials) {
    mark_srgb(mat.pbrData.baseColorTexture);
    mark_srgb(mat.emissiveTexture);
  }
  return cs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Texture slot resolution
// Fallback slots match upload order in Dockforge::init():
//   0 = white, 1 = normal blue, 2 = metallic-roughness, 3 = occlusion, 4 =
//   black
// ─────────────────────────────────────────────────────────────────────────────

constexpr u32 k_fb_albedo = 0u;
constexpr u32 k_fb_normal = 1u;
constexpr u32 k_fb_mr = 2u;
constexpr u32 k_fb_occlusion = 3u;
constexpr u32 k_fb_emissive = 4u;

[[nodiscard]] auto resolve_tex(const fastgltf::Asset &asset,
                               const std::vector<TextureHandle> &handles,
                               const auto &info, u32 fallback) -> u32 {
  if (!info.has_value())
    return fallback;
  const auto &tex = asset.textures[info->textureIndex];
  if (!tex.imageIndex.has_value())
    return fallback;
  return handles[*tex.imageIndex].index();
}

// ─────────────────────────────────────────────────────────────────────────────
// Material building
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] auto build_gpu_material(const fastgltf::Material &mat,
                                      const fastgltf::Asset &asset,
                                      const std::vector<TextureHandle> &handles)
    -> GPUMaterial {
  GPUMaterial gpu{};
  const auto &pbr = mat.pbrData;

  gpu.albedo_factor[0] = pbr.baseColorFactor[0];
  gpu.albedo_factor[1] = pbr.baseColorFactor[1];
  gpu.albedo_factor[2] = pbr.baseColorFactor[2];
  gpu.albedo_factor[3] = pbr.baseColorFactor[3];

  gpu.emissive_factor[0] = mat.emissiveFactor[0];
  gpu.emissive_factor[1] = mat.emissiveFactor[1];
  gpu.emissive_factor[2] = mat.emissiveFactor[2];
  gpu.emissive_factor[3] = mat.emissiveStrength;

  gpu.metallic_factor = pbr.metallicFactor;
  gpu.roughness_factor = pbr.roughnessFactor;
  gpu.normal_scale = mat.normalTexture ? mat.normalTexture->scale : 1.0F;
  gpu.occlusion_strength =
      mat.occlusionTexture ? mat.occlusionTexture->strength : 1.0F;
  gpu.alpha_cutoff = mat.alphaCutoff;
  gpu.alpha_mode = static_cast<u32>(mat.alphaMode);

  gpu.albedo_index =
      resolve_tex(asset, handles, pbr.baseColorTexture, k_fb_albedo);
  gpu.normal_index =
      resolve_tex(asset, handles, mat.normalTexture, k_fb_normal);
  gpu.metallic_roughness_index =
      resolve_tex(asset, handles, pbr.metallicRoughnessTexture, k_fb_mr);
  gpu.emissive_index =
      resolve_tex(asset, handles, mat.emissiveTexture, k_fb_emissive);
  gpu.occlusion_index =
      resolve_tex(asset, handles, mat.occlusionTexture, k_fb_occlusion);

  // Base flags: depth prepass for opaque/masked
  gpu.flags = MaterialFlags::depth_prepass;
  if (mat.alphaMode == fastgltf::AlphaMode::Mask)
    set_flag(gpu.flags, MaterialFlags::alpha_mask);

  // Two-sided rendering
  if (mat.doubleSided)
    set_flag(gpu.flags, MaterialFlags::two_sided);

  if (auto *ext = mat.transmission.get(); ext != nullptr) {
    gpu.transmission_factor = ext->transmissionFactor;
    if (gpu.transmission_factor > 0.0F)
      set_flag(gpu.flags, MaterialFlags::has_transmission);
  }

  if (auto *ext = mat.anisotropy.get(); ext != nullptr) {
    gpu.anisotropy_factor = ext->anisotropyStrength;
    gpu.anisotropy_rotation = ext->anisotropyRotation;
    if (gpu.anisotropy_factor > 0.0F)
      set_flag(gpu.flags, MaterialFlags::has_anisotropy);
  }

  gpu.uv_scale_x = 1.0F;
  gpu.uv_scale_y = 1.0F;
  gpu.uv_offset_x = 0.0F;
  gpu.uv_offset_y = 0.0F;

  gpu.cull_mode = mat.doubleSided ? static_cast<u32>(CullMode::None)
                                  : static_cast<u32>(CullMode::Back);

  return gpu;
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive extraction
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] auto extract_primitive(const fastgltf::Asset &asset,
                                     const fastgltf::Primitive &prim)
    -> std::expected<PrimitiveResult, std::string> {
  const auto *pos_attr = prim.findAttribute("POSITION");
  if (pos_attr == prim.attributes.end())
    return std::unexpected("Primitive missing POSITION attribute");
  if (!prim.indicesAccessor.has_value())
    return std::unexpected("Non-indexed primitive — not supported");

  const auto &pos_acc = asset.accessors[pos_attr->accessorIndex];
  const usize vtx_count = pos_acc.count;

  PrimitiveData out;
  out.vertices.resize(vtx_count);

  auto aabb = AABB::create();

  fastgltf::iterateAccessorWithIndex<glm::vec3>(
      asset, pos_acc, [&](glm::vec3 v, usize i) {
        out.vertices[i].position[0] = v.x;
        out.vertices[i].position[1] = v.y;
        out.vertices[i].position[2] = v.z;
        aabb.update(v);
      });

  if (const auto *a = prim.findAttribute("NORMAL");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec3>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec3 n, usize i) {
          out.vertices[i].normal = glm::packSnorm4x8({n, 1.0f});
        });
  }

  if (const auto *a = prim.findAttribute("TEXCOORD_0");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec2>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec2 uv, usize i) {
          out.vertices[i].uvs = glm::packHalf2x16(uv);
        });
  }

  if (const auto *a = prim.findAttribute("TANGENT");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec4 t, usize i) {
          out.vertices[i].tangent = glm::packSnorm4x8(t);
        });
  }

  if (const auto *a = prim.findAttribute("BITANGENT");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec4 t, usize i) {
          out.vertices[i].bitangent = glm::packSnorm4x8(t);
        });
  }

  const auto &idx_acc = asset.accessors[*prim.indicesAccessor];
  out.indices.resize(idx_acc.count);
  fastgltf::copyFromAccessor<u32>(asset, idx_acc, out.indices.data());

  const bool has_tangents =
      prim.findAttribute("TANGENT") != prim.attributes.end();
  const bool has_normals =
      prim.findAttribute("NORMAL") != prim.attributes.end();
  const bool has_uvs =
      prim.findAttribute("TEXCOORD_0") != prim.attributes.end();

  if (!has_tangents) {
    if (!has_normals || !has_uvs)
      return std::unexpected(
          "Cannot generate tangents: missing NORMAL or TEXCOORD_0");

    if (auto res = mikkt::generate_mikktspace_tangents(out); !res)
      return std::unexpected(res.error());
  }

  return std::expected<PrimitiveResult, std::string>{std::in_place, out, aabb};
}

// ─────────────────────────────────────────────────────────────────────────────
// Node transform helpers
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] auto node_local_matrix(const fastgltf::Node &node) -> glm::mat4 {
  return std::visit(
      fastgltf::visitor{
          [](const fastgltf::TRS &trs) -> glm::mat4 {
            const glm::vec3 t{trs.translation[0], trs.translation[1],
                              trs.translation[2]};
            // fastgltf: [x,y,z,w]; glm ctor: (w,x,y,z)
            const glm::quat q{trs.rotation[3], trs.rotation[0], trs.rotation[1],
                              trs.rotation[2]};
            const glm::vec3 s{trs.scale[0], trs.scale[1], trs.scale[2]};
            return glm::translate(glm::mat4{1.f}, t) * glm::mat4_cast(q) *
                   glm::scale(glm::mat4{1.f}, s);
          },
          [](const fastgltf::math::fmat4x4 &m) -> glm::mat4 {
            glm::mat4 out;
            std::memcpy(glm::value_ptr(out), m.data(), sizeof(glm::mat4));
            return out;
          },
      },
      node.transform);
}

// ─────────────────────────────────────────────────────────────────────────────
// Node hierarchy flattening — pure output into MeshAsset::nodes
// ─────────────────────────────────────────────────────────────────────────────

struct NodeStackFrame {
  usize node_idx;
  i32 parent_flat_idx; // -1 = root
};

void flatten_nodes(const fastgltf::Asset &asset,
                   std::span<const std::size_t> root_indices, MeshAsset &out) {
  struct Frame {
    usize node_idx;
    i32 parent_flat_idx;
    bool is_root;
  };

  std::vector<Frame> dfs;
  dfs.reserve(root_indices.size());
  for (const auto idx : root_indices)
    dfs.push_back({
        .node_idx = idx,
        .parent_flat_idx = -1,
        .is_root = true,
    });

  while (!dfs.empty()) {
    auto [node_idx, parent_flat, is_root] = dfs.back();
    dfs.pop_back();

    const i32 flat_idx = static_cast<i32>(out.nodes.size());
    if (is_root)
      out.root_node_indices.push_back(static_cast<u32>(flat_idx));

    const auto &node = asset.nodes[node_idx];

    MeshNodeDescription desc;
    desc.name = node.name.empty() ? std::format("gltf_node_{}", node_idx)
                                  : std::string(node.name);
    desc.local_transform = node_local_matrix(node);
    desc.parent_index = parent_flat;

    if (node.meshIndex.has_value()) {
      const usize mi = *node.meshIndex;
      const auto &meshes = out.meshes[mi];
      const auto &gltf_mesh = asset.meshes[mi];
      desc.primitives.reserve(meshes.size());

      for (usize pi = 0; pi < meshes.size(); ++pi) {
        const auto &h = meshes[pi];
        const u32 mat_id =
            gltf_mesh.primitives[pi].materialIndex.has_value()
                ? out.material_slots[*gltf_mesh.primitives[pi].materialIndex]
                : 0u;

        desc.primitives.push_back({
            .mesh = h,
            .material_id = mat_id,
            .aabb = out.submesh_aabbs[mi][pi],
        });
      }
    }

    out.nodes.push_back(std::move(desc));

    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
      dfs.push_back({
          .node_idx = *it,
          .parent_flat_idx = flat_idx,
          .is_root = false,
      });
  }
}

} // namespace

namespace mesh {

[[nodiscard]] static auto hash_bytes(const void *data, usize len) -> u64 {
  const auto *p = static_cast<const u8 *>(data);
  u64 h = 14695981039346656037ULL;
  for (usize i = 0; i < len; ++i)
    h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}

auto load_from_memory(SceneRenderer &renderer, std::span<const Vertex> vertices,
                      std::span<const u32> indices)
    -> std::expected<MeshHandle, std::string> {
  auto &pool = *renderer.geometry_pool;

  MeshAsset result{
      .mesh_aabb = AABB::create(),
  };
  result.material_base_slot = 0u;
  result.material_count = 1u;
  result.material_slots = {0u};
  result.texture_handles = {};
  result.vertex_base_offset = pool.vertex_offset;
  result.shadow_vertex_base_offset = pool.shadow_vertex_offset;
  result.index_base_offset = pool.index_offset;

  auto &&[v_off, sv_off, i_off] = pool.allocate(vertices, indices);

  const Mesh mesh{
      .index_count = static_cast<u32>(indices.size()),
      .first_index = static_cast<u32>(i_off / sizeof(u32)),
      .vertex_offset = static_cast<i32>(v_off / sizeof(Vertex)),
  };

  result.meshes = {{mesh}};
  auto aabb = AABB::create();
  for (const auto &v : vertices)
    aabb.update({v.position[0], v.position[1], v.position[2]});

  MeshNodeDescription desc{};
  desc.name = "mesh_from_memory";
  desc.local_transform = glm::mat4{1.f};
  desc.parent_index = -1;
  desc.primitives = {{.mesh = mesh, .material_id = 0u, .aabb = aabb}};

  result.nodes = {std::move(desc)};
  result.root_node_indices = {0u};

  result.mesh_aabb = aabb;
  result.submesh_aabbs = {{aabb}};

  return renderer.register_gltf(std::move(result));
}

struct GeometryRequirements {
  size_t total_vertices = 0;
  size_t total_indices = 0;
};
static auto calculate_requirements(const auto &extracted_prims)
    -> GeometryRequirements {
  GeometryRequirements reqs;

  for (const auto &res : extracted_prims) {
    if (res) {
      reqs.total_vertices += res->data.vertices.size();
      reqs.total_indices += res->data.indices.size();
    }
  }

  return reqs;
}

auto load_from_path(const VFSPath &path, SceneRenderer &renderer)
    -> std::expected<MeshHandle, std::string> {

  auto &pool = *renderer.geometry_pool;
  const auto fs_path = VFS::get().resolve(path);
  if (!std::filesystem::exists(fs_path))
    return std::unexpected("File not found");

  const auto gltf_dir = fs_path.parent_path();
  fastgltf::Parser parser;
  auto data = fastgltf::GltfDataBuffer::FromPath(fs_path);
  auto asset_result = parser.loadGltf(data.get(), gltf_dir,
                                      fastgltf::Options::GenerateMeshIndices);
  if (!asset_result)
    return std::unexpected("Parse error");

  auto &asset = asset_result.get();
  MeshAsset output_result{
      .mesh_aabb = AABB::create(),
  };
  auto result = std::make_shared<MeshAsset>(output_result);
  result->texture_handles.resize(asset.images.size());
  result->material_slots.resize(asset.materials.size(), 0u);

  const auto color_spaces = classify_images(asset);

  static tf::Executor executor;
  tf::Taskflow taskflow;

  std::vector<std::expected<DecodedImage, std::string>> decoded_images(
      asset.images.size());
  {
    PROFILE_SCOPE("Decode images");
    [[maybe_unused]] auto image_group = taskflow.for_each_index(
        static_cast<usize>(0), asset.images.size(), usize(1), [&](usize i) {
          decoded_images[i] = load_image(asset, asset.images[i], gltf_dir);
        });
  }
  struct PrimWork {
    usize mesh_idx;
    usize prim_idx;
    const fastgltf::Primitive *ptr;
  };
  std::vector<PrimWork> prim_work_list;
  for (auto &&[mi, m] : std::views::enumerate(asset.meshes)) {
    for (auto &&[pi, p] : std::views::enumerate(m.primitives)) {
      prim_work_list.push_back({
          .mesh_idx = static_cast<usize>(mi),
          .prim_idx = static_cast<usize>(pi),
          .ptr = &p,
      });
    }
  }

  std::vector<std::expected<PrimitiveResult, std::string>> extracted_prims(
      prim_work_list.size(), std::unexpected<std::string>("Could not extract"));
  {
    PROFILE_SCOPE("Extract primitives");
    [[maybe_unused]] auto mesh_group = taskflow.for_each_index(
        static_cast<usize>(0), prim_work_list.size(), static_cast<usize>(1),
        [&](usize i) {
          extracted_prims[i] = extract_primitive(asset, *prim_work_list[i].ptr);
        });
  }

  {
    PROFILE_SCOPE("Wait for threads");
    executor.run(taskflow).wait();
  }

  {
    PROFILE_SCOPE("Decode and upload textures");
    for (usize i = 0; i < asset.images.size(); ++i) {
      if (!decoded_images[i]) {
        result->texture_handles[i] = renderer.dummy_texture_handle;
        continue;
      }

      auto &img = *decoded_images[i];
      const VkFormat fmt = (color_spaces[i] == ImageColorSpace::srgb)
                               ? VK_FORMAT_R8G8B8A8_SRGB
                               : VK_FORMAT_R8G8B8A8_UNORM;
      const std::string fmt_suffix =
          (fmt == VK_FORMAT_R8G8B8A8_SRGB) ? ":srgb" : ":linear";

      std::string cache_key;
      if (const auto *uri_src =
              std::get_if<fastgltf::sources::URI>(&asset.images[i].data)) {
        cache_key = (gltf_dir / uri_src->uri.fspath()).string() + fmt_suffix;
      } else {
        const u64 h =
            hash_bytes(img.pixels.data(), img.pixels.size() * sizeof(u32));
        cache_key = std::format("hash:{:016x}{}", h, fmt_suffix);
      }

      if (auto cached = renderer.texture_cache.get(cache_key)) {
        result->texture_handles[i] = *cached;
        continue;
      }

      const std::string debug_name =
          asset.images[i].name.empty()
              ? std::format("{}#img{}", fs_path.filename().string(), i)
              : std::string(asset.images[i].name);

      auto handle = renderer.upload_texture(img.pixels, debug_name, img.width,
                                            img.height, fmt, true, false);

      renderer.texture_cache.insert(cache_key, handle);
      result->texture_handles[i] = handle;
    }
  }

  if (!asset.materials.empty()) {
    PROFILE_SCOPE("Allocate materials");
    std::vector<GPUMaterial> gpu_mats;
    gpu_mats.reserve(asset.materials.size());
    for (const auto &mat : asset.materials)
      gpu_mats.push_back(
          build_gpu_material(mat, asset, result->texture_handles));

    auto mat_offset = pool.allocate_materials(gpu_mats);
    result->material_base_slot = mat_offset.start_index;
    result->material_count = static_cast<u32>(gpu_mats.size());
    for (usize i = 0; i < gpu_mats.size(); ++i)
      result->material_slots[i] = result->material_base_slot + (u32)i;
  }

  {
    PROFILE_SCOPE("Allocate geometry");

    // 1. Pre-calculate & Safety
    auto [total_v, total_i] = calculate_requirements(extracted_prims);
    pool.reserve(total_v, total_i);

    // 2. Start optimized batch
    auto batch = pool.begin_transaction();

    result->meshes.resize(asset.meshes.size());
    result->submesh_aabbs.resize(asset.meshes.size()); // same size as meshes
    result->vertex_base_offset = pool.vertex_offset;
    result->submesh_aabbs.clear();

    for (usize i = 0; i < prim_work_list.size(); ++i) {
      auto &res = extracted_prims[i];
      const usize mesh_idx = prim_work_list[i].mesh_idx;

      if (!res) {
        result->submesh_aabbs[mesh_idx].push_back(AABB::create());
        continue;
      }

      auto &[data, aabb] = *res;

      auto offsets = batch.allocate(data.vertices, data.indices);
      result->submesh_aabbs[mesh_idx].push_back(aabb); // ← per-submesh
      result->mesh_aabb.merge(aabb);         // ← accumulate overall
      result->meshes[mesh_idx].push_back(Mesh{
          .index_count = static_cast<u32>(data.indices.size()),
          .first_index = static_cast<u32>(offsets.index_offset / sizeof(u32)),
          .vertex_offset =
              static_cast<i32>(offsets.vertex_offset / sizeof(Vertex)),
      });
    }

    batch.commit();
  }

  const auto scene_roots =
      asset.defaultScene.has_value()
          ? asset.scenes[*asset.defaultScene].nodeIndices
          : std::vector<unsigned long,
                        std::pmr::polymorphic_allocator<unsigned long>>{};
  if (!scene_roots.empty()) {
    PROFILE_SCOPE("Iterate nodes");
    flatten_nodes(asset, scene_roots, *result);
  }

  info("load_gltf: '{}' — {} image(s), {} material(s), {} mesh(es), {} node(s)",
       fs_path.filename().string(), asset.images.size(), asset.materials.size(),
       asset.meshes.size(), result->nodes.size());
  return renderer.register_gltf(std::move(*result));
}

} // namespace mesh

} // namespace dy