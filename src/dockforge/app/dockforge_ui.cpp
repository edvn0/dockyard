#include <dockforge/dockforge.hpp>

#include <dockforge/component_renderers.hpp>
#include <dockforge/editor_camera.hpp>
#include <dockforge/matrix_cache.hpp>

#include <dockyard/components.hpp>
#include <dockyard/scene_renderer.hpp>
#include <dockyard/vfs.hpp>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <nfd.hpp>
#include <nfd_glfw3.h>

using namespace dy;

static constexpr ImVec2 icon_size{20.0F, 20.0F};
static constexpr float titlebar_height = 32.0F;
static constexpr float titlebar_btn_w = 46.0F;
static constexpr int titlebar_btn_count = 3;

static const std::filesystem::path script_settings_path =
    std::filesystem::path(ASSETS_ROOT_PATH) / "editor" / "script_path.txt";

auto draw_material_editor(GPUMaterial &mat) -> bool {
  bool changed = false;

  changed |= labelled_input("Albedo Factor", [&](const char *id) {
    return ImGui::ColorEdit4(id, mat.albedo_factor);
  });
  changed |= labelled_input("Metallic", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.metallic_factor, 0.0F, 1.0F);
  });
  changed |= labelled_input("Roughness", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.roughness_factor, 0.0F, 1.0F);
  });
  changed |= labelled_input("Normal Scale", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.normal_scale, 0.0F, 2.0F);
  });
  changed |= labelled_input("Occlusion", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.occlusion_strength, 0.0F, 1.0F);
  });
  changed |= labelled_input("Alpha Cutoff", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.alpha_cutoff, 0.0F, 1.0F);
  });
  changed |= labelled_input("Transmission", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.transmission_factor, 0.0F, 1.0F);
  });
  changed |= labelled_input("Anisotropy", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.anisotropy_factor, 0.0F, 1.0F);
  });
  changed |= labelled_input("Anisotropy Angle", [&](const char *id) {
    return ImGui::SliderFloat(id, &mat.anisotropy_rotation, 0.0F, 1.0F);
  });

  // Emissive: colour (RGB) and intensity (W) share a row
  ImGui::TextUnformatted("Emissive Factor");
  const float half =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
      0.5F;
  ImGui::SetNextItemWidth(half);
  changed |= ImGui::ColorEdit3("##EmissiveColor", mat.emissive_factor);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(half);
  changed |= ImGui::SliderFloat("##EmissiveIntensity", &mat.emissive_factor[3],
                                0.0F, 10.0F);

  static constexpr std::array<const char *, 3> alpha_modes = {"Opaque", "Mask",
                                                              "Blend"};
  int alpha_mode = static_cast<int>(mat.alpha_mode);
  ImGui::TextUnformatted("Alpha Mode");
  if (ImGui::Combo("##AlphaMode", &alpha_mode, alpha_modes.data(),
                   std::size(alpha_modes))) {
    mat.alpha_mode = static_cast<u32>(alpha_mode);
    changed = true;
  }

  ImGui::SeparatorText("Texture Slots");
  ImGui::BeginDisabled();

  auto texture_row = [](const char *label, u32 index) {
    static constexpr u32 none = ~0U;
    const float texture_icon_size = ImGui::GetFrameHeight();
    const ImVec2 icon_dim{texture_icon_size, texture_icon_size};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto *dl = ImGui::GetWindowDrawList();

    if (index != none) {
      ImGui::Image(ImTextureRef{ImTextureID{index}}, icon_dim, {0, 0}, {1, 1},
                   {1, 1, 1, 1}, ImGui::GetStyleColorVec4(ImGuiCol_Border));
    } else {
      // 2×2 checkerboard placeholder
      const float h = texture_icon_size * 0.5f;
      dl->AddRectFilled(p, {p.x + texture_icon_size, p.y + texture_icon_size},
                        IM_COL32(40, 40, 40, 255));
      dl->AddRectFilled({p.x, p.y}, {p.x + h, p.y + h},
                        IM_COL32(65, 65, 65, 255));
      dl->AddRectFilled({p.x + h, p.y + h},
                        {p.x + texture_icon_size, p.y + texture_icon_size},
                        IM_COL32(65, 65, 65, 255));
      dl->AddRect(p, {p.x + texture_icon_size, p.y + texture_icon_size},
                  ImGui::GetColorU32(ImGuiCol_Border));
      ImGui::Dummy(icon_dim);
    }

    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
    // vertically centre the InputInt against the icon
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        ((texture_icon_size - ImGui::GetFrameHeight()) * 0.5F));
    int idx = static_cast<int>(index);
    ImGui::InputInt(label, &idx, 0, 0, ImGuiInputTextFlags_ReadOnly);
  };

  texture_row("Albedo", mat.albedo_index);
  texture_row("Normal", mat.normal_index);
  texture_row("Metallic/Roughness", mat.metallic_roughness_index);
  texture_row("Emissive", mat.emissive_index);
  texture_row("Occlusion", mat.occlusion_index);

  ImGui::EndDisabled();
  return changed;
}

