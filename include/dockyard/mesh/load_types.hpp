#pragma once

#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh.hpp>
#include <dockyard/types.hpp>

#include <vector>

namespace dy::mesh::detail {

struct PrimitiveData {
  std::vector<Vertex> vertices;
  std::vector<u32> indices;
  // Empty unless the primitive has JOINTS_0 + WEIGHTS_0; parallel to vertices.
  std::vector<SkinVertex> skin;
};

struct PrimitiveResult {
  PrimitiveData data;
  AABB aabb;
};

// Per-primitive collision geometry in that primitive's own LOCAL space
// (coarsest generated LOD, not LOD0 — see upload_geometry), keyed by
// [mesh_idx][prim_idx_within_mesh] to mirror MeshAsset::meshes. flatten_nodes
// bakes each owning node's world transform into these before appending them
// into MeshAsset::collision_positions/collision_indices — a mesh's raw
// vertex data alone isn't enough, since the same mesh can be referenced by
// many differently-transformed nodes in the scene graph.
struct RawCollisionPrimitive {
  std::vector<PositionOnlyVertex> positions;
  std::vector<u32> indices;
};
using RawCollisionGeometry = std::vector<std::vector<RawCollisionPrimitive>>;

struct DecodedImage {
  std::vector<std::byte> pixels;
  u32 width{};
  u32 height{};
};

struct GeometryRequirements {
  usize total_vertices = 0;
  usize total_indices = 0;
};

auto calculate_requirements(const auto &extracted_prims)
    -> GeometryRequirements {
  GeometryRequirements reqs;
  for (const auto &res : extracted_prims) {
    if (res) {
      reqs.total_vertices += res->data.vertices.size();
      reqs.total_indices += res->data.indices.size();
    }
  }
  return reqs;
}

} // namespace dy::mesh::detail
