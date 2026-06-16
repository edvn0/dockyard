#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_ASSERT_LEAK

#include <volk.h>

/*#define VMA_DEBUG_LOG_FORMAT(format, ...) \
  do {                                                                         \
    printf((format), __VA_ARGS__);                                             \
    printf("\n");                                                              \
  } while (false)*/
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>