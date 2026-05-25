#include <dockyard/components.hpp>

namespace dy::Components {

auto DebugFrustum::matrices(const glm::vec3 &position) const
    -> std::pair<glm::mat4, glm::mat4> {
  const auto view = glm::lookAtLH(position, center, glm::vec3{0, 1, 0});
  const auto proj = glm::perspectiveLH_ZO(
      glm::radians(projection_config.fov_degrees), projection_config.aspect,
      projection_config.near, projection_config.far);
  return {view, proj};
}

} // namespace dy::Components