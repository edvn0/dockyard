#include "dockyard/bindless_handle.hpp"
#include "dockyard/scene_serialiser.hpp"
#include <assert.h>
#include <cstdint>
#include <dockyard/component_traits.hpp>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dy {
namespace {
constexpr u32 max_string_length = 4096;
[[maybe_unused]] auto is_valid_utf8(std::string_view str) -> bool {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(str.data());
  std::size_t i = 0;
  std::size_t len = str.size();

  while (i < len) {
    if (bytes[i] <= 0x7F) {
      i += 1;
    } else if ((bytes[i] & 0xE0) == 0xC0) {
      if (i + 1 >= len || (bytes[i + 1] & 0xC0) != 0x80)
        return false;
      if (bytes[i] < 0xC2)
        return false;
      i += 2;
    } else if ((bytes[i] & 0xE0) == 0xE0) {
      if (i + 2 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
          (bytes[i + 2] & 0xC0) != 0x80)
        return false;
      if (bytes[i] == 0xE0 && bytes[i + 1] < 0xA0)
        return false;
      if (bytes[i] == 0xED && bytes[i + 1] >= 0xA0)
        return false;
      i += 3;
    } else if ((bytes[i] & 0xF8) == 0xF0) {
      if (i + 3 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
          (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80)
        return false;
      if (bytes[i] == 0xF0 && bytes[i + 1] < 0x90)
        return false;
      if (bytes[i] == 0xF4 && bytes[i + 1] >= 0x90)
        return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

void write_safe_string(auto &archive, std::string_view view) {
  auto size = static_cast<u32>(view.size());
  archive.writer.write(&size, sizeof(size));
  if (size > 0) {
    archive.writer.write(view.data(), size);
  }
}

template <typename... Args>
  requires(sizeof...(Args) >= 1)
void write_safe_string(auto &archive, std::format_string<Args...> fmt,
                       Args &&...args) {
  auto view = std::format(fmt, std::forward<Args>(args)...);
  auto size = static_cast<u32>(view.size());
  archive.writer.write(&size, sizeof(size));
  if (size > 0) {
    archive.writer.write(view.data(), size);
  }
}

auto read_safe_string(auto &archive) -> std::string {
  u32 size = 0;
  archive.reader.read(&size, sizeof(size));

  if (size > max_string_length) {
    throw std::runtime_error(
        "Deserialization failed: String payload length limits exceeded.");
  }

  std::string str;
  str.resize(size);
  if (size > 0) {
    archive.reader.read(str.data(), size);
  }

#ifndef NDEBUG
  if (!is_valid_utf8(str)) {
    std::abort();
  }
#endif

  return str;
}

} // namespace

void ComponentSerializer<Components::Tag>::save(auto &archive,
                                                const Components::Tag &value) {
  write_safe_string(archive, value.tag);
}

void ComponentSerializer<Components::Tag>::load(auto &archive,
                                                Components::Tag &value) {
  value.tag = read_safe_string(archive);
}

void ComponentSerializer<Components::MeshRequest>::save(
    auto &archive, const Components::MeshRequest &value) {
  std::string_view view = value.path.view();
  write_safe_string(archive, view);
}

void ComponentSerializer<Components::MeshRequest>::load(
    auto &archive, Components::MeshRequest &value) {
  std::string path_str = read_safe_string(archive);

  if (path_str.empty()) {
    value.path = NullableVFSPath{
        .path = std::nullopt,
    };
  } else {
    value.path = NullableVFSPath::create("{}", path_str);
  }
}

void ComponentSerializer<Components::Mesh>::save(auto &archive,
                                                 const Components::Mesh &mesh) {
  write_safe_string(archive, mesh.source_path.view());
}

void ComponentSerializer<Components::Mesh>::load(auto &archive,
                                                 Components::Mesh &m) {
  const std::string path_str = read_safe_string(archive);
  if (path_str.empty()) {
    m.source_path = NullableVFSPath{};
  } else {
    m.source_path = NullableVFSPath::create("{}", path_str);
  }
  m.handle = {};
}

void ComponentSerializer<Components::Camera>::save(
    auto &archive, const Components::Camera &value) {
  archive.writer.write(&value.fov_degrees, sizeof(float));
  archive.writer.write(&value.near_plane, sizeof(float));
  archive.writer.write(&value.far_plane, sizeof(float));
  archive.writer.write(&value.aspect, sizeof(float));
  archive.writer.write(&value.position, sizeof(glm::vec3));
  archive.writer.write(&value.yaw, sizeof(float));
  archive.writer.write(&value.pitch, sizeof(float));
  archive.writer.write(&value.is_perspective, sizeof(bool));
  archive.writer.write(&value.ortho_left, sizeof(f32));
  archive.writer.write(&value.ortho_right, sizeof(f32));
  archive.writer.write(&value.ortho_bottom, sizeof(f32));
  archive.writer.write(&value.ortho_top, sizeof(f32));

  bool has_override = value.forward_override.has_value();
  archive.writer.write(&has_override, sizeof(bool));
  if (has_override) {
    archive.writer.write(&value.forward_override.value(), sizeof(glm::vec3));
  }
}

void ComponentSerializer<Components::Camera>::load(auto &archive,
                                                   Components::Camera &value) {
  archive.reader.read(&value.fov_degrees, sizeof(float));
  archive.reader.read(&value.near_plane, sizeof(float));
  archive.reader.read(&value.far_plane, sizeof(float));
  archive.reader.read(&value.aspect, sizeof(float));
  archive.reader.read(&value.position, sizeof(glm::vec3));
  archive.reader.read(&value.yaw, sizeof(float));
  archive.reader.read(&value.pitch, sizeof(float));
  archive.reader.read(&value.is_perspective, sizeof(bool));
  archive.reader.read(&value.ortho_left, sizeof(f32));
  archive.reader.read(&value.ortho_right, sizeof(f32));
  archive.reader.read(&value.ortho_bottom, sizeof(f32));
  archive.reader.read(&value.ortho_top, sizeof(f32));

  bool has_override = false;
  archive.reader.read(&has_override, sizeof(bool));
  if (has_override) {
    glm::vec3 override_vec{};
    archive.reader.read(&override_vec, sizeof(glm::vec3));
    value.forward_override = override_vec;
  } else {
    value.forward_override = std::nullopt;
  }
}

void ComponentSerializer<Components::Transform>::save(
    auto &archive, const Components::Transform &value) {
  auto accessor = value.get();
  archive.writer.write(&accessor.position, sizeof(glm::vec3));
  archive.writer.write(&accessor.rotation, sizeof(glm::quat));
  archive.writer.write(&accessor.scale, sizeof(glm::vec3));
}

void ComponentSerializer<Components::Transform>::load(
    auto &archive, Components::Transform &value) {
  glm::vec3 position{};
  glm::quat rotation{};
  glm::vec3 scale{};

  archive.reader.read(&position, sizeof(glm::vec3));
  archive.reader.read(&rotation, sizeof(glm::quat));
  archive.reader.read(&scale, sizeof(glm::vec3));

  auto mutator = value.mut();
  mutator.position = position;
  mutator.rotation = rotation;
  mutator.scale = scale;

  value.set_dirty(true);
}

void ComponentSerializer<Components::LocalToWorld>::save(
    auto &archive, const Components::LocalToWorld &value) {
  archive.writer.write(&value.matrix, sizeof(glm::mat4));
}

void ComponentSerializer<Components::LocalToWorld>::load(
    auto &archive, Components::LocalToWorld &value) {
  archive.reader.read(&value.matrix, sizeof(glm::mat4));
}

void ComponentSerializer<Components::PointLight>::save(
    auto &archive, const Components::PointLight &value) {
  archive.writer.write(&value.color, sizeof(glm::vec3));
  archive.writer.write(&value.intensity, sizeof(float));
  archive.writer.write(&value.radius, sizeof(float));
}

void ComponentSerializer<Components::PointLight>::load(
    auto &archive, Components::PointLight &value) {
  archive.reader.read(&value.color, sizeof(glm::vec3));
  archive.reader.read(&value.intensity, sizeof(float));
  archive.reader.read(&value.radius, sizeof(float));
}

void ComponentSerializer<AnimationState>::save(auto &archive,
                                               const AnimationState &s) {
  write_safe_string(archive, s.clip ? s.clip->name : s.pending_clip_name);
  archive.writer.write(&s.skeleton_index, sizeof(i32));
  archive.writer.write(&s.time, sizeof(f32));
  archive.writer.write(&s.loop, sizeof(bool));
}

void ComponentSerializer<AnimationState>::load(auto &archive,
                                               AnimationState &s) {
  s.pending_clip_name = read_safe_string(archive);
  archive.reader.read(&s.skeleton_index, sizeof(i32));
  archive.reader.read(&s.time, sizeof(f32));
  archive.reader.read(&s.loop, sizeof(bool));
  s.skeleton = nullptr;
  s.clip = nullptr;
}

void ComponentFixup<Components::Mesh>::fixup(entt::registry &,
                                             entt::entity,
                                             Components::Mesh &mesh,
                                             const FixupContext &ctx) {
  if (!ctx.find_mesh || !mesh.source_path.path)
    return;
  auto [handle, asset] = ctx.find_mesh(mesh.source_path);
  if (asset != nullptr)
    mesh.handle = handle;
}

void ComponentFixup<AnimationState>::fixup(entt::registry &reg,
                                           entt::entity entity,
                                           AnimationState &anim,
                                           const FixupContext &ctx) {
  if (!ctx.find_mesh || anim.pending_clip_name.empty())
    return;

  const auto *mesh_comp = reg.try_get<Components::Mesh>(entity);
  if (mesh_comp == nullptr)
    return;

  auto [handle, asset] = ctx.find_mesh(mesh_comp->source_path);
  if (asset == nullptr)
    return;

  const Skeleton *skeleton = nullptr;
  if (anim.skeleton_index >= 0 &&
      std::cmp_less(anim.skeleton_index ,asset->skeletons.size()))
    skeleton = &asset->skeletons[anim.skeleton_index];

  const AnimationClip *clip = nullptr;
  for (const auto &c : asset->animations) {
    if (c.name == anim.pending_clip_name) {
      clip = &c;
      break;
    }
  }
  if ((clip == nullptr) && !asset->animations.empty())
    clip = asset->animations.data();

  if ((skeleton == nullptr) || (clip == nullptr))
    return;

  const f32 saved_time = anim.time;
  const bool saved_loop = anim.loop;
  anim = AnimationState::create(skeleton, clip, anim.skeleton_index);
  anim.time = saved_time;
  anim.loop = saved_loop;
}

void ComponentSerializer<Components::MaterialOverride>::save(
    auto &archive, const Components::MaterialOverride &m) {
  archive.writer.write(&m.material, sizeof(m.material));
}
void ComponentSerializer<Components::MaterialOverride>::load(
    auto &archive, Components::MaterialOverride &m) {
  archive.reader.read(&m.material, sizeof(m.material));
  m.gpu_slot = Components::MaterialOverride::invalid_material;
  m.dirty = true;
}

#define EXPLICITLY_DEFINE(T)                                                   \
  template void ComponentSerializer<T>::load(CompileTimeInputArchive &, T &);  \
  template void ComponentSerializer<T>::save(CompileTimeOutputArchive &,       \
                                             const T &);

EXPLICITLY_DEFINE(Components::Tag);
EXPLICITLY_DEFINE(Components::MeshRequest);
EXPLICITLY_DEFINE(Components::Camera);
EXPLICITLY_DEFINE(Components::Transform);
EXPLICITLY_DEFINE(Components::LocalToWorld);
EXPLICITLY_DEFINE(Components::PointLight);
EXPLICITLY_DEFINE(Components::Mesh);
EXPLICITLY_DEFINE(Components::MaterialOverride);
EXPLICITLY_DEFINE(AnimationState);

} // namespace dy
