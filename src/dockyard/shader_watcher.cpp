#include <chrono>
#include <dockyard/shader_watcher.hpp>
#include <format>

namespace dy::shader {

ShaderWatcher::ShaderWatcher(std::filesystem::path real_path,
                             std::string_view mount, ChangeCallback on_change)
    : real_path(std::move(real_path)), mount(mount),
      on_change(std::move(on_change)) {

  info("ShaderWatcher initialized for mount '{}', watching '{}'", mount,
       this->real_path.string());
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

  // Safely stop and join the debounce thread
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
      std::unique_lock<std::mutex> lock(debounce_mutex);

      debounce_cv.wait(
          lock, [this] { return !is_running || !pending_changes.empty(); });

      if (!is_running)
        return;

      while (debounce_cv.wait_for(lock, std::chrono::milliseconds(100)) ==
             std::cv_status::no_timeout) {
        if (!is_running)
          return;
      }

      changes_to_fire = std::move(pending_changes);
      pending_changes.clear();
    }

    for (const auto &path : changes_to_fire) {
      on_change(VFSPath::create(path));
    }
  }
}

} // namespace dy::shader