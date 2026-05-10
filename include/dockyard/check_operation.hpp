#pragma once

#include <concepts>

#include <dockyard/log.hpp>

namespace dy {

namespace detail {
template <typename T>
concept IsExpected = requires(T e) {
  { !e } -> std::convertible_to<bool>;
  e.error();
};
} // namespace detail

template <detail::IsExpected... Args> bool check_all(Args &&...args) {
  bool all_ok = true;
  (
      [&](auto &outcome) {
        if (!outcome) {
          dy::error("Pipeline stage failed: {}", outcome.error());
          all_ok = false;
        }
      }(args),
      ...);
  return all_ok;
}

} // namespace dy
