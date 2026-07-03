#pragma once

#include <dockyard/types.hpp>

#include <volk.h>

#include <ktx.h>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace dy::mesh::detail {

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

// Target formats after UASTC transcode.
// BC7  — best quality colour/RGBA (requires BC7 feature, universally supported
//         on desktop Vulkan)
// BC5  — two-channel RG, ideal for normal maps (xy only, reconstruct z)
// BC1  — fallback for very simple single-channel data (not used by default)
//
// Transcode target is chosen based on the intended VkFormat so the caller
// can stay format-agnostic.
[[nodiscard]] auto ktx_transcode_format(VkFormat hint, bool is_normal)
    -> ktx_transcode_fmt_e;

[[nodiscard]] auto vk_format_for_transcode(ktx_transcode_fmt_e tf, bool srgb)
    -> VkFormat;

[[nodiscard]] auto is_ktx2(std::span<const std::byte> bytes) -> bool;

[[nodiscard]] auto decode_ktx2_bytes(std::span<const std::byte> bytes,
                                     VkFormat hint, bool srgb)
    -> std::expected<KtxDecodeResult, std::string>;

[[nodiscard]] auto decode_ktx2_file(const std::filesystem::path &path,
                                    VkFormat hint, bool srgb)
    -> std::expected<KtxDecodeResult, std::string>;

} // namespace dy::mesh::detail