auto Dockforge::duplicate_entity(Entity src) -> Entity {
  static constexpr std::string_view name = "duplicate";
  auto dst = active_scene->make(name);

  for_each_type<MasterComponentList>([&]<typename T>() {
    if (auto *comp = src.try_get<T>())
      dst.emplace_or_replace<T>(*comp);
  });

  if (auto *tag = dst.try_get<Components::Tag>())
    tag->tag += " (copy)";

  if (auto *ov = dst.try_get<Components::MaterialOverride>()) {
    ov->gpu_slot = Components::MaterialOverride::invalid_material;
    ov->dirty = true;
  }

  editor_state.cache_dirty = true;
  return dst;
}

auto Dockforge::draw_debug_shapes() -> void {
  ZoneScopedNC("Dockforge::draw_debug_shapes", 0xAAAAAA);
  for (auto &&[e, line] : active_scene->view<Components::DebugLine>().each()) {
    canvas_renderer->line(line.p1, line.p2, line.color);
  }

  for (auto &&[e, xt, box] :
       active_scene->view<Components::Transform, Components::DebugBox>()
           .each()) {
    canvas_renderer->box(cached_matrix(e, xt),
                         box.size * 0.5F, // canvas API takes half-extents
                         box.color);
  }

  for (auto &&[e, xt, plane] :
       active_scene->view<Components::Transform, Components::DebugPlane>()
           .each()) {
    const glm::vec3 origin = glm::vec3(cached_matrix(e, xt)[3]);
    canvas_renderer->plane(origin, plane.v1, plane.v2, plane.n1, plane.n2,
                           plane.s1, plane.s2, plane.color, plane.outline);
  }

  for (auto &&[e, frustum, xt] :
       active_scene->view<Components::DebugFrustum, Components::Transform>()
           .each()) {
    auto &&[pos, rot, scl] = xt.get();
    auto &&[view, proj] = frustum.matrices(pos, rot);
    canvas_renderer->frustum(view, proj, frustum.color);
  }
}

namespace {
struct SpanTelemetry {
  static constexpr size_t max_points = 500;

  std::array<double, max_points * 2> storage_pool{};
  size_t write_index = 0;

  auto push(double dt_ms) -> void {
    storage_pool[write_index] = dt_ms;
    storage_pool[write_index + max_points] = dt_ms;

    write_index = (write_index + 1) % max_points;
  }

  [[nodiscard]] auto get_history_span() const noexcept
      -> std::span<const double, max_points> {
    return std::span<const double, max_points>(&storage_pool[write_index],
                                               max_points);
  }
};

SpanTelemetry telemetry;
} // namespace

auto draw_performance_overlay() -> void {
  auto current_dt_ms = static_cast<double>(ImGui::GetIO().DeltaTime * 1000.0F);
  telemetry.push(current_dt_ms);

  if (ImGui::Begin("Telemetry Profiler")) {
    ImGui::Text("FPS: %.1f | Current Frame: %.2f ms", ImGui::GetIO().Framerate,
                current_dt_ms);

    ImPlot::SetNextAxesToFit();
    ImPlot::SetNextAxisLimits(ImAxis_Y1, 0.0, 40.0, ImGuiCond_Always);

    if (ImPlot::BeginPlot("##FrametimeGraph", ImVec2(-1, 150),
                          ImPlotFlags_NoLegend)) {
      ImPlot::SetupAxis(ImAxis_X1, "Frames (History)", ImPlotAxisFlags_None);
      ImPlot::SetupAxis(ImAxis_Y1, "Frametime (ms)", ImPlotAxisFlags_None);

      double budget_60fps = 16.66;
      double budget_30fps = 33.33;
      ImPlot::DragLineY(0, &budget_60fps, ImVec4(0.0f, 1.0f, 0.0f, 0.4f), 1.0f,
                        1);
      ImPlot::DragLineY(1, &budget_30fps, ImVec4(1.0f, 0.5f, 0.0f, 0.4f), 1.0f,
                        1);

      auto history_view = telemetry.get_history_span();

      ImPlot::PlotLine("Frametime", history_view.data(),
                       static_cast<int>(history_view.size()), 1.0, 0.0);

      ImPlot::EndPlot();
    }
  }
  ImGui::End();
}

