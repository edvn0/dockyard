#include "dockyard/scene_serialiser.hpp"
#include <assert.h>
#include <cstdint>
#include <dockyard/component_traits.hpp>
#include <stdexcept>
#include <string_view>

namespace dy {
namespace {

constexpr u32 max_string_length = 4096;

// Validates that a string sequence conforms strictly to RFC 3629 UTF-8.
auto is_valid_utf8(std::string_view str) -> bool {
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

  if (!is_valid_utf8(str)) {
    std::abort();
  }

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

void ComponentSerializer<Components::Camera>::save(
    auto &archive, const Components::Camera &value) {
  archive.writer.write(&value.fov_degrees, sizeof(float));
  archive.writer.write(&value.near_plane, sizeof(float));
  archive.writer.write(&value.far_plane, sizeof(float));
  archive.writer.write(&value.aspect, sizeof(float));
  archive.writer.write(&value.position, sizeof(glm::vec3));
  archive.writer.write(&value.yaw, sizeof(float));
  archive.writer.write(&value.pitch, sizeof(float));

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

template void
ComponentSerializer<Components::Tag>::load(CompileTimeInputArchive &,
                                           Components::Tag &);
template void
ComponentSerializer<Components::Tag>::save(CompileTimeOutputArchive &,
                                           const Components::Tag &);
template void
ComponentSerializer<Components::MeshRequest>::load(CompileTimeInputArchive &,
                                                   Components::MeshRequest &);
template void ComponentSerializer<Components::MeshRequest>::save(
    CompileTimeOutputArchive &, const Components::MeshRequest &);
template void
ComponentSerializer<Components::Camera>::load(CompileTimeInputArchive &,
                                              Components::Camera &);
template void
ComponentSerializer<Components::Camera>::save(CompileTimeOutputArchive &,
                                              const Components::Camera &);
template void
ComponentSerializer<Components::Transform>::load(CompileTimeInputArchive &,
                                                 Components::Transform &);
template void
ComponentSerializer<Components::Transform>::save(CompileTimeOutputArchive &,
                                                 const Components::Transform &);
template void
ComponentSerializer<Components::LocalToWorld>::load(CompileTimeInputArchive &,
                                                    Components::LocalToWorld &);
template void ComponentSerializer<Components::LocalToWorld>::save(
    CompileTimeOutputArchive &, const Components::LocalToWorld &);

} // namespace dy