#pragma once

#include <dockyard/scene.hpp>

[[nodiscard]] auto cached_matrix(entt::entity,
                                 const dy::Components::Transform &)
    -> const glm::mat4 &;
