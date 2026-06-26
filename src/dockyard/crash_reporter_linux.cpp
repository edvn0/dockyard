// Platform crash handler — Linux only.
// Kept as a separate TU to isolate POSIX signal headers from the rest of the build.

#include <dockyard/crash_reporter.hpp>
#include "crash_reporter_impl.hpp"

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

namespace dy {

namespace {

auto signal_name(int sig) -> const char* {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE:  return "SIGFPE (floating-point exception)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    default:      return "unknown signal";
    }
}

void signal_handler(int sig, siginfo_t* /*sig_info*/, void* /*ctx*/) {
    // Guard against re-entry (e.g. crash inside the handler itself).
    static volatile sig_atomic_t entered = 0;
    if (entered != 0)
        return;
    entered = 1;

    const auto snap = crash::capture();
    crash::report(signal_name(sig), snap);

    // backtrace_symbols_fd is async-signal-safe; write directly to stderr fd.
    void* frames[64]; // NOLINT(modernize-avoid-c-arrays)
    const int frame_count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);

    // Reset to default and re-raise so the OS produces a core dump.
    struct sigaction default_sa{};
    default_sa.sa_handler = SIG_DFL;
    sigemptyset(&default_sa.sa_mask);
    sigaction(sig, &default_sa, nullptr);
    raise(sig);
}

} // namespace

void install_crash_handler() {
    struct sigaction crash_sa{};
    crash_sa.sa_sigaction = signal_handler;
    crash_sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&crash_sa.sa_mask);

    sigaction(SIGSEGV, &crash_sa, nullptr);
    sigaction(SIGABRT, &crash_sa, nullptr);
    sigaction(SIGFPE,  &crash_sa, nullptr);
    sigaction(SIGBUS,  &crash_sa, nullptr);
    sigaction(SIGILL,  &crash_sa, nullptr);
}

} // namespace dy
