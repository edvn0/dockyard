// Platform crash handler — Windows only.
// Kept as a separate TU to isolate Windows headers from the rest of the build.
// WIN32_LEAN_AND_MEAN and NOMINMAX are injected by CMake target definitions.

#include <windows.h>
#include <dbghelp.h>

#include <dockyard/crash_reporter.hpp>
#include "crash_reporter_impl.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <print>

namespace dy {

namespace {

void write_minidump(EXCEPTION_POINTERS* exc_info) {
    time_t now = time(nullptr);
    tm     tm_buf{};
    gmtime_s(&tm_buf, &now);

    std::array<char, 64> filename{};
    strftime(filename.data(), filename.size(),
             "dockyard_crash_%Y%m%d_%H%M%S.dmp", &tm_buf);

    HANDLE dump_file = CreateFileA(filename.data(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump_file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = exc_info;
    mei.ClientPointers    = FALSE;

    const auto dump_type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump_file,
                      dump_type, exc_info != nullptr ? &mei : nullptr,
                      nullptr, nullptr);
    CloseHandle(dump_file);
    std::println(stderr, "[dockyard] Minidump written to: {}", filename.data());
}

LONG WINAPI seh_filter(EXCEPTION_POINTERS* exc_info) {
    static bool entered = false;
    if (entered)
        return EXCEPTION_CONTINUE_SEARCH;
    entered = true;

    std::array<char, 128> reason{};
    snprintf(reason.data(), reason.size(),
             "Unhandled SEH exception 0x%08lX at 0x%p",
             exc_info != nullptr ? exc_info->ExceptionRecord->ExceptionCode : 0UL,
             exc_info != nullptr ? exc_info->ExceptionRecord->ExceptionAddress
                                 : nullptr);

    const auto snap = crash::capture();
    crash::report(reason.data(), snap);
    write_minidump(exc_info);

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install_crash_handler() {
    SetUnhandledExceptionFilter(seh_filter);
}

} // namespace dy
