#include "dockyard/vfs_path.hpp"
#include <dockyard/vfs.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace dy {

namespace {

auto valid_vfs_scheme(std::string_view scheme) -> bool {
  if (scheme.empty())
    return false;

  return std::ranges::all_of(scheme, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  });
}

auto generic_vfs_filename(const std::filesystem::path &path) -> std::string {
  return path.filename().generic_string();
}

} // namespace

void VFS::initialize(const std::filesystem::path &assets_root) {
  std::scoped_lock lock(mutex);

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
  mounts["fonts"] = root / "fonts";
  mounts["editor"] = root / "editor";
  mounts["textures"] = root / "textures";
  mounts["meshes"] = root / "meshes";
  mounts["binary"] = root / "binary";

  info("[VFS] Initialized with root: {}", root);
  for (auto &&[k, v] : mounts) {
    info("\t[VFS]: Mount {} -> {}", k, v);
  }
}

auto VFS::resolve_to_output_stream(const VFSPath &path)
    -> std::expected<std::ofstream, std::string> {
  return resolve_to_output_stream(path.view());
}
auto VFS::resolve_to_input_stream(const VFSPath &path)
    -> std::expected<std::ifstream, std::string> {
  return resolve_to_input_stream(path.view());
}
auto VFS::resolve_to_output_stream(std::string_view p)
    -> std::expected<std::ofstream, std::string> {
  auto real_path = resolve(p);
  std::ofstream f{real_path.string()};
  if (!f.is_open())
    return std::unexpected(
        std::format("Failed to open '{}' for writing", real_path.string()));
  return f;
}

auto VFS::resolve_to_input_stream(std::string_view p)
    -> std::expected<std::ifstream, std::string> {
  auto real_path = resolve(p);
  std::ifstream f{real_path.string()};
  if (!f.is_open())
    return std::unexpected(
        std::format("Failed to open '{}' for reading", real_path.string()));
  return f;
}

auto VFS::last_write_time(const VFSPath &path)
    -> std::expected<std::filesystem::file_time_type, std::error_code> {
  std::error_code ec{};
  auto const time{std::filesystem::last_write_time(resolve(path), ec)};

  if (ec) {
    return std::unexpected{ec};
  }

  return time;
}

auto VFS::resolve(const VFSPath &virtual_path) -> std::filesystem::path {
  return resolve(virtual_path.view());
}

auto VFS::resolve(std::string_view virtual_path) -> std::filesystem::path {
  ensure_initialised();

  std::string path_str(virtual_path);
  size_t sep = path_str.find("://");

  if (sep == std::string::npos)
    return root / path_str;

  std::string prefix = path_str.substr(0, sep);
  std::string relative = path_str.substr(sep + 3);

  std::scoped_lock lock(mutex);
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

auto VFS::list(std::string_view virtual_path, const Filter &filter)
    -> std::vector<std::filesystem::path> {
  const std::filesystem::path physical = resolve(virtual_path);
  std::vector<std::filesystem::path> paths;

  if (!std::filesystem::exists(physical) ||
      !std::filesystem::is_directory(physical)) {
    warn("VFS: path does not exist or is not a directory: {}",
         physical.string());
    return paths;
  }

  for (auto it = std::filesystem::recursive_directory_iterator(physical);
       it != std::filesystem::recursive_directory_iterator{}; ++it) {
    if (it.depth() > static_cast<int>(filter.depth)) {
      it.disable_recursion_pending();
      continue;
    }

    if (!it->is_regular_file())
      continue;

    const auto rel = std::filesystem::relative(it->path(), physical);

    if (filter.ignored_dirs.contains(
            it->path().parent_path().filename().string()))
      continue;

    if (!filter.included_extensions.empty() &&
        !filter.included_extensions.contains(it->path().extension().string()))
      continue;

    paths.push_back(rel);
  }

  return paths;
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

VFS::ScopedMount::ScopedMount(std::string s) : scheme(std::move(s)) {}

VFS::ScopedMount::ScopedMount(ScopedMount &&o) noexcept
    : scheme(std::move(o.scheme)) {}

auto VFS::ScopedMount::operator=(ScopedMount &&o) noexcept -> ScopedMount & {
  if (this != &o) {
    if (!scheme.empty())
      VFS::get().unmount(scheme);
    scheme = std::move(o.scheme);
  }
  return *this;
}

VFS::ScopedMount::~ScopedMount() {
  if (!scheme.empty())
    VFS::get().unmount(scheme);
}

auto VFS::unmount(std::string_view scheme) -> void {
  std::scoped_lock lock(mutex);
  mounts.erase(std::string{scheme});
}

auto VFS::mount_scoped(std::string_view scheme,
                       const std::filesystem::path &physical_root)
    -> ScopedMount {
  mount(scheme, physical_root);
  return ScopedMount{std::string{scheme}};
}

auto VFS::mount_file_scoped(std::string_view scheme,
                             const std::filesystem::path &file)
    -> std::pair<ScopedMount, VFSPath> {
  auto path = mount_file(scheme, file);
  return {ScopedMount{std::string{scheme}}, path};
}

auto VFS::mount(std::string_view scheme,
                const std::filesystem::path &physical_root) -> void {
  ensure_initialised();

  if (!valid_vfs_scheme(scheme)) {
    error("VFS: invalid mount scheme: {}", scheme);
    std::abort();
  }

  if (!std::filesystem::exists(physical_root) ||
      !std::filesystem::is_directory(physical_root)) {
    error("VFS: mount root is not a directory: {}", physical_root.string());
    std::abort();
  }

  const auto canonical_root = std::filesystem::canonical(physical_root);

  std::scoped_lock lock(mutex);
  mounts[std::string{scheme}] = canonical_root;

  info("[VFS] Mount {} -> {}", scheme, canonical_root.string());
}

auto VFS::mount_file(std::string_view scheme, const std::filesystem::path &file)
    -> VFSPath {
  ensure_initialised();

  if (!std::filesystem::exists(file) ||
      !std::filesystem::is_regular_file(file)) {
    error("VFS: mount_file target is not a file: {}", file.string());
    std::abort();
  }

  const auto canonical_file = std::filesystem::canonical(file);
  const auto parent = canonical_file.parent_path();
  const auto filename = generic_vfs_filename(canonical_file);

  mount(scheme, parent);

  return VFSPath::create("{}://{}", scheme, filename);
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
