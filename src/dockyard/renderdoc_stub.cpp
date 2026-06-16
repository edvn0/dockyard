#include <dockyard/renderdoc.hpp>

struct RenderDocApi {};

RenderDocContext::~RenderDocContext() = default;

RenderDocContext::RenderDocContext(RenderDocContext &&) noexcept = default;

auto RenderDocContext::operator=(RenderDocContext &&) noexcept
    -> RenderDocContext & = default;

auto renderdoc_init() -> RenderDocContext { return {}; }

auto RenderDocContext::is_capturing() const -> bool { return false; }

auto RenderDocContext::begin_frame_capture(void *, void *) const -> void {}

auto RenderDocContext::end_frame_capture(void *, void *) const -> void {}

auto RenderDocContext::trigger_capture() const -> void {}

auto RenderDocContext::set_capture_path(std::string_view) const -> void {}
