#pragma once

#include <functional>
#include <string_view>
#include <vector>

namespace dy {

struct RendererSettingsRegistry {
  struct Section {
    std::string_view name;
    std::function<void()> draw;
  };

  auto add(std::string_view name, std::function<void()> draw) -> void {
    sections.push_back({.name = name, .draw = std::move(draw)});
  }

  std::vector<Section> sections;
};

} // namespace dy
