#include <dockyard/buffer.hpp>

namespace dy {

Buffer::Buffer(VmaAllocator alloc, VkDeviceSize size,
               VkBufferUsageFlags usage):allocator(alloc) {
  VkBufferCreateInfo buffer_ci{};
  buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_ci.size = size;
  buffer_ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo alloc_ci{};
  alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

  vk::check(vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer,
                                &allocation, &allocation_info));

  VmaAllocatorInfo info{};
  vmaGetAllocatorInfo(allocator, &info);

  const auto address_info = VkBufferDeviceAddressInfo{
    .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
    .pNext = nullptr,
    .buffer = buffer,
};
  address =
      DeviceAddress {vkGetBufferDeviceAddress(info.device, &address_info)};
  mapped_data = allocation_info.pMappedData;
}

auto Buffer::create(VmaAllocator allocator, VkDeviceSize size,
                    VkBufferUsageFlags usage) -> std::unique_ptr<Buffer> {
  return  std::unique_ptr<Buffer>{new Buffer {allocator, size, usage}};
}

Buffer::~Buffer() {
  if (buffer)
    vmaDestroyBuffer(allocator, buffer, allocation);
}

}
