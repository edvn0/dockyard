#include <dockyard/bindless_descriptor.hpp>

#include <dockyard/vk_check.hpp>

namespace dy {

using namespace detail;

auto BindlessSet::init(VkDevice dev, BindlessCaps const &caps_init,
                       u32 initial_textures, u32 initial_samplers,
                       u32 initial_comparison_samplers,
                       u32 initial_storage_images, u32 initial_accel_structs)
    -> void {
  device = dev;
  caps = caps_init;

  max_textures = std::min(initial_textures, caps.max_textures);
  max_samplers = std::min(initial_samplers, caps.max_samplers);
  max_comparison_samplers =
      std::min(initial_comparison_samplers, caps.max_samplers);
  max_storage_images =
      std::min(initial_storage_images, caps.max_storage_images);
  max_accel_structs = std::min(initial_accel_structs, caps.max_accel_structs);
  // cubemaps and 3D images share the sampled-image cap
  max_cubemaps = std::min(initial_textures, caps.max_textures);
  max_3d_images = std::min(initial_storage_images, caps.max_textures);

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

// ---------------------------------------------------------------------------
// grow_if_needed
// ---------------------------------------------------------------------------

auto BindlessSet::grow_if_needed(u32 req_textures, u32 req_samplers,
                                 u32 req_storage, u32 req_accel) -> bool {
  bool grew = false;

  auto grow_and_clamp = [&](u32 &current, u32 requested, u32 cap) {
    if (requested <= current)
      return;
    const u32 clamped = std::min(std::max(current * 2u, requested), cap);
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

  if (!grew && layout != VK_NULL_HANDLE)
    return false;

  info("BindlessSet growing: textures={} samplers={} comparison_samplers={} "
       "storage={} cubemaps={} 3d={} accel={}",
       max_textures, max_samplers, max_comparison_samplers, max_storage_images,
       max_cubemaps, max_3d_images, max_accel_structs);

  destroy();
  need_repopulate = true;
  recreate();
  return true;
}

// ---------------------------------------------------------------------------
// flush_pending_writes  (incremental path)
// ---------------------------------------------------------------------------

auto BindlessSet::flush_pending_writes(VkImageView dummy_sampled,
                                       VkImageView dummy_storage) -> void {
  if (pending_texture_writes.empty())
    return;

  // A full repopulate supersedes any pending incremental writes.
  if (need_repopulate) {
    pending_texture_writes.clear();
    return;
  }

  // Any out-of-range slot means we need a full repopulate next frame.
  for (const auto &pw : pending_texture_writes) {
    if (pw.pool_index >= max_textures) {
      need_repopulate = true;
      pending_texture_writes.clear();
      return;
    }
  }

  const u32 n = static_cast<u32>(pending_texture_writes.size());

  // Keep info arrays alive until vkUpdateDescriptorSets returns.
  // Separate vectors per view type so that .back() pointers never
  // invalidate when the vectors grow.
  std::vector<VkDescriptorImageInfo> sampled_infos(n);
  std::vector<VkDescriptorImageInfo> storage_infos(n);
  std::vector<VkDescriptorImageInfo> cubemap_infos;
  std::vector<VkDescriptorImageInfo> image_3d_infos;
  cubemap_infos.reserve(n);
  image_3d_infos.reserve(n);

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(n * 4u);

  for (u32 i = 0u; i < n; ++i) {
    const auto &pw = pending_texture_writes[i];

    const VkImageView sv =
        pw.sampled_view != VK_NULL_HANDLE ? pw.sampled_view : dummy_sampled;
    const VkImageView stv =
        pw.storage_view != VK_NULL_HANDLE ? pw.storage_view : dummy_storage;

    sampled_infos[i] = {VK_NULL_HANDLE, sv, VK_IMAGE_LAYOUT_GENERAL};
    storage_infos[i] = {VK_NULL_HANDLE, stv, VK_IMAGE_LAYOUT_GENERAL};

    // binding 0 — sampled
    {
      VkWriteDescriptorSet wds{};
      wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      wds.dstSet = set;
      wds.dstBinding = 0u;
      wds.dstArrayElement = pw.pool_index;
      wds.descriptorCount = 1u;
      wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      wds.pImageInfo = &sampled_infos[i];
      writes.emplace_back(std::move(wds));
    }

    // binding 2 — storage
    if (pw.pool_index < max_storage_images) {
      VkWriteDescriptorSet wds{};
      wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      wds.dstSet = set;
      wds.dstBinding = 2u;
      wds.dstArrayElement = pw.pool_index;
      wds.descriptorCount = 1u;
      wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      wds.pImageInfo = &storage_infos[i];
      writes.emplace_back(std::move(wds));
    }

    // binding 4 — cubemap
    if (is_cubemap_view(pw.view_type) && pw.pool_index < max_cubemaps) {
      cubemap_infos.push_back(sampled_infos[i]);
      VkWriteDescriptorSet wds{};
      wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      wds.dstSet = set;
      wds.dstBinding = 4u;
      wds.dstArrayElement = pw.pool_index;
      wds.descriptorCount = 1u;
      wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      wds.pImageInfo = &cubemap_infos.back();
      writes.emplace_back(wds);
    }

    // binding 5 — 3D
    if (is_3d_view(pw.view_type) && pw.pool_index < max_3d_images) {
      image_3d_infos.push_back(sampled_infos[i]);
      VkWriteDescriptorSet wds{};
      wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      wds.dstSet = set;
      wds.dstBinding = 5u;
      wds.dstArrayElement = pw.pool_index;
      wds.descriptorCount = 1u;
      wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      wds.pImageInfo = &image_3d_infos.back();
      writes.emplace_back(std::move(wds));
    }
  }

  vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(),
                         0u, nullptr);
  pending_texture_writes.clear();
}

// ---------------------------------------------------------------------------
// repopulate_if_needed  (full path)
// ---------------------------------------------------------------------------

auto BindlessSet::repopulate_if_needed(
    TexturePool &textures, SamplerPool &samplers,
    ComparisonSamplerPool &comparison_samplers) -> bool {
  // Fast path — just flush any pending incremental writes.
  if (!need_repopulate) [[likely]] {
    if (!pending_texture_writes.empty()) {
      const auto &dummy_tex = *textures.get(textures.handle_at(0u));
      const VkImageView ds = dummy_tex.sampled_view;
      const VkImageView dst = dummy_tex.storage_view != VK_NULL_HANDLE
                                  ? dummy_tex.storage_view
                                  : dummy_tex.sampled_view;
      flush_pending_writes(ds, dst);
    }
    return false;
  }

  // Full repopulate — discard stale incremental queue.
  pending_texture_writes.clear();

  // Opportunistically grow to fit the current pool contents.
  const bool did_resize =
      grow_if_needed(textures.num_objects(), samplers.num_objects(),
                     textures.num_objects(), 0u);

  // Dummy entries — slot 0 must always be a valid fallback.
  const auto &dummy_tex = *textures.get(textures.handle_at(0u));
  const auto &dummy_sampler = *samplers.get(samplers.handle_at(0u));
  const VkImageView dummy_sampled = dummy_tex.sampled_view;
  const VkImageView dummy_storage = dummy_tex.storage_view != VK_NULL_HANDLE
                                        ? dummy_tex.storage_view
                                        : dummy_tex.sampled_view;
  const VkSampler dummy_vk_sampler = dummy_sampler.sampler;

  // --- Build info arrays, pre-filled with dummy values ---

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

  // --- Textures (bindings 0, 2, 4, 5) ---
  {
    const u32 tex_cap = static_cast<u32>(textures.data().size());
    const u32 limit = std::min(tex_cap, max_textures);

    for (u32 i = 0u; i < limit; ++i) {
      if (!textures.is_live(i))
        continue;

      const auto &tex = textures.data()[i].object;

      if (tex.sampled_view != VK_NULL_HANDLE)
        sampled_infos[i].imageView = tex.sampled_view;

      if (i < max_storage_images && tex.storage_view != VK_NULL_HANDLE)
        storage_infos[i].imageView = tex.storage_view;

      if (i < max_cubemaps && is_cubemap_view(tex.sampled_view_type) &&
          tex.sampled_view != VK_NULL_HANDLE)
        cubemap_infos[i].imageView = tex.sampled_view;

      if (i < max_3d_images && is_3d_view(tex.sampled_view_type) &&
          tex.sampled_view != VK_NULL_HANDLE)
        image_3d_infos[i].imageView = tex.sampled_view;
    }
  }

  // --- Samplers (binding 1) ---
  {
    const u32 limit =
        std::min(static_cast<u32>(samplers.data().size()), max_samplers);
    for (u32 i = 0u; i < limit; ++i) {
      if (!samplers.is_live(i))
        continue;
      const VkSampler s = samplers.data()[i].object.sampler;
      sampler_infos[i].sampler = s != VK_NULL_HANDLE ? s : dummy_vk_sampler;
    }
  }

  // --- Comparison samplers (binding 3) ---
  {
    const u32 limit =
        std::min(static_cast<u32>(comparison_samplers.data().size()),
                 max_comparison_samplers);
    for (u32 i = 0u; i < limit; ++i) {
      if (!comparison_samplers.is_live(i))
        continue;
      const VkSampler s = comparison_samplers.data()[i].object.sampler;
      comparison_sampler_infos[i].sampler =
          s != VK_NULL_HANDLE ? s : dummy_vk_sampler;
    }
  }

  // --- Issue single batched write ---
  /**
sType
pNext
dstSet
dstBinding
dstArrayElement
descriptorCount
descriptorType
pImageInfo
pBufferInfo
pTexelBufferView
*/

  constexpr auto make_image_read_wds = [](const auto &ptr, auto type,
                                          const auto &set, uint32_t binding,
                                          auto count) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = count,
        .descriptorType = type,
        .pImageInfo = ptr.data(),
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
  };

  const std::array<VkWriteDescriptorSet, 6u> writes{
      {
          make_image_read_wds(sampled_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                              set, 0u, max_textures),
          make_image_read_wds(sampler_infos, VK_DESCRIPTOR_TYPE_SAMPLER, set,
                              1u, max_samplers),
          make_image_read_wds(storage_infos, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                              set, 2u, max_storage_images),
          make_image_read_wds(comparison_sampler_infos,
                              VK_DESCRIPTOR_TYPE_SAMPLER, set, 3u,
                              max_comparison_samplers),
          make_image_read_wds(cubemap_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                              set, 4u, max_cubemaps),
          make_image_read_wds(image_3d_infos, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                              set, 5u, max_3d_images),
      },
  };

  vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(),
                         0u, nullptr);
  need_repopulate = false;
  return did_resize;
}

