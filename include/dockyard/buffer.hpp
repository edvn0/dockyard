#pragma once

#include <volk.h>

#include <cstring>
#include <ranges>
#include <vk_mem_alloc.h>

#include <dockyard/device_geometry.hpp>
#include <dockyard/vk_check.hpp>

namespace dy {
class Buffer {
  VmaAllocator allocator{nullptr};
  VkBuffer buffer{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  VmaAllocationInfo allocation_info{};
  DeviceAddress address{};
  void *mapped_data = nullptr;

  Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);

public:
  ~Buffer();

  auto upload(std::ranges::contiguous_range auto &&range) {
    const auto bytes = std::ranges::size(range) *
                       sizeof(std::ranges::range_value_t<decltype(range)>);
    if (mapped_data) {
      std::memcpy(mapped_data, std::ranges::data(range), bytes);
    }
  }

  [[nodiscard]] auto get_mapped_pointer() const { return mapped_data; }
  [[nodiscard]] auto get_device_address() const { return address; }
  [[nodiscard]] auto get_allocation_info() const -> const auto & {
    return allocation_info;
  }
  [[nodiscard]] auto get_allocation() const -> const auto & {
    return allocation;
  }
  [[nodiscard]] auto get_buffer() const -> const auto & { return buffer; }

  static auto create(VmaAllocator allocator, VkDeviceSize size,
                     VkBufferUsageFlags usage) -> std::unique_ptr<Buffer>;
};
} // namespace dy