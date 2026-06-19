#pragma once

#include <dockyard/vfs_path.hpp>
#include <expected>
#include <memory>
#include <string>

namespace dy {
class Scene;
struct IAssetLoader;

class ScriptEngine {
public:
  ScriptEngine(const ScriptEngine &) = delete;
  ScriptEngine &operator=(const ScriptEngine &) = delete;
  ScriptEngine(ScriptEngine &&) = delete;
  ScriptEngine &operator=(ScriptEngine &&) = delete;
  ~ScriptEngine();

  static auto create() -> std::unique_ptr<ScriptEngine>;

  // Load (or reload) the game script. On failure the previously loaded game
  // table is preserved and an error string is returned.
  auto load(const VFSPath &path) -> std::expected<void, std::string>;
  [[nodiscard]] auto loaded() const -> bool;

  auto pre_init(IAssetLoader &) -> void;
  auto init(Scene *, IAssetLoader &) -> void;
  auto update(Scene *, float dt) -> void;
  auto destroy(Scene *) -> void;

  // Returns true if the script was successfully hot-reloaded this tick.
  auto poll_reload() -> bool;
  auto start_watching() -> void;
  auto stop_watching() -> void;

private:
  ScriptEngine();
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace dy
