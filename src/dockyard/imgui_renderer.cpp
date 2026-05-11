#include "dockyard/vfs_path.hpp"
#include <volk.h>

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vk_check.hpp>

#include <backends/imgui_impl_glfw.h>
#include <misc/freetype/imgui_freetype.h>

#include <ImGuizmo.h>
#include <bit>
#include <filesystem>
#include <imgui.h>
#include <implot.h>

#include <fstream>

#include <unordered_map>
#include <vulkan/vulkan_core.h>

namespace dy {

auto create_pipeline(VkPipelineLayout &, SceneRenderer &, VkFormat)
    -> std::expected<PipelineHandle, shader::CompilationError>;

ImGuiRenderer::ImGuiRenderer(GLFWwindow *w, u32 initial_slot_count,
                             SceneRenderer &r, FontChoice font)
    : renderer(r) {

  std::ignore = ImGui::CreateContext();
  std::ignore = ImPlot::CreateContext();

  ImGuiIO &io = ImGui::GetIO();
  io.BackendRendererName = "imgui-custom-vulkan";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

  update_font(std::move(font));
  ImGui_ImplGlfw_InitForVulkan(w, true);
  slots_per_frame = std::max(1u, initial_slot_count);
  drawables.resize(frames_in_flight * slots_per_frame);
}

ImGuiRenderer::~ImGuiRenderer() {
  ImGuiIO &io = ImGui::GetIO();
  io.Fonts->TexID = nullptr;

  if (pipeline_layout)
    vkDestroyPipelineLayout(renderer.ctx.device, pipeline_layout, nullptr);

  ImGui_ImplGlfw_Shutdown();

  ImGui::DestroyPlatformWindows();

  ImPlot::DestroyContext();
  ImGui::DestroyContext();
}

auto ImGuiRenderer::begin_frame(ImGuiFramebuffer fb) -> void {
  const auto &dim = std::get<VkExtent2D>(fb);

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<f32>(dim.width) / display_scale,
                          static_cast<f32>(dim.height) / display_scale);
  io.DisplayFramebufferScale = ImVec2(display_scale, display_scale);
  auto str = VFS::get().resolve(*config_path).string();
  io.IniFilename = str.c_str();

  if (force_recompile_primary || !main_pipeline.valid()) {
    main_pipeline =
        create_pipeline(pipeline_layout, renderer, std::get<1>(fb)).value();
    force_recompile_primary = false;
  }

  slot_cursor = 0;
  frame_cursor = (frame_cursor + 1) % frames_in_flight;

  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();
}

auto ImGuiRenderer::acquire_draw_slot() -> DrawableData & {
  if (slot_cursor >= slots_per_frame) {
    u32 new_slots_per_frame = std::max(slots_per_frame * 2u, slot_cursor + 1u);
    std::vector<DrawableData> new_drawables(frames_in_flight *
                                            new_slots_per_frame);

    for (u32 f = 0; f < frames_in_flight; ++f) {
      for (u32 s = 0; s < slots_per_frame; ++s) {
        new_drawables[f * new_slots_per_frame + s] =
            std::move(drawables[f * slots_per_frame + s]);
      }
    }

    drawables = std::move(new_drawables);
    slots_per_frame = new_slots_per_frame;
  }

  DrawableData &out = drawables[frame_cursor * slots_per_frame + slot_cursor];
  slot_cursor++;
  return out;
}

auto ImGuiRenderer::end_frame() -> void {
  ImGui::EndFrame();
  ImGui::Render();

  if (auto &io = ImGui::GetIO();
      io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
  }
}

auto ImGuiRenderer::render(VkCommandBuffer cmd) -> void {
  render_draw_data(cmd, ImGui::GetDrawData(), main_pipeline);
}

constexpr usize next_power_of_two(usize n) {
  if (n == 0)
    return 1;
  return std::bit_ceil(n);
}

