#pragma once
#include <entt/entt.hpp>
#include <vector>

struct SceneOutlinerState {
  std::vector<entt::entity> entity_cache;
  entt::entity selected = entt::null;
  bool cache_dirty = true;
};