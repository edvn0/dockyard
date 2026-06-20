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

#include <volk.h>

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
#include <charconv>
#include <execution>
#include <expected>
#include <format>
#include <numeric>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <meshoptimizer.h>

namespace dy {

struct PrimitiveData {
  std::vector<Vertex> vertices;
  std::vector<u32> indices;
  // Empty unless the primitive has JOINTS_0 + WEIGHTS_0; parallel to vertices.
  std::vector<SkinVertex> skin;
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

// Target formats after UASTC transcode.
// BC7  — best quality colour/RGBA (requires BC7 feature, universally supported
//         on desktop Vulkan)
// BC5  — two-channel RG, ideal for normal maps (xy only, reconstruct z)
// BC1  — fallback for very simple single-channel data (not used by default)
//
// Transcode target is chosen based on the intended VkFormat so the caller
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

constexpr u32 fb_albedo = 0u;
constexpr u32 fb_normal = 1u;
constexpr u32 fb_mr = 2u;
constexpr u32 fb_occlusion = 3u;
constexpr u32 fb_emissive = 4u;

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
      resolve_tex(asset, handles, pbr.baseColorTexture, fb_albedo);
  gpu.normal_index =
      resolve_tex(asset, handles, mat.normalTexture, fb_normal);
  gpu.metallic_roughness_index =
      resolve_tex(asset, handles, pbr.metallicRoughnessTexture, fb_mr);
  gpu.emissive_index =
      resolve_tex(asset, handles, mat.emissiveTexture, fb_emissive);
  gpu.occlusion_index =
      resolve_tex(asset, handles, mat.occlusionTexture, fb_occlusion);

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

  // Skinning attributes: only meaningful when both joints and weights exist.
  const auto *joints_attr = prim.findAttribute("JOINTS_0");
  const auto *weights_attr = prim.findAttribute("WEIGHTS_0");
  if (joints_attr != prim.attributes.end() &&
      weights_attr != prim.attributes.end()) {
    std::vector<glm::u16vec4> joints(vtx_count, glm::u16vec4{0});
    std::vector<glm::vec4> weights(vtx_count, glm::vec4{0.0F});

    fastgltf::iterateAccessorWithIndex<glm::u16vec4>(
        asset, asset.accessors[joints_attr->accessorIndex],
        [&](glm::u16vec4 j, usize i) { joints[i] = j; });
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[weights_attr->accessorIndex],
        [&](glm::vec4 w, usize i) { weights[i] = w; });

    out.skin.resize(vtx_count);
    for (usize i = 0; i < vtx_count; ++i)
      out.skin[i] = pack_skin_vertex(
          {joints[i].x, joints[i].y, joints[i].z, joints[i].w}, weights[i]);
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
  static constexpr std::array<f32, 5> lod_targets = {0.50F, 0.25F, 0.125F,
                                                       0.0625F, 0.03125F};

  std::vector<std::vector<u32>> result;
  thread_local std::vector<u32> tl_simplified;

  const auto *prev_indices = lod0.indices.data();
  usize prev_count = lod0.indices.size();

  const f32 *position_ptr = &lod0.vertices[0].position[0];
  const usize vertex_stride = sizeof(Vertex);

