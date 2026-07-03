#include <dockyard/mesh/animation_load.hpp>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <format>
#include <string>

namespace dy::mesh::detail {

auto node_local_matrix(const fastgltf::Node &node) -> glm::mat4 {
  return std::visit(
      fastgltf::visitor{
          [](const fastgltf::TRS &trs) -> glm::mat4 {
            const glm::vec3 t{trs.translation[0], trs.translation[1],
                              trs.translation[2]};
            const glm::quat q{trs.rotation[3], trs.rotation[0], trs.rotation[1],
                              trs.rotation[2]};
            const glm::vec3 s{trs.scale[0], trs.scale[1], trs.scale[2]};
            return glm::translate(glm::mat4{1.f}, t) * glm::mat4_cast(q) *
                   glm::scale(glm::mat4{1.f}, s);
          },
          [](const fastgltf::math::fmat4x4 &m) -> glm::mat4 {
            glm::mat4 out;
            std::memcpy(glm::value_ptr(out), m.data(), sizeof(glm::mat4));
            return out;
          },
      },
      node.transform);
}

auto node_bind_trs(const fastgltf::Node &node)
    -> std::tuple<glm::vec3, glm::quat, glm::vec3> {
  return std::visit(
      fastgltf::visitor{
          [](const fastgltf::TRS &trs) {
            return std::tuple{
                glm::vec3{trs.translation[0], trs.translation[1],
                          trs.translation[2]},
                glm::quat{trs.rotation[3], trs.rotation[0], trs.rotation[1],
                          trs.rotation[2]},
                glm::vec3{trs.scale[0], trs.scale[1], trs.scale[2]}};
          },
          [](const fastgltf::math::fmat4x4 &mat) {
            glm::mat4 glm_mat;
            std::memcpy(glm::value_ptr(glm_mat), mat.data(), sizeof(glm::mat4));
            glm::vec3 translation;
            glm::vec3 scale;
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::quat rotation;
            glm::decompose(glm_mat, scale, rotation, translation, skew,
                           perspective);
            return std::tuple{translation, rotation, scale};
          },
      },
      node.transform);
}

auto build_node_parents(const fastgltf::Asset &asset) -> std::vector<i32> {
  std::vector<i32> node_parent(asset.nodes.size(), -1);
  for (usize n = 0; n < asset.nodes.size(); ++n)
    for (const auto child : asset.nodes[n].children)
      node_parent[child] = static_cast<i32>(n);
  return node_parent;
}

auto load_skeletons(const fastgltf::Asset &asset) -> std::vector<Skeleton> {
  std::vector<Skeleton> skeletons;
  if (asset.skins.empty())
    return skeletons;

  const auto node_parent = build_node_parents(asset);
  skeletons.reserve(asset.skins.size());

  for (const auto &skin : asset.skins) {
    const usize jc = skin.joints.size();

    std::vector<glm::mat4> inverse_binds(jc, glm::mat4{1.0F});
    if (skin.inverseBindMatrices.has_value()) {
      const auto &acc = asset.accessors[*skin.inverseBindMatrices];
      fastgltf::copyFromAccessor<glm::mat4>(asset, acc, inverse_binds.data());
    }

    std::vector<i32> joint_nodes(jc);
    for (usize i = 0; i < jc; ++i)
      joint_nodes[i] = static_cast<i32>(skin.joints[i]);
    const auto parents = resolve_joint_parents(joint_nodes, node_parent);

    Skeleton skel;
    skel.joints.resize(jc);
    for (usize i = 0; i < jc; ++i) {
      const usize node_idx = skin.joints[i];
      const auto &node = asset.nodes[node_idx];
      auto [translation, rotation, scale] = node_bind_trs(node);

      skel.joints[i] = SkeletonJoint{
          .parent = parents[i],
          .node_index = static_cast<i32>(node_idx),
          .inverse_bind = inverse_binds[i],
          .bind_translation = translation,
          .bind_rotation = rotation,
          .bind_scale = scale,
          .name = node.name.empty() ? std::format("joint_{}", i)
                                    : std::string(node.name),
      };
    }
    skeletons.push_back(std::move(skel));
  }

  return skeletons;
}

auto read_sampler_output(const fastgltf::Asset &asset,
                         const fastgltf::Accessor &acc)
    -> std::vector<glm::vec4> {
  std::vector<glm::vec4> out(acc.count, glm::vec4{0.0F});
  if (acc.type == fastgltf::AccessorType::Vec4) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, acc, [&](glm::vec4 v, usize i) { out[i] = v; });
  } else { // Vec3 — translation / scale
    fastgltf::iterateAccessorWithIndex<glm::vec3>(
        asset, acc, [&](glm::vec3 v, usize i) { out[i] = glm::vec4{v, 0.0F}; });
  }
  return out;
}

