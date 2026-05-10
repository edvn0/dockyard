#pragma once

#include <dockyard/types.hpp>

#include <cstdint>

namespace dy::events {
struct window_resized {
  u32 width;
  u32 height;
};

struct window_closed {};

struct window_minimized {
  bool minimized;
};

struct swapchain_invalidated {
  u32 width;
  u32 height;
};

struct swapchain_resized {
  u32 width;
  u32 height;
};

struct key_pressed {
  int key;
  int scancode;
  int mods;
};
struct key_released {
  int key;
  int scancode;
  int mods;
};

struct mouse_button_pressed {
  int button;
  int mods;
};
struct mouse_button_released {
  int button;
  int mods;
};

struct mouse_moved {
  float x;
  float y;
  float dx;
  float dy;
};
struct mouse_scrolled {
  float dx;
  float dy;
};

} // namespace dy::events