#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dockyard/log.hpp>
#include <dockyard/types.hpp>
#include <string>
#include <string_view>

namespace dy {

class VFSPath {
public:
  static auto create(std::string_view raw) -> VFSPath {
    const auto sep = raw.find("://");
    if (sep == std::string_view::npos || sep == 0) {
      error("Invalid VFS path: {}", raw);
      std::abort();
    }

    const auto scheme = raw.substr(0, sep);
    const bool valid = std::ranges::all_of(scheme, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });

    if (!valid) {
      error("Invalid VFS scheme/root: {} ({})", scheme, raw);
      std::abort();
    }

    const auto rest = raw.substr(sep + 3);
    if (rest.empty()) {
      error("Only specified a scheme, no path: {} ({})", rest, raw);
      std::abort();
    }

    if (sep > 255) {
      error("Scheme too long (max 255 chars): {} ({})", scheme, raw);
      std::abort();
    }

    return VFSPath{raw, static_cast<u8>(sep)};
  }

  template <typename... Args>
  static auto create(std::format_string<Args...> fmt, Args &&...args) {
    return create(std::format(fmt, std::forward<Args>(args)...));
  }

  [[nodiscard]] auto scheme() const -> std::string_view {
    return std::string_view{path}.substr(0, sep);
  }

  [[nodiscard]] auto relative_path() const -> std::string_view {
    return std::string_view{path}.substr(sep + 3);
  }

  [[nodiscard]] auto view() const -> std::string_view { return path; }

  [[nodiscard]] auto extension() const -> std::string_view {
    const auto rel = relative_path();
    const auto dot = rel.rfind('.');
    if (dot == std::string_view::npos)
      return {};
    return rel.substr(dot);
  }

  [[nodiscard]] auto stem() const -> std::string_view {
    const auto rel = relative_path();
    const auto slash = rel.rfind('/');
    const auto filename =
        (slash == std::string_view::npos) ? rel : rel.substr(slash + 1);
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos)
      return filename;
    return filename.substr(0, dot);
  }

  [[nodiscard]] auto with_extension(std::string_view new_ext) const -> VFSPath {
    const auto rel = relative_path();
    const auto dot = rel.rfind('.');
    const auto base =
        (dot == std::string_view::npos) ? rel : rel.substr(0, dot);
    return VFSPath::create("{}://{}{}", scheme(), base, new_ext);
  }

private:
  explicit VFSPath(std::string_view d, u8 s) : path(d), sep(s) {}

  std::string path;
  u8 sep; // index of ':'
};

struct NullableVFSPath {
  std::optional<VFSPath> path;

  template <typename... Args>
  static auto create(std::format_string<Args...> fmt, Args &&...args) {
    return NullableVFSPath{
        VFSPath::create(std::format(fmt, std::forward<Args>(args)...)),
    };
  }

  auto operator=(const VFSPath& other) -> auto& {
      path = other;
      return *this;
  }

  [[nodiscard]] auto valid() const -> bool { return path.has_value(); }
  [[nodiscard]] auto view() const -> std::string_view {
    return path ? path->view() : std::string_view{};
  }
  [[nodiscard]] auto value() const { return *path; }
};

} // namespace dy
