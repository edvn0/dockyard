#pragma once

#include <volk.h>

#include <cassert>
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

  template <typename T> [[nodiscard]] auto read() const -> T {
    return read_with_offset<T>(0);
  }

  template <typename T>
  [[nodiscard]] auto read_with_offset(usize byte_offset) const -> T {
    assert(
        mapped_data != nullptr &&
        "Attempted to read from a buffer that is not host-visible or mapped");

    assert(byte_offset + sizeof(T) <= allocation_info.size &&
           "Buffer overflow: read extent exceeds total allocation size");

    assert(reinterpret_cast<uintptr_t>(static_cast<const u8 *>(mapped_data) +
                                       byte_offset) %
                   alignof(T) ==
               0 &&
           "Misaligned read: memory address is not a multiple of the requested "
           "type alignment");

    T value;
    std::memcpy(&value, static_cast<const u8 *>(mapped_data) + byte_offset,
                sizeof(T));
    return value;
  }

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
                          usize byte_offset, bool flush = false) {
    const auto bytes = std::ranges::size(range) *
                       sizeof(std::ranges::range_value_t<decltype(range)>);
    assert(byte_offset + bytes <= allocation_info.size &&
           "Buffer overflow in upload");

    if (mapped_data) {
      u8 *destination = static_cast<u8 *>(mapped_data) + byte_offset;
      std::memcpy(destination, std::ranges::data(range), bytes);
    }

    if (flush) {
      vmaFlushAllocation(allocator, allocation, byte_offset, bytes);
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