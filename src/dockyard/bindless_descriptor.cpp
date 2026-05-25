#include "dockyard/texture.hpp"
#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/vk_check.hpp>

namespace dy {

namespace detail {

inline auto is_cubemap_view(VkImageViewType t) -> bool {
  return t == VK_IMAGE_VIEW_TYPE_CUBE || t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
}

inline auto is_array_view(VkImageViewType t) -> bool {
  return t == VK_IMAGE_VIEW_TYPE_2D_ARRAY || t == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
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

auto BindlessSet::init(VkDevice dev, BindlessCaps const &caps_init,
                       u32 initial_textures, u32 initial_samplers,
                       u32 initial_comparison_samplers,
                       u32 initial_storage_images, u32 initial_accel_structs,
                       u32 initial_sub_images) -> void {
  device = dev;
  caps = caps_init;

  max_textures = std::min(initial_textures, caps.max_textures);
  max_samplers = std::min(initial_samplers, caps.max_samplers);
  max_comparison_samplers =
      std::min(initial_comparison_samplers, caps.max_samplers);
  max_storage_images =
      std::min(initial_storage_images, caps.max_storage_images);
  max_accel_structs = std::min(initial_accel_structs, caps.max_accel_structs);
  max_cubemaps = std::min(initial_textures, caps.max_textures);
  max_3d_images = std::min(initial_storage_images, caps.max_textures);
  max_2d_arrays = std::min(initial_textures, caps.max_textures);
  max_sub_images = std::min(initial_sub_images, caps.max_textures);

  recreate();
  need_repopulate = true;
}

auto BindlessSet::destroy() -> void {
  if (device == VK_NULL_HANDLE)
    return;

  vkDeviceWaitIdle(device);

  if (pool)
    vkDestroyDescriptorPool(device, pool, nullptr);
  if (pipeline_layout)
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
  if (layout)
    vkDestroyDescriptorSetLayout(device, layout, nullptr);

  pool = VK_NULL_HANDLE;
  pipeline_layout = VK_NULL_HANDLE;
  layout = VK_NULL_HANDLE;
  set = VK_NULL_HANDLE;
}

auto BindlessSet::grow_if_needed(u32 req_textures, u32 req_samplers,
                                 u32 req_storage, u32 req_accel,
                                 u32 req_sub_images) -> bool {
  bool grew = false;

  auto grow_and_clamp = [&](u32 &current, u32 requested, u32 cap) {
    if (requested <= current)
      return;
    const u32 clamped = std::min(std::max(current * 2U, requested), cap);
    if (clamped > current) {
      current = clamped;
      grew = true;
    }
  };

  grow_and_clamp(max_textures, req_textures, caps.max_textures);
  grow_and_clamp(max_samplers, req_samplers, caps.max_samplers);
  grow_and_clamp(max_comparison_samplers, req_samplers, caps.max_samplers);
  grow_and_clamp(max_storage_images, req_storage, caps.max_storage_images);
  grow_and_clamp(max_accel_structs, req_accel, caps.max_accel_structs);
  grow_and_clamp(max_cubemaps, req_textures, caps.max_textures);
  grow_and_clamp(max_3d_images, req_textures, caps.max_textures);
  grow_and_clamp(max_2d_arrays, req_textures, caps.max_textures);
  grow_and_clamp(max_sub_images, req_sub_images, caps.max_storage_images);

  if (!grew && layout != VK_NULL_HANDLE)
    return false;

  info("BindlessSet growing: textures={} samplers={} comparison_samplers={} "
       "storage={} cubemaps={} 3d={} accel={} 2d_arrays={} sub_images={}",
       max_textures, max_samplers, max_comparison_samplers, max_storage_images,
       max_cubemaps, max_3d_images, max_accel_structs, max_2d_arrays,
       max_sub_images);

  destroy();
  need_repopulate = true;
  recreate();
  return true;
}

auto BindlessSet::repopulate_if_needed(
    TexturePool &textures, SamplerPool &samplers,
    ComparisonSamplerPool &comparison_samplers, SubImagePool &sub_images)
    -> bool {
  if (!need_repopulate) [[likely]]
    return false;

  const bool did_resize =
      grow_if_needed(textures.num_objects(), samplers.num_objects(),
                     textures.num_objects(), 0u, sub_images.num_objects());

  const auto &dummy_tex = textures.get(textures.handle_at(0u))->texture;
  const auto &dummy_sampler = *samplers.get(samplers.handle_at(0u));
  const VkImageView dummy_sampled = dummy_tex.sampled_view;
  const VkImageView dummy_storage = dummy_tex.storage_view != VK_NULL_HANDLE
                                        ? dummy_tex.storage_view
                                        : dummy_tex.sampled_view;
  const VkSampler dummy_vk_sampler = dummy_sampler.sampler;

  std::vector<VkDescriptorImageInfo> sampled_infos(
      max_textures, {VK_NULL_HANDLE, dummy_sampled, VK_IMAGE_LAYOUT_GENERAL});
  std::vector<VkDescriptorImageInfo> storage_infos(
      max_storage_images,
      {VK_NULL_HANDLE, dummy_storage, VK_IMAGE_LAYOUT_GENERAL});
  std::vector<VkDescriptorImageInfo> sampler_infos(
      max_samplers,
      {dummy_vk_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
  std::vector<VkDescriptorImageInfo> comparison_sampler_infos(
      max_comparison_samplers,
      {dummy_vk_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
  std::vector<VkDescriptorImageInfo> cubemap_infos(
      max_cubemaps, {VK_NULL_HANDLE, dummy_sampled, VK_IMAGE_LAYOUT_GENERAL});
  std::vector<VkDescriptorImageInfo> image_3d_infos(
      max_3d_images, {VK_NULL_HANDLE, dummy_sampled, VK_IMAGE_LAYOUT_GENERAL});
  std::vector<VkDescriptorImageInfo> image_2d_array_infos(
      max_2d_arrays, {VK_NULL_HANDLE, dummy_sampled, VK_IMAGE_LAYOUT_GENERAL});
  std::vector<VkDescriptorImageInfo> sub_image_infos(
      max_sub_images, {VK_NULL_HANDLE, dummy_storage, VK_IMAGE_LAYOUT_GENERAL});

  {
    const u32 limit =
        std::min(static_cast<u32>(textures.data().size()), max_textures);
    for (u32 i = 0U; i < limit; ++i) {
      if (!textures.is_live(i))
        continue;

      const auto &tex = textures.data()[i].object.texture;
      const auto view_type = textures.data()[i].object.sampled_view_type;

      if (tex.sampled_view != VK_NULL_HANDLE)
        sampled_infos[i].imageView = tex.sampled_view;

      if (i < max_storage_images && tex.storage_view != VK_NULL_HANDLE)
        storage_infos[i].imageView = tex.storage_view;

      if (i < max_cubemaps && detail::is_cubemap_view(view_type) &&
          tex.sampled_view != VK_NULL_HANDLE)
        cubemap_infos[i].imageView = tex.sampled_view;

      if (i < max_3d_images && detail::is_3d_view(view_type) &&
          tex.sampled_view != VK_NULL_HANDLE)
        image_3d_infos[i].imageView = tex.sampled_view;

      if (i < max_2d_arrays && detail::is_array_view(view_type) &&
          tex.sampled_view != VK_NULL_HANDLE)
        image_2d_array_infos[i].imageView = tex.sampled_view;
    }
  }

  {
    const u32 limit =
        std::min(static_cast<u32>(samplers.data().size()), max_samplers);
    for (u32 i = 0U; i < limit; ++i) {
      if (!samplers.is_live(i))
        continue;
      const VkSampler s = samplers.data()[i].object.sampler;
      sampler_infos[i].sampler = s != VK_NULL_HANDLE ? s : dummy_vk_sampler;
    }
  }

  {
    const u32 limit =
        std::min(static_cast<u32>(comparison_samplers.data().size()),
                 max_comparison_samplers);
    for (u32 i = 0U; i < limit; ++i) {
      if (!comparison_samplers.is_live(i))
        continue;
      const VkSampler s = comparison_samplers.data()[i].object.sampler;
      comparison_sampler_infos[i].sampler =
          s != VK_NULL_HANDLE ? s : dummy_vk_sampler;
    }
  }

  {
    const u32 limit = std::min(sub_images.capacity(), max_sub_images);
    const auto data = sub_images.data();
    for (u32 i = 0U; i < limit; ++i) {
      if (!sub_images.is_live(i))
        continue;
      const VkImageView v = data[i].object.view;
      if (v != VK_NULL_HANDLE)
        sub_image_infos[i].imageView = v;
    }
  }

  constexpr auto make_wds = [](const auto &infos, VkDescriptorType type,
                               VkDescriptorSet dst_set, u32 binding,
                               u32 count) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = dst_set,
        .dstBinding = binding,
        .dstArrayElement = 0U,
        .descriptorCount = count,
        .descriptorType = type,
        .pImageInfo = infos.data(),
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
  };

  const std::array<VkWriteDescriptorSet, 8U> writes{{
      make_wds(sampled_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set, 0U,
               max_textures),
      make_wds(sampler_infos, VK_DESCRIPTOR_TYPE_SAMPLER, set, 1U,
               max_samplers),
      make_wds(storage_infos, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set, 2U,
               max_storage_images),
      make_wds(comparison_sampler_infos, VK_DESCRIPTOR_TYPE_SAMPLER, set, 3U,
               max_comparison_samplers),
      make_wds(cubemap_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set, 4U,
               max_cubemaps),
      make_wds(image_3d_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set, 5U,
               max_3d_images),
      make_wds(image_2d_array_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set, 7U,
               max_2d_arrays),
      make_wds(sub_image_infos, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set, 8U,
               max_sub_images),
  }};

  vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(),
                         0U, nullptr);
  need_repopulate = false;
  return did_resize;
}

auto BindlessSet::recreate() -> void {
  const bool accel_enabled = max_accel_structs > 0U;

  std::vector<VkDescriptorSetLayoutBinding> bindings{{
      {
          .binding = 0U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = max_textures,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 1U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
          .descriptorCount = max_samplers,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 2U,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = max_storage_images,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 3U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
          .descriptorCount = max_comparison_samplers,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 4U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = max_cubemaps,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 5U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = max_3d_images,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 7U,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = max_2d_arrays,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
      {
          .binding = 8U,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = max_sub_images,
          .stageFlags = VK_SHADER_STAGE_ALL,
          .pImmutableSamplers = nullptr,
      },
  }};

  if (accel_enabled) {
    bindings.push_back({
        .binding = 6U,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .descriptorCount = max_accel_structs,
        .stageFlags = VK_SHADER_STAGE_ALL,
        .pImmutableSamplers = nullptr,
    });
  }

  const VkDescriptorBindingFlags per_binding_flags =
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
      VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

  std::vector<VkDescriptorBindingFlags> binding_flags(bindings.size(),
                                                      per_binding_flags);

  VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{};
  bfci.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  bfci.bindingCount = static_cast<u32>(binding_flags.size());
  bfci.pBindingFlags = binding_flags.data();

  const VkDescriptorSetLayoutCreateInfo lci{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .pNext = &bfci,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
      .bindingCount = static_cast<u32>(bindings.size()),
      .pBindings = bindings.data(),
  };
  vk::check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout));

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1U;
  plci.pSetLayouts = &layout;
  vk::check(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout));

  std::vector<VkDescriptorPoolSize> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_textures},
      {VK_DESCRIPTOR_TYPE_SAMPLER, max_samplers},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_storage_images},
      {VK_DESCRIPTOR_TYPE_SAMPLER, max_comparison_samplers},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_cubemaps},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_3d_images},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_2d_arrays},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_sub_images},
  }};

  if (accel_enabled) {
    pool_sizes.push_back({
        .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .descriptorCount = max_accel_structs,
    });
  }

  VkDescriptorPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 1U;
  pci.poolSizeCount = static_cast<u32>(pool_sizes.size());
  pci.pPoolSizes = pool_sizes.data();
  vk::check(vkCreateDescriptorPool(device, &pci, nullptr, &pool));

  VkDescriptorSetAllocateInfo dai{};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = pool;
  dai.descriptorSetCount = 1U;
  dai.pSetLayouts = &layout;
  vk::check(vkAllocateDescriptorSets(device, &dai, &set));
}

auto query_bindless_caps(VkPhysicalDevice pd) -> BindlessCaps {
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

} // namespace dy