auto ImGuiRenderer::render_draw_data(VkCommandBuffer cmd, ImDrawData *dd,
                                     const PipelineHandle &pipeline) -> void {
  if (!dd || dd->TotalIdxCount == 0) {
    return;
  }

  const float fb_width = dd->DisplaySize.x * dd->FramebufferScale.x;
  const float fb_height = dd->DisplaySize.y * dd->FramebufferScale.y;

  VkViewport vp{
      .x = 0,
      .y = fb_height,
      .width = fb_width,
      .height = -fb_height,
      .minDepth = 0.0F,
      .maxDepth = 1.0F,
  };
  vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
  vkCmdSetDepthBounds(cmd, 0.0F, 1.0F);
  vkCmdSetDepthTestEnable(cmd, VK_FALSE);
  vkCmdSetDepthWriteEnable(cmd, VK_FALSE);

  vkCmdSetViewport(cmd, 0, 1, &vp);

  const float L = dd->DisplayPos.x;
  const float R = dd->DisplayPos.x + dd->DisplaySize.x;
  const float T = dd->DisplayPos.y;
  const float B = dd->DisplayPos.y + dd->DisplaySize.y;
  const ImVec2 clip_offset = dd->DisplayPos;
  const ImVec2 clip_scale = dd->FramebufferScale;

  DrawableData &drawable = acquire_draw_slot();

  if (static_cast<i32>(drawable.index_count) < dd->TotalIdxCount) {
    const auto size =
        static_cast<usize>(dd->TotalIdxCount * 4) * sizeof(ImDrawIdx);
    const auto actual_size = static_cast<std::size_t>(next_power_of_two(size));
    info("(ImGui) Reallocating index buffer to {} bytes", actual_size);
    drawable.index = Buffer::create(renderer.ctx.allocator, actual_size,
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    drawable.index_count = static_cast<u32>(actual_size / sizeof(ImDrawIdx));
  }
  if (static_cast<i32>(drawable.vertex_count) < dd->TotalVtxCount) {
    const auto size =
        static_cast<usize>(dd->TotalVtxCount * 4) * sizeof(ImDrawVert);
    const auto actual_size = static_cast<std::size_t>(next_power_of_two(size));
    info("(ImGui) Reallocating vertex buffer to {} bytes", actual_size);
    drawable.vertex = Buffer::create(renderer.ctx.allocator, actual_size,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    drawable.vertex_count = static_cast<u32>(actual_size / sizeof(ImDrawVert));
  }

  {
    std::vector<ImDrawVert> all_vtx;
    std::vector<ImDrawIdx> all_itx;

    all_vtx.reserve(static_cast<std::size_t>(dd->TotalVtxCount));
    all_itx.reserve(static_cast<std::size_t>(dd->TotalIdxCount));

    for (int n = 0; n < dd->CmdListsCount; n++) {
      const auto *imgui_cmd = dd->CmdLists[n];
      all_vtx.insert(all_vtx.end(), imgui_cmd->VtxBuffer.Data,
                     imgui_cmd->VtxBuffer.Data + imgui_cmd->VtxBuffer.Size);
      all_itx.insert(all_itx.end(), imgui_cmd->IdxBuffer.Data,
                     imgui_cmd->IdxBuffer.Data + imgui_cmd->IdxBuffer.Size);
    }

    drawable.vertex->upload(all_vtx);
    drawable.index->upload(all_itx);
  }

  auto pipe = renderer.pipeline_registry->get(pipeline);

  vkCmdBindIndexBuffer(cmd, drawable.index->get_buffer(), 0,
                       VK_INDEX_TYPE_UINT16);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0, 1, &renderer.bindless.set, 0, nullptr);

  u32 index_offset = 0;
  u32 vertex_offset = 0;

  for (int n = 0; n < dd->CmdListsCount; n++) {
    const auto *command_list = dd->CmdLists[n];

    for (int cmd_i = 0; cmd_i < command_list->CmdBuffer.Size; cmd_i++) {
      const auto &imgui_cmd = command_list->CmdBuffer[cmd_i];

      ImVec2 clipMin((imgui_cmd.ClipRect.x - clip_offset.x) * clip_scale.x,
                     (imgui_cmd.ClipRect.y - clip_offset.y) * clip_scale.y);
      ImVec2 clipMax((imgui_cmd.ClipRect.z - clip_offset.x) * clip_scale.x,
                     (imgui_cmd.ClipRect.w - clip_offset.y) * clip_scale.y);

      clipMin.x = std::max(clipMin.x, 0.0f);
      clipMin.y = std::max(clipMin.y, 0.0f);
      clipMax.x = std::min(clipMax.x, fb_width);
      clipMax.y = std::min(clipMax.y, fb_height);

      if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
        continue;
      }

      struct {
        std::array<float, 4> lrtb{};
        const DeviceAddress vb;
        u32 base_vertex;
        u32 texture_id{0};
        u32 sampler_id{0};
      } pc{
          .lrtb = {L, R, T, B},
          .vb = drawable.vertex->get_device_address(),
          .base_vertex = vertex_offset + imgui_cmd.VtxOffset,
          .texture_id = static_cast<u32>(imgui_cmd.GetTexID()),
          .sampler_id = sampler.index(),
      };

      vkCmdPushConstants(cmd, pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(pc), &pc);

      VkRect2D scissor{
          .offset = {static_cast<i32>(clipMin.x), static_cast<i32>(clipMin.y)},
          .extent = {static_cast<u32>(clipMax.x - clipMin.x),
                     static_cast<u32>(clipMax.y - clipMin.y)},
      };
      vkCmdSetScissor(cmd, 0, 1, &scissor);

      vkCmdDrawIndexed(
          cmd, imgui_cmd.ElemCount, 1, index_offset + imgui_cmd.IdxOffset,
          static_cast<i32>(vertex_offset + imgui_cmd.VtxOffset), 0);
    }

    index_offset += static_cast<u32>(command_list->IdxBuffer.Size);
    vertex_offset += static_cast<u32>(command_list->VtxBuffer.Size);
  }
}

