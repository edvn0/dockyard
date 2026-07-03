#pragma once

#include <dockyard/animation.hpp>
#include <dockyard/types.hpp>

#include <fastgltf/core.hpp>

#include <glm/glm.hpp>

#include <tuple>
#include <unordered_map>
#include <vector>

namespace dy::mesh::detail {

[[nodiscard]] auto node_local_matrix(const fastgltf::Node &node) -> glm::mat4;

[[nodiscard]] auto node_bind_trs(const fastgltf::Node &node)
    -> std::tuple<glm::vec3, glm::quat, glm::vec3>;

[[nodiscard]] auto build_node_parents(const fastgltf::Asset &asset)
    -> std::vector<i32>;

[[nodiscard]] auto load_skeletons(const fastgltf::Asset &asset)
    -> std::vector<Skeleton>;

[[nodiscard]] auto read_sampler_output(const fastgltf::Asset &asset,
                                       const fastgltf::Accessor &acc)
    -> std::vector<glm::vec4>;

[[nodiscard]] auto to_interpolation(fastgltf::AnimationInterpolation in)
    -> AnimationInterpolation;

// glTF node index -> (skeleton index, joint index within that skeleton).
using JointLookup = std::unordered_map<usize, std::pair<i32, i32>>;

[[nodiscard]] auto build_joint_lookup(std::span<const Skeleton> skeletons)
    -> JointLookup;

[[nodiscard]] auto load_animations(const fastgltf::Asset &asset,
                                   const JointLookup &joint_lookup)
    -> std::vector<AnimationClip>;

} // namespace dy::mesh::detail
