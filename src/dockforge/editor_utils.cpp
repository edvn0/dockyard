#include <dockforge/editor_utils.hpp>

auto screen_to_ray(glm::vec2 mouse_screen, glm::vec2 viewport_pos,
                   glm::vec2 viewport_size, const glm::mat4 &view,
                   const glm::mat4 &proj) -> Ray {
  glm::vec2 ndc{
      (((mouse_screen.x - viewport_pos.x) / viewport_size.x) * 2.0F) - 1.0F,
      -((((mouse_screen.y - viewport_pos.y) / viewport_size.y) * 2.0F) - 1.0F),
  };

  // Sanity check — if ndc is outside [-1,1] the click was outside the viewport
  //  FIXME: USE THIS: if (ndc.x < -1.0F || ndc.x > 1.0F || ndc.y < -1.0F ||
  //  ndc.y > 1.0F)
  const glm::mat4 inv_vp = glm::inverse(proj * view);

  glm::vec4 near_h = inv_vp * glm::vec4(ndc.x, ndc.y, 1.0F, 1.0F);
  glm::vec4 far_h = inv_vp * glm::vec4(ndc.x, ndc.y, 0.0F, 1.0F);

  // w approaching zero means the matrix inverse is degenerate
  //  FIXME: USE THIS if (std::abs(near_h.w) < 1e-6f || std::abs(far_h.w) <
  //  1e-6f)

  near_h /= near_h.w;
  far_h /= far_h.w;

  const glm::vec3 origin = glm::vec3(near_h);
  const glm::vec3 dir = glm::normalize(glm::vec3(far_h) - origin);

  // const float dir_len = glm::length(dir);
  // FIXME: USE THIS: if (std::abs(dir_len - 1.0F) > 1e-4f)

  return {.origin = origin, .dir = dir};
}

auto ray_aabb(const Ray &ray, glm::vec3 bmin, glm::vec3 bmax) -> float {
  const glm::vec3 inv_dir = 1.0F / ray.dir;
  const glm::vec3 t0 = (bmin - ray.origin) * inv_dir;
  const glm::vec3 t1 = (bmax - ray.origin) * inv_dir;
  const glm::vec3 tmin = glm::min(t0, t1);
  const glm::vec3 tmax = glm::max(t0, t1);
  const float enter = glm::compMax(tmin);
  const float exit = glm::compMin(tmax);
  if (exit < 0.0F || enter > exit)
    return -1.0F;
  return enter < 0.0F ? exit : enter;
}