#pragma once

#include "dockyard/log.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
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

    return VFSPath{raw};
  }

  template <typename... Args>
  static auto create(std::format_string<Args...> fmt, Args &&...args) {
    return create(std::format(fmt, std::forward<Args>(args)...));
  }

  auto scheme() const -> std::string_view {
    return std::string_view{path}.substr(0, path.find("://"));
  }

  auto relative_path() const -> std::string_view {
    const auto pos = path.find("://");
    return std::string_view{path}.substr(pos + 3);
  }

  auto view() const -> std::string_view { return path; }

private:
  explicit VFSPath(std::string_view d) : path(d) {}
  std::string path;
};

} // namespace dy