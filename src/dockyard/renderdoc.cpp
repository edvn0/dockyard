#include <dockyard/renderdoc.hpp>

#include <renderdoc_app.h>

#include <dockyard/log.hpp>

#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct RenderDocApi {
  RENDERDOC_API_1_7_0 *rdoc{nullptr};
};

namespace {

[[nodiscard]] auto load_renderdoc_module() -> void * {
#if defined(_WIN32)
  return static_cast<void *>(GetModuleHandleA("renderdoc.dll"));
#else
  return dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
#endif
}

[[nodiscard]] auto get_api_proc(void *module) -> pRENDERDOC_GetAPI {
#if defined(_WIN32)
  return reinterpret_cast<pRENDERDOC_GetAPI>(
      GetProcAddress(static_cast<HMODULE>(module), "RENDERDOC_GetAPI"));
#else
  return reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
#endif
}

} // namespace

RenderDocContext::~RenderDocContext() = default;

RenderDocContext::RenderDocContext(RenderDocContext &&) noexcept = default;

auto RenderDocContext::operator=(RenderDocContext &&) noexcept
    -> RenderDocContext & = default;

auto renderdoc_init() -> RenderDocContext {
  void *mod = load_renderdoc_module();
  if (!mod) {
    return RenderDocContext{};
  }

  pRENDERDOC_GetAPI get_api = get_api_proc(mod);
  if (!get_api) {
    dy::warn("RenderDoc module found but RENDERDOC_GetAPI symbol is missing — "
             "skipping.");
    return RenderDocContext{};
  }

  auto storage = std::make_unique<RenderDocApi>();

  const int ok = get_api(eRENDERDOC_API_Version_1_7_0,
                         reinterpret_cast<void **>(&storage->rdoc));

  if (ok != 1 || !storage->rdoc) {
    dy::warn("RenderDoc: RENDERDOC_GetAPI failed (ret={}) — capture disabled.",
             ok);
    return RenderDocContext{};
  }

  auto *rdoc = storage->rdoc;

  rdoc->MaskOverlayBits(eRENDERDOC_Overlay_All, eRENDERDOC_Overlay_All);
  rdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1u);
  rdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1u);
  rdoc->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 0u);
  rdoc->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 0u);

  int major{};
  int minor{};
  int patch{};
  rdoc->GetAPIVersion(&major, &minor, &patch);

  dy::info("RenderDoc in-application API active — version {}.{}.{}", major,
           minor, patch);

  RenderDocContext ctx{};
  ctx.api = std::move(storage);
  return ctx;
}

auto RenderDocContext::is_capturing() const -> bool {
  if (!api || !api->rdoc)
    return false;

  return api->rdoc->IsFrameCapturing() != 0u;
}

auto RenderDocContext::begin_frame_capture(void *vk_instance,
                                           void *wnd_handle) const -> void {
  if (!api || !api->rdoc)
    return;

  if (api->rdoc->IsFrameCapturing())
    return;

  const RENDERDOC_DevicePointer dev_ptr =
      vk_instance ? RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(vk_instance)
                  : nullptr;

  api->rdoc->StartFrameCapture(dev_ptr,
                               static_cast<RENDERDOC_WindowHandle>(wnd_handle));

  dy::info("RenderDoc: frame capture started.");
}

auto RenderDocContext::end_frame_capture(void *vk_instance,
                                         void *wnd_handle) const -> void {
  if (!api || !api->rdoc)
    return;

  if (!api->rdoc->IsFrameCapturing())
    return;

  const RENDERDOC_DevicePointer dev_ptr =
      vk_instance ? RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(vk_instance)
                  : nullptr;

  const auto ok = api->rdoc->EndFrameCapture(
      dev_ptr, static_cast<RENDERDOC_WindowHandle>(wnd_handle));

  if (ok) {
    const auto num = api->rdoc->GetNumCaptures();
    dy::info("RenderDoc: frame capture ended. Total captures: {}", num);
  } else {
    dy::warn("RenderDoc: EndFrameCapture reported failure.");
  }
}

auto RenderDocContext::trigger_capture() const -> void {
  if (!api || !api->rdoc)
    return;

  api->rdoc->TriggerCapture();
  dy::info("RenderDoc: one-shot capture triggered.");
}

auto RenderDocContext::set_capture_path(std::string_view path_template) const
    -> void {
  if (!api || !api->rdoc)
    return;

  const std::string tmp{path_template};
  api->rdoc->SetCaptureFilePathTemplate(tmp.c_str());
}
