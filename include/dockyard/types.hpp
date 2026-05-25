#pragma once

#include <dockyard/log.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <unordered_map>


namespace dy {

template <typename T> class Badge {
  friend T;

private:
  constexpr Badge() {}
};

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

struct NanoProfiler {
  std::string scope;
  std::chrono::high_resolution_clock::time_point start;

  // Use string_view to avoid unnecessary string allocations
  explicit NanoProfiler(std::string_view name)
      : scope(name), start(std::chrono::high_resolution_clock::now()) {}

  ~NanoProfiler() {
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    trace("[{}]: {}ms", scope, elapsed.count());
  }
};

#define PROFILE_CONCAT_INNER(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name)                                                    \
  NanoProfiler PROFILE_CONCAT(profiler_, __COUNTER__)(name)

#define MAKE_BITFIELD(X)                                                       \
  constexpr auto operator|(X a, X b)->X {                                      \
    return static_cast<X>(std::to_underlying(a) | std::to_underlying(b));      \
  }                                                                            \
  constexpr auto operator&(X a, X b)->X {                                      \
    return static_cast<X>(std::to_underlying(a) & std::to_underlying(b));      \
  }                                                                            \
  constexpr bool operator!(X f) { return f == X::None; }                       \
  constexpr auto operator|=(X &a, X b)->X & {                                  \
    a = a | b;                                                                 \
    return a;                                                                  \
  }                                                                            \
  constexpr auto operator&=(X &a, X b)->X & {                                  \
    a = a & b;                                                                 \
    return a;                                                                  \
  }                                                                            \
  constexpr auto has_flag(X flags, X flag) -> bool {                           \
    return (flags & flag) == flag;                                             \
  }                                                                            \
  constexpr auto set_flag(X &flags, X flag) -> void { flags |= flag; }         \
  constexpr auto clear_flag(X &flags, X flag) -> void {                        \
    flags &= static_cast<X>(~std::to_underlying(flag));                        \
  }

} // namespace dy
