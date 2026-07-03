#include <dockyard/mesh/primitive.hpp>

#include <dockyard/log.hpp>
#include <dockyard/mesh/tangents.hpp>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <glm/glm.hpp>
#include <glm/packing.hpp>

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace dy::mesh::detail {

auto extract_primitive(const fastgltf::Asset &asset,
                       const fastgltf::Primitive &prim)
    -> std::expected<PrimitiveResult, std::string> {
  const auto *pos_attr = prim.findAttribute("POSITION");
  if (pos_attr == prim.attributes.end())
    return std::unexpected("Primitive missing POSITION attribute");
  if (!prim.indicesAccessor.has_value())
    return std::unexpected("Non-indexed primitive — not supported");

  const auto &pos_acc = asset.accessors[pos_attr->accessorIndex];
  const usize vtx_count = pos_acc.count;

  PrimitiveData out;
  out.vertices.resize(vtx_count);

  auto aabb = AABB::create();

  fastgltf::iterateAccessorWithIndex<glm::vec3>(
      asset, pos_acc, [&](glm::vec3 v, usize i) {
        out.vertices[i].position[0] = v.x;
        out.vertices[i].position[1] = v.y;
        out.vertices[i].position[2] = v.z;
        aabb.update(v);
      });

  if (const auto *a = prim.findAttribute("NORMAL");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec3>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec3 n, usize i) {
          out.vertices[i].normal = glm::packSnorm4x8(glm::vec4(n, 0.0F));
        });
  }

  if (const auto *a = prim.findAttribute("TEXCOORD_0");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec2>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec2 uv, usize i) {
          out.vertices[i].uvs = glm::packHalf2x16(uv);
        });
  }

  if (const auto *a = prim.findAttribute("TEXCOORD_1");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec2>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec2 uv, usize i) {
          out.vertices[i].uvs1 = glm::packHalf2x16(uv);
        });
  }

  if (const auto *a = prim.findAttribute("TANGENT");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec4 t, usize i) {
          out.vertices[i].tangent = glm::packSnorm4x8(t);
        });
  }

  if (const auto *a = prim.findAttribute("BITANGENT");
      a != prim.attributes.end()) {
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[a->accessorIndex], [&](glm::vec4 t, usize i) {
          out.vertices[i].bitangent = glm::packSnorm4x8(t);
        });
  }

  // Skinning attributes: only meaningful when both joints and weights exist.
  const auto *joints_attr = prim.findAttribute("JOINTS_0");
  const auto *weights_attr = prim.findAttribute("WEIGHTS_0");
  if (joints_attr != prim.attributes.end() &&
      weights_attr != prim.attributes.end()) {
    std::vector<glm::u16vec4> joints(vtx_count, glm::u16vec4{0});
    std::vector<glm::vec4> weights(vtx_count, glm::vec4{0.0F});

    fastgltf::iterateAccessorWithIndex<glm::u16vec4>(
        asset, asset.accessors[joints_attr->accessorIndex],
        [&](glm::u16vec4 j, usize i) { joints[i] = j; });
    fastgltf::iterateAccessorWithIndex<glm::vec4>(
        asset, asset.accessors[weights_attr->accessorIndex],
        [&](glm::vec4 w, usize i) { weights[i] = w; });

    out.skin.resize(vtx_count);
    for (usize i = 0; i < vtx_count; ++i)
      out.skin[i] = pack_skin_vertex(
          {joints[i].x, joints[i].y, joints[i].z, joints[i].w}, weights[i]);
  }

  const auto &idx_acc = asset.accessors[*prim.indicesAccessor];
  out.indices.resize(idx_acc.count);
  fastgltf::copyFromAccessor<u32>(asset, idx_acc, out.indices.data());

  const bool has_tangents =
      prim.findAttribute("TANGENT") != prim.attributes.end();
  const bool has_normals =
      prim.findAttribute("NORMAL") != prim.attributes.end();
  const bool has_uvs =
      prim.findAttribute("TEXCOORD_0") != prim.attributes.end();

  if (!has_normals) {
    // Generate smooth normals by accumulating face normals per vertex.
    std::vector<glm::vec3> accum(vtx_count, glm::vec3{0.f});
    for (usize t = 0; t + 2 < out.indices.size(); t += 3) {
      const u32 i0 = out.indices[t], i1 = out.indices[t + 1],
                i2 = out.indices[t + 2];
      const glm::vec3 p0{out.vertices[i0].position[0],
                         out.vertices[i0].position[1],
                         out.vertices[i0].position[2]};
      const glm::vec3 p1{out.vertices[i1].position[0],
                         out.vertices[i1].position[1],
                         out.vertices[i1].position[2]};
      const glm::vec3 p2{out.vertices[i2].position[0],
                         out.vertices[i2].position[1],
                         out.vertices[i2].position[2]};
      const glm::vec3 face_n = glm::cross(p1 - p0, p2 - p0);
      accum[i0] += face_n;
      accum[i1] += face_n;
      accum[i2] += face_n;
    }
    for (usize i = 0; i < vtx_count; ++i) {
      const glm::vec3 n = glm::length(accum[i]) > 0.f
                              ? glm::normalize(accum[i])
                              : glm::vec3{0.f, 1.f, 0.f};
      out.vertices[i].normal = glm::packSnorm4x8(glm::vec4(n, 0.f));
    }
  }

  if (!has_tangents) {
    if (!has_uvs)
      warn("extract_primitive: no TANGENT and no TEXCOORD_0 — tangents will "
           "be zero (normal mapping unavailable for this primitive)");
    else if (auto res = generate_mikktspace_tangents(out); !res)
      return std::unexpected(res.error());
  }

  return std::expected<PrimitiveResult, std::string>{std::in_place, out, aabb};
}

