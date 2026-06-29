#pragma once

#include <dockyard/vfs_path.hpp>
#include <efsw/efsw.hpp>

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace dy::shader {

class ShaderWatcher : efsw::FileWatchListener {
public:
  using ChangeCallback = std::function<void(dy::VFSPath)>;

  // watch_root is a VFS mount root, e.g. VFSPath::create("shaders://"). Its
  // scheme is reused when reporting changed files back through on_change.
  ShaderWatcher(const VFSPath &watch_root, ChangeCallback on_change);
  ~ShaderWatcher() override;

  ShaderWatcher(const ShaderWatcher &) = delete;
  ShaderWatcher &operator=(const ShaderWatcher &) = delete;

  void handleFileAction(efsw::WatchID, const std::string &dir,
                        const std::string &filename, efsw::Action action,
                        const std::string &old_filename) override;
  auto start() -> void;
  auto stop() -> void;

private:
  void debounce_loop();

  std::filesystem::path real_path;
  std::string mount;
  ChangeCallback on_change;

  efsw::FileWatcher watcher;
  efsw::WatchID watch_id{-1};

  // Debounce state management
  std::thread debounce_thread;
  std::mutex debounce_mutex;
  std::condition_variable debounce_cv;
  std::unordered_set<std::string> pending_changes;
  bool is_running{false};
};

} // namespace dy::shader