#include <dockyard/crash_reporter.hpp>
#include <dockyard/log.hpp>
#include "crash_reporter_impl.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <print>

namespace dy {

namespace {
std::array<const char*, crash::k_crumb_cap> g_crumbs{};
std::atomic<uint32_t>                       g_write_idx{0};
std::atomic<uint64_t>                       g_frame_id{0};
} // namespace

void breadcrumb(std::string_view label) {
    const uint32_t slot = g_write_idx.fetch_add(1U, std::memory_order_relaxed);
    g_crumbs[slot % crash::k_crumb_cap] = label.data();
}

void set_crash_frame(uint64_t frame_id) {
    g_frame_id.store(frame_id, std::memory_order_relaxed);
}

namespace crash {

auto capture() -> Snapshot {
    Snapshot snap;
    snap.frame_id        = g_frame_id.load(std::memory_order_relaxed);
    const uint32_t head  = g_write_idx.load(std::memory_order_relaxed);
    const uint32_t total = head < static_cast<uint32_t>(k_crumb_cap)
                               ? head
                               : static_cast<uint32_t>(k_crumb_cap);
    snap.count           = static_cast<int>(total);
    const uint32_t start = (head >= static_cast<uint32_t>(k_crumb_cap))
                               ? (head % k_crumb_cap)
                               : 0U;
    for (int idx = 0; idx < snap.count; ++idx)
        snap.crumbs[static_cast<size_t>(idx)] =
            g_crumbs[(start + static_cast<uint32_t>(idx)) % k_crumb_cap];
    return snap;
}

void flush_log() {
    dy::flush_log();
}

void report(const char* reason, const Snapshot& snap) {
    flush_log();

    time_t now = time(nullptr);
    tm     tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif

    std::array<char, 64> filename{};
    strftime(filename.data(), filename.size(),
             "dockyard_crash_%Y%m%d_%H%M%S.txt", &tm_buf);

    FILE* file_out = nullptr;
#ifdef _WIN32
    fopen_s(&file_out, filename.data(), "w");
#else
    file_out = fopen(filename.data(), "w"); // NOLINT(cppcoreguidelines-owning-memory)
#endif
    if (file_out == nullptr)
        file_out = stderr;

    std::array<char, 32> timebuf{};
    strftime(timebuf.data(), timebuf.size(), "%Y-%m-%d %H:%M:%S UTC", &tm_buf);

    std::println(file_out, "=== Dockyard Crash Report ===");
    std::println(file_out, "Time  : {}", timebuf.data());
    std::println(file_out, "Reason: {}", reason != nullptr ? reason : "(unknown)");
    std::println(file_out, "Frame : {}\n", snap.frame_id);
    std::println(file_out, "CPU Breadcrumbs (oldest to newest, last {} ops):", snap.count);
    for (int idx = 0; idx < snap.count; ++idx)
        std::println(file_out, "  [{:2d}] {}", idx,
                     snap.crumbs[static_cast<size_t>(idx)] != nullptr
                         ? snap.crumbs[static_cast<size_t>(idx)]
                         : "(null)");
    std::println(file_out, "\nSee dockyard_profile.log for full log output.");

    if (file_out != stderr) {
        fclose(file_out); // NOLINT(cppcoreguidelines-owning-memory)
        std::println(stderr, "[dockyard] Crash report written to: {}", filename.data());
    }
}

} // namespace crash

void report_crash(const char *reason) {
    const auto snap = crash::capture();
    crash::flush_log();
    crash::report(reason, snap);
}

} // namespace dy
