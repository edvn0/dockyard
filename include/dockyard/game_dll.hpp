#pragma once

#include <dockyard/igame.hpp>

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace dy {

class Scene;
struct GameMemory;

class GameDll {
public:
    [[nodiscard]] static auto load(std::filesystem::path path)
        -> std::expected<std::unique_ptr<GameDll>, std::string>;

    ~GameDll();

    GameDll(const GameDll&)            = delete;
    GameDll& operator=(const GameDll&) = delete;
    GameDll(GameDll&&)                 = delete;
    GameDll& operator=(GameDll&&)      = delete;

    [[nodiscard]] auto game() -> IGame* { return game_instance; }

    // Call each frame; returns true if a reload occurred.
    auto poll_reload() -> bool;

    auto start_watching() -> void;
    auto stop_watching() -> void;

private:
    GameDll() = default;

    auto do_reload() -> bool;
    auto unload_instance() -> void;

    std::filesystem::path            source_path;
    std::filesystem::path            loaded_path;
    void*                            handle        = nullptr;
    IGame*                           game_instance = nullptr;
    std::filesystem::file_time_type  last_write_time{};
    u32                              generation    = 0;

    std::atomic<bool> reload_pending{false};
    std::atomic<bool> watching{false};
    std::thread       watch_thread;
};

} // namespace dy
