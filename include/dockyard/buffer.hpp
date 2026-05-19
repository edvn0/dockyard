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
  VkBufferUsageFlags usage_flags{};
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

  template <typename T> auto for_each_with_flush(auto &&func) {
    if (mapped_data) {
      auto *typed_ptr = static_cast<T *>(mapped_data);
      auto count = allocation_info.size / sizeof(T);
      for (size_t i = 0; i < count; ++i) {
        func(typed_ptr[i]);
      }
      vmaFlushAllocation(allocator, allocation, 0, allocation_info.size);
    }
  }

  auto upload_with_offset(std::ranges::contiguous_range auto &&range,
                          usize byte_offset) {
    const auto bytes = std::ranges::size(range) *
                       sizeof(std::ranges::range_value_t<decltype(range)>);
    assert(byte_offset + bytes <= allocation_info.size &&
           "Buffer overflow in upload");

    if (mapped_data) {
      u8 *destination = static_cast<u8 *>(mapped_data) + byte_offset;
      std::memcpy(destination, std::ranges::data(range), bytes);
    }
  }

  [[nodiscard]] auto get_mapped_pointer() const { return mapped_data; }
  [[nodiscard]] auto get_device_address() const { return address; }
  [[nodiscard]] auto get_allocation_info() const -> const auto & {
    return allocation_info;
  }
  [[nodiscard]] constexpr auto size() const { return allocation_info.size; }
  [[nodiscard]] auto get_allocation() const -> const auto & {
    return allocation;
  }
  [[nodiscard]] auto get_buffer() const -> const auto & { return buffer; }
  [[nodiscard]] auto get_usage_flags() const -> const auto & {
    return usage_flags;
  }

  static auto create(VmaAllocator allocator, VkDeviceSize size,
                     VkBufferUsageFlags usage) -> std::unique_ptr<Buffer>;
};
} // namespace dy