auto Dockforge::draw_hdr_selector() -> void {
  static NullableVFSPath selected_hdr;

  if (!ImGui::Begin("Environment")) {
    ImGui::End();
    return;
  }

  ImGui::Text("HDR map");

  if (!selected_hdr.valid()) {
    ImGui::TextDisabled("No external HDR selected");
  } else {
    ImGui::TextWrapped("%.*s", static_cast<int>(selected_hdr.view().size()),
                       selected_hdr.view().data());
  }

  if (ImGui::Button("Browse HDR...")) {
    NFD::Guard nfd_guard;

    constexpr std::array<nfdfilteritem_t, 3> filters = {
        nfdfilteritem_t{"HDR images", "hdr,exr"},
        nfdfilteritem_t{"Ktx2 images", "ktx2"},
        nfdfilteritem_t{"All files", "*"},
    };

    NFD::UniquePath out_path;
    nfdwindowhandle_t parent{};
    NFD_GetNativeWindowFromGLFWWindow(get_window(), &parent);
    const nfdresult_t result = NFD::OpenDialog(
        out_path, filters.data(), std::size(filters), nullptr, parent);

    if (result == NFD_OKAY) {
      const auto virtual_hdr_path = VFS::get().mount_file(
          "external_hdr", std::filesystem::path{out_path.get()});

      selected_hdr = NullableVFSPath{virtual_hdr_path};
      renderer->set_hdr_map(virtual_hdr_path);
    } else if (result == NFD_ERROR) {
      warn("Native file dialog failed: {}", NFD::GetError());
    }
  }

  ImGui::SameLine();

  if (ImGui::Button("Use packaged sunset")) {
    const auto sunset = VFSPath::create("textures://env/sunset_f16.ktx2");
    selected_hdr = NullableVFSPath{sunset};
    renderer->set_hdr_map(sunset);
  }

  if (renderer->ibl_probe.valid()) {
    ImGui::SeparatorText("GPU handles");

    auto probe_row = [](const char *label, u32 index) {
      const float sz = ImGui::GetFrameHeight();
      const ImVec2 dim{sz, sz};
      ImGui::Image(ImTextureRef{ImTextureID{index}}, dim, {0, 0}, {1, 1},
                   {1, 1, 1, 1}, ImGui::GetStyleColorVec4(ImGuiCol_Border));
      ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                           ((sz - ImGui::GetFrameHeight()) * 0.5F));
      int idx = static_cast<int>(index);
      ImGui::InputInt(label, &idx, 0, 0, ImGuiInputTextFlags_ReadOnly);
    };

    probe_row("Env map", renderer->ibl_probe.env_map.index());
    probe_row("Irradiance", renderer->ibl_probe.irradiance.index());
    probe_row("Prefiltered", renderer->ibl_probe.prefiltered.index());
    probe_row("BRDF LUT", renderer->ibl_probe.brdf_lut.index());
  }

  ImGui::End();
}

