#include <dockyard/vfs.hpp>
#include <dockyard/game_dll.hpp>
#include <dockyard/log.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dy {

namespace {

auto platform_load(const std::filesystem::path& path) -> void* {
#ifdef _WIN32
    return LoadLibraryW(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

auto platform_symbol(void* handle, const char* name) -> void* {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

auto platform_free(void* handle) -> void {
    if (handle == nullptr) return;

#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

auto GameDll::load(const VFSPath path)
    -> std::expected<std::unique_ptr<GameDll>, std::string> {
    auto dll = std::unique_ptr<GameDll>(new GameDll{});

    dll->vfs_path    = path;
    auto resolved    = VFS::get().resolve(dll->vfs_path.view());
    dll->source_path = std::filesystem::absolute(resolved);
    dll->generation  = 0;
    dll->loaded_path = dll->make_loaded_path();

    std::error_code error_code{};
    std::filesystem::copy_file(
        resolved,
        dll->loaded_path,
        std::filesystem::copy_options::overwrite_existing,
        error_code);

    if (error_code) {
        return std::unexpected(error_code.message());
    }

    dll->handle = platform_load(dll->loaded_path);
    if (dll->handle == nullptr) {
#ifdef _WIN32
        char msg[256]{};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(),
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       msg, sizeof(msg), nullptr);
        return std::unexpected(std::format("Failed to load game DLL ({}): {}", dll->loaded_path.string(), msg));
#else
        return std::unexpected(std::format("Failed to load game DLL ({}): {}", dll->loaded_path.string(), dlerror()));
#endif
    }

    auto* factory = reinterpret_cast<GameFactory>(
        platform_symbol(dll->handle, "create_game"));

    if (factory == nullptr) {
        dll->unload_instance();
        std::filesystem::remove(dll->loaded_path, error_code);
        return std::unexpected("create_game symbol not found in game DLL");
    }

    dll->game_instance = factory();

    if (dll->game_instance == nullptr) {
        dll->unload_instance();
        std::filesystem::remove(dll->loaded_path, error_code);
        return std::unexpected("create_game returned nullptr");
    }

    if (auto write_time = VFS::get().last_write_time(path)) {
        dll->last_write_time = write_time.value();
    } else {
        warn("Could not check last write time for '{}'", path.view());
    }

    info("GameDll: loaded '{}' (gen {})", dll->source_path.filename().string(), dll->generation);
    return dll;
}

auto GameDll::start_watching(BS::priority_thread_pool& pool) -> void {
    thread_pool = &pool;
    watching    = std::make_shared<std::atomic<bool>>(true);

    pool.detach_task(
        [this, alive = watching] {
            poll_task(alive);
        },
        BS::pr::lowest);
}

auto GameDll::stop_watching() -> void {
    if (watching) watching->store(false);

    watching    = nullptr;
    thread_pool = nullptr;
}

auto GameDll::force_reload() -> bool {
    return do_reload();
}

auto GameDll::poll_reload() -> bool {
    if (!reload_pending.exchange(false)) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    return do_reload();
}

auto GameDll::poll_task(std::shared_ptr<std::atomic<bool>> alive) -> void {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (!alive->load()) return;

    if (auto t = VFS::get().last_write_time(vfs_path.value())) {
        if (*t != last_write_time) {
            last_write_time = *t;
            reload_pending  = true;
            info("Reload queued!");
        }
    }

    if (alive->load() && (thread_pool != nullptr)) {
        thread_pool->detach_task(
            [this, alive = std::move(alive)] {
                poll_task(std::move(alive));
            },
            BS::pr::lowest);
    }
}

auto GameDll::make_loaded_path() const -> std::filesystem::path {
    return source_path.parent_path() /
           (source_path.stem().string() + "_hot_" +
            std::to_string(generation) +
            source_path.extension().string());
}

auto GameDll::do_reload() -> bool {
    auto old_handle        = handle;
    auto old_game_instance = game_instance;
    auto old_loaded_path   = loaded_path;
    auto old_generation    = generation;

    ++generation;
    loaded_path = make_loaded_path();

    void* new_handle = nullptr;
    IGame* new_game_instance = nullptr;

    std::error_code ec{};
    std::filesystem::copy_file(
        source_path,
        loaded_path,
        std::filesystem::copy_options::overwrite_existing,
        ec);

    if (ec) {
        dy::error("GameDll: reload copy failed: {}", ec.message());

        generation  = old_generation;
        loaded_path = std::move(old_loaded_path);
        reload_pending = true;

        return false;
    }

    new_handle = platform_load(loaded_path);

    if (new_handle == nullptr) {
        dy::error("GameDll: failed to load reloaded DLL");

        std::filesystem::remove(loaded_path, ec);
        generation  = old_generation;
        loaded_path = std::move(old_loaded_path);
        reload_pending = true;

        return false;
    }

    auto* factory = reinterpret_cast<GameFactory>(
        platform_symbol(new_handle, "create_game"));

    if (factory == nullptr) {
        dy::error("GameDll: create_game not found after reload");

        platform_free(new_handle);
        std::filesystem::remove(loaded_path, ec);

        generation  = old_generation;
        loaded_path = std::move(old_loaded_path);
        reload_pending = true;

        return false;
    }

    new_game_instance = factory();

    if (new_game_instance == nullptr) {
        dy::error("GameDll: create_game returned nullptr after reload");

        platform_free(new_handle);
        std::filesystem::remove(loaded_path, ec);

        generation  = old_generation;
        loaded_path = std::move(old_loaded_path);
        reload_pending = true;

        return false;
    }

    game_instance = new_game_instance;
    handle        = new_handle;

    std::default_delete<IGame> del{};
    del(old_game_instance);

    if (old_handle != nullptr) {
        platform_free(old_handle);
    }

    std::filesystem::remove(old_loaded_path, ec);
    if (ec) {
        dy::warn(
            "GameDll: could not remove previous hot DLL '{}': {}",
            old_loaded_path.string(),
            ec.message());
    }

    dy::info("GameDll: hot reloaded '{}'", source_path.filename().string());
    return true;
}

auto GameDll::unload_instance() -> void {
    if (game_instance != nullptr) {
        delete game_instance;
        game_instance = nullptr;
    }

    if (handle != nullptr) {
        platform_free(handle);
        handle = nullptr;
    }
}

GameDll::~GameDll() {
    stop_watching();
    unload_instance();

    std::error_code ec{};
    std::filesystem::remove(loaded_path, ec);
}

} // namespace dy