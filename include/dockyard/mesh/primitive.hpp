#pragma once

#include <dockyard/mesh/load_types.hpp>

#include <fastgltf/core.hpp>

#include <expected>
#include <string>
#include <vector>

namespace dy::mesh::detail {

[[nodiscard]] auto extract_primitive(const fastgltf::Asset &asset,
                                     const fastgltf::Primitive &prim)
    -> std::expected<PrimitiveResult, std::string>;

// Progressive index-buffer simplifications of lod0, coarsest last. May return
// fewer than the requested number of LODs if simplification bottoms out early
// (see the out_count == prev_count check in the implementation).
[[nodiscard]] auto generate_lods(const PrimitiveData &lod0)
    -> std::vector<std::vector<u32>>;

} // namespace dy::mesh::detail