auto Dockforge::draw_titlebar() -> void {
  auto glfw_window = App::get_window();
  const ImGuiViewport *vp = ImGui::GetMainViewport();

  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize({vp->Size.x, titlebar_height});
  ImGui::SetNextWindowViewport(vp->ID);

  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.08F, 0.08F, 0.09F, 1.0F});
  ImGui::Begin("##Titlebar", nullptr, flags);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  const float right = ImGui::GetContentRegionMax().x;
  const float center_y = (titlebar_height - ImGui::GetTextLineHeight()) * 0.5F;
  const float drag_w =
      right - titlebar_btn_w * static_cast<float>(titlebar_btn_count);

  // ── Invisible drag region (input layer) ───────────────────────────────────
  // On Win32 the HTCAPTION wndproc intercepts these events at the OS level,
  // so this code only fires on X11 / Wayland.
  ImGui::SetCursorPos({0.0F, 0.0F});
  ImGui::InvisibleButton("##drag", {drag_w, titlebar_height});

  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    int win_x = 0;
    int win_y = 0;
    glfwGetWindowPos(glfw_window, &win_x, &win_y);
    glfwSetWindowPos(glfw_window, win_x + static_cast<int>(delta.x),
                     win_y + static_cast<int>(delta.y));
  }

  if (ImGui::IsItemHovered() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    const bool maximised =
        glfwGetWindowAttrib(glfw_window, GLFW_MAXIMIZED) != 0;
    maximised ? glfwRestoreWindow(glfw_window)
              : glfwMaximizeWindow(glfw_window);
  }

  // ── Visual content drawn over the drag region ─────────────────────────────
  ImGui::SetCursorPos({8.0F, center_y});
  ImGui::TextColored({0.55F, 0.55F, 0.60F, 1.0F}, "Dockforge");

  ImGui::SameLine();
  ImGui::SetCursorPosY(center_y);
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::SetCursorPosY(center_y);
  if (sim_state.in<sim::S::Playing>())
    ImGui::TextColored({0.35F, 0.85F, 0.35F, 1.0F}, "Playing");
  else if (sim_state.in<sim::S::Paused>())
    ImGui::TextColored({0.90F, 0.70F, 0.25F, 1.0F}, "Paused");
  else
    ImGui::TextDisabled("Editor");

  // ── Window control buttons ────────────────────────────────────────────────
  constexpr float btn_w = titlebar_btn_w;
  constexpr float btn_h = titlebar_height;

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_Button, {0.0F, 0.0F, 0.0F, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1.0F, 1.0F, 1.0F, 0.08F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1.0F, 1.0F, 1.0F, 0.16F});

  ImGui::SetCursorPos({right - btn_w * 3.0F, 0.0F});
  if (ImGui::Button("_##min", {btn_w, btn_h}))
    glfwIconifyWindow(glfw_window);

  ImGui::SetCursorPos({right - btn_w * 2.0F, 0.0F});
  const bool maximised = glfwGetWindowAttrib(glfw_window, GLFW_MAXIMIZED) != 0;
  if (ImGui::Button(maximised ? "#r##max" : "#m##max", {btn_w, btn_h}))
    maximised ? glfwRestoreWindow(glfw_window)
              : glfwMaximizeWindow(glfw_window);

  ImGui::SetCursorPos({right - btn_w, 0.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.85F, 0.18F, 0.18F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.70F, 0.10F, 0.10F, 1.0F});
  if (ImGui::Button("x##close", {btn_w, btn_h}))
    glfwSetWindowShouldClose(glfw_window, GLFW_TRUE);
  ImGui::PopStyleColor(2);

  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);

  ImGui::End();
}

