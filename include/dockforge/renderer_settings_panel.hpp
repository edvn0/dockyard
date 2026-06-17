#pragma once

#include <dockforge/ipanel.hpp>
#include <dockyard/renderer_settings_registry.hpp>

#include <imgui.h>
#include <string>

struct RendererSettingsPanel : IPanel {
  dy::RendererSettingsRegistry &registry;

  explicit RendererSettingsPanel(dy::RendererSettingsRegistry &reg)
      : registry(reg) {}

  auto draw(EditorState & /*state*/, const EditorActions & /*actions*/)
      -> void override {
    if (!open)
      return;
    const auto window_name = std::string{name()};
    if (ImGui::Begin(window_name.c_str(), &open)) {
      for (auto &section : registry.sections) {
        const auto section_name = std::string{section.name};
        if (ImGui::CollapsingHeader(section_name.c_str())) {
          section.draw();
        }
      }
    }
    ImGui::End();
  }

  [[nodiscard]] auto name() const -> std::string_view override {
    return "Renderer Settings";
  }
};
