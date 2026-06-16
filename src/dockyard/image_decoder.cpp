#include <dockyard/image_decoder.hpp>
#include <dockyard/vfs.hpp>

#include <cstring>
#include <format>

#include <stb_image.h>

namespace dy {

namespace {

auto result_from_raw(stbi_uc *raw, int w, int h) -> ImageDecoder::Result {
  const usize byte_count = static_cast<usize>(w * h * 4);
  std::vector<std::byte> pixels(byte_count);
  std::memcpy(pixels.data(), raw, byte_count);
  stbi_image_free(raw);

  return {
      .pixels            = std::move(pixels),
      .width             = static_cast<u32>(w),
      .height            = static_cast<u32>(h),
      .channels          = 4,
      .bytes_per_channel = 1,
  };
}

} // namespace

auto ImageDecoder::from_memory(std::span<const u8> bytes)
    -> std::expected<Result, std::string> {
  int w{}, h{}, ch{};
  stbi_uc *raw = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                        &w, &h, &ch, STBI_rgb_alpha);
  if (!raw)
    return std::unexpected(std::format("stbi: {}", stbi_failure_reason()));
  return result_from_raw(raw, w, h);
}

auto ImageDecoder::from_path(const VFSPath &path)
    -> std::expected<Result, std::string> {
  const auto physical = VFS::get().resolve(path).string();
  int w{}, h{}, ch{};
  stbi_uc *raw = stbi_load(physical.c_str(), &w, &h, &ch, STBI_rgb_alpha);
  if (!raw)
    return std::unexpected(std::format("stbi '{}': {}", physical, stbi_failure_reason()));
  return result_from_raw(raw, w, h);
}

auto ImageDecoder::from_vfs(const VFSPath &path)
    -> std::expected<Result, std::string> {
  auto bytes = VFS::get().read_bytes(path);
  if (!bytes)
    return std::unexpected(bytes.error());
  return from_memory(*bytes);
}

} // namespace dy
