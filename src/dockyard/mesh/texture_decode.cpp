#include <dockyard/mesh/texture_decode.hpp>

#include <ktxvulkan.h>

#include <algorithm>
#include <array>
#include <format>

namespace dy::mesh::detail {

namespace {

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

struct KtxGuard {
  ktxTexture2 *p;
  ~KtxGuard() { ktxTexture2_Destroy(p); }
};

constexpr std::array<std::byte, 12> ktx2_magic = {
    std::byte{0xAB}, std::byte{0x4B}, std::byte{0x54}, std::byte{0x58},
    std::byte{0x20}, std::byte{0x32}, std::byte{0x30}, std::byte{0xBB},
    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
};

} // namespace

auto ktx_transcode_format(VkFormat, bool is_normal) -> ktx_transcode_fmt_e {
  if (is_normal) {
    return KTX_TTF_BC5_RG;
  }
  return KTX_TTF_BC7_RGBA;
}

auto vk_format_for_transcode(ktx_transcode_fmt_e tf, bool srgb) -> VkFormat {
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

auto is_ktx2(std::span<const std::byte> bytes) -> bool {
  if (bytes.size() < ktx2_magic.size())
    return false;
  return std::equal(ktx2_magic.begin(), ktx2_magic.end(), bytes.begin());
}

auto decode_ktx2_bytes(std::span<const std::byte> bytes, VkFormat hint,
                       bool srgb) -> std::expected<KtxDecodeResult, std::string> {
  ktxTexture2 *ktx = nullptr;
  const KTX_error_code create_err = ktxTexture2_CreateFromMemory(
      reinterpret_cast<const ktx_uint8_t *>(bytes.data()),
      static_cast<ktx_size_t>(bytes.size()),
      KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);

  if (create_err != KTX_SUCCESS)
    return std::unexpected(
        std::format("ktxTexture2_CreateFromMemory failed: {}",
                    std::to_underlying(create_err)));

  KtxGuard guard{ktx};

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

auto decode_ktx2_file(const std::filesystem::path &path, VkFormat hint,
                      bool srgb) -> std::expected<KtxDecodeResult, std::string> {
  ktxTexture2 *ktx = nullptr;
  const KTX_error_code err = ktxTexture2_CreateFromNamedFile(
      path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);

  if (err != KTX_SUCCESS)
    return std::unexpected(std::format(
        "ktxTexture2_CreateFromNamedFile failed: {}", std::to_underlying(err)));

  KtxGuard guard{ktx};

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

} // namespace dy::mesh::detail
