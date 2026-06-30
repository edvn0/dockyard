#pragma once

namespace dy {
class Scene;
}

extern "C" {
struct GLFWwindow;
}

namespace first_person_system {
auto update(dy::Scene &scene, GLFWwindow *window, float ts) -> void;
}
