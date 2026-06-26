#pragma once
#include <cstdint>
#include <string_view>

namespace dy {

// Call once at startup before any other subsystem.
void install_crash_handler();

// Mark a checkpoint. label must have static storage duration (string literals).
// Stored in a lock-free ring; safe to call from any thread.
void breadcrumb(std::string_view label);

// Update the frame counter shown in crash reports.
void set_crash_frame(uint64_t frame_id);

} // namespace dy
