#include <dockyard/animation.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace dy {

auto resolve_joint_parents(std::span<const i32> joint_nodes,
                           std::span<const i32> node_parent)
    -> std::vector<i32> {
  std::unordered_map<i32, i32> node_to_joint;
  node_to_joint.reserve(joint_nodes.size());
  for (u32 i = 0; i < joint_nodes.size(); ++i)
    node_to_joint.emplace(joint_nodes[i], static_cast<i32>(i));

  std::vector<i32> parents(joint_nodes.size(), -1);

  for (u32 i = 0; i < joint_nodes.size(); ++i) {
    i32 node = joint_nodes[i];
    i32 ancestor = (node >= 0 && node < static_cast<i32>(node_parent.size()))
                       ? node_parent[node]
                       : -1;

    while (ancestor >= 0) {
      if (const auto it = node_to_joint.find(ancestor);
          it != node_to_joint.end()) {
        parents[i] = it->second;
        break;
      }
      ancestor = ancestor < static_cast<i32>(node_parent.size())
                     ? node_parent[ancestor]
                     : -1;
    }
  }

  return parents;
}

auto compute_clip_duration(std::span<const AnimationSampler> samplers) -> f32 {
  f32 duration = 0.0F;
  for (const auto &sampler : samplers)
    if (!sampler.inputs.empty())
      duration = std::max(duration, sampler.inputs.back());
  return duration;
}

auto sample_channel(const AnimationSampler &s, AnimationTargetPath path,
                    f32 t) -> glm::vec4 {
  if (s.inputs.empty())
    return glm::vec4{0.0F};

  t = std::clamp(t, s.inputs.front(), s.inputs.back());

  const auto it = std::upper_bound(s.inputs.begin(), s.inputs.end(), t);
  const auto hi = static_cast<u32>(it - s.inputs.begin());

  // t is at or past the last keyframe — return final value directly
  if (hi == static_cast<u32>(s.inputs.size())) {
    const u32 last = hi - 1;
    return (s.interpolation == AnimationInterpolation::CubicSpline)
               ? s.outputs[last * 3 + 1]
               : s.outputs[last];
  }

  const u32 lo = hi - 1;

  if (s.interpolation == AnimationInterpolation::Step)
    return s.outputs[lo];

  const f32 seg_dt = s.inputs[hi] - s.inputs[lo];
  const f32 alpha  = (seg_dt > 0.0F) ? (t - s.inputs[lo]) / seg_dt : 0.0F;

  if (s.interpolation == AnimationInterpolation::Linear) {
    if (path == AnimationTargetPath::Rotation) {
      const glm::quat q0{s.outputs[lo].w, s.outputs[lo].x, s.outputs[lo].y,
                         s.outputs[lo].z};
      const glm::quat q1{s.outputs[hi].w, s.outputs[hi].x, s.outputs[hi].y,
                         s.outputs[hi].z};
      const glm::quat r = glm::slerp(q0, q1, alpha);
      return {r.x, r.y, r.z, r.w};
    }
    return glm::mix(s.outputs[lo], s.outputs[hi], alpha);
  }

  // CubicSpline — cubic Hermite: h00·p0 + h10·m0 + h01·p1 + h11·m1
  const f32 t2 = alpha * alpha;
  const f32 t3 = t2 * alpha;
  const glm::vec4 p0 = s.outputs[lo * 3 + 1];
  const glm::vec4 m0 = s.outputs[lo * 3 + 2] * seg_dt;
  const glm::vec4 p1 = s.outputs[hi * 3 + 1];
  const glm::vec4 m1 = s.outputs[hi * 3 + 0] * seg_dt;

  glm::vec4 result = (2.0F * t3 - 3.0F * t2 + 1.0F) * p0 +
                     (t3 - 2.0F * t2 + alpha) * m0 +
                     (-2.0F * t3 + 3.0F * t2) * p1 + (t3 - t2) * m1;

  if (path == AnimationTargetPath::Rotation) {
    const glm::quat qn = glm::normalize(glm::quat{result.w, result.x,
                                                   result.y, result.z});
    return {qn.x, qn.y, qn.z, qn.w};
  }
  return result;
}

auto compute_joint_palette(const Skeleton &skeleton, const AnimationClip &clip,
                            i32 skeleton_index, f32 t)
    -> std::vector<glm::mat4> {
  const u32 n = skeleton.joint_count();

  std::vector<glm::vec3> translations(n);
  std::vector<glm::quat> rotations(n);
  std::vector<glm::vec3> scales(n);
  for (u32 i = 0; i < n; ++i) {
    const auto &j  = skeleton.joints[i];
    translations[i] = j.bind_translation;
    rotations[i]    = j.bind_rotation;
    scales[i]       = j.bind_scale;
  }

  for (const auto &ch : clip.channels) {
    if (ch.skeleton_index != skeleton_index) continue;
    if (ch.joint_index < 0 || ch.joint_index >= static_cast<i32>(n)) continue;
    const auto &sampler = clip.samplers[ch.sampler_index];
    const glm::vec4 v   = sample_channel(sampler, ch.path, t);
    const u32 ji        = static_cast<u32>(ch.joint_index);
    switch (ch.path) {
    case AnimationTargetPath::Translation: translations[ji] = glm::vec3{v};                           break;
    case AnimationTargetPath::Rotation:    rotations[ji]    = glm::quat{v.w, v.x, v.y, v.z};         break;
    case AnimationTargetPath::Scale:       scales[ji]       = glm::vec3{v};                           break;
    }
  }

  // Compose world transforms. Joints must be in topological order (parent < child).
  std::vector<glm::mat4> world(n);
  for (u32 i = 0; i < n; ++i) {
    glm::mat4 local = glm::translate(glm::mat4{1.0F}, translations[i]);
    local           = local * glm::mat4_cast(rotations[i]);
    local           = glm::scale(local, scales[i]);

    const i32 parent = skeleton.joints[i].parent;
    world[i]         = (parent >= 0) ? world[static_cast<u32>(parent)] * local : local;
  }

  std::vector<glm::mat4> palette(n);
  for (u32 i = 0; i < n; ++i)
    palette[i] = world[i] * skeleton.joints[i].inverse_bind;

  return palette;
}

auto AnimationState::advance(f32 dt) -> void {
  if (!clip || !skeleton) return;
  time += dt;
  if (loop && clip->duration > 0.0F)
    time = std::fmod(time, clip->duration);
  else
    time = std::min(time, clip->duration);
  joint_palette = compute_joint_palette(*skeleton, *clip, skeleton_index, time);
}

} // namespace dy
