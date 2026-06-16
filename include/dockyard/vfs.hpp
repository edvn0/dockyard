#pragma once

#include <dockyard/log.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dy {

class VFS {

public:
  static auto get() -> VFS & {
    static VFS instance;
    return instance;
  }

  VFS(const VFS &) = delete;
  VFS &operator=(const VFS &) = delete;
  VFS(VFS &&) = delete;
  VFS &operator=(VFS &&) = delete;

  /**
   * @brief Required Initialization.
   * @param assets_root The physical path to the primary assets folder.
   */
  void initialize(const std::filesystem::path &);

  /**
   * @brief Maps "prefix://path" to an absolute physical path.
   */
  auto resolve(std::string_view virtual_path) -> std::filesystem::path;
  /**
   * @brief Maps "prefix://path" to an absolute physical path.
   */
  auto resolve(const VFSPath &virtual_path) -> std::filesystem::path;

  auto resolve_to_output_stream(const VFSPath &)
      -> std::expected<std::ofstream, std::string>;
  auto resolve_to_output_stream(std::string_view)
      -> std::expected<std::ofstream, std::string>;
  auto resolve_to_input_stream(const VFSPath &)
      -> std::expected<std::ifstream, std::string>;
  auto resolve_to_input_stream(std::string_view)
      -> std::expected<std::ifstream, std::string>;

  /**
   * @brief Synchronous binary read.
   */
  auto read_binary(std::string_view virtual_path)
      -> std::expected<std::vector<u32>, std::string>;

  auto read_binary(const VFSPath &)
      -> std::expected<std::vector<u32>, std::string>;

  auto read_bytes(const VFSPath &)
      -> std::expected<std::vector<u8>, std::string>;

  /**
   * @brief Asynchronous binary read.
   */
  auto read_binary_async(std::string_view virtual_path)
      -> std::future<std::expected<std::vector<u32>, std::string>>;

  /**
   * @brief Adds or replaces a mount point.
   * Example: mount("external_hdr", "C:/assets/hdr") allows
   * "external_hdr://studio.hdr".
   */
  auto mount(std::string_view scheme,
             const std::filesystem::path &physical_root) -> void;

  /**
   * @brief Mounts the parent directory of a file and returns a VFS path to the
   * file. Example: C:/foo/studio.hdr -> external_hdr://studio.hdr
   */
  auto mount_file(std::string_view scheme, const std::filesystem::path &file)
      -> VFSPath;

  struct Filter {
    std::unordered_set<std::string_view> ignored_dirs;
    std::unordered_set<std::string_view> included_extensions;
    u32 depth = static_cast<u32>(~0); // No depth
  };
  auto list(std::string_view virtual_path,
            const Filter &filter = {
                .depth = static_cast<u32>(~0),
            }) -> std::vector<std::filesystem::path>;

  auto last_write_time(const VFSPath &)
      -> std::expected<std::filesystem::file_time_type, std::error_code>;

  auto initialised() const -> bool;

private:
  VFS() = default;

  void ensure_initialised();

  std::filesystem::path root{};
  StringMap<std::filesystem::path> mounts{};
  std::mutex mutex{};
  bool is_initialised = false;
};
} // namespace dy