auto create_pipeline(VkPipelineLayout &out_layout, SceneRenderer &r,
                     VkFormat fb)
    -> std::expected<PipelineHandle, shader::CompilationError> {
  struct PC {
    glm::vec4 lrtb;
    const DeviceAddress vertices;
    u32 base_vertex;
    u32 texture_id;
    u32 sampler_id;
  };
  VkPushConstantRange push_constant_range{
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = sizeof(PC),
  };
  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &r.bindless.layout,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_constant_range,
  };
  vk::check(
      vkCreatePipelineLayout(r.ctx.device, &layout_ci, nullptr, &out_layout));

  return r.pipeline_registry
      ->create_graphics(GraphicsPipelineDescription{
          .shader_path = VFSPath::create("shaders://gui.slang"),
          .layout = out_layout,
          .render_targets = {.color_formats =
                                 {
                                     fb,
                                 }},
          .cull_mode = VK_CULL_MODE_NONE,
          .blending = {BlendMode::alpha()},
      })
      .value();
}

auto ImGuiRenderer::set_app_name(const std::string_view name) -> void {
  config_name = std::format("{}.ini", name);
  config_path =
      std::make_unique<VFSPath>(VFSPath::create("editor://{}.ini", name));
}

auto ImGuiRenderer::update_font(FontChoice f) -> void {
  ImGuiIO &io = ImGui::GetIO();
  ImFontConfig cfg{};
  cfg.FontDataOwnedByAtlas = false;
  cfg.RasterizerMultiply = 1.5f;
  cfg.SizePixels = std::ceilf(f.size);
  cfg.PixelSnapH = true;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint |
                        ImGuiFreeTypeLoaderFlags_LightHinting;

  ImFont *font = nullptr;
  if (std::filesystem::exists(f.font_path.view())) {
    std::filesystem::path path = f.font_path.view();
    font = io.Fonts->AddFontFromFileTTF(path.c_str(), cfg.SizePixels, &cfg);
  }

  io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

  unsigned char *pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  auto *as_u32 = std::bit_cast<u32 *>(pixels);

  auto tex = Texture::from_bytes(
      renderer.ctx, "imgui_fonts",
      Texture::CreateInfo{
          .bytes = std::span(as_u32, static_cast<usize>(width * height * 4)),
          .width = static_cast<u32>(width),
          .height = static_cast<u32>(height),
          .format = VK_FORMAT_R8G8B8A8_UNORM,
          .generate_mips = true,
          .storage_view = true,
          .upload_queue = renderer.ctx.graphics_queue(),
          .upload_queue_family = renderer.ctx.graphics_queue_index,
      });
  font_texture =
      renderer.textures.create({std::move(tex), VK_IMAGE_VIEW_TYPE_2D});

  io.Fonts->TexID = font_texture.index();
  io.FontDefault = font;

  auto ci = VkSamplerCreateInfo();
  ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  ci.magFilter = VK_FILTER_LINEAR;
  ci.minFilter = VK_FILTER_LINEAR;
  ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  ci.compareOp = VK_COMPARE_OP_ALWAYS;
  ci.maxLod = VK_LOD_CLAMP_NONE;
  ci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  VkSampler current_sampler{};
  vk::check(
      vkCreateSampler(renderer.ctx.device, &ci, nullptr, &current_sampler));

  sampler = renderer.samplers.create(SamplerEntry{current_sampler});
}

} // namespace dy