auto to_interpolation(fastgltf::AnimationInterpolation in)
    -> AnimationInterpolation {
  switch (in) {
  case fastgltf::AnimationInterpolation::Step:
    return AnimationInterpolation::Step;
  case fastgltf::AnimationInterpolation::CubicSpline:
    return AnimationInterpolation::CubicSpline;
  case fastgltf::AnimationInterpolation::Linear:
  default:
    return AnimationInterpolation::Linear;
  }
}

auto build_joint_lookup(std::span<const Skeleton> skeletons) -> JointLookup {
  JointLookup lookup;
  for (u32 si = 0; si < skeletons.size(); ++si)
    for (u32 ji = 0; ji < skeletons[si].joints.size(); ++ji)
      lookup.emplace(static_cast<usize>(skeletons[si].joints[ji].node_index),
                     std::pair{static_cast<i32>(si), static_cast<i32>(ji)});
  return lookup;
}

auto load_animations(const fastgltf::Asset &asset,
                     const JointLookup &joint_lookup)
    -> std::vector<AnimationClip> {
  std::vector<AnimationClip> clips;
  clips.reserve(asset.animations.size());

  for (const auto &anim : asset.animations) {
    AnimationClip clip;
    clip.name = anim.name.empty() ? std::format("clip_{}", clips.size())
                                  : std::string(anim.name);

    clip.samplers.reserve(anim.samplers.size());
    for (const auto &sampler : anim.samplers) {
      const auto &in_acc = asset.accessors[sampler.inputAccessor];
      AnimationSampler out;
      out.interpolation = to_interpolation(sampler.interpolation);
      out.inputs.resize(in_acc.count);
      fastgltf::copyFromAccessor<f32>(asset, in_acc, out.inputs.data());
      out.outputs =
          read_sampler_output(asset, asset.accessors[sampler.outputAccessor]);
      clip.samplers.push_back(std::move(out));
    }

    clip.channels.reserve(anim.channels.size());
    for (const auto &channel : anim.channels) {
      // Morph-target weight animation is out of scope for skeletal skinning.
      if (channel.path == fastgltf::AnimationPath::Weights)
        continue;

      AnimationChannel out;
      out.sampler_index = static_cast<u32>(channel.samplerIndex);
      out.path = channel.path == fastgltf::AnimationPath::Rotation
                     ? AnimationTargetPath::Rotation
                 : channel.path == fastgltf::AnimationPath::Scale
                     ? AnimationTargetPath::Scale
                     : AnimationTargetPath::Translation;

      if (channel.nodeIndex.has_value()) {
        if (const auto it = joint_lookup.find(*channel.nodeIndex);
            it != joint_lookup.end()) {
          out.skeleton_index = it->second.first;
          out.joint_index = it->second.second;
        }
      }
      clip.channels.push_back(out);
    }

    clip.duration = compute_clip_duration(clip.samplers);
    clips.push_back(std::move(clip));
  }

  return clips;
}

} // namespace dy::mesh::detail
