#include <dockyard/mesh/lod.hpp>

#include <dockyard/log.hpp>

#include <fastgltf/types.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <limits>
#include <unordered_map>

namespace dy::mesh::detail {

auto parse_lod_suffix(std::string_view name)
    -> std::optional<std::pair<std::string, u32>> {
  constexpr std::string_view suffix = "_lod";
  const auto pos = name.rfind(suffix);
  if (pos == std::string_view::npos)
    return std::nullopt;

  const auto level_str = name.substr(pos + suffix.size());
  if (level_str.empty() ||
      !std::ranges::all_of(level_str, [](char c) { return std::isdigit(c); }))
    return std::nullopt;

  u32 level = 0;
  const auto [ptr, ec] = std::from_chars(
      level_str.data(), level_str.data() + level_str.size(), level);
  if (ec != std::errc{})
    return std::nullopt;

  if (level >= static_cast<u32>(max_lods))
    return std::nullopt;

  return std::make_pair(std::string(name.substr(0, pos)), level);
}

auto detect_lod_groups(const fastgltf::Asset &asset,
                       const std::optional<std::string> &base_name_filter)
    -> std::vector<ExplicitLodGroup> {

  std::unordered_map<std::string, ExplicitLodGroup> by_base;

  for (usize mi = 0; mi < asset.meshes.size(); ++mi) {
    auto parsed = parse_lod_suffix(asset.meshes[mi].name);
    if (!parsed)
      continue;
    auto &[base, level] = *parsed;

    if (base_name_filter && base != *base_name_filter)
      continue;

    auto &group = by_base[base];
    if (group.base_name.empty())
      group.base_name = base;
    group.members.emplace_back(level, mi);
  }

  std::vector<ExplicitLodGroup> result;
  result.reserve(by_base.size());

  for (auto &[key, group] : by_base) {
    // Sort so lod0 is always members[0].
    std::ranges::sort(group.members, {}, &std::pair<u32, usize>::first);

    // Only treat as a real LOD chain if LOD0 is present.
    if (group.members[0].first != 0) {
      warn("LOD group '{}' has no LOD0 — skipping", group.base_name);
      continue;
    }
    result.push_back(std::move(group));
  }

  return result;
}

auto remap_lod_indices_to_lod0(const PrimitiveData &lod0,
                               const PrimitiveData &lod_n) -> RemapResult {

  // Build exact position map: packed u32 key → lod0 vertex index.
  // We use the raw float bits, good enough for positions authored in the
  // same DCC tool.
  struct Vec3Key {
    u32 x, y, z;
    bool operator==(const Vec3Key &) const = default;
  };
  struct Vec3Hash {
    usize operator()(const Vec3Key &k) const noexcept {
      // FNV-1a variant over three u32s
      usize h = 14695981039346656037ULL;
      for (u32 v : {k.x, k.y, k.z}) {
        h ^= static_cast<usize>(v);
        h *= 1099511628211ULL;
      }
      return h;
    }
  };

  std::unordered_map<Vec3Key, u32, Vec3Hash> pos_to_lod0;
  pos_to_lod0.reserve(lod0.vertices.size());

  for (u32 vi = 0; vi < static_cast<u32>(lod0.vertices.size()); ++vi) {
    const auto &p = lod0.vertices[vi].position;
    Vec3Key key{
        std::bit_cast<u32>(p[0]),
        std::bit_cast<u32>(p[1]),
        std::bit_cast<u32>(p[2]),
    };
    // First occurrence wins (multiple LOD0 verts at same position are fine;
    // any of them is a valid remap target).
    pos_to_lod0.try_emplace(key, vi);
  }

  RemapResult out;
  out.indices.reserve(lod_n.indices.size());
  out.exact = true;

  // We only need the unique set of vertices referenced by lod_n's indices.
  // Build a local remap table so we don't redo the lookup per-index.
  std::vector<u32> lod_n_to_lod0(lod_n.vertices.size(),
                                 std::numeric_limits<u32>::max());

  for (u32 vi = 0; vi < static_cast<u32>(lod_n.vertices.size()); ++vi) {
    const auto &p = lod_n.vertices[vi].position;
    Vec3Key key{
        std::bit_cast<u32>(p[0]),
        std::bit_cast<u32>(p[1]),
        std::bit_cast<u32>(p[2]),
    };

    if (const auto it = pos_to_lod0.find(key); it != pos_to_lod0.end()) {
      lod_n_to_lod0[vi] = it->second;
    } else {
      // Nearest-neighbour fallback. This is O(N) per unmatched vertex but
      // should only fire if the author slightly perturbed positions between
      // LOD levels (rare; log a warning).
      out.exact = false;
      f32 best_dist_sq = std::numeric_limits<f32>::max();
      u32 best_idx = 0;
      for (u32 li = 0; li < static_cast<u32>(lod0.vertices.size()); ++li) {
        const auto &lp = lod0.vertices[li].position;
        const f32 dx = p[0] - lp[0];
        const f32 dy = p[1] - lp[1];
        const f32 dz = p[2] - lp[2];
        const f32 dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq < best_dist_sq) {
          best_dist_sq = dist_sq;
          best_idx = li;
        }
      }
      lod_n_to_lod0[vi] = best_idx;
    }
  }

  for (const u32 idx : lod_n.indices)
    out.indices.push_back(lod_n_to_lod0[idx]);

  return out;
}

} // namespace dy::mesh::detail
