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

#include <ktx.h>
#include <ktxvulkan.h>

#include <mikktspace.h>
#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <execution>
#include <expected>
#include <format>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

#include <meshoptimizer.h>

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

namespace {
struct GeometryRequirements {
  dy::usize total_vertices = 0;
  dy::usize total_indices = 0;
};

auto calculate_requirements(const auto &extracted_prims)
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

template <typename O> auto cast(void *ptr, dy::usize offset) -> O * {
  return reinterpret_cast<O *>(static_cast<dy::u8 *>(ptr) + offset);
}
} // namespace

// ── MikkTSpace (unchanged)
// ────────────────────────────────────────────────────

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

// ── KTX2 transcode ───────────────────────────────────────────────────────────

// Target formats after UASTC transcode.
// BC7  — best quality colour/RGBA (requires BC7 feature, universally supported
//         on desktop Vulkan)
// BC5  — two-channel RG, ideal for normal maps (xy only, reconstruct z)
// BC1  — fallback for very simple single-channel data (not used by default)
//
// We pick the transcode target based on the intended VkFormat so the caller
// can stay format-agnostic.
struct KtxMip {
  std::vector<std::byte> data;
  u32 width{};
  u32 height{};
};

struct KtxDecodeResult {
  std::vector<KtxMip> mips; // mips[0] = full res
  VkFormat vk_format{VK_FORMAT_UNDEFINED};
  u32 base_width{};
  u32 base_height{};
};

[[nodiscard]] auto ktx_transcode_format(VkFormat, bool is_normal)
    -> ktx_transcode_fmt_e {
  if (is_normal) {
    return KTX_TTF_BC5_RG;
  }
  return KTX_TTF_BC7_RGBA;
}

[[nodiscard]] auto vk_format_for_transcode(ktx_transcode_fmt_e tf, bool srgb)
    -> VkFormat {
  switch (tf) {
  case KTX_TTF_BC7_RGBA:
    return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
  case KTX_TTF_BC5_RG:
    return VK_FORMAT_BC5_UNORM_BLOCK;
  case KTX_TTF_BC1_RGB:
    return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

constexpr auto is_normal_mode = [](auto texture) -> bool {
  char *value{};
  u32 length{};

  if (KTX_SUCCESS == ktxHashList_FindValue(&texture->kvDataHead,
                                           "KTXwriterScParams", &length,
                                           reinterpret_cast<void **>(&value))) {
    std::string params(value, length);

    if (params.find("--normal-mode") != std::string::npos) {
      return true;
    }
  }
  return false;
};

[[nodiscard]] static auto decode_ktx2_bytes(std::span<const std::byte> bytes,
                                            VkFormat hint, bool srgb)
    -> std::expected<KtxDecodeResult, std::string> {
  ktxTexture2 *ktx = nullptr;
  const KTX_error_code create_err = ktxTexture2_CreateFromMemory(
      reinterpret_cast<const ktx_uint8_t *>(bytes.data()),
      static_cast<ktx_size_t>(bytes.size()),
      KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);

  if (create_err != KTX_SUCCESS)
    return std::unexpected(
        std::format("ktxTexture2_CreateFromMemory failed: {}",
                    std::to_underlying(create_err)));

  struct KtxGuard {
    ktxTexture2 *p;
    ~KtxGuard() { ktxTexture2_Destroy(p); }
  } guard{ktx};

  // Transcode UASTC → BC7 (or whatever target) if supercompressed
  const auto is_normal = is_normal_mode(ktx);
  if (ktxTexture2_NeedsTranscoding(ktx)) {
    const auto tf = ktx_transcode_format(hint, is_normal);
    const KTX_error_code transcode_err = ktxTexture2_TranscodeBasis(ktx, tf, 0);
    if (transcode_err != KTX_SUCCESS)
      return std::unexpected(
          std::format("ktxTexture2_TranscodeBasis failed: {}",
                      std::to_underlying(transcode_err)));
  }

  KtxDecodeResult out;
  out.base_width = ktx->baseWidth;
  out.base_height = ktx->baseHeight;

  // Pick VkFormat — if the KTX2 file already has a native Vulkan format
  // (non-supercompressed path), use that; otherwise derive from transcode.
  if (ktx->vkFormat != VK_FORMAT_UNDEFINED) {
    // Patch sRGB-ness if the caller knows better (e.g. albedo vs linear)
    out.vk_format = static_cast<VkFormat>(ktx->vkFormat);
  } else {
    out.vk_format =
        vk_format_for_transcode(ktx_transcode_format(hint, is_normal), srgb);
  }

  const u32 mip_count = ktx->numLevels;
  out.mips.reserve(mip_count);

  for (u32 level = 0; level < mip_count; ++level) {
    ktx_size_t offset = 0;
    ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset);

    const ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(ktx), level);

    const auto *src = reinterpret_cast<const std::byte *>(ktx->pData + offset);

    KtxMip mip;
    mip.width = std::max(1u, ktx->baseWidth >> level);
    mip.height = std::max(1u, ktx->baseHeight >> level);
    mip.data.assign(src, src + size);

    out.mips.push_back(std::move(mip));
  }

  return out;
}

