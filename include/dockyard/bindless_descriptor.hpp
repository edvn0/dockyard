#pragma once

#include <cassert>
#include <vector>
#include <volk.h>

#include <dockyard/log.hpp>
#include <dockyard/texture.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vk_check.hpp>

// ---------------------------------------------------------------------------
// Handle<Tag>
//
// Packs a 20-bit slot index and a 12-bit generation into a single u32.
// Adjust the split via k_index_bits if you need >1M slots.
//
//   bits [0 .. k_index_bits)        → slot index  (max 1 048 575 live objects)
//   bits [k_index_bits .. 32)       → generation  (wraps at 4096)
//
// Generation 0 is reserved for the "empty" sentinel so that a zero-initialised
// handle is always invalid regardless of what slot 0 contains.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Pool<Tag, Impl>
//
// Generation contract:
//   odd  generation  → slot is live
//   even generation  → slot is dead / on free-list
//   0                → impossible in a live handle (reserved for empty)
//
// Slot gen starts at 1 (live after first create).
// destroy() increments to 2 (dead).
// Next create() increments to 3 (live again), and so on.
// ---------------------------------------------------------------------------

template <typename Tag, typename Impl> class Pool {
  static constexpr u32 k_sentinel = 0xffff'ffffu;

public:
  using handle_type = Handle<Tag>;

  struct Slot {
    Impl object = {};
    u32 gen = 0u; // 0 = never used; 1 = first live
    u32 next_free = k_sentinel;
  };

  // -----------------------------------------------------------------------
  // Mutation
  // -----------------------------------------------------------------------

  auto create(Impl obj) -> handle_type {
    u32 idx = 0u;

    if (free_head_ != k_sentinel) {
      idx = free_head_;
      free_head_ = slots_[idx].next_free;
      slots_[idx].next_free = k_sentinel;
      slots_[idx].gen += 1u; // even → odd: mark live
      slots_[idx].object = std::move(obj);
    } else {
      idx = static_cast<u32>(slots_.size());
      auto &slot = slots_.emplace_back();
      slot.gen = 1u; // first use
      slot.object = std::move(obj);
    }

    ++num_live_;
    return handle_type{idx, slots_[idx].gen};
  }

  auto destroy(handle_type h) -> void {
    if (h.empty())
      return;

    const u32 idx = h.index();
    assert(idx < slots_.size() && "index out of range");
    assert((slots_[idx].gen & 1u) == 1u && "slot is already dead");
    assert(h.gen() == slots_[idx].gen && "stale handle / double-destroy");

    slots_[idx].object = Impl{};
    slots_[idx].gen += 1u; // odd → even: mark dead
    slots_[idx].next_free = free_head_;
    free_head_ = idx;
    --num_live_;
  }

  // -----------------------------------------------------------------------
  // Access
  // -----------------------------------------------------------------------

  [[nodiscard]] auto get(handle_type h) -> Impl * {
    if (h.empty())
      return nullptr;
    const u32 idx = h.index();
    assert(idx < slots_.size() && "index out of range");
    assert(h.gen() == slots_[idx].gen && "stale handle");
    return &slots_[idx].object;
  }

  [[nodiscard]] auto get(handle_type h) const -> const Impl * {
    if (h.empty())
      return nullptr;
    const u32 idx = h.index();
    assert(idx < slots_.size() && "index out of range");
    assert(h.gen() == slots_[idx].gen && "stale handle");
    return &slots_[idx].object;
  }

  // Reconstruct a valid handle from a raw slot index.
  // Only safe to call when you know the slot is live (e.g. iterating data()
  // after an is_live() check, or fetching the dummy slot at index 0).
  [[nodiscard]] auto handle_at(u32 idx) const -> handle_type {
    assert(idx < slots_.size() && "index out of range");
    assert(is_live(idx) && "slot is not live");
    return handle_type{idx, slots_[idx].gen};
  }

  [[nodiscard]] auto is_live(u32 idx) const -> bool {
    return idx < slots_.size() && (slots_[idx].gen & 1u) == 1u;
  }

  [[nodiscard]] auto data() const -> const std::span<const Slot> {
    return slots_;
  }
  [[nodiscard]] auto mutable_data() -> std::span<Slot> {
    return std::span(slots_);
  }
  [[nodiscard]] auto num_objects() const -> u32 { return num_live_; }
  [[nodiscard]] auto capacity() const -> u32 {
    return static_cast<u32>(slots_.size());
  }

  auto clear() -> void {
    slots_.clear();
    free_head_ = k_sentinel;
    num_live_ = 0u;
  }

private:
  std::vector<Slot> slots_;
  u32 free_head_ = k_sentinel;
  u32 num_live_ = 0u;
};

// ---------------------------------------------------------------------------
// Entry shapes stored inside pool slots
//
// operator== is required by Pool::Slot to reset on destroy().
// ---------------------------------------------------------------------------

struct TextureEntry {
  Texture texture{};
  VkImageViewType sampled_view_type{VK_IMAGE_VIEW_TYPE_2D};
  auto operator==(const TextureEntry &) const -> bool = default;
};

struct SamplerEntry {
  VkSampler sampler = VK_NULL_HANDLE;
  auto operator==(const SamplerEntry &) const -> bool = default;
};

// ---------------------------------------------------------------------------
// Tag types and typed handles
// ---------------------------------------------------------------------------

struct TextureTag {};
struct SamplerTag {};
struct ComparisonSamplerTag {};

