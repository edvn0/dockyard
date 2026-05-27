#include <volk.h>

#include <dockyard/bindless_descriptor.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/imgui_renderer.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vfs_path.hpp>
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
#include <utility>

namespace dy {

struct PC {
  std::array<float, 4> lrtb{};
  DeviceAddress vb;
  u32 base_vertex;
  TextureHandle texture_id;
  SamplerHandle sampler_id;
  u32 _pad;
};

auto create_pipeline(SceneRenderer &, VkFormat)
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
    style.WindowRounding = 0.0F;
    style.Colors[ImGuiCol_WindowBg].w = 1.0F;
  }

  update_font(std::move(font));
  ImGui_ImplGlfw_InitForVulkan(w, true);
  slots_per_frame = std::max(1u, initial_slot_count);
  drawables.resize(frames_in_flight * slots_per_frame);
}

ImGuiRenderer::~ImGuiRenderer() {
  ImGuiIO &io = ImGui::GetIO();
  io.Fonts->TexID = nullptr;

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
  // FIXME: Lifetime bug
  static auto str = VFS::get().resolve(*config_path).string();
  io.IniFilename = str.c_str();

  if (force_recompile_primary || !main_pipeline.valid()) {
    main_pipeline = create_pipeline(renderer, std::get<1>(fb)).value();
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

  if (std::cmp_less(drawable.index_count, dd->TotalIdxCount)) {
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

  auto &pipe = renderer.pipeline_registry->get_entry(pipeline);

  vkCmdBindIndexBuffer(cmd, drawable.index->get_buffer(), 0,
                       VK_INDEX_TYPE_UINT16);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.layout, 0,
                          1, &renderer.bindless.set, 0, nullptr);

  u32 index_offset = 0;
  u32 vertex_offset = 0;

  for (int n = 0; n < dd->CmdListsCount; n++) {
    const auto *command_list = dd->CmdLists[n];

    for (int cmd_i = 0; cmd_i < command_list->CmdBuffer.Size; cmd_i++) {
      const auto &imgui_cmd = command_list->CmdBuffer[cmd_i];

      ImVec2 clip_min((imgui_cmd.ClipRect.x - clip_offset.x) * clip_scale.x,
                      (imgui_cmd.ClipRect.y - clip_offset.y) * clip_scale.y);
      ImVec2 clip_max((imgui_cmd.ClipRect.z - clip_offset.x) * clip_scale.x,
                      (imgui_cmd.ClipRect.w - clip_offset.y) * clip_scale.y);

      clip_min.x = std::max(clip_min.x, 0.0F);
      clip_min.y = std::max(clip_min.y, 0.0F);
      clip_max.x = std::min(clip_max.x, fb_width);
      clip_max.y = std::min(clip_max.y, fb_height);

      if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
        continue;
      }

      auto tex_id = renderer.textures.handle_at(imgui_cmd.GetTexID());
      PC pc{
          .lrtb = {L, R, T, B},
          .vb = drawable.vertex->get_device_address(),
          .base_vertex = vertex_offset + imgui_cmd.VtxOffset,
          .texture_id = tex_id,
          .sampler_id = sampler,
      };

      vkCmdPushConstants(cmd, pipe.layout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(pc), &pc);

      VkRect2D scissor{
          .offset =
              {
                  .x = static_cast<i32>(clip_min.x),
                  .y = static_cast<i32>(clip_min.y),
              },
          .extent =
              {
                  .width = static_cast<u32>(clip_max.x - clip_min.x),
                  .height = static_cast<u32>(clip_max.y - clip_min.y),
              },
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

auto create_pipeline(SceneRenderer &r, VkFormat fb)
    -> std::expected<PipelineHandle, shader::CompilationError> {

  return r.pipeline_registry
      ->create_graphics(GraphicsPipelineDescription{
          .shader_path = VFSPath::create("shaders://gui.slang"),
          .layout = VK_NULL_HANDLE,
          .descriptor_set_layout = r.bindless.layout,
          .render_targets =
              {
                  .color_formats =
                      {
                          fb,
                      },
              },
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
  cfg.OversampleH = 4;
  cfg.OversampleV = 4;
  cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint |
                        ImGuiFreeTypeLoaderFlags_LightHinting;

  ImFont *font = nullptr;
  auto resolved_font_path = VFS::get().resolve(f.font_path);
  if (std::filesystem::exists(resolved_font_path)) {
    const auto path = resolved_font_path.string();
    font = io.Fonts->AddFontFromFileTTF(path.c_str(), cfg.SizePixels, &cfg);
  }

  io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

  unsigned char *pixels;
  int width;
  int height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  const auto *as_bytes = std::bit_cast<const std::byte *>(pixels);

  auto tex = Texture::from_bytes(
      renderer.ctx, "imgui_fonts",
      Texture::CreateInfo{
          .bytes = std::span<const std::byte>(
              as_bytes, static_cast<usize>(width * height * 4)),
          .width = static_cast<u32>(width),
          .height = static_cast<u32>(height),
          .format = VK_FORMAT_R8G8B8A8_UNORM,
          .generate_mips = true,
          .storage_view = true,
      });
  font_texture = renderer.textures.create(
      {.texture = tex, .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D});

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
