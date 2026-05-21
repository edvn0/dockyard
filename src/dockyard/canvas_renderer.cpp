#include <dockyard/canvas_renderer.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vk_check.hpp>

#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>

namespace dy {

CanvasRenderer::CanvasRenderer(SceneRenderer &r) : line_canvas_3d(r) {}

auto CanvasRenderer::clear() -> void {
  line_canvas.clear();
  line_canvas_3d.clear();
}

auto CanvasRenderer::line(const glm::vec2 &p1, const glm::vec2 &p2,
                          const glm::vec4 &color) -> void {
  line_canvas.line(p1, p2, color);
}

auto CanvasRenderer::line(const glm::vec3 &p1, const glm::vec3 &p2,
                          const glm::vec4 &color) -> void {
  line_canvas_3d.line(p1, p2, color);
}

auto CanvasRenderer::plane(const glm::vec3 &orig, const glm::vec3 &v1,
                           const glm::vec3 &v2, int n1, int n2, float s1,
                           float s2, const glm::vec4 &color,
                           const glm::vec4 &outline) -> void {
  line_canvas_3d.plane(orig, v1, v2, n1, n2, s1, s2, color, outline);
}

auto CanvasRenderer::box(const glm::mat4 &m, const AABB &b,
                         const glm::vec4 &color) -> void {
  line_canvas_3d.box(m, b, color);
}

auto CanvasRenderer::box(const glm::mat4 &m, const glm::vec3 &size,
                         const glm::vec4 &color) -> void {
  line_canvas_3d.box(m, size, color);
}

auto CanvasRenderer::frustum(const glm::mat4 &view, const glm::mat4 &proj,
                             const glm::vec4 &color) -> void {
  line_canvas_3d.frustum(view, proj, color);
}

auto CanvasRenderer::render(
    VkCommandBuffer cmd, const glm::mat4 &mvp,
    std::tuple<VkFormat, VkFormat, VkViewport, VkRect2D> format_tuple) -> void {
  line_canvas_3d.set_mvp(mvp);
  line_canvas_3d.render(cmd, std::move(format_tuple), force_recompile);
  force_recompile = false;
  line_canvas_3d.clear();
}

auto CanvasRenderer::render_2d() -> void { line_canvas.render("line_canvas"); }

// ---- LineCanvas2D -----------------------------------------------------------

auto CanvasRenderer::LineCanvas2D::render(std::string_view window_name)
    -> void {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
  ImGui::Begin(
      window_name.data(), nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);

  ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
  for (const LineData &l : lines) {
    draw_list->AddLine(ImVec2(l.p1.x, l.p1.y), ImVec2(l.p2.x, l.p2.y),
                       ImColor(l.color.r, l.color.g, l.color.b, l.color.a));
  }

  ImGui::End();
}

CanvasRenderer::LineCanvas3D::~LineCanvas3D() {
  if (pipeline_layout)
    vkDestroyPipelineLayout(renderer.ctx.device, pipeline_layout, nullptr);
}

auto CanvasRenderer::LineCanvas3D::line(const glm::vec3 &p1,
                                        const glm::vec3 &p2, const glm::vec4 &c)
    -> void {
  lines.emplace_back(glm::vec4(p1, 1.0f), c);
  lines.emplace_back(glm::vec4(p2, 1.0f), c);
}

auto CanvasRenderer::LineCanvas3D::plane(
    const glm::vec3 &o, const glm::vec3 &v1, const glm::vec3 &v2, int n1,
    int n2, float s1, float s2, const glm::vec4 &color,
    const glm::vec4 &outline_colour) -> void {
  line(o - s1 / 2.0f * v1 - s2 / 2.0f * v2, o - s1 / 2.0f * v1 + s2 / 2.0f * v2,
       outline_colour);
  line(o + s1 / 2.0f * v1 - s2 / 2.0f * v2, o + s1 / 2.0f * v1 + s2 / 2.0f * v2,
       outline_colour);
  line(o - s1 / 2.0f * v1 + s2 / 2.0f * v2, o + s1 / 2.0f * v1 + s2 / 2.0f * v2,
       outline_colour);
  line(o - s1 / 2.0f * v1 - s2 / 2.0f * v2, o + s1 / 2.0f * v1 - s2 / 2.0f * v2,
       outline_colour);

  for (int i = 1; i < n1; i++) {
    const float t = ((float)i - (float)n1 / 2.0f) * s1 / (float)n1;
    const glm::vec3 o1 = o + t * v1;
    line(o1 - s2 / 2.0f * v2, o1 + s2 / 2.0f * v2, color);
  }
  for (int i = 1; i < n2; i++) {
    const float t = ((float)i - (float)n2 / 2.0f) * s2 / (float)n2;
    const glm::vec3 o2 = o + t * v2;
    line(o2 - s1 / 2.0f * v1, o2 + s1 / 2.0f * v1, color);
  }
}

auto CanvasRenderer::LineCanvas3D::box(const glm::mat4 &m,
                                       const glm::vec3 &size,
                                       const glm::vec4 &colour) -> void {
  std::array points = {glm::vec3(+size.x, +size.y, +size.z),
                       glm::vec3(+size.x, +size.y, -size.z),
                       glm::vec3(+size.x, -size.y, +size.z),
                       glm::vec3(+size.x, -size.y, -size.z),
                       glm::vec3(-size.x, +size.y, +size.z),
                       glm::vec3(-size.x, +size.y, -size.z),
                       glm::vec3(-size.x, -size.y, +size.z),
                       glm::vec3(-size.x, -size.y, -size.z)};
  for (auto &p : points)
    p = glm::vec3(m * glm::vec4(p, 1.f));

  line(points[0], points[1], colour);
  line(points[2], points[3], colour);
  line(points[4], points[5], colour);
  line(points[6], points[7], colour);
  line(points[0], points[2], colour);
  line(points[1], points[3], colour);
  line(points[4], points[6], colour);
  line(points[5], points[7], colour);
  line(points[0], points[4], colour);
  line(points[1], points[5], colour);
  line(points[2], points[6], colour);
  line(points[3], points[7], colour);
}

auto CanvasRenderer::LineCanvas3D::box(const glm::mat4 &m, const AABB &b,
                                       const glm::vec4 &color) -> void {
  const glm::vec3 center = 0.5f * (b.get_min() + b.get_max());
  const glm::vec3 half = 0.5f * glm::abs(b.get_max() - b.get_min());
  const glm::mat4 world = glm::translate(glm::mat4(1.f), center) * m;
  box(world, half, color);
}

auto CanvasRenderer::LineCanvas3D::frustum(const glm::mat4 &cam_view,
                                           const glm::mat4 &cam_proj,
                                           const glm::vec4 &color) -> void {
  const glm::vec3 corners[] = {
      {-1, +1, 1}, {+1, +1, 1}, {+1, -1, 1}, {-1, -1, 1}, // near is 1.0
      {-1, +1, 0}, {+1, +1, 0}, {+1, -1, 0}, {-1, -1, 0}, // far is 0.0
  };
  glm::vec3 pp[8]{};
  glm::mat4 inverse_vp = glm::inverse(cam_proj * cam_view);
  for (int i = 0; i < 8; i++) {
    glm::vec4 q = inverse_vp * glm::vec4(corners[i], 1.0f);
    pp[i] = glm::vec3(q) / q.w;
  }

  line(pp[0], pp[4], color);
  line(pp[1], pp[5], color);
  line(pp[2], pp[6], color);
  line(pp[3], pp[7], color);
  line(pp[0], pp[1], color);
  line(pp[1], pp[2], color);
  line(pp[2], pp[3], color);
  line(pp[3], pp[0], color);
  line(pp[0], pp[2], color);
  line(pp[1], pp[3], color);
  line(pp[4], pp[5], color);
  line(pp[5], pp[6], color);
  line(pp[6], pp[7], color);
  line(pp[7], pp[4], color);
  line(pp[4], pp[6], color);
  line(pp[5], pp[7], color);

  const glm::vec4 grid_color = color * 0.7f;
  constexpr int grid_lines = 5;

  auto fill_face = [&](glm::vec3 p1, glm::vec3 p2, const glm::vec3 &s1,
                       const glm::vec3 &s2) {
    for (int i = 0; i < grid_lines; i++, p1 += s1, p2 += s2)
      line(p1, p2, grid_color);
  };
  fill_face(pp[0], pp[1], (pp[4] - pp[0]) / float(grid_lines),
            (pp[5] - pp[1]) / float(grid_lines));
  fill_face(pp[2], pp[3], (pp[6] - pp[2]) / float(grid_lines),
            (pp[7] - pp[3]) / float(grid_lines));
  fill_face(pp[0], pp[3], (pp[4] - pp[0]) / float(grid_lines),
            (pp[7] - pp[3]) / float(grid_lines));
  fill_face(pp[1], pp[2], (pp[5] - pp[1]) / float(grid_lines),
            (pp[6] - pp[2]) / float(grid_lines));
}

auto CanvasRenderer::LineCanvas3D::render(
    VkCommandBuffer cmd,
    std::tuple<VkFormat, VkFormat, VkViewport, VkRect2D> format_tuple,
    bool force_recompile) -> void {
  if (lines.empty())
    return;

  frame_cursor = (frame_cursor + 1) % frames_in_flight;

  const usize required_size = lines.size() * sizeof(LineData);

  if (!line_buffers[frame_cursor] ||
      buffer_sizes[frame_cursor] < required_size) {
    const usize actual_size = next_power_of_two(required_size);
    info("(Canvas) Reallocating line buffer to {} bytes", actual_size);
    line_buffers[frame_cursor] =
        Buffer::create(renderer.ctx.allocator, actual_size,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    buffer_sizes[frame_cursor] = actual_size;
  }
  line_buffers[frame_cursor]->upload(lines);

  auto &&[colour, depth, vp, scissor] = format_tuple;

  if (force_recompile || !pipeline.valid()) {
    pipeline = create_pipeline(colour, depth).value();
    force_recompile = false;
  }

  struct {
    glm::mat4 mvp;
    DeviceAddress address;
  } pc{
      .mvp = mvp,
      .address = line_buffers[frame_cursor]->get_device_address(),
  };

  auto pipe = renderer.pipeline_registry->get(pipeline);

  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
  vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER_OR_EQUAL);
  vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
  vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                          0, 1, &renderer.bindless.set, 0, nullptr);
  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(pc), &pc);
  vkCmdDraw(cmd, static_cast<u32>(lines.size()), 1, 0, 0);
}

auto CanvasRenderer::LineCanvas3D::create_pipeline(VkFormat colour,
                                                   VkFormat depth)
    -> std::expected<PipelineHandle, shader::CompilationError> {
  struct PC {
    glm::mat4 mvp;
    DeviceAddress vertices;
  };
  const VkPushConstantRange push_constant_range{
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = sizeof(PC),
  };
  const VkPipelineLayoutCreateInfo layout_ci{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &renderer.bindless.layout,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_constant_range,
  };
  vk::check(vkCreatePipelineLayout(renderer.ctx.device, &layout_ci, nullptr,
                                   &pipeline_layout));

  return renderer.pipeline_registry
      ->create_graphics(GraphicsPipelineDescription{
          .shader_path = VFSPath::create("shaders://canvas_renderer.slang"),
          .layout = pipeline_layout,
          .render_targets =
              {
                  .color_formats = {colour},
                  .depth_format = depth,
              },
          .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
          .cull_mode = VK_CULL_MODE_NONE,
          .samples = VK_SAMPLE_COUNT_4_BIT,
          .depth = {.test = true,
                    .write = false,
                    .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL},
          .blending = {BlendMode::alpha()},
          .line_width = 4.0F,
      })
      .value();
}

} // namespace dy