auto Dockforge::draw_toolbar() -> void {
  if (!ImGui::Begin("Toolbar")) {
    ImGui::End();
    return;
  }

  const bool editing = sim_state.in<sim::S::Editing>();
  const bool playing = sim_state.in<sim::S::Playing>();
  const bool paused = sim_state.in<sim::S::Paused>();
  const bool has_dll = script_engine && script_engine->loaded();

  auto icon_button = [](const char *id, TextureHandle handle,
                        bool enabled) -> bool {
    ImGui::BeginDisabled(!enabled || !handle.valid());
    const bool pressed = ImGui::ImageButton(
        id, ImTextureRef{ImTextureID{handle.index()}}, icon_size);
    ImGui::EndDisabled();
    return pressed && enabled;
  };

  if (icon_button("##play", icon_play, (editing && has_dll) || paused)) {
    if (paused)
      resume();
    else
      play();
  }
  ImGui::SameLine();

  if (icon_button("##pause", icon_pause, playing))
    pause();
  ImGui::SameLine();

  if (icon_button("##stop", icon_stop, playing || paused))
    stop();
  ImGui::SameLine();

  if (icon_button("##step", icon_step, paused && has_dll))
    step();

  if (editing) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const float btn_h = icon_size.y + ImGui::GetStyle().FramePadding.y * 2.0F;
    const ImVec2 op_sz{0.0F, btn_h};

    auto op_button = [&](const char *label, GizmoOp op) {
      if (gizmo_op == op)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(label, op_sz))
        gizmo_op = op;
      if (gizmo_op == op)
        ImGui::PopStyleColor();
    };

    op_button("T##gizmo_op", GizmoOp::Translate);
    ImGui::SameLine();
    op_button("R##gizmo_op", GizmoOp::Rotate);
    ImGui::SameLine();
    op_button("S##gizmo_op", GizmoOp::Scale);

    if (gizmo_op == GizmoOp::Translate) {
      ImGui::SameLine();
      const bool is_local = gizmo_mode == GizmoMode::Local;
      if (is_local)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(is_local ? "Local##mode" : "World##mode", op_sz))
        gizmo_mode = is_local ? GizmoMode::World : GizmoMode::Local;
      if (is_local)
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    if (ImGui::Button("Unload Scene##scene", op_sz))
      unload_scene();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_dll);
    if (ImGui::Button("Reload Scene##scene", op_sz))
      reload_scene();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const auto dll_label = script_path.empty()
                               ? std::string{"No script loaded"}
                               : script_path.filename().string();
    const float button_h =
        icon_size.y + ImGui::GetStyle().FramePadding.y * 2.0F;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (button_h - ImGui::GetTextLineHeight()) * 0.5F);
    ImGui::TextDisabled("%s", dll_label.c_str());
    if (!script_path.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", script_path.string().c_str());
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                         (button_h - ImGui::GetTextLineHeight()) * 0.5F);

    ImGui::SameLine();
    const ImVec2 browse_sz{0.0F, icon_size.y +
                                     ImGui::GetStyle().FramePadding.y * 2.0F};
    if (ImGui::Button("...##browse", browse_sz)) {
      nfdnchar_t *out = nullptr;
#if _WIN32
      const nfdnfilteritem_t filter{
          .name = L"Lua Script",
          .spec = L"lua",
      };
#else
      const nfdnfilteritem_t filter{
          .name = "Lua Script",
          .spec = "lua",
      };
#endif
      nfdwindowhandle_t parent{};
      NFD_GetNativeWindowFromGLFWWindow(get_window(), &parent);
      if (NFD::OpenDialog(out, &filter, 1, nullptr, parent) == NFD_OKAY) {
        const std::filesystem::path chosen{out};
        NFD::FreePath(out);
        load_script(chosen);
        if (std::ofstream ofs{script_settings_path}; ofs)
          ofs << chosen.string();
      }
    }

    ImGui::SameLine();
    if (icon_button("##reload", icon_reload, has_dll)) {
      PROFILE_SCOPE("Reload script");
      unload_scene();
      load_script(script_path);
    }
  }

  ImGui::End();
}

