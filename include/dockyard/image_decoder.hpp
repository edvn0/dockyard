#pragma once

#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace dy {

struct ImageDecoder {
  struct Result {
    std::vector<std::byte> pixels;
    u32 width{};
    u32 height{};
    u32 channels{};
    u32 bytes_per_channel{};
  };

  static auto from_vfs(const VFSPath &path)
      -> std::expected<Result, std::string>;
  static auto from_path(const VFSPath &path)
      -> std::expected<Result, std::string>;
  static auto from_memory(std::span<const u8> bytes)
      -> std::expected<Result, std::string>;
};

} // namespace dy
