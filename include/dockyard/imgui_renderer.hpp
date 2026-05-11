#pragma once

#include "dockyard/pipeline_builder.hpp"
#include "dockyard/vfs_path.hpp"
#include <dockyard/buffer.hpp>
#include <dockyard/context.hpp>
#include <dockyard/types.hpp>

#include <volk.h>

#include <filesystem>
#include <string_view>
#include <tuple>
#include <vector>

#include <imgui.h>

namespace dy {

struct FontChoice {
  VFSPath font_path;
  f32 size{20.0F};
};

using ImGuiFramebuffer = std::tuple<VkExtent2D, VkFormat>;

class ImGuiRenderer {
public:
  ImGuiRenderer(GLFWwindow *main_window, u32 initial_slot_count,
                SceneRenderer &, FontChoice);
  ~ImGuiRenderer();

  ImGuiRenderer(ImGuiRenderer &&) = delete;
  auto operator=(ImGuiRenderer &&) -> ImGuiRenderer & = delete;

  auto update_font(FontChoice) -> void;
  auto set_app_name(std::string_view) -> void;

  auto begin_frame(ImGuiFramebuffer main_fb) -> void;

  auto render(VkCommandBuffer cmd) -> void;
  auto end_frame() -> void;

  auto set_should_recompile() -> void { force_recompile_primary = true; }

private:
  struct DrawableData {
    std::unique_ptr<Buffer> vertex;
    std::unique_ptr<Buffer> index;
    u32 index_count{0};
    u32 vertex_count{0};
  };

  PipelineHandle main_pipeline{};
  VkPipelineLayout pipeline_layout{};
  SamplerHandle sampler{};
  TextureHandle font_texture{};

  std::string config_name{"imgui.ini"};
  std::unique_ptr<VFSPath> config_path;

  SceneRenderer &renderer;

  f32 display_scale{1.0F};

  std::vector<DrawableData> drawables{};
  u32 slots_per_frame{0}; // how many slots we budget per frame
  u32 slot_cursor{0};     // how many slots used THIS frame
  u32 frame_cursor{0};    // which frame-in-flight slot base we're on

  bool force_recompile_primary{false};

private:
  auto render_draw_data(VkCommandBuffer cmd, ImDrawData *dd,
                        const PipelineHandle &) -> void;
  auto acquire_draw_slot() -> DrawableData &;
};

} // namespace dy
