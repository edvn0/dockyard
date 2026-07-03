#include <dockyard/texture_upload_pool.hpp>

#include <dockyard/crash_reporter.hpp>
#include <dockyard/scene_renderer.hpp>

namespace dy::pool {

void TextureUploadPool::poll_n(SceneRenderer &renderer, usize n) {
  auto batch = take_ready(n);

  if (batch.empty()) {
    return;
  }

  bool marked_dirty = false;

  breadcrumb("texture_upload_commit");
  for (auto &entry : batch) {
    auto data = entry.work.get();

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

  note_completed(batch.size());

  if (marked_dirty) {
    renderer.bindless.mark_dirty();
  }
}

} // namespace dy::pool