[[nodiscard]] static auto decode_ktx2_file(const std::filesystem::path &path,
                                           VkFormat hint, bool srgb)
    -> std::expected<KtxDecodeResult, std::string> {
  ktxTexture2 *ktx = nullptr;
  const KTX_error_code err = ktxTexture2_CreateFromNamedFile(
      path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);

  if (err != KTX_SUCCESS)
    return std::unexpected(std::format(
        "ktxTexture2_CreateFromNamedFile failed: {}", std::to_underlying(err)));

  struct KtxGuard {
    ktxTexture2 *p;
    ~KtxGuard() { ktxTexture2_Destroy(p); }
  } guard{ktx};

  const auto is_normal = is_normal_mode(ktx);
  if (ktxTexture2_NeedsTranscoding(ktx)) {
    const auto tf = ktx_transcode_format(hint, is_normal);
    if (ktxTexture2_TranscodeBasis(ktx, tf, 0) != KTX_SUCCESS)
      return std::unexpected("ktxTexture2_TranscodeBasis failed");
  }

  KtxDecodeResult out;
  out.base_width = ktx->baseWidth;
  out.base_height = ktx->baseHeight;
  out.vk_format = ktx->vkFormat != VK_FORMAT_UNDEFINED
                      ? static_cast<VkFormat>(ktx->vkFormat)
                      : vk_format_for_transcode(
                            ktx_transcode_format(hint, is_normal), srgb);

  const u32 mip_count = ktx->numLevels;
  out.mips.reserve(mip_count);

  for (u32 level = 0; level < mip_count; ++level) {
    ktx_size_t offset = 0;
    ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset);
    const ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(ktx), level);
    const auto *src = reinterpret_cast<const std::byte *>(ktx->pData + offset);

    KtxMip mip;
    mip.width = std::max(1u, ktx->baseWidth >> level);
    mip.height = std::max(1u, ktx->baseHeight >> level);
    mip.data.assign(src, src + size);
    out.mips.push_back(std::move(mip));
  }

  return out;
}

// ── Image colour-space classification (unchanged) ────────────────────────────

struct DecodedImage {
  std::vector<std::byte> pixels;
  u32 width{};
  u32 height{};
};
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

  gpu.flags = MaterialFlags::depth_prepass;
  if (mat.alphaMode == fastgltf::AlphaMode::Mask)
    set_flag(gpu.flags, MaterialFlags::alpha_mask);
  if (mat.doubleSided)
    set_flag(gpu.flags, MaterialFlags::two_sided);

  const bool has_occlusion_ref = mat.occlusionTexture.has_value();
  const bool has_mr = pbr.metallicRoughnessTexture.has_value();

  if (has_occlusion_ref && has_mr &&
      mat.occlusionTexture->textureIndex ==
          pbr.metallicRoughnessTexture->textureIndex) {
    set_flag(gpu.flags, MaterialFlags::combined_orm);
  } else if (!has_occlusion_ref && has_mr) {
    set_flag(gpu.flags, MaterialFlags::combined_orm);
  } else if (!has_occlusion_ref && !has_mr) {
    set_flag(gpu.flags, MaterialFlags::no_occlusion);
  }

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

