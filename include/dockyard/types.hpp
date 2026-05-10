#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <unordered_map>

namespace dy {

using i32 = std::int32_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using f32 = float;
using f64 = double;

inline constexpr u32 frames_in_flight = 3;

namespace detail {
struct string_hash {
  using is_transparent = void;

  constexpr auto operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

struct string_equal {
  using is_transparent = void;

  constexpr auto operator()(std::string_view lhs, std::string_view rhs) const {
    return lhs == rhs;
  }
};

struct string_compare {
  using is_transparent = void;

  auto operator()(std::string_view lhs, std::string_view rhs) const {
    return lhs <=> rhs; // Beautiful, clean C++20 comparison
  }
};
} // namespace detail

template <typename T>
using StringMap = std::unordered_map<std::string, T, detail::string_hash,
                                     detail::string_equal>;
template <typename T>
using OrderedStringMap = std::map<std::string, T, detail::string_compare>;

} // namespace dy
