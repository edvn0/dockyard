#include <algorithm>
#include <dockyard/animation.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace dy {

auto resolve_joint_parents(std::span<const i32> joint_nodes,
                           std::span<const i32> node_parent)
    -> std::vector<i32> {
  if (joint_nodes.empty())
    return {};

  i32 max_node = std::ranges::fold_left(
      joint_nodes, 0, [](i32 acc, i32 node) { return std::max(acc, node); });

  std::vector<i32> node_to_joint(max_node + 1, -1);
  for (u32 i = 0; i < joint_nodes.size(); ++i) {
    if (joint_nodes[i] >= 0) {
      node_to_joint[joint_nodes[i]] = static_cast<i32>(i);
    }
  }

  std::vector<i32> parents(joint_nodes.size(), -1);

  for (u32 i = 0; i < joint_nodes.size(); ++i) {
    i32 node = joint_nodes[i];
    i32 ancestor = (node >= 0 && node < static_cast<i32>(node_parent.size()))
                       ? node_parent[node]
                       : -1;

    while (ancestor >= 0) {
      if (ancestor <= max_node && node_to_joint[ancestor] != -1) {
        parents[i] = node_to_joint[ancestor];
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

auto sample_channel(const AnimationSampler &s, AnimationTargetPath path, f32 t,
                    u32 &cursor) -> glm::vec4 {
  if (s.inputs.empty())
    return glm::vec4{0.0F};

  t = std::clamp(t, s.inputs.front(), s.inputs.back());

  if (t < s.inputs[cursor]) {
    cursor = 0;
  }
  while (cursor < s.inputs.size() - 1 && t >= s.inputs[cursor + 1]) {
    cursor++;
  }

  const u32 lo = cursor;
  const u32 hi = std::min(lo + 1, static_cast<u32>(s.inputs.size() - 1));

  if (lo == hi) {
    return (s.interpolation == AnimationInterpolation::CubicSpline)
               ? s.outputs[(lo * 3) + 1]
               : s.outputs[lo];
  }

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
      const auto r = glm::slerp(q0, q1, alpha);
      return {r.x, r.y, r.z, r.w};
    }
    return glm::mix(s.outputs[lo], s.outputs[hi], alpha);
  }

  const auto t2 = alpha * alpha;
  const auto t3 = t2 * alpha;
  const auto p0 = s.outputs[(lo * 3) + 1];
  const auto m0 = s.outputs[(lo * 3) + 2] * seg_dt;
  const auto p1 = s.outputs[(hi * 3) + 1];
  const auto m1 = s.outputs[(hi * 3) + 0] * seg_dt;

  glm::vec4 result = ((2.0F * t3) - (3.0F * t2) + 1.0F) * p0 +
                     (t3 - (2.0F * t2) + alpha) * m0 +
                     ((-2.0F * t3) + (3.0F * t2)) * p1 + (t3 - t2) * m1;

  if (path == AnimationTargetPath::Rotation) {
    const auto qn =
        glm::normalize(glm::quat{result.w, result.x, result.y, result.z});
    return {qn.x, qn.y, qn.z, qn.w};
  }
  return result;
}

auto AnimationState::init(const Skeleton *target_skeleton,
                          const AnimationClip *target_clip, i32 skel_idx)
    -> void {
  skeleton = target_skeleton;
  clip = target_clip;
  skeleton_index = skel_idx;
  time = 0.0F;

  if (skeleton != nullptr) {
    size_t joint_count = skeleton->joints.size();
    local_transforms.resize(joint_count);
    world_transforms.resize(joint_count);
    joint_palette.resize(joint_count);
  } else {
    local_transforms.clear();
    world_transforms.clear();
    joint_palette.clear();
  }

  if (clip != nullptr) {
    channel_cursors.assign(clip->channels.size(), 0);
  } else {
    channel_cursors.clear();
  }
}

auto AnimationState::update_animation(const AnimationClip &new_clip) -> void {
  if (this->clip != &new_clip) {
    this->init(this->skeleton, &new_clip, this->skeleton_index);
  }
}

auto compute_joint_palette(AnimationState &state) -> void {
  if (state.skeleton == nullptr || state.clip == nullptr)
    return;

  const u32 n = state.skeleton->joint_count();

  for (u32 i = 0; i < n; ++i) {
    const auto &j = state.skeleton->joints[i];
    state.local_transforms[i].translation = j.bind_translation;
    state.local_transforms[i].rotation = j.bind_rotation;
    state.local_transforms[i].scale = j.bind_scale;
  }

  for (u32 i = 0; i < state.clip->channels.size(); ++i) {
    const auto &ch = state.clip->channels[i];
    if (ch.skeleton_index != state.skeleton_index)
      continue;
    if (ch.joint_index < 0 || static_cast<u32>(ch.joint_index) >= n)
      continue;

    const auto &sampler = state.clip->samplers[ch.sampler_index];
    const auto v =
        sample_channel(sampler, ch.path, state.time, state.channel_cursors[i]);
    const u32 ji = static_cast<u32>(ch.joint_index);

    switch (ch.path) {
    case AnimationTargetPath::Translation:
      state.local_transforms[ji].translation = glm::vec3{v};
      break;
    case AnimationTargetPath::Rotation:
      state.local_transforms[ji].rotation = glm::quat{v.w, v.x, v.y, v.z};
      break;
    case AnimationTargetPath::Scale:
      state.local_transforms[ji].scale = glm::vec3{v};
      break;
    }
  }

  for (u32 i = 0; i < n; ++i) {
    const auto &trs = state.local_transforms[i];

    glm::mat4 local = glm::mat4_cast(trs.rotation);
    local[0] *= trs.scale.x;
    local[1] *= trs.scale.y;
    local[2] *= trs.scale.z;
    local[3] = glm::vec4(trs.translation, 1.0F);

    const i32 parent = state.skeleton->joints[i].parent;
    state.world_transforms[i] =
        (parent >= 0) ? state.world_transforms[static_cast<u32>(parent)] * local
                      : local;
  }

  for (u32 i = 0; i < n; ++i) {
    state.joint_palette[i] =
        state.world_transforms[i] * state.skeleton->joints[i].inverse_bind;
  }
}

auto AnimationState::advance(f32 dt) -> void {
  if (clip == nullptr || skeleton == nullptr)
    return;

  time += dt;
  if (loop && clip->duration > 0.0F)
    time = std::fmod(time, clip->duration);
  else
    time = std::min(time, clip->duration);

  compute_joint_palette(*this);
}

} // namespace dy