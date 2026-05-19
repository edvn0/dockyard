#pragma once

#include <dockyard/types.hpp>
#include <optional>
#include <vector>

namespace dy {

struct FreeListPool {
  u32 base_slot = 0;
  u32 capacity = 0;
  u32 next = 0;
  bool needs_grow = false;

  std::vector<u32> free_slots;

  [[nodiscard]] auto alloc() -> std::optional<u32> {
    if (!free_slots.empty()) {
      u32 slot = free_slots.back();
      free_slots.pop_back();
      return slot;
    }
    if (next >= capacity) {
      needs_grow = true;
      return std::nullopt;
    }
    return base_slot + next++;
  }

  auto free(u32 slot) -> void { free_slots.emplace_back(slot); }
};

} // namespace dy