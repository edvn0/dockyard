#pragma once

#include "dockyard/bindless_handle.hpp"
#include <dockyard/compiler.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <volk.h>

#include <expected>
#include <string>
#include <variant>
#include <vector>

namespace dy {

// ── Blend state
// ───────────────────────────────────────────────────────────────

struct BlendMode {
  bool enabled = false;
  VkBlendFactor src_color = VK_BLEND_FACTOR_SRC_ALPHA;
  VkBlendFactor dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  VkBlendOp color_op = VK_BLEND_OP_ADD;
  VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst_alpha = VK_BLEND_FACTOR_ZERO;
  VkBlendOp alpha_op = VK_BLEND_OP_ADD;
  VkColorComponentFlags write_mask = 0xFu;

  [[nodiscard]] auto to_vk() const -> VkPipelineColorBlendAttachmentState;

  [[nodiscard]] static constexpr auto opaque() -> BlendMode { return {}; }

  [[nodiscard]] static constexpr auto alpha() -> BlendMode {
    return {
        .enabled = true,
        .src_color = VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .color_op = VK_BLEND_OP_ADD,
        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ZERO,
        .alpha_op = VK_BLEND_OP_ADD,
    };
  }

  [[nodiscard]] static constexpr auto additive() -> BlendMode {
    return {
        .enabled = true,
        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ONE,
        .color_op = VK_BLEND_OP_ADD,
        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ONE,
        .alpha_op = VK_BLEND_OP_ADD,
    };
  }

  [[nodiscard]] static constexpr auto premultiplied() -> BlendMode {
    return {
        .enabled = true,
        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .color_op = VK_BLEND_OP_ADD,
        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alpha_op = VK_BLEND_OP_ADD,
    };
  }
};

struct DepthState {
  bool test = false;
  bool write = false;
  VkCompareOp compare_op = VK_COMPARE_OP_LESS;
};

// ── Render target layout
// ──────────────────────────────────────────────────────

struct RenderTargetLayout {
  std::vector<VkFormat> color_formats{};
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkFormat stencil_format = VK_FORMAT_UNDEFINED;
};

// ── Pipeline descriptions
// ─────────────────────────────────────────────────────

struct GraphicsPipelineDescription {
  VFSPath shader_path; // e.g. "shaders://forward.slang"
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  RenderTargetLayout render_targets{};

  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

  DepthState depth{};
  std::vector<BlendMode> blending{};

  std::vector<VkDynamicState> extra_dynamic_states{};

  f32 line_width{1.0F};
};

struct ComputePipelineDescription {
  VFSPath shader_path;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

using PipelineDescription =
    std::variant<std::monostate, GraphicsPipelineDescription,
                 ComputePipelineDescription>;

struct PipelineTag {};
using PipelineHandle = dy::Handle<PipelineTag>;

struct PipelineRegistry {
  VkDevice device = VK_NULL_HANDLE;

  struct Entry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    bool owns_layout = false;
    PipelineDescription desc{std::monostate{}};
  };

  dy::Pool<PipelineTag, Entry> pool{};

  explicit PipelineRegistry(VkDevice d) : device(d) {}
  PipelineRegistry(const PipelineRegistry &) = delete;
  PipelineRegistry &operator=(const PipelineRegistry &) = delete;

  [[nodiscard]] auto create_graphics(GraphicsPipelineDescription desc)
      -> std::expected<PipelineHandle, std::string>;
  [[nodiscard]] auto create_compute(ComputePipelineDescription desc)
      -> std::expected<PipelineHandle, std::string>;

  auto poll_and_update_dirty_pipelines() -> void;

  [[nodiscard]] auto reload(PipelineHandle h)
      -> std::expected<void, std::string>;
  auto reload_by_shader(const VFSPath &path) -> void;
  auto reload_all() -> void;

  [[nodiscard]] auto get(PipelineHandle h) const -> VkPipeline {
    const auto *e = pool.get(h);
    assert(e && "invalid pipeline handle");
    return e->pipeline;
  }

  [[nodiscard]] auto get_unsafe(u32 h) const -> VkPipeline {
    const auto *e = pool.get(pool.handle_at(h));
    assert(e && "invalid pipeline handle");
    return e->pipeline;
  }

  [[nodiscard]] auto get_entry(PipelineHandle h) const -> const Entry & {
    const auto *e = pool.get(h);
    assert(e && "invalid pipeline handle");
    return *e;
  }

  auto destroy(PipelineHandle) -> void;
  auto destroy(auto &&...h) { (destroy(h), ...); }
  auto cleanup() -> void;
};

} // namespace dy