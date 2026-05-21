#pragma once

#include "dockyard/mesh.hpp"
#include <dockyard/buffer.hpp>
#include <dockyard/compiler.hpp>
#include <dockyard/pipeline_builder.hpp>
#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <glm/glm.hpp>
#include <volk.h>

#include <array>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

namespace dy {

struct SceneRenderer;

class CanvasRenderer {
public:
  explicit CanvasRenderer(SceneRenderer &r);

  CanvasRenderer(CanvasRenderer &&) = delete;
  auto operator=(CanvasRenderer &&) -> CanvasRenderer & = delete;

  auto clear() -> void;
  auto line(const glm::vec2 &p1, const glm::vec2 &p2, const glm::vec4 &color)
      -> void;
  auto line(const glm::vec3 &p1, const glm::vec3 &p2, const glm::vec4 &color)
      -> void;
  auto plane(const glm::vec3 &orig, const glm::vec3 &v1, const glm::vec3 &v2,
             int n1, int n2, float s1, float s2, const glm::vec4 &color,
             const glm::vec4 &outline) -> void;
  auto box(const glm::mat4 &m, const AABB &b, const glm::vec4 &color) -> void;
  auto box(const glm::mat4 &m, const glm::vec3 &size, const glm::vec4 &color)
      -> void;
  auto frustum(const glm::mat4 &view, const glm::mat4 &proj,
               const glm::vec4 &color) -> void;

  auto render(VkCommandBuffer cmd, const glm::mat4 &mvp,
              std::tuple<VkFormat, VkFormat, VkViewport, VkRect2D>) -> void;
  auto render_2d() -> void;

  auto set_should_recompile() -> void { force_recompile = true; }

private:
  struct LineCanvas2D {
    auto clear() -> void { lines.clear(); }
    auto line(const glm::vec2 &p1, const glm::vec2 &p2, const glm::vec4 &c)
        -> void {
      lines.emplace_back(p1, p2, c);
    }
    auto render(std::string_view window_name) -> void;

  private:
    struct LineData {
      glm::vec2 p1;
      glm::vec2 p2;
      glm::vec4 color;
    };
    std::vector<LineData> lines;
  };

  class LineCanvas3D {
  public:
    explicit LineCanvas3D(SceneRenderer &r) : renderer(r) {}
    ~LineCanvas3D();

    auto clear() -> void { lines.clear(); }
    auto line(const glm::vec3 &p1, const glm::vec3 &p2, const glm::vec4 &c)
        -> void;
    auto plane(const glm::vec3 &orig, const glm::vec3 &v1, const glm::vec3 &v2,
               int n1, int n2, float s1, float s2, const glm::vec4 &color,
               const glm::vec4 &outline) -> void;
    auto box(const glm::mat4 &m, const AABB &b, const glm::vec4 &color) -> void;
    auto box(const glm::mat4 &m, const glm::vec3 &size, const glm::vec4 &color)
        -> void;
    auto frustum(const glm::mat4 &view, const glm::mat4 &proj,
                 const glm::vec4 &color) -> void;

    auto set_mvp(const glm::mat4 &m) -> void { mvp = m; }
    auto render(VkCommandBuffer cmd,
                std::tuple<VkFormat, VkFormat, VkViewport, VkRect2D>,
                bool force_recompile) -> void;

  private:
    glm::mat4 mvp{1.0f};
    SceneRenderer &renderer;

    struct LineData {
      glm::vec4 pos;
      glm::vec4 color;
    };
    std::vector<LineData> lines;

    PipelineHandle pipeline{};
    VkPipelineLayout pipeline_layout{};

    std::array<std::unique_ptr<Buffer>, frames_in_flight> line_buffers{};
    std::array<usize, frames_in_flight> buffer_sizes{};
    u32 frame_cursor{0};

    auto create_pipeline(VkFormat colour, VkFormat depth)
        -> std::expected<PipelineHandle, shader::CompilationError>;
  };

  LineCanvas3D line_canvas_3d;
  LineCanvas2D line_canvas;
  bool force_recompile{false};
};

} // namespace dy