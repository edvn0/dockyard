#include <dockyard/game_memory.hpp>

#include <cassert>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace dy {

static constexpr usize page_size = 4096;

auto GameMemory::create(usize reserve_bytes) -> GameMemory {
    GameMemory mem;
    mem.reserved = reserve_bytes;

#ifdef _WIN32
    mem.base = VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, reserve_bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mem.base = (p == MAP_FAILED) ? nullptr : p;
#endif

    assert(mem.base != nullptr && "Failed to reserve virtual memory for GameMemory");
    return mem;
}

auto GameMemory::destroy() -> void {
    if (base == nullptr) return;

#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, reserved);
#endif

    base      = nullptr;
    used      = 0;
    committed = 0;
    reserved  = 0;
}

auto GameMemory::reset() -> void {
    used = 0;
}


constexpr auto round_up_to_alignment = [](std::unsigned_integral auto v, auto align_size) {
    return v + (align_size -1) & ~(align_size - 1);
};

auto GameMemory::push(usize bytes, usize align) -> void* {
    usize aligned = round_up_to_alignment(used, align);
    usize end     = aligned + bytes;
    assert(end <= reserved && "GameMemory exhausted");
    commit_up_to(end);
    used = end;
    return static_cast<char*>(base) + aligned;
}


auto GameMemory::commit_up_to(usize needed) -> void {
    if (needed <= committed) return;
    usize new_committed = round_up_to_alignment(needed, page_size);

#ifdef _WIN32
    VirtualAlloc(static_cast<char*>(base) + committed,
                 new_committed - committed,
                 MEM_COMMIT, PAGE_READWRITE);
#else
    mprotect(static_cast<char*>(base) + committed,
             new_committed - committed,
             PROT_READ | PROT_WRITE);
#endif

    committed = new_committed;
}

} // namespace dy