using TextureHandle = Handle<TextureTag>;
using SamplerHandle = Handle<SamplerTag>;
using ComparisonSamplerHandle = Handle<ComparisonSamplerTag>;

// ---------------------------------------------------------------------------
// Pool aliases
// ---------------------------------------------------------------------------

using TexturePool = Pool<TextureTag, TextureEntry>;
using SamplerPool = Pool<SamplerTag, SamplerEntry>;
using ComparisonSamplerPool = Pool<ComparisonSamplerTag, SamplerEntry>;

// ---------------------------------------------------------------------------
// BindlessCaps + query
// ---------------------------------------------------------------------------

struct BindlessCaps {
  u32 max_textures;
  u32 max_samplers;
  u32 max_storage_images;
  u32 max_accel_structs;
};

inline auto query_bindless_caps(VkPhysicalDevice pd) -> BindlessCaps {
  VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_props{};
  accel_props.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceVulkan12Properties props12{};
  props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
  props12.pNext = &accel_props;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &props12;
  vkGetPhysicalDeviceProperties2(pd, &props2);

  return BindlessCaps{
      .max_textures = props12.maxDescriptorSetUpdateAfterBindSampledImages,
      .max_samplers = props12.maxDescriptorSetUpdateAfterBindSamplers,
      .max_storage_images =
          props12.maxDescriptorSetUpdateAfterBindStorageImages,
      .max_accel_structs =
          accel_props.maxPerStageDescriptorAccelerationStructures,
  };
}

namespace detail {

inline auto is_cubemap_view(VkImageViewType t) -> bool {
  return t == VK_IMAGE_VIEW_TYPE_CUBE || t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
}

inline auto is_3d_view(VkImageViewType t) -> bool {
  return t == VK_IMAGE_VIEW_TYPE_3D;
}

inline auto is_depth_format(VkFormat f) -> bool {
  switch (f) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D16_UNORM_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
    return true;
  default:
    return false;
  }
}

} // namespace detail

struct PendingTextureWrite {
  u32 pool_index;
  VkImageView sampled_view; // VK_NULL_HANDLE → use dummy
  VkImageView storage_view; // VK_NULL_HANDLE → use dummy
  VkImageViewType view_type;
};

// ---------------------------------------------------------------------------
// Descriptor set bindings
//
//  0 — SAMPLED_IMAGE                (2D textures)
//  1 — SAMPLER                      (samplers)
//  2 — STORAGE_IMAGE                (storage images)
//  3 — SAMPLER                      (comparison samplers)
//  4 — SAMPLED_IMAGE                (cubemaps)
//  5 — SAMPLED_IMAGE                (3D images)
//  6 — ACCELERATION_STRUCTURE_KHR   (optional, only when max_accel_structs > 0)
// ---------------------------------------------------------------------------

struct BindlessSet {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;

  u32 max_textures = 1u;
  u32 max_samplers = 1u;
  u32 max_comparison_samplers = 1u;
  u32 max_storage_images = 1u;
  u32 max_accel_structs = 0u;
  u32 max_cubemaps = 1u;
  u32 max_3d_images = 1u;

  bool need_repopulate = false;

  VkDevice device = VK_NULL_HANDLE;
  BindlessCaps caps{};

  std::vector<PendingTextureWrite> pending_texture_writes;

  auto init(VkDevice dev, BindlessCaps const &caps_init, u32 initial_textures,
            u32 initial_samplers, u32 initial_comparison_samplers,
            u32 initial_storage_images, u32 initial_accel_structs) -> void;

  // Destroys all Vulkan objects; safe to call multiple times.
  auto destroy() -> void;

  // -----------------------------------------------------------------------
  // Per-frame update
  // -----------------------------------------------------------------------

  // Call at the top of each frame before recording draw commands.
  // Returns true if the set was recreated (i.e. all bound pipelines that
  // reference the old layout are now invalid and must be re-bound).
  auto repopulate_if_needed(TexturePool &textures, SamplerPool &samplers,
                            ComparisonSamplerPool &comparison_samplers) -> bool;

  // -----------------------------------------------------------------------
  // Incremental writes
  // -----------------------------------------------------------------------

  // Push a single texture slot update into the pending queue.
  // Flushed automatically by repopulate_if_needed when need_repopulate
  // is false, or discarded when a full repopulate is scheduled.
  auto queue_texture_write(u32 pool_index, VkImageView sampled_view,
                           VkImageView storage_view, VkImageViewType view_type)
      -> void {
    pending_texture_writes.push_back({
        .pool_index = pool_index,
        .sampled_view = sampled_view,
        .storage_view = storage_view,
        .view_type = view_type,
    });
  }

  // -----------------------------------------------------------------------
  // Growth
  // -----------------------------------------------------------------------

  // Expands capacities (doubling strategy, clamped to caps) when any
  // requested count exceeds the current maximum.  Triggers a recreate and
  // sets need_repopulate.  Returns true if the set was recreated.
  auto grow_if_needed(u32 req_textures, u32 req_samplers, u32 req_storage,
                      u32 req_accel) -> bool;

private:
  // Flushes pending_texture_writes via targeted vkUpdateDescriptorSets
  // calls instead of a full repopulate.
  auto flush_pending_writes(VkImageView dummy_sampled,
                            VkImageView dummy_storage) -> void;

  // Destroys and rebuilds layout / pool / set from current max_* values.
  auto recreate() -> void;
};

} // namespace dy