  for (size_t i = 0; i < lod_targets.size(); ++i) {
    const f32 target_ratio = lod_targets[i];

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

[[nodiscard]] auto node_bind_trs(const fastgltf::Node &node)
    -> std::tuple<glm::vec3, glm::quat, glm::vec3> {
  return std::visit(
      fastgltf::visitor{
          [](const fastgltf::TRS &trs) {
            return std::tuple{
                glm::vec3{trs.translation[0], trs.translation[1],
                          trs.translation[2]},
                glm::quat{trs.rotation[3], trs.rotation[0], trs.rotation[1],
                          trs.rotation[2]},
                glm::vec3{trs.scale[0], trs.scale[1], trs.scale[2]}};
          },
          [](const fastgltf::math::fmat4x4 &mat) {
            glm::mat4 glm_mat;
            std::memcpy(glm::value_ptr(glm_mat), mat.data(), sizeof(glm::mat4));
            glm::vec3 translation;
            glm::vec3 scale;
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::quat rotation;
            glm::decompose(glm_mat, scale, rotation, translation, skew,
                           perspective);
            return std::tuple{translation, rotation, scale};
          },
      },
      node.transform);
}

[[nodiscard]] auto build_node_parents(const fastgltf::Asset &asset)
    -> std::vector<i32> {
  std::vector<i32> node_parent(asset.nodes.size(), -1);
  for (usize n = 0; n < asset.nodes.size(); ++n)
    for (const auto child : asset.nodes[n].children)
      node_parent[child] = static_cast<i32>(n);
  return node_parent;
}

[[nodiscard]] auto load_skeletons(const fastgltf::Asset &asset)
    -> std::vector<Skeleton> {
  std::vector<Skeleton> skeletons;
  if (asset.skins.empty())
    return skeletons;

  const auto node_parent = build_node_parents(asset);
  skeletons.reserve(asset.skins.size());

  for (const auto &skin : asset.skins) {
    const usize jc = skin.joints.size();

    std::vector<glm::mat4> inverse_binds(jc, glm::mat4{1.0F});
    if (skin.inverseBindMatrices.has_value()) {
      const auto &acc = asset.accessors[*skin.inverseBindMatrices];
      fastgltf::copyFromAccessor<glm::mat4>(asset, acc, inverse_binds.data());
    }

    std::vector<i32> joint_nodes(jc);
    for (usize i = 0; i < jc; ++i)
      joint_nodes[i] = static_cast<i32>(skin.joints[i]);
    const auto parents = resolve_joint_parents(joint_nodes, node_parent);

    Skeleton skel;
    skel.joints.resize(jc);
    for (usize i = 0; i < jc; ++i) {
      const usize node_idx = skin.joints[i];
      const auto &node = asset.nodes[node_idx];
      auto [translation, rotation, scale] = node_bind_trs(node);

      skel.joints[i] = SkeletonJoint{
          .parent = parents[i],
          .node_index = static_cast<i32>(node_idx),
          .inverse_bind = inverse_binds[i],
          .bind_translation = translation,
          .bind_rotation = rotation,
          .bind_scale = scale,
          .name = node.name.empty() ? std::format("joint_{}", i)
                                    : std::string(node.name),
      };
    }
    skeletons.push_back(std::move(skel));
  }

  return skeletons;
}

[[nodiscard]] auto
read_sampler_output(const fastgltf::Asset &asset,
                    const fastgltf::Accessor &acc) -> std::vector<glm::vec4> {
  std::vector<glm::vec4> out(acc.count, glm::vec4{0.0F});
  if (acc.type == fastgltf::AccessorType::Vec4) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, acc, [&](glm::vec4 v, usize i) { out[i] = v; });
  } else { // Vec3 — translation / scale
    fastgltf::iterateAccessorWithIndex<glm::vec3>(
        asset, acc, [&](glm::vec3 v, usize i) { out[i] = glm::vec4{v, 0.0F}; });
  }
  return out;
}

[[nodiscard]] auto to_interpolation(fastgltf::AnimationInterpolation in)
    -> AnimationInterpolation {
  switch (in) {
  case fastgltf::AnimationInterpolation::Step:
    return AnimationInterpolation::Step;
  case fastgltf::AnimationInterpolation::CubicSpline:
    return AnimationInterpolation::CubicSpline;
  case fastgltf::AnimationInterpolation::Linear:
  default:
    return AnimationInterpolation::Linear;
  }
}

// glTF node index -> (skeleton index, joint index within that skeleton).
using JointLookup = std::unordered_map<usize, std::pair<i32, i32>>;

[[nodiscard]] auto build_joint_lookup(std::span<const Skeleton> skeletons)
    -> JointLookup {
  JointLookup lookup;
  for (u32 si = 0; si < skeletons.size(); ++si)
    for (u32 ji = 0; ji < skeletons[si].joints.size(); ++ji)
      lookup.emplace(static_cast<usize>(skeletons[si].joints[ji].node_index),
                     std::pair{static_cast<i32>(si), static_cast<i32>(ji)});
  return lookup;
}

[[nodiscard]] auto load_animations(const fastgltf::Asset &asset,
                                   const JointLookup &joint_lookup)
    -> std::vector<AnimationClip> {
  std::vector<AnimationClip> clips;
  clips.reserve(asset.animations.size());

  for (const auto &anim : asset.animations) {
    AnimationClip clip;
    clip.name = anim.name.empty() ? std::format("clip_{}", clips.size())
                                  : std::string(anim.name);

    clip.samplers.reserve(anim.samplers.size());
    for (const auto &sampler : anim.samplers) {
      const auto &in_acc = asset.accessors[sampler.inputAccessor];
      AnimationSampler out;
      out.interpolation = to_interpolation(sampler.interpolation);
      out.inputs.resize(in_acc.count);
      fastgltf::copyFromAccessor<f32>(asset, in_acc, out.inputs.data());
      out.outputs =
          read_sampler_output(asset, asset.accessors[sampler.outputAccessor]);
      clip.samplers.push_back(std::move(out));
    }

    clip.channels.reserve(anim.channels.size());
    for (const auto &channel : anim.channels) {
      // Morph-target weight animation is out of scope for skeletal skinning.
      if (channel.path == fastgltf::AnimationPath::Weights)
        continue;

      AnimationChannel out;
      out.sampler_index = static_cast<u32>(channel.samplerIndex);
      out.path = channel.path == fastgltf::AnimationPath::Rotation
                     ? AnimationTargetPath::Rotation
                 : channel.path == fastgltf::AnimationPath::Scale
                     ? AnimationTargetPath::Scale
                     : AnimationTargetPath::Translation;

      if (channel.nodeIndex.has_value()) {
        if (const auto it = joint_lookup.find(*channel.nodeIndex);
            it != joint_lookup.end()) {
          out.skeleton_index = it->second.first;
          out.joint_index = it->second.second;
        }
      }
      clip.channels.push_back(out);
    }

    clip.duration = compute_clip_duration(clip.samplers);
    clips.push_back(std::move(clip));
  }

  return clips;
}

void flatten_nodes(
    const fastgltf::Asset &asset, std::span<const std::size_t> root_indices,
    MeshAsset &out, const std::unordered_set<usize> &suppressed_mesh_indices,
    const std::unordered_map<usize, usize> &lod_to_primary_mesh) {
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

    if (node.meshIndex.has_value()) {
      // Remap to primary mesh if this node holds a LOD0 that was merged.
      const usize mi = lod_to_primary_mesh.contains(*node.meshIndex)
                           ? lod_to_primary_mesh.at(*node.meshIndex)
                           : *node.meshIndex;
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

struct ExplicitLodGroup {
  std::string base_name;
  // Sorted by lod_index ascending. Entry [0] is always LOD0 (the primary).
  std::vector<std::pair<u32 /*lod_index*/, usize /*mesh_idx*/>> members;

  [[nodiscard]] usize primary_mesh_idx() const { return members[0].second; }
};

// Parses a mesh name for a trailing _lod{N} suffix.
// Returns (base_name, lod_index) or nullopt.
[[nodiscard]] static auto parse_lod_suffix(std::string_view name)
    -> std::optional<std::pair<std::string, u32>> {
  constexpr std::string_view suffix = "_lod";
  const auto pos = name.rfind(suffix);
  if (pos == std::string_view::npos)
    return std::nullopt;

  const auto level_str = name.substr(pos + suffix.size());
  if (level_str.empty() ||
      !std::ranges::all_of(level_str, [](char c) { return std::isdigit(c); }))
    return std::nullopt;

  u32 level = 0;
  const auto [ptr, ec] = std::from_chars(
      level_str.data(), level_str.data() + level_str.size(), level);
  if (ec != std::errc{})
    return std::nullopt;

  if (level >= static_cast<u32>(max_lods))
    return std::nullopt;

  return std::make_pair(std::string(name.substr(0, pos)), level);
}

// Scans asset.meshes for _lod{N} naming and returns all detected groups.
// An optional name filter restricts detection to a single base name.
[[nodiscard]] static auto detect_lod_groups(
    const fastgltf::Asset &asset,
    const std::optional<std::string> &base_name_filter = std::nullopt)
    -> std::vector<ExplicitLodGroup> {

  std::unordered_map<std::string, ExplicitLodGroup> by_base;

  for (usize mi = 0; mi < asset.meshes.size(); ++mi) {
    auto parsed = parse_lod_suffix(asset.meshes[mi].name);
    if (!parsed)
      continue;
    auto &[base, level] = *parsed;

    if (base_name_filter && base != *base_name_filter)
      continue;

    auto &group = by_base[base];
    if (group.base_name.empty())
      group.base_name = base;
    group.members.emplace_back(level, mi);
  }

  std::vector<ExplicitLodGroup> result;
  result.reserve(by_base.size());

  for (auto &[key, group] : by_base) {
    // Sort so lod0 is always members[0].
    std::ranges::sort(group.members, {}, &std::pair<u32, usize>::first);

    // Only treat as a real LOD chain if LOD0 is present.
    if (group.members[0].first != 0) {
      warn("LOD group '{}' has no LOD0 — skipping", group.base_name);
      continue;
    }
    result.push_back(std::move(group));
  }

  return result;
}

// (LOD0's vertices) all higher LODs need their indices remapped to point into
// LOD0's vertex range.
//
// Strategy:
//   1. Build a position→vertex_index lookup from LOD0's vertices.
//   2. For every vertex in LODn, find the closest LOD0 vertex by position.
//   3. Rewrite LODn's index buffer so every index points into LOD0.
//
// Position matching is exact (bit-identical float comparison). If an author
// has altered vertex positions between LODs this will fall back to a
// nearest-neighbour search with a configurable tolerance. In practice,
// mesh simplification tools (Blender Decimate, meshopt) preserve the original
// vertex positions for remaining verts, so exact matching covers all real
// cases.
//
// Returns a remapped copy of lod_n_data.indices, or the original indices with
// a warning if remapping fails for any vertex.

struct RemapResult {
  std::vector<u32> indices;
  bool exact{true}; // false if any vertex needed nearest-neighbour fallback
};

[[nodiscard]] static auto remap_lod_indices_to_lod0(const PrimitiveData &lod0,
                                                    const PrimitiveData &lod_n)
    -> RemapResult {

  // Build exact position map: packed u32 key → lod0 vertex index.
  // We use the raw float bits, good enough for positions authored in the
  // same DCC tool.
  struct Vec3Key {
    u32 x, y, z;
    bool operator==(const Vec3Key &) const = default;
  };
  struct Vec3Hash {
    usize operator()(const Vec3Key &k) const noexcept {
      // FNV-1a variant over three u32s
      usize h = 14695981039346656037ULL;
      for (u32 v : {k.x, k.y, k.z}) {
        h ^= static_cast<usize>(v);
        h *= 1099511628211ULL;
      }
      return h;
    }
  };

  std::unordered_map<Vec3Key, u32, Vec3Hash> pos_to_lod0;
  pos_to_lod0.reserve(lod0.vertices.size());

  for (u32 vi = 0; vi < static_cast<u32>(lod0.vertices.size()); ++vi) {
    const auto &p = lod0.vertices[vi].position;
    Vec3Key key{
        std::bit_cast<u32>(p[0]),
        std::bit_cast<u32>(p[1]),
        std::bit_cast<u32>(p[2]),
    };
    // First occurrence wins (multiple LOD0 verts at same position are fine;
    // any of them is a valid remap target).
    pos_to_lod0.try_emplace(key, vi);
  }

  RemapResult out;
  out.indices.reserve(lod_n.indices.size());
  out.exact = true;

  // We only need the unique set of vertices referenced by lod_n's indices.
  // Build a local remap table so we don't redo the lookup per-index.
  std::vector<u32> lod_n_to_lod0(lod_n.vertices.size(),
                                 std::numeric_limits<u32>::max());

  for (u32 vi = 0; vi < static_cast<u32>(lod_n.vertices.size()); ++vi) {
    const auto &p = lod_n.vertices[vi].position;
    Vec3Key key{
        std::bit_cast<u32>(p[0]),
        std::bit_cast<u32>(p[1]),
        std::bit_cast<u32>(p[2]),
    };

    if (const auto it = pos_to_lod0.find(key); it != pos_to_lod0.end()) {
      lod_n_to_lod0[vi] = it->second;
    } else {
      // Nearest-neighbour fallback. This is O(N) per unmatched vertex but
      // should only fire if the author slightly perturbed positions between
      // LOD levels (rare; log a warning).
      out.exact = false;
      f32 best_dist_sq = std::numeric_limits<f32>::max();
      u32 best_idx = 0;
      for (u32 li = 0; li < static_cast<u32>(lod0.vertices.size()); ++li) {
        const auto &lp = lod0.vertices[li].position;
        const f32 dx = p[0] - lp[0];
        const f32 dy = p[1] - lp[1];
        const f32 dz = p[2] - lp[2];
        const f32 dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq < best_dist_sq) {
          best_dist_sq = dist_sq;
          best_idx = li;
        }
      }
      lod_n_to_lod0[vi] = best_idx;
    }
  }

  for (const u32 idx : lod_n.indices)
    out.indices.push_back(lod_n_to_lod0[idx]);

  return out;
}

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

static constexpr std::array<std::byte, 12> ktx2_magic = {
    std::byte{0xAB}, std::byte{0x4B}, std::byte{0x54}, std::byte{0x58},
    std::byte{0x20}, std::byte{0x32}, std::byte{0x30}, std::byte{0xBB},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
};

[[nodiscard]] static auto is_ktx2(std::span<const std::byte> bytes) -> bool {
  if (bytes.size() < ktx2_magic.size())
    return false;
  return std::equal(ktx2_magic.begin(), ktx2_magic.end(), bytes.begin());
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

    // Only count vertices for LOD0 (or non-LOD) entries.  Higher LOD meshes
    // share LOD0's vertex range — their vertices are never uploaded.
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
    pool.reserve(total_v, total_i + total_lod_indices);
    if (total_skin > 0)
      pool.ensure_skin_capacity(total_skin);
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
  // Skin vertices are packed densely (only skinned prims contribute), starting
  // at the pool's current skin offset. skin_base stays valid for the whole loop
  // because ensure_skin_capacity already reserved the full range above.
  const usize skin_base = pool.skin_vertex_offset;
  usize cur_skin = 0;

  {
    PROFILE_SCOPE("Serial Processing Loop");

    for (usize i = 0; i < prim_work_list.size(); ++i) {
      const auto &w = prim_work_list[i];
      const auto &res = extracted_prims[i];

      // For LOD meshes with slot > 0 we only need to write their (remapped)
      // index data into the pool — their vertices live in LOD0's range.
      const bool uploads_vertices = !w.is_explicit_lod || w.lod_slot == 0;

      // Determine which mesh owns the MeshLodGroup for this primitive.
      // For LOD0 / non-LOD prims it's w.mesh_idx itself.
      // For LODn (n>0) it's w.primary_mesh_idx.
      const usize owning_mesh_idx = (w.is_explicit_lod && w.lod_slot > 0)
                                        ? w.primary_mesh_idx
                                        : w.mesh_idx;

      if (!res) {
        if (uploads_vertices)
          result.submesh_aabbs[owning_mesh_idx].push_back(AABB::create());
        continue;
      }

      const auto &[pdata, aabb] = *res;
      const auto ispan = std::span(pdata.indices);

      // ----------------------------------------------------------------
      // Vertex upload (skipped for LODn > 0)
      // ----------------------------------------------------------------
      AllocatedOffset offsets{};
      if (uploads_vertices) {
        const auto vspan = std::span(pdata.vertices);

        offsets = AllocatedOffset{
            .vertex_offset = pool.vertex_offset + cur_v,
            .shadow_vertex_offset = pool.shadow_vertex_offset + cur_sv,
            .index_offset = pool.index_offset + cur_i,
        };

        auto *v_dst = cast<Vertex>(v_base, cur_v);
        auto *sv_dst = cast<PositionOnlyVertex>(sv_base, cur_sv);

        std::memcpy(v_dst, vspan.data(), vspan.size_bytes());
        for (usize idx = 0; idx < vspan.size(); ++idx) {
          sv_dst[idx].position[0] = vspan[idx].position[0];
          sv_dst[idx].position[1] = vspan[idx].position[1];
          sv_dst[idx].position[2] = vspan[idx].position[2];
        }

        cur_v += vspan.size_bytes();
        cur_sv += vspan.size() * sizeof(PositionOnlyVertex);
      } else {
        // Reuse LOD0's vertex offset. Find the owning (LOD0) entry.
        // We scan backwards since LOD0 must have been processed earlier.
        for (usize j = i; j-- > 0;) {
          if (prim_work_list[j].mesh_idx == w.primary_mesh_idx &&
              prim_work_list[j].lod_slot == 0 && extracted_prims[j]) {
            offsets.vertex_offset =
                pool.vertex_offset +
                /* byte offset already advanced when lod0 was written */
                /* we stored it in the MeshLodGroup — recover from there */
                static_cast<usize>(result.meshes[w.primary_mesh_idx][w.prim_idx]
                                       .vertex_offset) *
                    sizeof(Vertex);
            break;
          }
        }
        // index_offset advances normally
        offsets.index_offset = pool.index_offset + cur_i;
      }

      // ----------------------------------------------------------------
      // Index upload (always)
      // ----------------------------------------------------------------
      auto *i_dst = cast<u32>(i_base, cur_i);
      std::memcpy(i_dst, ispan.data(), ispan.size_bytes());
      cur_i += ispan.size_bytes();

      // ----------------------------------------------------------------
      // MeshLodGroup bookkeeping
      // ----------------------------------------------------------------
      if (uploads_vertices) {
        // LOD0 / standalone: create the group entry.
        MeshLodGroup lod_group;
        lod_group.vertex_offset =
            static_cast<i32>(offsets.vertex_offset / sizeof(Vertex));
        lod_group.lods[0].first_index =
            static_cast<u32>(offsets.index_offset / sizeof(u32));
        lod_group.lods[0].index_count = static_cast<u32>(pdata.indices.size());
        lod_group.vertex_count = static_cast<u32>(pdata.vertices.size());
        lod_group.lod_count = 1;

        // Skin vertices (when present) are written densely into the skin buffer
        // parallel to this LOD0's vertices; higher LODs reuse this offset since
        // they share LOD0's vertex range.
        if (!pdata.skin.empty()) {
          const usize skin_idx = skin_base + cur_skin;
          std::memcpy(pool.skin_mapped_pointer(skin_idx), pdata.skin.data(),
                      pdata.skin.size() * sizeof(SkinVertex));
          lod_group.skin_vertex_offset = static_cast<i32>(skin_idx);
          cur_skin += pdata.skin.size();
        }

        // Append meshopt-generated LODs (only for non-explicit-lod prims).
        if (!w.is_explicit_lod) {
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
        }

        result.meshes[owning_mesh_idx].push_back(lod_group);
        result.submesh_aabbs[owning_mesh_idx].push_back(aabb);
        result.mesh_aabb.merge(aabb);
      } else {
        // LODn > 0: patch the lod_slot into the existing MeshLodGroup.
        auto &lod_group = result.meshes[owning_mesh_idx][w.prim_idx];
        if (w.lod_slot < static_cast<u32>(max_lods)) {
          auto &lod_entry = lod_group.lods[w.lod_slot];
          lod_entry.first_index =
              static_cast<u32>(offsets.index_offset / sizeof(u32));
          lod_entry.index_count = static_cast<u32>(pdata.indices.size());
          lod_group.lod_count =
              static_cast<u8>(glm::max(static_cast<u32>(lod_group.lod_count),
                                       static_cast<u32>(w.lod_slot + 1)));
        }
      }
    }
  }

  pool.vertex_offset += cur_v;
  pool.shadow_vertex_offset += cur_sv;
  pool.index_offset += cur_i;

  if (cur_skin > 0) {
    pool.flush_skin_range(skin_base, cur_skin);
    pool.skin_vertex_offset += cur_skin;
  }

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
                      std::span<const u32> indices,
                      NullableVFSPath source_path)
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

auto load_from_path(const VFSPath &path, SceneRenderer &renderer,
                    const LoadOptions &opts)
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
  result->source_path = NullableVFSPath{path};
  result->texture_handles.resize(asset.images.size(),
                                 renderer.dummy_texture_handle);
  result->material_slots.resize(asset.materials.size(), 0u);

  const auto color_spaces = classify_images(asset);
  auto image_sources =
      collect_image_sources(asset, gltf_dir, fs_path, color_spaces);
  auto pending = launch_texture_futures(image_sources, renderer, *result);

  // -------------------------------------------------------------------------
  // Detect explicit LOD groups from mesh names (e.g. _lod0 … _lod5)
  // -------------------------------------------------------------------------
  std::vector<ExplicitLodGroup> lod_groups;
  if (opts.lod_detection != LodDetection::none) {
    lod_groups = detect_lod_groups(asset, opts.lod_base_name_filter);
    if (!lod_groups.empty()) {
      info("load_gltf: '{}' — detected {} explicit LOD group(s):",
           fs_path.filename().string(), lod_groups.size());
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
  // primitives for the same group, because upload_geometry processes them
  // serially and LODn patching reads from the already-written MeshLodGroup.
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
  // Parallel extract → remap → LOD generation → upload
  // -------------------------------------------------------------------------
  auto extracted_prims =
      extract_primitives_parallel(asset, prim_work_list, renderer.thread_pool);

  // Remap LODn indices to point into LOD0's vertex range.
  if (!lod_groups.empty())
    remap_explicit_lod_indices(prim_work_list, extracted_prims,
                               renderer.thread_pool);

  auto [prim_lods, total_lod_indices] = generate_lods_parallel(
      prim_work_list, extracted_prims, renderer.thread_pool);

  allocate_materials(asset, pool, *result);

  for (auto &pu : pending)
    pu.patches =
        build_patch_list(asset, pu.image_idx, result->material_base_slot);

  upload_geometry(asset, prim_work_list, extracted_prims, prim_lods,
                  total_lod_indices, pool, *result);

  {
    PROFILE_SCOPE("Load skeletons & animations");
    result->skeletons = load_skeletons(asset);
    const auto joint_lookup = build_joint_lookup(result->skeletons);
    result->animations = load_animations(asset, joint_lookup);
  }

  using Def = decltype(asset.scenes[0].nodeIndices);
  const auto scene_roots = asset.defaultScene.has_value()
                               ? asset.scenes[*asset.defaultScene].nodeIndices
                               : Def{};
  if (!scene_roots.empty()) {
    PROFILE_SCOPE("Iterate nodes");
    flatten_nodes(asset, scene_roots, *result, suppressed_mesh_indices,
                  lod_to_primary_mesh);
  }

  // Count effective LOD levels for the log message.
  u32 max_lod_count = 0;
  for (const auto &mesh_lod_groups : result->meshes)
    for (const auto &lg : mesh_lod_groups)
      max_lod_count = glm::max(max_lod_count, static_cast<u32>(lg.lod_count));

  info("load_gltf: '{}' - {} image(s), {} material(s), {} mesh(es), {} "
       "node(s), {} LOD level(s)",
       fs_path.filename().string(), asset.images.size(), asset.materials.size(),
       asset.meshes.size(), result->nodes.size(), max_lod_count);

  auto handle = renderer.register_gltf(std::move(*result));
  submit_texture_uploads(pending, renderer, handle);
  return handle;
}

} // namespace mesh
} // namespace dy
