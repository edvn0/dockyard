#pragma once

#include <entt/entt.hpp>

namespace dy {
class Scene;
struct SceneRenderer;
} // namespace dy

struct EditorState {
  entt::entity selected = entt::null;
  bool hierarchy_dirty = false;
  bool cache_dirty = true; // signals panels to rebuild display caches
  dy::Scene *active_scene = nullptr;
  dy::SceneRenderer *renderer = nullptr;
};
