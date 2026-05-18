#pragma once

#include <bit>
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
using u8 = std::uint8_t;
using usize = std::size_t;
using f32 = float;
using f64 = double;

inline constexpr u32 frames_in_flight = 3;

constexpr usize next_power_of_two(usize n) {
  if (n == 0)
    return 1;
  return std::bit_ceil(n);
}

namespace detail {
struct StringHash {
  using is_transparent = void;

  constexpr auto operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

struct StringEqual {
  using is_transparent = void;

  constexpr auto operator()(std::string_view lhs, std::string_view rhs) const {
    return lhs == rhs;
  }
};

struct StringCompare {
  using is_transparent = void;

  auto operator()(std::string_view lhs, std::string_view rhs) const {
    return lhs <=> rhs; // Beautiful, clean C++20 comparison
  }
};
} // namespace detail

template <typename T>
using StringMap =
    std::unordered_map<std::string, T, detail::StringHash, detail::StringEqual>;
template <typename T>
using OrderedStringMap = std::map<std::string, T, detail::StringCompare>;

#define MAKE_BITFIELD(T)                                                       \
  inline constexpr T operator|(T lhs, T rhs) {                                 \
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) |        \
                          static_cast<std::underlying_type_t<T>>(rhs));        \
  }                                                                            \
  inline constexpr T &operator|=(T &lhs, T rhs) { return lhs = (lhs | rhs); }  \
  inline constexpr T operator&(T lhs, T rhs) {                                 \
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) &        \
                          static_cast<std::underlying_type_t<T>>(rhs));        \
  }

} // namespace dy
