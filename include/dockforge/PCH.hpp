#pragma once

// Vulkan (must come first — volk.h defines VK_NO_PROTOTYPES-safe dispatch)
#include <volk.h>

// Standard library
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <execution>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Third-party — window/GPU
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

// Third-party — ECS
#include <entt/entt.hpp>

// Third-party — UI (order matters: imgui before ImGuizmo/implot)
#include <imgui.h>
#include <ImGuizmo.h>
#include <implot.h>

// Third-party — profiling
#include <tracy/Tracy.hpp>

// Third-party — math extensions
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/random.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

// Project base types
#include <dockyard/types.hpp>
