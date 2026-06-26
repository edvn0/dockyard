#pragma once
#include <array>
#include <cstdint>

// Internal API shared between crash_reporter_common.cpp and platform TUs.
// Not part of the public include tree.

namespace dy::crash {

constexpr int k_crumb_cap = 32;

struct Snapshot {
    std::array<const char*, k_crumb_cap> crumbs{};
    int                                  count{};
    uint64_t                             frame_id{};
};

// Snapshot the ring buffer (safe to call from crash/signal handlers).
auto capture() -> Snapshot;

// Flush the spdlog logger if it has been initialised.
void flush_log();

// Write a human-readable crash report to dockyard_crash_<timestamp>.txt
// in the working directory and echo a summary to stderr.
void report(const char* reason, const Snapshot& snap);

} // namespace dy::crash