// ---------------------------------------------------------------------------
// recreate  (private)
// ---------------------------------------------------------------------------

auto BindlessSet::recreate() -> void {
  const bool accel_enabled = max_accel_structs > 0u;

  // --- Descriptor set layout ---

  std::vector<VkDescriptorSetLayoutBinding> bindings{{
      {0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_textures, VK_SHADER_STAGE_ALL,
       nullptr},
      {1u, VK_DESCRIPTOR_TYPE_SAMPLER, max_samplers, VK_SHADER_STAGE_ALL,
       nullptr},
      {2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_storage_images,
       VK_SHADER_STAGE_ALL, nullptr},
      {3u, VK_DESCRIPTOR_TYPE_SAMPLER, max_comparison_samplers,
       VK_SHADER_STAGE_ALL, nullptr},
      {4u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_cubemaps, VK_SHADER_STAGE_ALL,
       nullptr},
      {5u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_3d_images, VK_SHADER_STAGE_ALL,
       nullptr},
  }};

  if (accel_enabled) {
    bindings.push_back({
        6u,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        max_accel_structs,
        VK_SHADER_STAGE_ALL,
        nullptr,
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
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
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

  // --- Pipeline layout ---
  // Owns only the bindless set (set=0).  Wire this into
  // create_main_pipeline_layout() as the bindless_layout argument when you
  // integrate with SceneRenderer; push constant ranges are added there.

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1u;
  plci.pSetLayouts = &layout;
  vk::check(vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout));

  // --- Descriptor pool ---
  // UPDATE_AFTER_BIND_BIT instead of FREE_DESCRIPTOR_SET_BIT because the
  // pool holds exactly one set and is reset wholesale on grow.

  std::vector<VkDescriptorPoolSize> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_textures},
      {VK_DESCRIPTOR_TYPE_SAMPLER, max_samplers},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_storage_images},
      {VK_DESCRIPTOR_TYPE_SAMPLER, max_comparison_samplers},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_cubemaps},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_3d_images},
  }};

  if (accel_enabled) {
    pool_sizes.push_back({
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        max_accel_structs,
    });
  }

  VkDescriptorPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 1u;
  pci.poolSizeCount = static_cast<u32>(pool_sizes.size());
  pci.pPoolSizes = pool_sizes.data();
  vk::check(vkCreateDescriptorPool(device, &pci, nullptr, &pool));

  // --- Allocate the single set ---

  VkDescriptorSetAllocateInfo dai{};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = pool;
  dai.descriptorSetCount = 1u;
  dai.pSetLayouts = &layout;
  vk::check(vkAllocateDescriptorSets(device, &dai, &set));
}
} // namespace dy
