#pragma once

#include <concepts>
#include <string_view>

#include <dockyard/log.hpp>

namespace dy {

template <std::invocable F> struct scope_exit {
  F fn;
  std::string_view call;

  ~scope_exit() {
    info("DEFER {}", call);
    fn();
  }

  explicit scope_exit(F fn, std::string_view call)
      : fn(std::move(fn)), call(call) {}

  scope_exit(const scope_exit &) = delete;
  scope_exit(scope_exit &&) = delete;
  scope_exit &operator=(const scope_exit &) = delete;
  scope_exit &operator=(scope_exit &&) = delete;
};

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

#define DEFER(fn)                                                              \
  dy::scope_exit CONCAT(defer_, __COUNTER__) {                                 \
    [&] { fn; }, #fn                                                           \
  }

} // namespace dy