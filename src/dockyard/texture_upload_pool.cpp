#include <dockyard/texture_upload_pool.hpp>

#include <dockyard/crash_reporter.hpp>
#include <dockyard/scene_renderer.hpp>

namespace dy::pool {

auto TextureUploadPool::drop() -> void {
  std::scoped_lock lock{work_mutex};
  stopped.store(true, std::memory_order_relaxed);

  for (auto &entry : pending_work) {
    entry.stop_src.request_stop();
  }

  pending_work.clear();
}

void TextureUploadPool::poll_n(SceneRenderer &renderer, usize n) {
  std::vector<PendingUpload> batch;
  batch.reserve(n);

  {
    std::scoped_lock lock{work_mutex};

    auto it = pending_work.begin();
    while (it != pending_work.end() && batch.size() < n) {
      if (it->cpu_work.wait_for(std::chrono::seconds{0}) ==
          std::future_status::ready) {
        batch.push_back(std::move(*it));
        it = pending_work.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (batch.empty()) {
    return;
  }

  bool marked_dirty = false;

  breadcrumb("texture_upload_commit");
  for (auto &entry : batch) {
    auto data = entry.cpu_work.get();

    if (data.width == 0 || data.height == 0) {
      entry.on_complete(TextureHandle{});
      continue;
    }

    if (!data.cache_key.empty()) {
      if (auto cached = renderer.texture_cache.get(data.cache_key)) {
        entry.on_complete(*cached);
        continue;
      }
    }

    auto tex =
        Texture::from_bytes(renderer.ctx, data.name, data.to_create_info());
    auto handle = renderer.textures.create(
        {.texture = tex, .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D});

    if (!data.cache_key.empty()) {
      renderer.texture_cache.insert(data.cache_key, handle);
    }

    marked_dirty = true;
    entry.on_complete(handle);
  }

  completed.fetch_add(batch.size(), std::memory_order_relaxed);

  if (marked_dirty) {
    renderer.bindless.mark_dirty();
  }
}

auto TextureUploadPool::stats() const -> Stats {
  std::scoped_lock lock{work_mutex};
  return {
      .pending = pending_work.size(),
      .total_submitted = submitted.load(std::memory_order_relaxed),
      .total_completed = completed.load(std::memory_order_relaxed),
  };
}

[[nodiscard]] bool TextureUploadPool::empty() const {
  std::scoped_lock lock{work_mutex};
  return pending_work.empty();
}

} // namespace dy::pool