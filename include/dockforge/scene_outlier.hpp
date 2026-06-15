#pragma once

#include <dockyard/types.hpp>
#include <entt/entt.hpp>
#include <unordered_set>
#include <vector>

struct CachedEntity {
  entt::entity entity;
  dy::u32 depth{0};
  bool is_visible{true};
};
struct SceneOutlinerState {
  bool cache_dirty = true;
  std::vector<CachedEntity> entity_cache;
  std::unordered_set<entt::entity> expanded_entities;
};