// ── Primitive extraction (unchanged) ─────────────────────────────────────────

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
          out.vertices[i].normal = glm::packSnorm4x8(glm::vec4(n, 0.0F));
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

[[nodiscard]] auto generate_lods(const PrimitiveData &lod0)
    -> std::vector<std::vector<u32>> {

  // Progressively lower target ratios
  static constexpr std::array<f32, 5> k_lod_targets = {0.50F, 0.25F, 0.125F,
                                                       0.0625F, 0.03125F};

  std::vector<std::vector<u32>> result;
  thread_local std::vector<u32> tl_simplified;

  const auto *prev_indices = lod0.indices.data();
  usize prev_count = lod0.indices.size();

  const f32 *position_ptr = &lod0.vertices[0].position[0];
  const usize vertex_stride = sizeof(Vertex);

  for (size_t i = 0; i < k_lod_targets.size(); ++i) {
    const f32 target_ratio = k_lod_targets[i];

    const usize target_count =
        std::max(static_cast<usize>(3),
                 static_cast<usize>(static_cast<f32>(lod0.indices.size()) *
                                    target_ratio));
    const usize rounded = (target_count / 3) * 3;

    tl_simplified.resize(prev_count);
    usize out_count = 0;

    if (i < 3) {
      const f32 current_lod_error = 0.1F + (static_cast<f32>(i) * 0.3F);
      f32 result_error = 0.0F;

      out_count =
          meshopt_simplify(tl_simplified.data(), prev_indices, prev_count,
                           position_ptr, lod0.vertices.size(), vertex_stride,
                           rounded, current_lod_error, 0, &result_error);
    } else {
      const f32 sloppy_error = 0.5F;
      out_count = meshopt_simplifySloppy(
          tl_simplified.data(), prev_indices, prev_count, position_ptr,
          lod0.vertices.size(), vertex_stride, rounded, sloppy_error, nullptr);
    }

    if (out_count == 0 || out_count == prev_count) {
      break;
    }

    result.emplace_back(tl_simplified.begin(),
                        tl_simplified.begin() + out_count);
    prev_indices = result.back().data();
    prev_count = result.back().size();
  }

  return result;
}

[[nodiscard]] auto node_local_matrix(const fastgltf::Node &node) -> glm::mat4 {
  return std::visit(
      fastgltf::visitor{
          [](const fastgltf::TRS &trs) -> glm::mat4 {
            const glm::vec3 t{trs.translation[0], trs.translation[1],
                              trs.translation[2]};
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
    dfs.push_back({.node_idx = idx, .parent_flat_idx = -1, .is_root = true});

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
      const auto &lod_groups = out.meshes[mi];
      const auto &gltf_mesh = asset.meshes[mi];
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
      }
    }

    out.nodes.push_back(std::move(desc));

    for (usize i = node.children.size(); i-- > 0;)
      dfs.push_back({
          .node_idx = node.children[i],
          .parent_flat_idx = flat_idx,
          .is_root = false,
      });
  }
}

} // namespace

namespace mesh {

struct MaterialTexturePatch {
  u32 pool_slot;
  std::function<void(GPUMaterial &, TextureHandle)> apply;
};

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
};

struct PrimLods {
  std::vector<std::vector<u32>> extra;
};

static auto parse_gltf_file(const std::filesystem::path &fs_path,
                            const std::filesystem::path &gltf_dir)
    -> std::expected<fastgltf::Asset, std::string> {
  fastgltf::Parser parser;
  auto data = fastgltf::GltfDataBuffer::FromPath(fs_path);
  auto result = parser.loadGltf(data.get(), gltf_dir,
                                fastgltf::Options::GenerateMeshIndices);
  if (!result)
    return std::unexpected("Parse error");
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

  info("No suitable image source found for image {}", image_idx);
  return std::nullopt;
}

