#pragma once

#include <dockyard/scene.hpp>
#include <functional>
#include <string_view>

struct EditorActions {
  std::function<auto(dy::Entity)->dy::Entity> duplicate_entity;
  std::function<auto(std::string_view)->dy::Entity> create_entity;
  std::function<auto(dy::Entity)->void> destroy_entity;
};