auto generate_lods(const PrimitiveData &lod0) -> std::vector<std::vector<u32>> {

  // Progressively lower target ratios
  static constexpr std::array<f32, 5> lod_targets = {0.50F, 0.25F, 0.125F,
                                                     0.0625F, 0.03125F};

  std::vector<std::vector<u32>> result;
  thread_local std::vector<u32> tl_simplified;

  const auto *prev_indices = lod0.indices.data();
  usize prev_count = lod0.indices.size();

  const f32 *position_ptr = &lod0.vertices[0].position[0];
  const usize vertex_stride = sizeof(Vertex);

  for (size_t i = 0; i < lod_targets.size(); ++i) {
    const f32 target_ratio = lod_targets[i];

    const usize target_count =
        std::max(static_cast<usize>(3),
                 static_cast<usize>(static_cast<f32>(lod0.indices.size()) *
                                    target_ratio));
    // target_count is a ratio of the ORIGINAL lod0 index count, but meshopt's
    // target_index_count must never exceed the previous LOD's actual index
    // count (prev_count). A simplification step — especially
    // meshopt_simplifySloppy, used below for i >= 3 — can legitimately
    // undershoot its own target on flat/degenerate geometry; when it does,
    // the next iteration's fixed-ratio target can exceed the now-smaller
    // prev_count and trip meshopt's `target_index_count <= index_count`
    // assertion. Clamping here keeps the request valid; the loop's
    // out_count == prev_count check then ends the chain gracefully instead.
    const usize rounded = std::min((target_count / 3) * 3, prev_count);

    tl_simplified.resize(prev_count);
    usize out_count = 0;

    if (i < 3) {
      const f32 current_lod_error = 0.1F + (static_cast<f32>(i) * 0.3F);
      f32 result_error = 0.0F;

      out_count =
          meshopt_simplify(tl_simplified.data(), prev_indices, prev_count,
                           position_ptr, lod0.vertices.size(), vertex_stride,
                           rounded, current_lod_error, 0, &result_error);
    } else {
      const f32 sloppy_error = 0.5F;
      out_count = meshopt_simplifySloppy(
          tl_simplified.data(), prev_indices, prev_count, position_ptr,
          lod0.vertices.size(), vertex_stride, rounded, sloppy_error, nullptr);
    }

    if (out_count == 0 || out_count == prev_count) {
      break;
    }

    result.emplace_back(tl_simplified.begin(),
                        tl_simplified.begin() + out_count);
    prev_indices = result.back().data();
    prev_count = result.back().size();
  }

  return result;
}

} // namespace dy::mesh::detail
