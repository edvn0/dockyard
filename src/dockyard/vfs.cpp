#include "dockyard/vfs_path.hpp"
#include <dockyard/vfs.hpp>

namespace dy {

void VFS::initialize(const std::filesystem::path& assets_root) {
  std::lock_guard lock(mutex);

  if (!std::filesystem::exists(assets_root) ||
      !std::filesystem::is_directory(assets_root)) {
    throw std::runtime_error(
        std::format("VFS Initialization Failed: '{}' is not a valid directory.",
                    assets_root.string()));
  }

  root = std::filesystem::canonical(assets_root);
  is_initialised = true;

  mounts["res"] = root;
  mounts["shaders"] = root / "shaders";
  mounts["scripts"] = root / "scripts";

  info("[VFS] Initialized with root: {}", root);
  for (auto &&[k, v] : mounts) {
    info("\t[VFS]: Mount {} -> {}", k, v);
  }
}
auto VFS::resolve(std::string_view virtual_path) -> std::filesystem::path {
  ensure_initialised();

  std::string path_str(virtual_path);
  size_t sep = path_str.find("://");

  if (sep == std::string::npos)
    return root / path_str;

  std::string prefix = path_str.substr(0, sep);
  std::string relative = path_str.substr(sep + 3);

  std::lock_guard lock(mutex);
  if (auto it = mounts.find(prefix); it != mounts.end()) {
    return it->second / relative;
  }

  return root / relative;
}

auto VFS::read_binary(std::string_view virtual_path)
    -> std::expected<std::vector<u32>, std::string> {
  std::filesystem::path physical = resolve(virtual_path);

  std::ifstream file(physical, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return std::unexpected(
        std::format("VFS: Could not open file: {}", physical.string()));
  }

  auto size = file.tellg();
  if (size <= 0)
    return std::vector<u32>{};

  size_t vector_size =
      (static_cast<size_t>(size) + sizeof(u32) - 1) / sizeof(u32);
  std::vector<u32> buffer(vector_size);

  file.seekg(0);
  file.read(reinterpret_cast<char *>(buffer.data()), size);

  return buffer;
}

auto VFS::read_binary(const VFSPath &p)
    -> std::expected<std::vector<u32>, std::string> {
  return read_binary(p.view());
}

auto VFS::read_bytes(const VFSPath &p)
    -> std::expected<std::vector<u8>, std::string> {
  const std::filesystem::path physical = resolve(p.view());

  std::ifstream file(physical, std::ios::binary | std::ios::ate);
  if (!file.is_open())
    return std::unexpected(
        std::format("VFS: could not open: {}", physical.string()));

  const auto size = file.tellg();
  if (size <= 0)
    return std::vector<u8>{};

  std::vector<u8> buf(static_cast<usize>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(buf.data()), size);
  return buf;
}

auto VFS::read_binary_async(std::string_view virtual_path)
    -> std::future<std::expected<std::vector<u32>, std::string>> {
  std::packaged_task<std::expected<std::vector<u32>, std::string>()> task(
      [this, path = std::string(virtual_path)] {
        return this->read_binary(path);
      });
  auto fut = task.get_future();
  std::thread(std::move(task)).detach();
  return fut;
}

auto VFS::initialised() const -> bool { return is_initialised; }

void VFS::ensure_initialised() {
#ifdef DOCKYARD_DEBUG
  if (!is_initialised) [[unlikely]] {
    std::abort();
  }
#endif
}
} // namespace dy