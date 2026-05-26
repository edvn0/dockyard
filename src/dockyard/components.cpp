#include <dockyard/components.hpp>

namespace dy::Components {

auto DebugFrustum::matrices(const glm::vec3 &position,
                            const glm::quat &rotation) const
    -> std::pair<glm::mat4, glm::mat4> {
  const auto forward = rotation * glm::vec3{0.0F, 0.0F, 1.0F};
  const auto up = rotation * glm::vec3{0.0F, 1.0F, 0.0F};

  const glm::mat4 view = glm::lookAtLH(position, position + forward, up);
  const glm::mat4 proj = glm::perspectiveLH_ZO(
      glm::radians(projection_config.fov_degrees), projection_config.aspect,
      projection_config.near, projection_config.far);

  return {view, proj};
}

} // namespace dy::Components