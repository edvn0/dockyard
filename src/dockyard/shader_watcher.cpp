#include <chrono>
#include <dockyard/shader_watcher.hpp>
#include <dockyard/vfs.hpp>
#include <format>

namespace dy::shader {

ShaderWatcher::ShaderWatcher(const VFSPath &watch_root, ChangeCallback on_change)
    : real_path(VFS::get().resolve(watch_root)), mount(watch_root.scheme()),
      on_change(std::move(on_change)) {

  info("ShaderWatcher initialized for mount '{}', watching '{}'", mount,
       real_path.string());
}

ShaderWatcher::~ShaderWatcher() { stop(); }

auto ShaderWatcher::start() -> void {
  is_running = true;
  debounce_thread = std::thread(&ShaderWatcher::debounce_loop, this);

  watch_id = watcher.addWatch(real_path.string(), this, /*recursive=*/true);
  if (watch_id < 0)
    warn("ShaderWatcher: failed to watch '{}': {}", real_path.string(),
         efsw::Errors::Log::getLastErrorLog());
  watcher.watch();
}

auto ShaderWatcher::stop() -> void {
  if (watch_id >= 0) {
    watcher.removeWatch(watch_id);
    watch_id = -1;
  }

  {
    std::scoped_lock lock(debounce_mutex);
    is_running = false;
  }
  debounce_cv.notify_all();

  if (debounce_thread.joinable()) {
    debounce_thread.join();
  }
}

void ShaderWatcher::handleFileAction(efsw::WatchID, const std::string &dir,
                                     const std::string &filename,
                                     efsw::Action action, const std::string &) {
  if (action == efsw::Actions::Delete)
    return;

  const std::filesystem::path name{filename};
  if (name.extension() != ".slang")
    return;

  const auto rel = std::filesystem::relative(dir / name, real_path);
  const auto path = std::format("{}://{}", mount, rel.generic_string());

  {
    std::scoped_lock lock(debounce_mutex);
    pending_changes.insert(path);
  }
  debounce_cv.notify_all();
}

void ShaderWatcher::debounce_loop() {
  while (true) {
    std::unordered_set<std::string> changes_to_fire;

    {
      std::unique_lock lock(debounce_mutex);

      debounce_cv.wait(
          lock, [this] { return !is_running || !pending_changes.empty(); });

      if (!is_running)
        return;

      while (!pending_changes.empty()) {
        const auto status =
            debounce_cv.wait_for(lock, std::chrono::milliseconds(100));

        if (!is_running) {
          warn("Got here for some reason.");
          return;
        }

        if (status == std::cv_status::timeout)
          break;
      }

      changes_to_fire = std::move(pending_changes);
    }

    for (const auto &path : changes_to_fire) {
      on_change(VFSPath::create(path));
    }
  }
}

} // namespace dy::shader