auto Dockforge::build_ui() -> void {
  ZoneScopedNC("Dockforge::build_ui", 0xFFD700);
  draw_titlebar();

  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + titlebar_height});
  ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y - titlebar_height});
  ImGui::SetNextWindowViewport(vp->ID);

  ImGuiWindowFlags host_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoDocking;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
  ImGui::Begin("##DockHost", nullptr, host_flags);
  ImGui::PopStyleVar(3);

  ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
  ImGui::DockSpace(dockspace_id, {0.0f, 0.0f},
                   ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::End();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
  ImGui::Begin("Viewport");
  ImGui::PopStyleVar();

  const ImVec2 panel_size = ImGui::GetContentRegionAvail();
  const u32 panel_w = static_cast<u32>(panel_size.x);
  const u32 panel_h = static_cast<u32>(panel_size.y);

  if (panel_w > 0 && panel_h > 0) {
    if (panel_w != last_ui_size.width || panel_h != last_ui_size.height) {
      last_ui_size = {.width = panel_w, .height = panel_h};
      last_ui_offset = {
          .width = static_cast<u32>(ImGui::GetCursorScreenPos().x),
          .height = static_cast<u32>(ImGui::GetCursorScreenPos().y),
      };
      last_resize_change_time = glfwGetTime();
    }
  }

  const auto viewport_screen_pos = ImGui::GetCursorScreenPos();

  ImGui::Image(ImTextureRef{ImTextureID{
                   viewport_resources.display_target.index(),
               }},
               panel_size);

  ImGuizmo::SetDrawlist();
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetRect(viewport_screen_pos.x, viewport_screen_pos.y, panel_size.x,
                    panel_size.y);
  viewport_hovered = ImGui::IsWindowHovered();
  const bool viewport_focused = ImGui::IsWindowFocused();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGuizmo::IsOver()) {
    const ImVec2 mp = ImGui::GetMousePos();
    pending_pick = glm::vec2(mp.x, mp.y);
  }

  if (editor_state.selected != entt::null) {
    Entity selected_entity{*active_scene, editor_state.selected};
    auto &transform = selected_entity.get<Components::Transform>();
    auto &ltw = selected_entity.get<Components::LocalToWorld>();
    auto &&[view, proj] = resolve_camera();

    glm::mat4 world_matrix = ltw.matrix;

    if (auto *mesh = selected_entity.try_get<Components::Mesh>();
        mesh != nullptr) {
      if (const auto *asset = renderer->resolve(mesh->handle)) {
        canvas_renderer->box(transform.matrix_without_rotation(),
                             asset->mesh_aabb,
                             glm::vec4{0.9F, 0.1F, 0.1F, 1.0F});
      }
    }

    // World mode only makes sense for translation; rotate/scale always use local.
    const ImGuizmo::MODE mode =
        (gizmo_op == GizmoOp::Translate)
            ? static_cast<ImGuizmo::MODE>(std::to_underlying(gizmo_mode))
            : ImGuizmo::LOCAL;
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                         static_cast<ImGuizmo::OPERATION>(std::to_underlying(gizmo_op)),
                         mode, glm::value_ptr(world_matrix));

    if (ImGuizmo::IsUsing()) {
      glm::mat4 local_matrix = world_matrix;

      if (auto *relation = selected_entity.try_get<Components::ParentOf>();
          relation) {
        auto &registry = active_scene->registry();
        if (registry.valid(relation->parent)) {
          if (auto *parent_ltw =
                  registry.try_get<Components::LocalToWorld>(relation->parent);
              parent_ltw) {
            local_matrix = glm::inverse(parent_ltw->matrix) * world_matrix;
          }
        }
      }

      glm::vec3 new_pos;
      glm::vec3 new_scale;
      glm::vec3 skew;
      glm::vec4 persp;
      glm::quat new_rot;

      if (glm::decompose(local_matrix, new_scale, new_rot, new_pos, skew,
                         persp)) {
        auto &&[pos, rot, scale] = transform.mut();
        pos = new_pos;
        rot = glm::normalize(new_rot);
        scale = new_scale;

        editor_state.hierarchy_dirty = true;
      }
    }
  }
  ImGui::End();

  draw_debug_shapes();
  canvas_renderer->render_2d();

  if (ImGui::Begin("Sun direction")) {
    if (ImGui::DragFloat3("Sun direction",
                          glm::value_ptr(renderer->sun_direction), 0.1F, -1.0F,
                          1.0F)) {
      editor_state.hierarchy_dirty = true;
    }

    ImGui::Separator();
    ImGui::SeparatorText("Shadow Settings");
    ImGui::DragFloat("Shadow Distance", &shadow_map_state.far_plane, 1.0F,
                     10.0F, 1000.0F, "%.0f m");
    static bool always_invalidate_shadows = true;
    ImGui::Checkbox("Always invalidate", &always_invalidate_shadows);
    if (always_invalidate_shadows || ImGui::IsItemDeactivatedAfterEdit()) {
      shadow_map_state.invalid = true;
      editor_state.hierarchy_dirty = true;
    }
  }
  ImGui::End();
  {
    ZoneScopedNC("draw_toolbar", 0x888888);
    draw_toolbar();
  }
  for (auto &panel : panels)
    panel->draw(editor_state, editor_actions);
  {
    ZoneScopedNC("draw_performance_overlay", 0x888888);
    draw_performance_overlay();
  }
  {
    ZoneScopedNC("draw_hdr_selector", 0x888888);
    draw_hdr_selector();
  }

  auto draw_csm = [](CsmResources &resources) {
    if (ImGui::Begin("Cascaded Shadow Map Debug")) {
      static int selected_cascade = 0;
      if (selected_cascade >= shadow_map_cascade_count) {
        selected_cascade = 0;
      }

      std::string preview_text = "Cascade " + std::to_string(selected_cascade);

      if (ImGui::BeginCombo("Level", preview_text.c_str())) {
        for (int i = 0; i < shadow_map_cascade_count; i++) {
          const bool is_selected = (selected_cascade == i);
          std::string item_text = "Cascade " + std::to_string(i);

          if (ImGui::Selectable(item_text.c_str(), is_selected)) {
            selected_cascade = i;
          }

          if (is_selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }

      ImGui::Image(ImTextureRef{ImTextureID{
                       resources.layer_handles[selected_cascade].index()}},
                   ImVec2(256, 256));
    }
    ImGui::End();
  };
  draw_csm(renderer->csm);
  if (viewport_hovered || viewport_focused) {
    ImGui::GetIO().WantCaptureMouse = false;
    ImGui::GetIO().WantCaptureKeyboard = false;
  }
}
