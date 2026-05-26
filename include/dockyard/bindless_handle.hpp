#pragma once

#include <cassert>
#include <dockyard/types.hpp>
#include <span>

namespace dy {

template <typename Tag> struct Handle {
  static constexpr u32 k_index_bits = 20u;
  static constexpr u32 k_gen_bits = 32u - k_index_bits;
  static constexpr u32 k_index_mask = (1u << k_index_bits) - 1u;
  static constexpr u32 k_gen_mask = (1u << k_gen_bits) - 1u;

  Handle() = default;

  Handle(u32 index, u32 gen)
      : handle_value((gen & k_gen_mask) << k_index_bits |
                     (index & k_index_mask)) {
    assert(gen != 0u && "generation 0 is reserved for the empty sentinel");
    assert(index <= k_index_mask);
  }

  [[nodiscard]] auto index() const -> u32 {
    return handle_value & k_index_mask;
  }
  [[nodiscard]] auto gen() const -> u32 { return handle_value >> k_index_bits; }
  [[nodiscard]] auto empty() const -> bool { return handle_value == 0u; }
  [[nodiscard]] auto valid() const -> bool { return !empty(); }

  auto operator==(const Handle &) const -> bool = default;

private:
  u32 handle_value = 0u;
};

template <typename TPool, typename TDestroyer> struct Holder {
  using THandle = TPool::handle_type;

  Holder() = default;

  Holder(TPool &pool, THandle handle, TDestroyer destroyer)
      : pool(&pool), handle(handle), destroyer(std::move(destroyer)) {}

  ~Holder() { reset(); }

  Holder(const Holder &) = delete;
  Holder &operator=(const Holder &) = delete;

  Holder(Holder &&o) noexcept
      : pool(o.pool), handle(o.handle), destroyer(std::move(o.destroyer)) {
    o.pool = nullptr;
  }

  Holder &operator=(Holder &&o) noexcept {
    if (this != &o) {
      reset();
      pool = o.pool;
      handle = o.handle;
      destroyer = std::move(o.destroyer);
      o.pool = nullptr;
    }
    return *this;
  }

  [[nodiscard]] auto get() const -> THandle { return handle; }
  [[nodiscard]] auto operator*() const -> THandle { return handle; }
  [[nodiscard]] auto valid() const -> bool {
    return pool != nullptr && handle.valid();
  }
  explicit operator bool() const { return valid(); }

  auto reset() -> void {
    if (pool && handle.valid()) {
      destroyer();
      pool->destroy(handle);
      handle = THandle{};
      pool = nullptr;
    }
  }

  auto release() -> THandle {
    pool = nullptr;
    return handle;
  }

private:
  TPool *pool = nullptr;
  THandle handle{};
  TDestroyer destroyer;
};

template <typename TPool, typename TDestroyer>
Holder(TPool &, typename TPool::handle_type, TDestroyer)
    -> Holder<TPool, TDestroyer>;

struct VulkanContext;
template <typename T> struct EntryDestroyer {
  static auto destroy(const VulkanContext &ctx, T &val) -> void = delete;
};

template <typename TPool>
auto make_holder(TPool &pool, const VulkanContext &ctx,
                 typename TPool::value_type entry) {
  auto handle = pool.create(std::move(entry));
  return Holder(pool, handle, [&pool, &ctx, handle] {
    if (auto *e = pool.get(handle))
      EntryDestroyer<typename TPool::value_type>::destroy(ctx, *e);
  });
}

template <typename Tag, typename Impl> class Pool {
  static constexpr u32 k_sentinel = 0xffff'ffffU;

public:
  using handle_type = Handle<Tag>;
  using value_type = Impl;

  struct Slot {
    Impl object = {};
    u32 gen = 0u; // 0 = never used; 1 = first live
    u32 next_free = k_sentinel;
  };

  auto create(Impl obj) -> handle_type {
    u32 idx = 0u;

    if (freelist_head != k_sentinel) {
      idx = freelist_head;
      freelist_head = pool_slots[idx].next_free;
      pool_slots[idx].next_free = k_sentinel;
      pool_slots[idx].gen += 1u; // even → odd: mark live
      pool_slots[idx].object = std::move(obj);
    } else {
      idx = static_cast<u32>(pool_slots.size());
      auto &slot = pool_slots.emplace_back();
      slot.gen = 1u; // first use
      slot.object = std::move(obj);
    }

    ++number_alive;
    return handle_type{idx, pool_slots[idx].gen};
  }

  auto destroy(handle_type h) -> void {
    if (h.empty())
      return;

    const u32 idx = h.index();
    assert(idx < pool_slots.size() && "index out of range");
    assert((pool_slots[idx].gen & 1u) == 1u && "slot is already dead");
    assert(h.gen() == pool_slots[idx].gen && "stale handle / double-destroy");

    pool_slots[idx].object = Impl{};
    pool_slots[idx].gen += 1u; // odd → even: mark dead
    pool_slots[idx].next_free = freelist_head;
    freelist_head = idx;
    --number_alive;
  }

  [[nodiscard]] auto get(handle_type h) -> Impl * {
    if (h.empty())
      return nullptr;
    const u32 idx = h.index();
    assert(idx < pool_slots.size() && "index out of range");
    assert(h.gen() == pool_slots[idx].gen && "stale handle");
    return &pool_slots[idx].object;
  }

  [[nodiscard]] auto get(handle_type h) const -> const Impl * {
    if (h.empty())
      return nullptr;
    const u32 idx = h.index();
    assert(idx < pool_slots.size() && "index out of range");
    assert(h.gen() == pool_slots[idx].gen && "stale handle");
    return &pool_slots[idx].object;
  }

  [[nodiscard]] auto handle_at(u32 idx) const -> handle_type {
    assert(idx < pool_slots.size() && "index out of range");
    assert(is_live(idx) && "slot is not live");
    return handle_type{idx, pool_slots[idx].gen};
  }

  [[nodiscard]] auto is_live(u32 idx) const -> bool {
    return idx < pool_slots.size() && (pool_slots[idx].gen & 1u) == 1u;
  }

  [[nodiscard]] auto data() const -> const std::span<const Slot> {
    return pool_slots;
  }
  [[nodiscard]] auto mutable_data() -> std::span<Slot> {
    return std::span(pool_slots);
  }
  [[nodiscard]] auto num_objects() const -> u32 { return number_alive; }
  [[nodiscard]] auto capacity() const -> u32 {
    return static_cast<u32>(pool_slots.size());
  }

  auto find_handle(const Impl &val) const -> handle_type {
    auto ret = std::ranges::find_if(pool_slots, [&val](const Slot &v) {
      return (v.gen & 1U) == 1U && v.object == val;
    });

    if (ret == pool_slots.end()) {
      return handle_type{};
    }

    u32 idx = static_cast<u32>(std::distance(pool_slots.begin(), ret));
    return handle_type{idx, ret->gen};
  }

  auto clear() -> void {
    pool_slots.clear();
    freelist_head = k_sentinel;
    number_alive = 0u;
  }

private:
  std::vector<Slot> pool_slots;
  u32 freelist_head = k_sentinel;
  u32 number_alive = 0u;
};

struct TextureTag {};
struct SamplerTag {};
struct ComparisonSamplerTag {};
struct SubImageTag {};
struct MeshAssetTag {};

using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using ComparisonSamplerHandle = Handle<ComparisonSamplerTag>;
using SubImageHandle = Handle<SubImageTag>;
using MeshAssetHandle = Handle<MeshAssetTag>;

} // namespace dy