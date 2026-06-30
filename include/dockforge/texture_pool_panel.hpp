#pragma once

#include <dockforge/editor_state.hpp>
#include <dockforge/ipanel.hpp>
#include <dockyard/scene_renderer.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>

struct TexturePoolPanel : IPanel {
  static constexpr dy::usize history_size = 500;

  std::array<double, history_size * 2> history{};
  dy::usize write_pos = 0;

  auto push(double depth) -> void {
    history[write_pos] = depth;
    history[write_pos + history_size] = depth;
    write_pos = (write_pos + 1) % history_size;
  }

  auto draw(EditorState &state, const EditorActions & /*actions*/) -> void override {
    if (!open)
      return;
    const auto title = std::string{name()};
    if (!ImGui::Begin(title.c_str(), &open)) {
      ImGui::End();
      return;
    }

    const auto *pool = state.renderer != nullptr
                           ? state.renderer->texture_upload_pool.get()
                           : nullptr;
    if (pool == nullptr) {
      ImGui::TextDisabled("No renderer attached");
      ImGui::End();
      return;
    }

    const auto stats = pool->stats();
    push(static_cast<double>(stats.pending));

    if (stats.pending > 0) {
      ImGui::TextColored({0.35F, 0.85F, 0.35F, 1.0F}, "Active");
    } else {
      ImGui::TextDisabled("Idle");
    }
    ImGui::SameLine();
    ImGui::Text("  %zu pending", stats.pending);

    ImGui::Separator();
    ImGui::Text("Submitted:  %llu",
                static_cast<unsigned long long>(stats.total_submitted));
    ImGui::Text("Completed:  %llu",
                static_cast<unsigned long long>(stats.total_completed));
    ImGui::Separator();

    const auto view = std::span<const double, history_size>(
        &history[write_pos], history_size);
    const double peak = *std::ranges::max_element(view);

    ImPlot::SetNextAxesToFit();
    ImPlot::SetNextAxisLimits(ImAxis_Y1, 0.0, std::max(4.0, peak * 1.25),
                              ImGuiCond_Always);

    if (ImPlot::BeginPlot("##QueueHistory", ImVec2(-1, 120),
                          ImPlotFlags_NoLegend)) {
      ImPlot::SetupAxis(ImAxis_X1, "Frames (history)", ImPlotAxisFlags_None);
      ImPlot::SetupAxis(ImAxis_Y1, "Queue depth", ImPlotAxisFlags_None);
      ImPlot::PlotLine("Pending", view.data(), static_cast<int>(view.size()),
                       1.0, 0.0);
      ImPlot::EndPlot();
    }

    ImGui::End();
  }

  [[nodiscard]] auto name() const -> std::string_view override {
    return "Texture Upload Pool";
  }
};
