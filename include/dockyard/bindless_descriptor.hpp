#pragma once

#include <cassert>
#include <vector>
#include <volk.h>

#include <dockyard/bindless_pool_entries.hpp>
#include <dockyard/log.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vk_check.hpp>

namespace dy {

struct BindlessCaps {
  u32 max_textures;
  u32 max_samplers;
  u32 max_storage_images;
  u32 max_accel_structs;
};

auto query_bindless_caps(VkPhysicalDevice pd) -> BindlessCaps;

struct BindlessSet {
private:
  bool need_repopulate = false;

public:
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;

  u32 max_textures = 1U;
  u32 max_samplers = 1U;
  u32 max_comparison_samplers = 1U;
  u32 max_storage_images = 1U;
  u32 max_accel_structs = 0U;
  u32 max_cubemaps = 1U;
  u32 max_3d_images = 1U;
  u32 max_2d_arrays = 1U;
  u32 max_sub_images = 1U;

  VkDevice device = VK_NULL_HANDLE;
  BindlessCaps caps{};

  auto init(VkDevice dev, BindlessCaps const &caps_init, u32 initial_textures,
            u32 initial_samplers, u32 initial_comparison_samplers,
            u32 initial_storage_images, u32 initial_accel_structs,
            u32 initial_sub_images) -> void;

  auto destroy() -> void;

  // Rebuilds the descriptor set from the live pools. Returns true if the
  // set was recreated (layouts changed), in which case the caller must skip
  // the current frame and re-record command buffers.
  auto repopulate_if_needed(TexturePool &, SamplerPool &,
                            ComparisonSamplerPool &, SubImagePool &) -> bool;

  auto grow_if_needed(u32 req_textures, u32 req_samplers, u32 req_storage,
                      u32 req_accel, u32 req_sub_images) -> bool;

  auto mark_dirty() -> void { need_repopulate = true; }

private:
  auto recreate() -> void;
};

} // namespace dy