// ── collect_image_sources
// ─────────────────────────────────────────────────────

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

static constexpr std::array<std::byte, 12> k_ktx2_magic = {
    std::byte{0xAB}, std::byte{0x4B}, std::byte{0x54}, std::byte{0x58},
    std::byte{0x20}, std::byte{0x32}, std::byte{0x30}, std::byte{0xBB},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
};

[[nodiscard]] static auto is_ktx2(std::span<const std::byte> bytes) -> bool {
  if (bytes.size() < k_ktx2_magic.size())
    return false;
  return std::equal(k_ktx2_magic.begin(), k_ktx2_magic.end(), bytes.begin());
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

          // ── Path 1: KTX2 sidecar ──────────────────────────────────────────
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

          // ── Path 2: embedded KTX2 (BufferView sources) ───────────────────
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

          // ── Path 3: stbi fallback ─────────────────────────────────────────
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

          // Single mip entry — upload path will generate the rest
          std::vector<pool::MipData> mips;
          mips.push_back({
              .pixels = std::move(pixels),
              .width = static_cast<u32>(w),
              .height = static_cast<u32>(h),
          });

          return pool::CpuTextureData{
              .mips = std::move(mips),
              .name = std::move(src.debug_name),
              .cache_key = std::move(cache_key),
              .width = static_cast<u32>(w),
              .height = static_cast<u32>(h),
              .format = src.format,
              .generate_mips = true,
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

// ── The rest is unchanged from the original
// ───────────────────────────────────

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

static void allocate_materials(const fastgltf::Asset &asset, GeometryPool &pool,
                               MeshAsset &result) {
  if (asset.materials.empty())
    return;
  PROFILE_SCOPE("Allocate materials");

  std::vector<GPUMaterial> gpu_mats;
  gpu_mats.reserve(asset.materials.size());
  for (const auto &mat : asset.materials)
    gpu_mats.push_back(build_gpu_material(mat, asset, result.texture_handles));

  const auto mat_offset = pool.allocate_materials(gpu_mats);
  result.material_base_slot = mat_offset.start_index;
  result.material_count = static_cast<u32>(gpu_mats.size());
  for (usize i = 0; i < gpu_mats.size(); ++i)
    result.material_slots[i] = result.material_base_slot + static_cast<u32>(i);
}

static void
upload_geometry(const fastgltf::Asset &asset,
                const std::vector<PrimWork> &prim_work_list,
                const std::vector<std::expected<PrimitiveResult, std::string>>
                    &extracted_prims,
                const std::vector<PrimLods> &prim_lods, usize total_lod_indices,
                GeometryPool &pool, MeshAsset &result) {
  PROFILE_SCOPE("Allocate geometry TOTAL");

  {
    PROFILE_SCOPE("Pool Reserve & Transaction Init");
    auto [total_v, total_i] = calculate_requirements(extracted_prims);
    pool.reserve(total_v, total_i + total_lod_indices);
  }

  auto batch = pool.begin_transaction();

  result.meshes.resize(asset.meshes.size());
  result.submesh_aabbs.resize(asset.meshes.size());
  result.vertex_base_offset = pool.vertex_offset;

  auto *v_base = cast<Vertex>(pool.vertex_buffer->get_mapped_pointer(),
                              pool.vertex_offset);
  auto *sv_base = cast<PositionOnlyVertex>(
      pool.position_only_vertex_buffer->get_mapped_pointer(),
      pool.shadow_vertex_offset);
  auto *i_base =
      cast<u32>(pool.index_buffer->get_mapped_pointer(), pool.index_offset);

  usize cur_v = 0;
  usize cur_sv = 0;
  usize cur_i = 0;

  {
    PROFILE_SCOPE("Serial Processing Loop");

    for (usize i = 0; i < prim_work_list.size(); ++i) {
      const usize mesh_idx = prim_work_list[i].mesh_idx;
      const auto &res = extracted_prims[i];

      if (!res) {
        result.submesh_aabbs[mesh_idx].push_back(AABB::create());
        continue;
      }

      const auto &[pdata, aabb] = *res;
      const auto vspan = std::span(pdata.vertices);
      const auto ispan = std::span(pdata.indices);

      AllocatedOffset offsets{
          .vertex_offset = pool.vertex_offset + cur_v,
          .shadow_vertex_offset = pool.shadow_vertex_offset + cur_sv,
          .index_offset = pool.index_offset + cur_i,
      };

      auto *v_dst = cast<Vertex>(v_base, cur_v);
      auto *sv_dst = cast<PositionOnlyVertex>(sv_base, cur_sv);
      auto *i_dst = cast<u32>(i_base, cur_i);

      std::memcpy(v_dst, vspan.data(), vspan.size_bytes());
      std::memcpy(i_dst, ispan.data(), ispan.size_bytes());
      for (usize idx = 0; idx < vspan.size(); ++idx) {
        sv_dst[idx].position[0] = vspan[idx].position[0];
        sv_dst[idx].position[1] = vspan[idx].position[1];
        sv_dst[idx].position[2] = vspan[idx].position[2];
      }

      cur_v += vspan.size_bytes();
      cur_sv += vspan.size() * sizeof(PositionOnlyVertex);
      cur_i += ispan.size_bytes();

      MeshLodGroup lod_group;
      lod_group.vertex_offset =
          static_cast<i32>(offsets.vertex_offset / sizeof(Vertex));
      lod_group.lods[0].first_index =
          static_cast<u32>(offsets.index_offset / sizeof(u32));
      lod_group.lods[0].index_count = static_cast<u32>(pdata.indices.size());
      lod_group.lod_count = 1;

      for (const auto &lod_indices : prim_lods[i].extra) {
        const auto as_span = std::span(lod_indices);
        auto *lod_i_dst = cast<u32>(i_base, cur_i);
        std::memcpy(lod_i_dst, lod_indices.data(), as_span.size_bytes());

        auto &lod = lod_group.lods[lod_group.lod_count++];
        lod.first_index =
            static_cast<u32>((pool.index_offset + cur_i) / sizeof(u32));
        lod.index_count = static_cast<u32>(as_span.size());
        cur_i += as_span.size_bytes();
      }

      result.submesh_aabbs[mesh_idx].push_back(aabb);
      result.mesh_aabb.merge(aabb);
      result.meshes[mesh_idx].push_back(lod_group);
    }
  }

  pool.vertex_offset += cur_v;
  pool.shadow_vertex_offset += cur_sv;
  pool.index_offset += cur_i;

  {
    PROFILE_SCOPE("Transaction Commit & VMA Flush");
    batch.commit();
  }
}

static void submit_texture_uploads(std::vector<PendingUpload> &pending,
                                   SceneRenderer &renderer,
                                   MeshAssetHandle handle) {
  for (auto &pu : pending) {
    renderer.texture_upload_pool->submit(
        std::move(pu.fut), std::move(pu.stop_src),
        [&renderer, mesh_handle = handle, image_idx = pu.image_idx,
         patches = std::move(pu.patches)](TextureHandle h) mutable {
          auto *mesh_asset = renderer.resolve_mut(mesh_handle);
          mesh_asset->texture_handles[image_idx] = h;
          for (auto &[slot, apply] : patches) {
            auto &mat = renderer.geometry_pool->get_material(slot);
            apply(mat, h);
            renderer.geometry_pool->update_material(slot, mat);
          }
        });
  }
}

auto build_patch_list(const fastgltf::Asset &asset, usize image_idx,
                      u32 material_base_slot)
    -> std::vector<MaterialTexturePatch> {
  std::vector<MaterialTexturePatch> patches;

  for (usize mi = 0; mi < asset.materials.size(); ++mi) {
    const auto &mat = asset.materials[mi];
    const u32 slot = material_base_slot + static_cast<u32>(mi);

    auto try_add = [&](const auto &tex_opt, auto setter) {
      if (!tex_opt.has_value())
        return;
      const auto &tex = asset.textures[tex_opt->textureIndex];
      if (tex.imageIndex.has_value() && *tex.imageIndex == image_idx)
        patches.push_back({slot, std::move(setter)});
    };

    try_add(mat.pbrData.baseColorTexture, [](GPUMaterial &g, TextureHandle h) {
      g.albedo_index = h.index();
    });
    try_add(mat.normalTexture, [](GPUMaterial &g, TextureHandle h) {
      g.normal_index = h.index();
    });
    try_add(mat.pbrData.metallicRoughnessTexture,
            [](GPUMaterial &g, TextureHandle h) {
              g.metallic_roughness_index = h.index();
            });
    try_add(mat.occlusionTexture, [](GPUMaterial &g, TextureHandle h) {
      g.occlusion_index = h.index();
    });
    try_add(mat.emissiveTexture, [](GPUMaterial &g, TextureHandle h) {
      g.emissive_index = h.index();
    });
  }
  return patches;
}

auto load_from_memory(SceneRenderer &renderer, std::span<const Vertex> vertices,
                      std::span<const u32> indices)
    -> std::expected<MeshAssetHandle, std::string> {
  auto &pool = *renderer.geometry_pool;

  MeshAsset result{.mesh_aabb = AABB::create()};
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

auto load_from_path(const VFSPath &path, SceneRenderer &renderer)
    -> std::expected<MeshAssetHandle, std::string> {
  auto &pool = *renderer.geometry_pool;
  const auto fs_path = VFS::get().resolve(path);
  const auto gltf_dir = fs_path.parent_path();

  if (!std::filesystem::exists(fs_path))
    return std::unexpected("File not found");

  auto asset_result = parse_gltf_file(fs_path, gltf_dir);
  if (!asset_result)
    return std::unexpected(asset_result.error());
  auto &asset = *asset_result;

  auto result =
      std::make_unique<MeshAsset>(MeshAsset{.mesh_aabb = AABB::create()});
  result->texture_handles.resize(asset.images.size(),
                                 renderer.dummy_texture_handle);
  result->material_slots.resize(asset.materials.size(), 0u);

  const auto color_spaces = classify_images(asset);
  auto image_sources =
      collect_image_sources(asset, gltf_dir, fs_path, color_spaces);
  auto pending = launch_texture_futures(image_sources, renderer, *result);

  std::vector<PrimWork> prim_work_list;
  for (auto &&[mi, m] : std::views::enumerate(asset.meshes))
    for (auto &&[pi, p] : std::views::enumerate(m.primitives))
      prim_work_list.push_back({
          .mesh_idx = static_cast<usize>(mi),
          .prim_idx = static_cast<usize>(pi),
          .ptr = &p,
      });

  auto extracted_prims =
      extract_primitives_parallel(asset, prim_work_list, renderer.thread_pool);
  auto [prim_lods, total_lod_indices] = generate_lods_parallel(
      prim_work_list, extracted_prims, renderer.thread_pool);

  allocate_materials(asset, pool, *result);

  for (auto &pu : pending)
    pu.patches =
        build_patch_list(asset, pu.image_idx, result->material_base_slot);

  upload_geometry(asset, prim_work_list, extracted_prims, prim_lods,
                  total_lod_indices, pool, *result);

  using Def = decltype(asset.scenes[0].nodeIndices);
  const auto scene_roots = asset.defaultScene.has_value()
                               ? asset.scenes[*asset.defaultScene].nodeIndices
                               : Def{};
  if (!scene_roots.empty()) {
    PROFILE_SCOPE("Iterate nodes");
    flatten_nodes(asset, scene_roots, *result);
  }

  info("load_gltf: '{}' - {} image(s), {} material(s), {} mesh(es), {} node(s)",
       fs_path.filename().string(), asset.images.size(), asset.materials.size(),
       asset.meshes.size(), result->nodes.size());

  auto handle = renderer.register_gltf(std::move(*result));
  submit_texture_uploads(pending, renderer, handle);
  return handle;
}

} // namespace mesh
} // namespace dy