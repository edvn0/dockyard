#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>

#include <dockyard/component_traits.hpp>

#include <dockyard/components.hpp>
#include <dockyard/forward.hpp>
#include <imgui.h>

struct Dockforge;

template <typename R>
concept has_add_hook = requires(dy::SceneRenderer &r, dy::Entity &e) {
  { R::on_add(r, e) } -> std::same_as<void>;
};

template <typename R>
concept has_remove_hook = requires(dy::SceneRenderer &r, dy::Entity &e) {
  { R::on_remove(r, e) } -> std::same_as<void>;
};

template <typename Derived> struct BaseComponentRenderer {
  static constexpr bool removable = true;
  static constexpr bool addable = true;
};

template <typename T> struct ComponentRenderer {};

template <typename T>
concept has_valid_renderer =
    requires(T &val, dy::SceneRenderer &r, dy::Entity &e) {
      { ComponentRenderer<T>::draw(val, r, e) } -> std::same_as<bool>;
      { ComponentRenderer<T>::label } -> std::convertible_to<std::string_view>;
      { ComponentRenderer<T>::addable } -> std::convertible_to<bool>;
      { ComponentRenderer<T>::removable } -> std::convertible_to<bool>;
    };

class ComponentInspector {
public:
  static auto draw(dy::SceneRenderer &, dy::Entity &) -> void;

private:
  template <typename T>
  static auto draw_one(dy::SceneRenderer &, dy::Entity &,
                       std::optional<std::function<void()>> &pending_remove)
      -> void;

  static auto draw_add_button(dy::SceneRenderer &, dy::Entity &,
                              std::optional<std::function<void()>> &pending_add)
      -> void;
};