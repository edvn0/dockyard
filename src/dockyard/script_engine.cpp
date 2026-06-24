// sol2 headers are included here only. Warning suppression for this TU is
// applied via set_source_files_properties in Targets.cmake.

#include <dockyard/script_engine.hpp>

#include <dockyard/asset_loader.hpp>
#include <dockyard/components.hpp>
#include <dockyard/log.hpp>
#include <dockyard/scene.hpp>
#include <dockyard/vfs.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <sol/sol.hpp>

#include <efsw/efsw.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <string>

namespace dy {

// efsw listener that sets a flag when the watched .lua file changes.
struct LuaFileWatcher : efsw::FileWatchListener {
  std::atomic<bool> reload_pending{false};
  std::string watched_filename;

  explicit LuaFileWatcher(std::string filename)
      : watched_filename(std::move(filename)) {}

  void handleFileAction(efsw::WatchID, const std::string &, const std::string &filename,
                        efsw::Action action, const std::string &) override {
    if (action == efsw::Actions::Delete)
      return;
    if (filename != watched_filename)
      return;
    reload_pending.store(true, std::memory_order_relaxed);
  }
};

struct ScriptEngine::Impl {
  sol::state lua;
  sol::table game_table;
  std::optional<VFSPath> loaded_path;

  std::unique_ptr<LuaFileWatcher> file_watcher;
  efsw::FileWatcher efsw_watcher;
  efsw::WatchID watch_id{-1};
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static auto call_game_fn(sol::table &table, const char *method, auto &&...args)
    -> void {
  sol::optional<sol::object> slot = table[method];
  if (!slot || !slot->valid())
    return;
  sol::protected_function pf = slot->as<sol::function>();
  auto result = pf(std::forward<decltype(args)>(args)...);
  if (!result.valid()) {
    sol::error err = result;
    dy::error("[Lua] {}: {}", method, err.what());
  }
}

// ---------------------------------------------------------------------------
// Binding registration
// ---------------------------------------------------------------------------

static auto register_bindings(sol::state &lua) -> void {
  // glm::vec3
  lua.new_usertype<glm::vec3>(
      "vec3",
      sol::call_constructor,
      sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
      "x", &glm::vec3::x,
      "y", &glm::vec3::y,
      "z", &glm::vec3::z,
      sol::meta_function::addition,
          [](const glm::vec3 &a, const glm::vec3 &b) { return a + b; },
      sol::meta_function::subtraction,
          [](const glm::vec3 &a, const glm::vec3 &b) { return a - b; },
      sol::meta_function::multiplication,
          sol::overload(
              [](const glm::vec3 &a, const glm::vec3 &b) { return a * b; },
              [](const glm::vec3 &a, float s) { return a * s; },
              [](float s, const glm::vec3 &a) { return s * a; }),
      sol::meta_function::unary_minus,
          [](const glm::vec3 &a) { return -a; },
      sol::meta_function::to_string, [](const glm::vec3 &v) {
        return std::format("vec3({}, {}, {})", v.x, v.y, v.z);
      });

  // glm::vec4
  lua.new_usertype<glm::vec4>(
      "vec4",
      sol::call_constructor,
      sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
      "x", &glm::vec4::x,
      "y", &glm::vec4::y,
      "z", &glm::vec4::z,
      "w", &glm::vec4::w,
      sol::meta_function::to_string, [](const glm::vec4 &v) {
        return std::format("vec4({}, {}, {}, {})", v.x, v.y, v.z, v.w);
      });

  // glm::quat
  lua.new_usertype<glm::quat>(
      "quat",
      sol::call_constructor,
      sol::constructors<glm::quat(), glm::quat(float, float, float, float)>(),
      "x", &glm::quat::x,
      "y", &glm::quat::y,
      "z", &glm::quat::z,
      "w", &glm::quat::w);
  lua.set_function("quat_from_euler", [](float x, float y, float z) {
    return glm::quat(glm::vec3(x, y, z));
  });

  // VFSPath
  lua.new_usertype<VFSPath>(
      "VFSPath",
      sol::no_constructor,
      "create",
          sol::factories([](std::string_view s) { return VFSPath::create(s); }),
      "view",
          [](const VFSPath &p) { return std::string(p.view()); });

  // MeshAssetHandle
  lua.new_usertype<MeshAssetHandle>(
      "MeshAssetHandle",
      sol::no_constructor,
      "index", &MeshAssetHandle::index,
      "valid", &MeshAssetHandle::valid);

  // AnimationState — opaque from Lua; created via assets:make_animation_state()
  lua.new_usertype<AnimationState>("AnimationState", sol::no_constructor);

  // IAssetLoader — load_mesh returns (handle, nil) or (nil, errmsg)
  //              — make_animation_state returns (state, nil) or (nil, errmsg)
  lua.new_usertype<IAssetLoader>(
      "IAssetLoader",
      sol::no_constructor,
      "load_mesh",
          [](sol::this_state L, IAssetLoader &loader,
             const VFSPath &path) -> sol::variadic_results {
            sol::state_view sv{L};
            auto result = loader.load_mesh(path);
            sol::variadic_results ret;
            if (result) {
              ret.push_back(sol::make_object(sv, *result));
              ret.push_back(sol::lua_nil);
            } else {
              ret.push_back(sol::lua_nil);
              ret.push_back(sol::make_object(sv, result.error()));
            }
            return ret;
          },
      "make_animation_state",
          [](sol::this_state L, IAssetLoader &loader, MeshAssetHandle handle,
             u32 skel_idx, u32 clip_idx) -> sol::variadic_results {
            sol::state_view sv{L};
            auto result = loader.make_animation_state(handle, skel_idx, clip_idx);
            sol::variadic_results ret;
            if (result) {
              ret.push_back(sol::make_object(sv, std::move(*result)));
              ret.push_back(sol::lua_nil);
            } else {
              ret.push_back(sol::lua_nil);
              ret.push_back(
                  sol::make_object(sv, std::string("no skeleton/animation at given indices")));
            }
            return ret;
          },
      "notify_material_overrides_added",
          &IAssetLoader::notify_material_overrides_added);

  // Components::Mesh
  lua.new_usertype<Components::Mesh>(
      "Mesh",
      "handle", &Components::Mesh::handle,
      "set_source_path", [](Components::Mesh &m, std::string_view s) {
        m.source_path = VFSPath::create(s);
      });

  // Components::Transform — uses sol::property to go through mut()/get()
  lua.new_usertype<Components::Transform>(
      "Transform",
      sol::no_constructor,
      "position",
          sol::property(
              [](Components::Transform &t) { return t.get().position; },
              [](Components::Transform &t, glm::vec3 v) {
                t.mut().position = v;
              }),
      "rotation",
          sol::property(
              [](Components::Transform &t) { return t.get().rotation; },
              [](Components::Transform &t, glm::quat q) {
                t.mut().rotation = q;
              }),
      "scale",
          sol::property(
              [](Components::Transform &t) { return t.get().scale; },
              [](Components::Transform &t, glm::vec3 v) {
                t.mut().scale = v;
              }));

  // Components::MaterialOverride — wraps the C-array albedo_factor
  lua.new_usertype<Components::MaterialOverride>(
      "MaterialOverride",
      sol::no_constructor,
      "set_albedo",
          [](Components::MaterialOverride &m, float r, float g, float b,
             float a) {
            m.material.albedo_factor[0] = r;
            m.material.albedo_factor[1] = g;
            m.material.albedo_factor[2] = b;
            m.material.albedo_factor[3] = a;
            m.dirty = true;
          },
      "set_roughness",
          [](Components::MaterialOverride &m, float v) {
            m.material.roughness_factor = v;
            m.dirty = true;
          },
      "set_metallic", [](Components::MaterialOverride &m, float v) {
        m.material.metallic_factor = v;
        m.dirty = true;
      });

  // Components::PointLight
  lua.new_usertype<Components::PointLight>(
      "PointLight",
      "color", &Components::PointLight::color,
      "intensity", &Components::PointLight::intensity,
      "radius", &Components::PointLight::radius);

  // Entity — one explicit method per component type (templates can't be
  // instantiated from Lua)
  lua.new_usertype<Entity>(
      "Entity",
      sol::no_constructor,
      "id", [](Entity &e) { return static_cast<u32>(e.id()); },
      "valid", &Entity::valid,
      "add_mesh",
          [](Entity &e) -> Components::Mesh & {
            return e.emplace<Components::Mesh>();
          },
      "get_mesh",
          [](Entity &e) -> Components::Mesh * {
            return e.try_get<Components::Mesh>();
          },
      "get_transform",
          [](Entity &e) -> Components::Transform & {
            return e.get<Components::Transform>();
          },
      "add_material_override",
          [](Entity &e) -> Components::MaterialOverride & {
            return e.emplace_or_replace<Components::MaterialOverride>();
          },
      "add_point_light",
          [](Entity &e) -> Components::PointLight & {
            return e.emplace<Components::PointLight>();
          },
      "add_animation_state",
          [](Entity &e, AnimationState anim) -> AnimationState & {
            return e.emplace<AnimationState>(std::move(anim));
          });

  // Scene
  lua.new_usertype<Scene>(
      "Scene",
      sol::no_constructor,
      "make",
          sol::overload(
              [](Scene &s, std::string_view name) { return s.make(name); },
              [](Scene &s, std::string_view name, Entity &parent) {
                return s.make(name, parent);
              }),
      // Destroy an entity by the integer id returned from Entity:id()
      "destroy_entity",
          [](Scene &s, u32 id) {
            auto e = static_cast<entt::entity>(id);
            if (s.registry().valid(e))
              s.registry().destroy(e);
          },
      "is_valid",
          [](Scene &s, u32 id) {
            return s.registry().valid(static_cast<entt::entity>(id));
          },
      // Get a Transform component for an entity by id
      "get_transform",
          [](Scene &s, u32 id) -> Components::Transform * {
            auto e = static_cast<entt::entity>(id);
            if (!s.registry().valid(e))
              return nullptr;
            return s.registry().try_get<Components::Transform>(e);
          },
      // Iterate all entities with both Transform and Mesh; cb(id, transform, mesh)
      "each_mesh", [](Scene &s, sol::function cb) {
        s.view<Components::Transform, Components::Mesh>().each(
            [&](entt::entity e, Components::Transform &t, Components::Mesh &m) {
              cb(static_cast<u32>(e), &t, &m);
            });
      });
}

// ---------------------------------------------------------------------------
// ScriptEngine lifecycle
// ---------------------------------------------------------------------------

ScriptEngine::ScriptEngine() : impl(std::make_unique<Impl>()) {}

ScriptEngine::~ScriptEngine() { stop_watching(); }

auto ScriptEngine::create() -> std::unique_ptr<ScriptEngine> {
  auto engine = std::unique_ptr<ScriptEngine>(new ScriptEngine{});
  engine->impl->lua.open_libraries(
      sol::lib::base, sol::lib::math, sol::lib::string,
      sol::lib::table, sol::lib::io, sol::lib::os);
  register_bindings(engine->impl->lua);
  return engine;
}

auto ScriptEngine::load(const VFSPath &path) -> std::expected<void, std::string> {
  const auto physical = VFS::get().resolve(path).string();

  auto result = impl->lua.safe_script_file(physical, &sol::script_pass_on_error);
  if (!result.valid()) {
    sol::error err = result;
    return std::unexpected(err.what());
  }

  sol::optional<sol::table> tbl = result;
  if (!tbl)
    return std::unexpected("Script did not return a table");

  impl->game_table = *tbl;
  impl->loaded_path = path;
  info("[ScriptEngine] Loaded '{}'", path.view());
  return {};
}

auto ScriptEngine::loaded() const -> bool {
  return impl && impl->game_table.valid();
}

auto ScriptEngine::pre_init(IAssetLoader &assets) -> void {
  if (!loaded())
    return;
  call_game_fn(impl->game_table, "pre_init", assets);
}

auto ScriptEngine::init(Scene *scene, IAssetLoader &assets) -> void {
  if (!loaded())
    return;
  call_game_fn(impl->game_table, "init", scene, assets);
}

auto ScriptEngine::update(Scene *scene, float dt) -> void {
  if (!loaded())
    return;
  call_game_fn(impl->game_table, "update", scene, dt);
}

auto ScriptEngine::destroy(Scene *scene) -> void {
  if (!loaded())
    return;
  call_game_fn(impl->game_table, "destroy", scene);
}

auto ScriptEngine::poll_reload() -> bool {
  if (!impl->file_watcher)
    return false;
  if (!impl->file_watcher->reload_pending.exchange(false,
                                                    std::memory_order_relaxed))
    return false;

  auto result = load(*impl->loaded_path);
  if (!result) {
    dy::error("[ScriptEngine] Hot reload failed: {}", result.error());
    // Keep the old game_table intact so the running game isn't destroyed
    impl->game_table = sol::table{};
    return false;
  }
  return true;
}

auto ScriptEngine::start_watching() -> void {
  if (!impl->loaded_path || impl->watch_id >= 0)
    return;

  const auto physical = VFS::get().resolve(*impl->loaded_path);
  const auto dir = physical.parent_path();
  const auto filename = physical.filename().string();

  impl->file_watcher = std::make_unique<LuaFileWatcher>(filename);
  impl->watch_id =
      impl->efsw_watcher.addWatch(dir.string(), impl->file_watcher.get(),
                                   /*recursive=*/false);
  if (impl->watch_id < 0)
    warn("[ScriptEngine] Failed to watch '{}': {}", dir.string(),
         efsw::Errors::Log::getLastErrorLog());
  else
    impl->efsw_watcher.watch();
}

auto ScriptEngine::stop_watching() -> void {
  if (impl->watch_id >= 0) {
    impl->efsw_watcher.removeWatch(impl->watch_id);
    impl->watch_id = -1;
  }
  impl->file_watcher.reset();
}

} // namespace dy
