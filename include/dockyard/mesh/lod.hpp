#pragma once

#include <dockyard/mesh/load_types.hpp>

#include <fastgltf/core.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dy::mesh::detail {

struct ExplicitLodGroup {
  std::string base_name;
  // Sorted by lod_index ascending. Entry [0] is always LOD0 (the primary).
  std::vector<std::pair<u32 /*lod_index*/, usize /*mesh_idx*/>> members;

  [[nodiscard]] usize primary_mesh_idx() const { return members[0].second; }
};

// Parses a mesh name for a trailing _lod{N} suffix.
// Returns (base_name, lod_index) or nullopt.
[[nodiscard]] auto parse_lod_suffix(std::string_view name)
    -> std::optional<std::pair<std::string, u32>>;

// Scans asset.meshes for _lod{N} naming and returns all detected groups.
// An optional name filter restricts detection to a single base name.
[[nodiscard]] auto
detect_lod_groups(const fastgltf::Asset &asset,
                  const std::optional<std::string> &base_name_filter = std::nullopt)
    -> std::vector<ExplicitLodGroup>;

struct RemapResult {
  std::vector<u32> indices;
  bool exact{true}; // false if any vertex needed nearest-neighbour fallback
};

// (LOD0's vertices) all higher LODs need their indices remapped to point into
// LOD0's vertex range.
//
// Strategy:
//   1. Build a position→vertex_index lookup from LOD0's vertices.
//   2. For every vertex in LODn, find the closest LOD0 vertex by position.
//   3. Rewrite LODn's index buffer so every index points into LOD0.
//
// Position matching is exact (bit-identical float comparison). If an author
// has altered vertex positions between LODs this will fall back to a
// nearest-neighbour search with a configurable tolerance. In practice,
// mesh simplification tools (Blender Decimate, meshopt) preserve the original
// vertex positions for remaining verts, so exact matching covers all real
// cases.
//
// Returns a remapped copy of lod_n_data.indices, or the original indices with
// a warning if remapping fails for any vertex.
[[nodiscard]] auto remap_lod_indices_to_lod0(const PrimitiveData &lod0,
                                             const PrimitiveData &lod_n)
    -> RemapResult;

} // namespace dy::mesh::detail
