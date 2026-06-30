#pragma once

#include <dockyard/vfs_path.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dy::archive {

// Single contiguous VM-mapped region; entries are non-owning views into it.
// Move-only. Freed on destruction.
struct MemoryBundle {
    using Entries = std::unordered_map<std::string, std::span<const std::byte>>;

    void*   base     = nullptr;
    size_t  reserved = 0; // total virtual address space reserved
    size_t  used     = 0; // bytes written (always <= reserved)
    Entries entries;

    MemoryBundle() = default;
    MemoryBundle(const MemoryBundle&)            = delete;
    MemoryBundle& operator=(const MemoryBundle&) = delete;
    MemoryBundle(MemoryBundle&&) noexcept;
    MemoryBundle& operator=(MemoryBundle&&) noexcept;
    ~MemoryBundle();

    [[nodiscard]] auto at(std::string_view key) const -> std::span<const std::byte>;
    [[nodiscard]] auto contains(std::string_view key) const -> bool;
    [[nodiscard]] auto begin() const noexcept { return entries.cbegin(); }
    [[nodiscard]] auto end()   const noexcept { return entries.cend();   }
    [[nodiscard]] auto empty() const noexcept { return entries.empty();  }
};

static constexpr size_t kDefaultMemoryBudget = 12ULL << 30; // 12 GiB

// Extract a ZIP, .tar.gz/.tgz, .tar.zst/.tzst, or raw .zstd archive into dest_dir.
// For raw .zstd, the decompressed file is written using the stem as filename
// (e.g. "model.glb.zstd" → dest_dir/model.glb).
auto extract(const dy::VFSPath& src, const dy::VFSPath& dest_dir)
    -> std::expected<void, std::string>;

// Extract the same formats into a VM-mapped region without touching disk.
// Reads the archive header first to estimate required size.
// Returns an error immediately if the estimated size exceeds budget,
// or mid-stream if the actual content exceeds the reservation (corrupt header).
// For raw .zstd, the bundle entry key is the stem filename (e.g. "model.glb").
auto extract_to_memory(const dy::VFSPath& src,
                       size_t budget = kDefaultMemoryBudget)
    -> std::expected<MemoryBundle, std::string>;

} // namespace dy::archive
