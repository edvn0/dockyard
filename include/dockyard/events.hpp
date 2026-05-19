#pragma once

#include <dockyard/types.hpp>

#include <cstdint>

namespace dy::events {

struct WindowResized {
  u32 width;
  u32 height;
};

struct WindowClosed {};

struct WindowMinimized {
  bool minimized;
};

struct SwapchainInvalidated {
  u32 width;
  u32 height;
};

struct SwapchainResized {
  u32 width;
  u32 height;
};

struct KeyPressed {
  int key;
  int scancode;
  int mods;
};

struct KeyReleased {
  int key;
  int scancode;
  int mods;
};

struct MouseButtonPressed {
  int button;
  int mods;
};

struct MouseButtonReleased {
  int button;
  int mods;
};

struct MouseMoved {
  float x;
  float y;
  float dx;
  float dy;
};

struct MouseScrolled {
  float dx;
  float dy;
};

} // namespace dy::events