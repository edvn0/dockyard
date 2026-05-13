#pragma once

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <dockyard/log.hpp>
#include <glm/gtx/component_wise.hpp>

struct Ray {
  glm::vec3 origin;
  glm::vec3 dir; // normalized
};

auto screen_to_ray(glm::vec2 mouse_screen, glm::vec2 viewport_pos,
                   glm::vec2 viewport_size, const glm::mat4 &view,
                   const glm::mat4 &proj) -> Ray;
auto ray_aabb(const Ray &ray, glm::vec3 bmin, glm::vec3 bmax) -> float;