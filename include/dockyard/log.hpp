#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

namespace dy {

enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };

namespace detail {
void private_log_message(LogLevel, std::string_view);
}

bool is_level_enabled(LogLevel);

template <typename... Args>
auto info(std::format_string<Args...> fmt, Args &&...args) {
  detail::private_log_message(LogLevel::Info,
                              std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
auto warn(std::format_string<Args...> fmt, Args &&...args) {
  detail::private_log_message(LogLevel::Warning,
                              std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
auto error(std::format_string<Args...> fmt, Args &&...args) {
  detail::private_log_message(LogLevel::Error,
                              std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args &&...args) {
  if (is_level_enabled(LogLevel::Trace)) {
    detail::private_log_message(LogLevel::Trace,
                                std::format(fmt, std::forward<Args>(args)...));
  }
}
} // namespace dy

namespace std {

template <typename T> struct formatter<vector<T>> {
  formatter<T> element_formatter;

  constexpr auto parse(format_parse_context &ctx) {
    return element_formatter.parse(ctx);
  }

  auto format(const vector<T> &vec, format_context &ctx) const {
    auto out = ctx.out();
    out = format_to(out, "[");
    for (size_t i = 0; i < vec.size(); ++i) {
      out = element_formatter.format(vec[i], ctx);
      if (i < vec.size() - 1) {
        out = format_to(out, ", ");
      }
    }
    return format_to(out, "]");
  }
};

template <> struct formatter<filesystem::path> : formatter<string_view> {
  auto format(const filesystem::path &p, format_context &ctx) const {
    return formatter<string_view>::format(p.string(), ctx);
  }
};
} // namespace std