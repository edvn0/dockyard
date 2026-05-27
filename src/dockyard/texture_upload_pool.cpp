#include <dockyard/texture_upload_pool.hpp>

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

void TextureUploadPool::poll_one(SceneRenderer &renderer) {
  PendingUpload entry;
  {
    std::scoped_lock lock{work_mutex};
    auto it = std::ranges::find_if(pending_work, [](const PendingUpload &p) {
      return p.cpu_work.wait_for(std::chrono::seconds{0}) ==
             std::future_status::ready;
    });
    if (it == pending_work.end())
      return;
    entry = std::move(*it);
    pending_work.erase(it);
  }

  auto data = entry.cpu_work.get();

  if (!data.cache_key.empty()) {
    if (auto cached = renderer.texture_cache.get(data.cache_key)) {
      entry.on_complete(*cached);
      return;
    }
  }

  auto tex =
      Texture::from_bytes(renderer.ctx, data.name, data.to_create_info());
  auto handle = renderer.textures.create(
      {.texture = tex, .sampled_view_type = VK_IMAGE_VIEW_TYPE_2D});
  if (!data.cache_key.empty())
    renderer.texture_cache.insert(data.cache_key, handle);

  renderer.bindless.mark_dirty();
  entry.on_complete(handle);
}

[[nodiscard]] bool TextureUploadPool::empty() const {
  std::scoped_lock lock{work_mutex};
  return pending_work.empty();
}

} // namespace dy::pool