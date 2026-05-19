#include <dockforge/matrix_cache.hpp>

#include <dockyard/types.hpp>

namespace {

using namespace dy;

struct alignas(128) MatrixCacheEntry {
  entt::entity entity = entt::null; // 4
  u32 _pad0 = 0;                    // 4  — keeps last_used 8-aligned
  u64 last_used = 0;                // 8
  glm::vec3 position{};             // 12
  u32 _pad1 = 0;                    // 4
  glm::quat rotation{1, 0, 0, 0};   // 16
  glm::vec3 scale{1, 1, 1};         // 12
  u32 _pad2 = 0;                    // 4
  glm::mat4 mat{1.0F};              // 64
                                    // total: 128
};
static_assert(sizeof(MatrixCacheEntry) == 128);

constexpr usize matrix_cache_size = 256;
std::array<MatrixCacheEntry, matrix_cache_size> matrix_cache{};
u64 *matrix_cache_frame{nullptr};

[[nodiscard]] auto trs_matches(const MatrixCacheEntry &entry,
                               const Components::Transform &t) -> bool {
  return entry.position == t.position && entry.rotation == t.rotation &&
         entry.scale == t.scale;
}

[[nodiscard]] auto compute_matrix(const Components::Transform &t) -> glm::mat4 {
  return glm::translate(glm::mat4{1.0F}, t.position) *
         glm::mat4_cast(t.rotation) * glm::scale(glm::mat4{1.0F}, t.scale);
}

[[nodiscard]] auto cached_matrix_impl(entt::entity e,
                                      const Components::Transform &t)
    -> const glm::mat4 & {

  MatrixCacheEntry *lru = matrix_cache.data();

  for (auto &entry : matrix_cache) {
    if (entry.entity == e) {
      if (!trs_matches(entry, t)) {
        // Transform was modified externally — recompute in place
        entry.mat = compute_matrix(t);
        entry.position = t.position;
        entry.rotation = t.rotation;
        entry.scale = t.scale;
      }
      entry.last_used = *matrix_cache_frame;
      return entry.mat;
    }
    if (entry.last_used < lru->last_used)
      lru = &entry;
  }

  // Cache miss: evict LRU slot
  lru->entity = e;
  lru->mat = compute_matrix(t);
  lru->position = t.position;
  lru->rotation = t.rotation;
  lru->scale = t.scale;
  lru->last_used = *matrix_cache_frame;
  return lru->mat;
}

} // namespace

auto cached_matrix(entt::entity e, const Components::Transform &t)
    -> const glm::mat4 & {
  return cached_matrix_impl(e, t);
}

auto initialise_matrix_cache(dy::u64 &frame_counter) -> void {
  matrix_cache_frame = &frame_counter;
}