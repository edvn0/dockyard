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
#include <unordered_map>
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
  void initialize(const std::filesystem::path&);

  /**
   * @brief Maps "prefix://path" to an absolute physical path.
   */
  auto resolve(std::string_view virtual_path) -> std::filesystem::path;

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