#pragma once

#include <dockyard/types.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <span>
#include <string>
#include <vector>

namespace dy {

// A skeleton joint, ordered identically to the source glTF skin's joints array
// (so packed SkinVertex joint indices index directly into Skeleton::joints).
struct SkeletonJoint {
  i32 parent = -1;     // index into Skeleton::joints; -1 for a root joint
  i32 node_index = -1; // source glTF node index (used to map animation channels)
  glm::mat4 inverse_bind{1.0F};

  // Default local transform (bind pose). The sampler starts from these and
  // overrides only the components an animation channel actually drives.
  glm::vec3 bind_translation{0.0F};
  glm::quat bind_rotation{1.0F, 0.0F, 0.0F, 0.0F};
  glm::vec3 bind_scale{1.0F};

  std::string name;
};

struct Skeleton {
  std::vector<SkeletonJoint> joints;

  [[nodiscard]] auto joint_count() const -> u32 {
    return static_cast<u32>(joints.size());
  }
};

enum class AnimationInterpolation : u8 { Linear, Step, CubicSpline };
enum class AnimationTargetPath : u8 { Translation, Rotation, Scale };

struct AnimationSampler {
  std::vector<f32> inputs;        // keyframe times, seconds, ascending
  std::vector<glm::vec4> outputs; // Translation/Scale use xyz; Rotation xyzw
  AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

struct AnimationChannel {
  i32 skeleton_index = -1; // index into MeshAsset::skeletons; -1 if not a joint
  i32 joint_index = -1;    // index into that skeleton's joints; -1 if not a joint
  AnimationTargetPath path = AnimationTargetPath::Translation;
  u32 sampler_index = 0;
};

struct AnimationClip {
  std::string name;
  f32 duration = 0.0F; // seconds; longest keyframe time across all samplers
  std::vector<AnimationSampler> samplers;
  std::vector<AnimationChannel> channels;
};

// Resolve each joint's parent within the joint set. For joint i, walk up the
// node hierarchy from its parent until reaching a node that is itself a joint;
// that joint's index becomes the parent, or -1 if no ancestor is a joint.
//
//   joint_nodes[i] : glTF node index of joint i
//   node_parent[n] : parent node index of node n, or -1 for a root
[[nodiscard]] auto resolve_joint_parents(std::span<const i32> joint_nodes,
                                         std::span<const i32> node_parent)
    -> std::vector<i32>;

// Longest keyframe time across all samplers; 0 when there are no keyframes.
[[nodiscard]] auto compute_clip_duration(std::span<const AnimationSampler>)
    -> f32;

// Sample one animation channel at time t. path controls whether slerp is used
// for Rotation; all other paths lerp. t is clamped to the keyframe range.
// Returns {0,0,0,0} when the sampler has no keyframes.
[[nodiscard]] auto sample_channel(const AnimationSampler& sampler,
                                  AnimationTargetPath path,
                                  f32 t) -> glm::vec4;

// Compose the joint palette for one skeleton driven by a clip at time t.
// Only channels whose skeleton_index matches the supplied value are applied;
// joints not targeted by any channel keep their bind-pose transform.
// Returns one mat4 per joint: global_joint_transform * inverse_bind.
// Joints must be stored in topological order (parent index < child index).
[[nodiscard]] auto compute_joint_palette(const Skeleton& skeleton,
                                         const AnimationClip& clip,
                                         i32 skeleton_index,
                                         f32 t) -> std::vector<glm::mat4>;

// ECS component: drives one skeleton with one clip.
struct AnimationState {
  const Skeleton*      skeleton       = nullptr; // non-owning
  const AnimationClip* clip           = nullptr; // non-owning
  i32                  skeleton_index = 0;
  f32                  time           = 0.0F;    // seconds
  bool                 loop           = true;
  std::vector<glm::mat4> joint_palette;           // size == skeleton->joint_count()

  // Advance playback time and recompute joint_palette. No-op when clip or skeleton is null.
  auto advance(f32 dt) -> void;
};

} // namespace dy
