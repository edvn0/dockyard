#pragma once

#include <dockyard/mesh/load_types.hpp>

#include <expected>
#include <string>

namespace dy::mesh::detail {

[[nodiscard]] auto generate_mikktspace_tangents(PrimitiveData &prim)
    -> std::expected<void, std::string>;

} // namespace dy::mesh::detail
