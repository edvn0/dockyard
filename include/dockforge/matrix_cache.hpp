#pragma once

#include <dockyard/scene.hpp>

auto initialise_matrix_cache(dy::u64 &frame_counter) -> void;

/// LRU cache for world matrices, to avoid recomputing them every frame for
/// entities
[[nodiscard]] auto cached_matrix(entt::entity,
                                 const dy::Components::Transform &)
    -> const glm::mat4 &;
