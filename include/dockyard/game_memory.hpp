#pragma once

#include <dockyard/types.hpp>

#include <cassert>
#include <cstddef>
#include <new>
#include <span>
#include <string>

namespace dy {

struct GameMemory {
    void*  base      = nullptr;
    usize used      = 0;
    usize reserved  = 0;
    usize committed = 0;

    static constexpr usize default_reserve = 256ULL * 1024 * 1024;

    [[nodiscard]] static auto create(usize reserve_bytes = default_reserve) -> GameMemory;
    auto destroy() -> void;
    auto reset() -> void;

    template <typename T>
    [[nodiscard]] auto root() -> T* {
        assert(base && "no allocation yet");
        return static_cast<T*>(base);
    }

    template <typename T>
    [[nodiscard]] auto allocate() -> T* {
        void* ptr = push(sizeof(T), alignof(T));
        return new (ptr) T{};
    }

    template <typename T>
    [[nodiscard]] auto allocate_array(usize count) -> std::span<T> {
        void* ptr = push(sizeof(T) * count, alignof(T));
        T* typed = static_cast<T*>(ptr);
        for (usize i = 0; i < count; ++i)
            new (typed + i) T{};
        return {typed, count};
    }

private:
    [[nodiscard]] auto push(usize bytes, usize align) -> void*;
    auto commit_up_to(usize needed) -> void;
};

} // namespace dy
