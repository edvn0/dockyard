#include <dockyard/game_dll.hpp>
#include <dockyard/log.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

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
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

auto GameDll::load(std::filesystem::path path)
    -> std::expected<std::unique_ptr<GameDll>, std::string> {
    auto dll = std::unique_ptr<GameDll>(new GameDll{});
    auto copy = std::move(path);
    dll->source_path = std::filesystem::absolute(copy);
    dll->loaded_path = copy.parent_path() / (copy.stem().string() + "_hot_0" +
                                             copy.extension().string());

    std::error_code error_code{};
    std::filesystem::copy_file(
        copy, dll->loaded_path,
        std::filesystem::copy_options::overwrite_existing, error_code);
    if (error_code) return std::unexpected(error_code.message());

    dll->handle = platform_load(dll->loaded_path);
    if (dll->handle == nullptr) return std::unexpected("Failed to load game DLL");

    auto* factory = reinterpret_cast<GameFactory>(
        platform_symbol(dll->handle, "create_game"));
    if (factory == nullptr) return std::unexpected("create_game symbol not found in game DLL");

    dll->game_instance   = factory();
    dll->last_write_time = std::filesystem::last_write_time(path, error_code);

    if (error_code) {
        warn("Could not check last write time: {}", error_code.message());
    }

    return dll;
}

auto GameDll::start_watching() -> void {
    watching = true;
    watch_thread = std::thread([this] {
        while (watching) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::error_code ec;
            auto t = std::filesystem::last_write_time(source_path, ec);
            if (!ec && t != last_write_time) {
                last_write_time = t;
                reload_pending  = true;
                info("Reload queued!");
            }
        }
    });
}

auto GameDll::stop_watching() -> void {
    watching = false;
    if (watch_thread.joinable()) watch_thread.join();
}

auto GameDll::poll_reload() -> bool {
    if (!reload_pending.exchange(false)) return false;
    // Brief wait for the linker to finish writing the file.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    return do_reload();
}

auto GameDll::do_reload() -> bool {
    auto prev_path = loaded_path;
    loaded_path = source_path.parent_path() /
                  (source_path.stem().string() + "_hot_" +
                   std::to_string(++generation) + source_path.extension().string());

    unload_instance();

    std::error_code ec;
    std::filesystem::remove(prev_path, ec);

    std::filesystem::copy_file(
        source_path, loaded_path,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        dy::error("GameDll: reload copy failed: {}", ec.message());
        reload_pending = true;
        return false;
    }

    handle = platform_load(loaded_path);
    if (handle == nullptr) {
      dy::error("GameDll: failed to load reloaded DLL");
      return false;
    }

    auto* factory = reinterpret_cast<GameFactory>(
        platform_symbol(handle, "create_game"));
    if (factory == nullptr) {
      dy::error("GameDll: create_game not found after reload");
      return false;
    }

    game_instance = factory();
    dy::info("GameDll: hot reloaded '{}'", source_path.filename());
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
    std::error_code ec;
    std::filesystem::remove(loaded_path, ec);
}

} // namespace dy
