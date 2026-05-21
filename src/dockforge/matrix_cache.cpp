#include <dockforge/matrix_cache.hpp>

#include <array>
#include <dockyard/types.hpp>

namespace {

using namespace dy;

// Aligned to 128 bytes (exactly 2 cache lines).
// The lookup keys occupy the first cache line; the heavy mat4 occupies the
// second.
struct alignas(128) MatrixCacheEntry {
  entt::entity entity = entt::null; // 4 bytes
  glm::vec3 position{};             // 12 bytes
  glm::quat rotation{1, 0, 0, 0};   // 16 bytes
  glm::vec3 scale{1, 1, 1};         // 12 bytes
  u32 _pad[5] = {0}; // 20 bytes padding to push 'mat' to 64-byte alignment

  glm::mat4 mat{1.0F}; // 64 bytes
};
static_assert(sizeof(MatrixCacheEntry) == 128,
              "MatrixCacheEntry must be exactly 2 cache lines.");

constexpr usize matrix_cache_size = 10242;
std::array<MatrixCacheEntry, matrix_cache_size> matrix_cache{};

[[nodiscard]] inline auto trs_matches(const MatrixCacheEntry &entry,
                                      const Components::Transform &t) -> bool {
  const auto &[pos, rot, scl] = t.get();
  return entry.position == pos && entry.rotation == rot && entry.scale == scl;
}

[[nodiscard]] inline auto compute_matrix(const Components::Transform &t)
    -> glm::mat4 {
  return t.matrix();
}

[[nodiscard]] auto cached_matrix_impl(entt::entity e,
                                      const Components::Transform &t)
    -> const glm::mat4 & {

  const usize cache_index = static_cast<usize>(e) % matrix_cache_size;
  MatrixCacheEntry &entry = matrix_cache[cache_index];

  // Direct Cache Hit
  if (entry.entity == e) {
    if (!trs_matches(entry, t)) [[unlikely]] {
      entry.mat = compute_matrix(t);
      const auto &[pos, rot, scl] = t.get();
      entry.position = pos;
      entry.rotation = rot;
      entry.scale = scl;
    }
    return entry.mat;
  }

  // Cache Miss / Collision: Instantly evict the previous occupant.
  // No LRU scans or frame timestamps needed.
  entry.entity = e;
  entry.mat = compute_matrix(t);
  const auto &[pos, rot, scl] = t.get();
  entry.position = pos;
  entry.rotation = rot;
  entry.scale = scl;

  return entry.mat;
}

} // namespace

auto cached_matrix(entt::entity e, const Components::Transform &t)
    -> const glm::mat4 & {
  return cached_matrix_impl(e, t);
}