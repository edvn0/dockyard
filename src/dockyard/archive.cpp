#include <dockyard/archive.hpp>
#include <dockyard/vfs.hpp>

#include <miniz.h>
#include <zstd.h>

#include <array>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Platform VM helpers
// ---------------------------------------------------------------------------

using dy::u64;
using dy::usize;

namespace {

constexpr usize page_size = 4096;
constexpr usize commit_chunk = 2ULL << 20; // 2 MiB commit granularity

auto vm_reserve(usize bytes) -> void * {
#ifdef _WIN32
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

// Ensure pages in [ptr, ptr + up_to) are committed/accessible.
// On POSIX this is a no-op; kernel lazily commits on first write.
auto vm_ensure_committed(void *base, usize already_committed, usize up_to,
                         usize reserved)
    -> std::expected<usize, std::string> {
#ifdef _WIN32
  if (up_to <= already_committed)
    return already_committed;
  usize new_commit = ((up_to + commit_chunk - 1) / commit_chunk) * commit_chunk;
  new_commit = std::min(new_commit, reserved); // chunk rounding must not exceed reserved
  if (!VirtualAlloc(static_cast<char *>(base) + already_committed,
                    new_commit - already_committed, MEM_COMMIT, PAGE_READWRITE))
    return std::unexpected("VirtualAlloc MEM_COMMIT failed");
  return new_commit;
#else
  (void)base;
  (void)already_committed;
  (void)up_to;
  return up_to; // always "committed" on POSIX
#endif
}

auto vm_fit(void *base, usize reserved, usize used) -> void {
  usize used_pages = (used + page_size - 1) & ~(page_size - 1);
  if (used_pages >= reserved)
    return;
#ifdef _WIN32
  VirtualFree(static_cast<char *>(base) + used_pages, reserved - used_pages,
              MEM_DECOMMIT);
#else
  madvise(static_cast<char *>(base) + used_pages, reserved - used_pages,
          MADV_DONTNEED);
#endif
}

auto vm_release(void *base, usize reserved) -> void {
  if (!base)
    return;
#ifdef _WIN32
  (void)reserved;
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, reserved);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// MemoryBundle implementation
// ---------------------------------------------------------------------------

namespace dy::archive {

MemoryBundle::MemoryBundle(MemoryBundle &&o) noexcept
    : base(std::exchange(o.base, nullptr)),
      reserved(std::exchange(o.reserved, 0)), used(std::exchange(o.used, 0)),
      entries(std::move(o.entries)) {}

MemoryBundle &MemoryBundle::operator=(MemoryBundle &&o) noexcept {
  if (this != &o) {
    vm_release(base, reserved);
    base = std::exchange(o.base, nullptr);
    reserved = std::exchange(o.reserved, 0);
    used = std::exchange(o.used, 0);
    entries = std::move(o.entries);
  }
  return *this;
}

MemoryBundle::~MemoryBundle() { vm_release(base, reserved); }

auto MemoryBundle::at(std::string_view key) const
    -> std::span<const std::byte> {
  auto it = entries.find(std::string(key));
  return it == entries.end() ? std::span<const std::byte>{} : it->second;
}

auto MemoryBundle::contains(std::string_view key) const -> bool {
  return entries.contains(std::string(key));
}

} // namespace dy::archive

// ---------------------------------------------------------------------------
// Internal: BundleWriter — staged writes into the VM region
// ---------------------------------------------------------------------------

namespace {

using MemoryBundle = dy::archive::MemoryBundle;

struct BundleWriter {
  MemoryBundle &b;
  usize committed = 0;

  // Validate that writing `size` more bytes fits, then commit pages if needed.
  auto ensure(usize size) -> std::expected<void, std::string> {
    const usize needed = b.used + size;
    if (needed > b.reserved)
      return std::unexpected(std::format(
          "archive content ({} bytes) exceeds reserved region ({} bytes); "
          "archive header may be corrupt",
          needed, b.reserved));
    auto r = vm_ensure_committed(b.base, committed, needed, b.reserved);
    if (!r)
      return std::unexpected(r.error());
    committed = *r;
    return {};
  }

  // Pointer to where the next entry should be written.
  [[nodiscard]] auto dest() const -> std::byte * {
    return static_cast<std::byte *>(b.base) + b.used;
  }

  // Record a written entry and advance the cursor.
  auto commit_entry(std::string name, usize size) -> void {
    b.entries[std::move(name)] = std::span{dest() - size, size};
    // dest() - size: the write happened before this call, so rewind
    // Actually: record based on b.used before the caller wrote.
    // Let caller use record_entry(name, ptr, size) instead.
  }
};

} // namespace

// Hmm, that has a pointer arithmetic issue. Let me redesign slightly:
// the caller saves dest() before writing, then calls register_entry.

namespace {

// Writes into BundleWriter b; saves the span start before writing.
struct EntryWriter {
  BundleWriter &bw;
  std::byte *start = nullptr;

  // Returns the destination pointer. Must call ensure() first.
  auto begin_entry() -> std::byte * {
    start = bw.dest();
    return start;
  }

  auto finish_entry(std::string name, usize size) -> void {
    bw.b.entries[std::move(name)] = std::span{start, size};
    bw.b.used += size;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Tar
// ---------------------------------------------------------------------------

namespace {

struct TarHeader {
  char name[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char size[12];
  char mtime[12];
  char checksum[8];
  char typeflag;
  char linkname[100];
  char magic[6];
  char version[2];
  char uname[32];
  char gname[32];
  char devmajor[8];
  char devminor[8];
  char prefix[155];
  char pad[12];
};
static_assert(sizeof(TarHeader) == 512);

auto parse_octal(const char *s, usize n) -> u64 {
  u64 v = 0;
  for (usize i = 0; i < n && s[i] >= '0' && s[i] <= '7'; ++i)
    v = v * 8 + static_cast<u64>(s[i] - '0');
  return v;
}

struct ByteStream {
  virtual auto read(std::byte *buf, usize n) -> usize = 0;
  virtual ~ByteStream() = default;
};

auto drain_blocks(ByteStream &stream, u64 padded_size,
                  std::array<std::byte, 512> &block) -> void {
  for (u64 d = 0; d < padded_size; d += 512)
    stream.read(block.data(), 512);
}

// dest  — disk output root (used when bw == nullptr)
// bw    — VM bundle writer (used when non-null)
static auto extract_tar_impl(ByteStream &stream, const fs::path &dest,
                             BundleWriter *bw)
    -> std::expected<void, std::string> {
  std::array<std::byte, 512> block{};
  std::string pending_long_name;

  for (;;) {
    if (stream.read(block.data(), 512) < 512)
      break;

    const auto &hdr = *reinterpret_cast<const TarHeader *>(block.data());
    if (hdr.name[0] == '\0')
      break;

    const auto size = parse_octal(hdr.size, sizeof(hdr.size));
    const char typeflag = hdr.typeflag == '\0' ? '0' : hdr.typeflag;
    const u64 padded = (size + 511u) & ~u64{511u};

    if (typeflag == 'L') {
      pending_long_name.resize(size);
      usize remaining = size, offset = 0;
      while (remaining > 0) {
        if (stream.read(block.data(), 512) < 512)
          return std::unexpected("truncated GNU long-name block");
        usize copy = std::min(usize{512}, remaining);
        std::memcpy(pending_long_name.data() + offset, block.data(), copy);
        offset += copy;
        remaining -= copy;
      }
      if (!pending_long_name.empty() && pending_long_name.back() == '\0')
        pending_long_name.pop_back();
      continue;
    }

    if (typeflag == 'x' || typeflag == 'g') {
      drain_blocks(stream, padded, block);
      continue;
    }

    std::string entry_name;
    if (!pending_long_name.empty()) {
      entry_name = std::move(pending_long_name);
    } else {
      if (hdr.prefix[0] != '\0') {
        entry_name.assign(hdr.prefix, strnlen(hdr.prefix, sizeof(hdr.prefix)));
        entry_name += '/';
      }
      entry_name += std::string(hdr.name, strnlen(hdr.name, sizeof(hdr.name)));
    }

    if (typeflag == '5') {
      if (!bw) {
        std::error_code ec;
        fs::create_directories(
            dest / fs::path(entry_name, fs::path::generic_format), ec);
        if (ec)
          return std::unexpected(std::format("mkdir: {}", ec.message()));
      }
      continue;
    }

    if (typeflag == '0' || typeflag == '7') {
      u64 remaining = size;

      if (bw) {
        if (auto r = bw->ensure(size); !r)
          return r;
        EntryWriter ew{*bw};
        auto *dst = ew.begin_entry();
        usize offset = 0;
        while (remaining > 0) {
          if (stream.read(block.data(), 512) < 512)
            return std::unexpected("truncated tar data");
          usize n = static_cast<usize>(std::min(remaining, u64{512}));
          std::memcpy(dst + offset, block.data(), n);
          offset += n;
          remaining -= static_cast<u64>(n);
        }
        ew.finish_entry(std::move(entry_name), size);
      } else {
        const fs::path entry_path =
            dest / fs::path(entry_name, fs::path::generic_format);
        std::error_code ec;
        fs::create_directories(entry_path.parent_path(), ec);
        if (ec)
          return std::unexpected(std::format("mkdir: {}", ec.message()));
        std::ofstream out(entry_path, std::ios::binary | std::ios::trunc);
        if (!out)
          return std::unexpected(
              std::format("cannot create '{}'", entry_path.string()));
        while (remaining > 0) {
          if (stream.read(block.data(), 512) < 512)
            return std::unexpected("truncated tar data");
          auto n = static_cast<std::streamsize>(std::min(remaining, u64{512}));
          out.write(reinterpret_cast<const char *>(block.data()), n);
          remaining -= static_cast<u64>(n);
        }
      }
      continue;
    }

    drain_blocks(stream, padded, block);
  }
  return {};
}

// ---------------------------------------------------------------------------
// Gzip stream
// ---------------------------------------------------------------------------

class GzipStream final : public ByteStream {
  static constexpr usize kDictSize = TINFL_LZ_DICT_SIZE;
  static constexpr usize kInBufSize = 65536;

  std::ifstream file_;
  tinfl_decompressor decomp_{};
  mz_uint8 dict_[kDictSize]{};

  mz_uint dict_ofs_ = 0;
  mz_uint read_pos_ = 0;
  usize avail_ = 0;

  uint8_t in_buf_[kInBufSize]{};
  usize in_pos_ = 0;
  usize in_avail_ = 0;
  bool src_eof_ = false;
  bool done_ = false;

  auto refill() -> void {
    if (in_pos_ < in_avail_ || src_eof_)
      return;
    file_.read(reinterpret_cast<char *>(in_buf_), kInBufSize);
    in_avail_ = static_cast<usize>(file_.gcount());
    in_pos_ = 0;
    if (in_avail_ < kInBufSize)
      src_eof_ = true;
  }

  auto pump() -> void {
    while (!done_ && avail_ == 0) {
      refill();
      if (in_avail_ == 0) {
        done_ = true;
        return;
      }

      if (dict_ofs_ == kDictSize)
        dict_ofs_ = 0;

      usize in_size = in_avail_ - in_pos_;
      usize out_size = kDictSize - dict_ofs_;
      mz_uint32 flags = src_eof_ ? 0u : TINFL_FLAG_HAS_MORE_INPUT;

      tinfl_status st =
          tinfl_decompress(&decomp_, in_buf_ + in_pos_, &in_size, dict_,
                           dict_ + dict_ofs_, &out_size, flags);

      in_pos_ += in_size;
      if (out_size > 0) {
        read_pos_ = dict_ofs_;
        avail_ = out_size;
        dict_ofs_ += static_cast<mz_uint>(out_size);
      }
      if (st <= TINFL_STATUS_DONE)
        done_ = true;
      if (in_size == 0 && out_size == 0 && !done_) {
        done_ = true;
        return;
      }
    }
  }

  auto read_raw_byte() -> std::expected<uint8_t, std::string> {
    if (in_pos_ >= in_avail_) {
      refill();
      if (in_avail_ == 0)
        return std::unexpected("unexpected EOF in gzip header");
    }
    return in_buf_[in_pos_++];
  }

public:
  explicit GzipStream(std::ifstream f) : file_(std::move(f)) {
    tinfl_init(&decomp_);
  }

  auto skip_header() -> std::expected<void, std::string> {
    auto expect = [&](uint8_t v) -> std::expected<void, std::string> {
      auto b = read_raw_byte();
      if (!b)
        return std::unexpected(b.error());
      if (*b != v)
        return std::unexpected(std::format(
            "bad gzip magic: expected 0x{:02x} got 0x{:02x}", v, *b));
      return {};
    };
    if (auto r = expect(0x1f); !r)
      return r;
    if (auto r = expect(0x8b); !r)
      return r;
    if (auto r = expect(8); !r)
      return r;

    auto flg = read_raw_byte();
    if (!flg)
      return std::unexpected(flg.error());

    for (int i = 0; i < 6; ++i) // MTIME(4) + XFL + OS
      if (auto r = read_raw_byte(); !r)
        return std::unexpected(r.error());

    if (*flg & 0x04) {
      auto lo = read_raw_byte();
      if (!lo)
        return std::unexpected(lo.error());
      auto hi = read_raw_byte();
      if (!hi)
        return std::unexpected(hi.error());
      uint16_t xlen = uint16_t(*lo) | uint16_t(uint16_t(*hi) << 8);
      for (uint16_t i = 0; i < xlen; ++i)
        if (auto r = read_raw_byte(); !r)
          return std::unexpected(r.error());
    }
    for (int f = 0; f < 2; ++f) { // FNAME (0x08), FCOMMENT (0x10)
      if (!(*flg & (0x08u << f)))
        continue;
      for (;;) {
        auto b = read_raw_byte();
        if (!b)
          return std::unexpected(b.error());
        if (*b == 0)
          break;
      }
    }
    if (*flg & 0x02) { // FHCRC
      if (auto r = read_raw_byte(); !r)
        return std::unexpected(r.error());
      if (auto r = read_raw_byte(); !r)
        return std::unexpected(r.error());
    }
    return {};
  }

  auto read(std::byte *buf, usize n) -> usize override {
    usize total = 0;
    while (total < n) {
      if (avail_ == 0) {
        pump();
        if (avail_ == 0)
          break;
      }
      usize copy = std::min(avail_, n - total);
      std::memcpy(buf + total, dict_ + read_pos_, copy);
      read_pos_ += static_cast<mz_uint>(copy);
      avail_ -= copy;
      total += copy;
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// Zstd stream
// ---------------------------------------------------------------------------

class ZstdStream final : public ByteStream {
  static constexpr usize kInBufSize = 131072;
  static constexpr usize kOutBufSize = 131072;

  std::ifstream file_;
  ZSTD_DStream *dstream_;
  uint8_t in_buf_[kInBufSize]{};
  uint8_t out_buf_[kOutBufSize]{};

  ZSTD_inBuffer zin_ = {in_buf_, 0, 0};
  usize out_pos_ = 0;
  usize out_avail_ = 0;
  bool src_eof_ = false;
  bool done_ = false;

  auto fill_in() -> void {
    if (zin_.pos < zin_.size || src_eof_)
      return;
    file_.read(reinterpret_cast<char *>(in_buf_), kInBufSize);
    usize n = static_cast<usize>(file_.gcount());
    zin_ = {in_buf_, n, 0};
    if (n < kInBufSize || file_.eof())
      src_eof_ = true;
  }

public:
  explicit ZstdStream(std::ifstream f)
      : file_(std::move(f)), dstream_(ZSTD_createDStream()) {
    ZSTD_initDStream(dstream_);
  }
  ~ZstdStream() override { ZSTD_freeDStream(dstream_); }

  auto read(std::byte *buf, usize n) -> usize override {
    usize total = 0;
    while (total < n && !done_) {
      if (out_avail_ > 0) {
        usize copy = std::min(out_avail_, n - total);
        std::memcpy(buf + total, out_buf_ + out_pos_, copy);
        out_pos_ += copy;
        out_avail_ -= copy;
        total += copy;
        continue;
      }
      fill_in();
      if (zin_.size == 0) {
        done_ = true;
        break;
      }
      ZSTD_outBuffer zout{out_buf_, kOutBufSize, 0};
      usize ret = ZSTD_decompressStream(dstream_, &zout, &zin_);
      if (ZSTD_isError(ret)) {
        done_ = true;
        break;
      }
      out_pos_ = 0;
      out_avail_ = zout.pos;
      if (out_avail_ == 0 && src_eof_ && zin_.pos == zin_.size)
        done_ = true;
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------

static auto extract_zip_impl(const fs::path &src, const fs::path &dest,
                             BundleWriter *bw)
    -> std::expected<void, std::string> {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, src.string().c_str(), 0))
    return std::unexpected(std::format("cannot open zip '{}'", src.string()));
  struct Guard {
    mz_zip_archive *z;
    ~Guard() { mz_zip_reader_end(z); }
  } g{&zip};

  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, i, &stat))
      return std::unexpected(std::format("cannot stat zip entry {}", i));

    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      if (!bw) {
        std::error_code ec;
        fs::create_directories(
            dest / fs::path(stat.m_filename, fs::path::generic_format), ec);
        if (ec)
          return std::unexpected(std::format("mkdir: {}", ec.message()));
      }
      continue;
    }

    if (bw) {
      if (auto r = bw->ensure(stat.m_uncomp_size); !r)
        return r;
      EntryWriter ew{*bw};
      auto *dst = ew.begin_entry();
      if (!mz_zip_reader_extract_to_mem(&zip, i, dst, stat.m_uncomp_size, 0))
        return std::unexpected(
            std::format("failed extracting '{}' to memory", stat.m_filename));
      ew.finish_entry(stat.m_filename, stat.m_uncomp_size);
    } else {
      const fs::path entry =
          dest / fs::path(stat.m_filename, fs::path::generic_format);
      std::error_code ec;
      fs::create_directories(entry.parent_path(), ec);
      if (ec)
        return std::unexpected(std::format("mkdir: {}", ec.message()));
      if (!mz_zip_reader_extract_to_file(&zip, i, entry.string().c_str(), 0))
        return std::unexpected(
            std::format("failed extracting '{}'", stat.m_filename));
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Raw zstd (single compressed file, not a tar archive)
// ---------------------------------------------------------------------------

static auto extract_raw_zstd_impl(const fs::path &src, BundleWriter *bw,
                                   const fs::path &dest)
    -> std::expected<void, std::string> {
  std::ifstream f(src, std::ios::binary | std::ios::ate);
  if (!f)
    return std::unexpected(std::format("cannot open '{}'", src.string()));

  const auto compressed_size = static_cast<usize>(f.tellg());
  f.seekg(0);
  std::vector<uint8_t> compressed(compressed_size);
  if (!f.read(reinterpret_cast<char *>(compressed.data()),
              static_cast<std::streamsize>(compressed_size)))
    return std::unexpected(std::format("read error for '{}'", src.string()));

  const unsigned long long content_size =
      ZSTD_getFrameContentSize(compressed.data(), compressed_size);
  if (content_size == ZSTD_CONTENTSIZE_ERROR)
    return std::unexpected("not a valid zstd frame");

  // Entry key is the stem (e.g. "model.glb" for "model.glb.zstd").
  const std::string entry_name = src.stem().string();

  if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
    const usize out_size = static_cast<usize>(content_size);
    if (bw) {
      if (auto r = bw->ensure(out_size); !r)
        return r;
      EntryWriter ew{*bw};
      auto *dst = ew.begin_entry();
      const usize ret =
          ZSTD_decompress(dst, out_size, compressed.data(), compressed_size);
      if (ZSTD_isError(ret))
        return std::unexpected(
            std::format("zstd decompress error: {}", ZSTD_getErrorName(ret)));
      ew.finish_entry(entry_name, out_size);
    } else {
      const fs::path out_path =
          dest / fs::path(entry_name, fs::path::generic_format);
      std::vector<std::byte> decompressed(out_size);
      const usize ret = ZSTD_decompress(decompressed.data(), out_size,
                                        compressed.data(), compressed_size);
      if (ZSTD_isError(ret))
        return std::unexpected(
            std::format("zstd decompress error: {}", ZSTD_getErrorName(ret)));
      std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
      if (!out)
        return std::unexpected(
            std::format("cannot create '{}'", out_path.string()));
      out.write(reinterpret_cast<const char *>(decompressed.data()),
                static_cast<std::streamsize>(out_size));
    }
    return {};
  }

  // Content size not stored in frame: stream-decompress into a staging buffer.
  ZSTD_DStream *ds = ZSTD_createDStream();
  struct ZstdGuard {
    ZSTD_DStream *p;
    ~ZstdGuard() { ZSTD_freeDStream(p); }
  } guard{ds};
  ZSTD_initDStream(ds);

  std::vector<uint8_t> out_chunk(ZSTD_DStreamOutSize());
  std::vector<std::byte> decompressed;
  ZSTD_inBuffer zin{compressed.data(), compressed_size, 0};

  while (zin.pos < zin.size) {
    ZSTD_outBuffer zout{out_chunk.data(), out_chunk.size(), 0};
    const usize ret = ZSTD_decompressStream(ds, &zout, &zin);
    if (ZSTD_isError(ret))
      return std::unexpected(
          std::format("zstd decompress error: {}", ZSTD_getErrorName(ret)));
    const auto *out_bytes =
        reinterpret_cast<const std::byte *>(out_chunk.data());
    decompressed.insert(decompressed.end(), out_bytes, out_bytes + zout.pos);
  }

  if (bw) {
    if (auto r = bw->ensure(decompressed.size()); !r)
      return r;
    EntryWriter ew{*bw};
    auto *dst = ew.begin_entry();
    std::memcpy(dst, decompressed.data(), decompressed.size());
    ew.finish_entry(entry_name, decompressed.size());
  } else {
    const fs::path out_path =
        dest / fs::path(entry_name, fs::path::generic_format);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out)
      return std::unexpected(
          std::format("cannot create '{}'", out_path.string()));
    out.write(reinterpret_cast<const char *>(decompressed.data()),
              static_cast<std::streamsize>(decompressed.size()));
  }
  return {};
}

// ---------------------------------------------------------------------------
// Size estimation (cheap header reads, no decompression)
// ---------------------------------------------------------------------------

// ZIP: sum m_uncomp_size from central directory — exact.
auto estimate_zip(const fs::path &src) -> std::optional<u64> {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, src.string().c_str(), 0))
    return std::nullopt;
  struct Guard {
    mz_zip_archive *z;
    ~Guard() { mz_zip_reader_end(z); }
  } g{&zip};
  u64 total = 0;
  mz_uint n = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st))
      return std::nullopt;
    if (!mz_zip_reader_is_file_a_directory(&zip, i))
      total += st.m_uncomp_size;
  }
  return total;
}

// ZSTD: read frame header (first ≤ 18 bytes) for stored content size.
// Returns nullopt when the encoder omitted the size field (streaming
// compression). For multi-frame zstd this only reads the first frame — use as
// an upper bound.
auto estimate_zstd(const fs::path &src) -> std::optional<u64> {
  constexpr usize kReadSize = 256;
  uint8_t buf[kReadSize];
  std::ifstream f(src, std::ios::binary);
  if (!f)
    return std::nullopt;
  f.read(reinterpret_cast<char *>(buf), kReadSize);
  usize n = static_cast<usize>(f.gcount());
  unsigned long long sz = ZSTD_getFrameContentSize(buf, n);
  if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR)
    return std::nullopt;
  return static_cast<u64>(sz);
}

// GZIP: read ISIZE from the last 4 bytes of the file.
// Only reliable for uncompressed content < 4 GiB; returns nullopt otherwise.
auto estimate_gzip(const fs::path &src) -> std::optional<u64> {
  std::ifstream f(src, std::ios::binary | std::ios::ate);
  if (!f)
    return std::nullopt;
  auto file_size = static_cast<u64>(f.tellg());
  if (file_size < 20)
    return std::nullopt; // minimum valid gzip
  f.seekg(-4, std::ios::end);
  uint8_t isize[4];
  if (f.read(reinterpret_cast<char *>(isize), 4).gcount() != 4)
    return std::nullopt;
  uint32_t sz = uint32_t(isize[0]) | (uint32_t(isize[1]) << 8) |
                (uint32_t(isize[2]) << 16) | (uint32_t(isize[3]) << 24);
  if (sz == 0)
    return std::nullopt; // ambiguous: could be 0 mod 2^32
  return static_cast<u64>(sz);
}

// ---------------------------------------------------------------------------
// Common dispatch
// ---------------------------------------------------------------------------

struct FormatFlags {
  bool is_zip = false;
  bool is_tar_gz = false;
  bool is_tar_zst = false;
  bool is_raw_zstd = false;
};

auto detect_format(const fs::path &src) -> FormatFlags {
  const auto ext = src.extension().string();
  const auto stem_ext = fs::path(src.stem()).extension().string();
  return {
      .is_zip = ext == ".zip",
      .is_tar_gz = (ext == ".gz" && stem_ext == ".tar") || ext == ".tgz",
      .is_tar_zst = (ext == ".zst" && stem_ext == ".tar") || ext == ".tzst",
      .is_raw_zstd = ext == ".zstd" || (ext == ".zst" && stem_ext != ".tar"),
  };
}

auto open_file(const fs::path &src)
    -> std::expected<std::ifstream, std::string> {
  std::ifstream f(src, std::ios::binary);
  if (!f)
    return std::unexpected(std::format("cannot open '{}'", src.string()));
  return f;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace dy::archive {

auto extract(const dy::VFSPath &src_vfs, const dy::VFSPath &dest_vfs)
    -> std::expected<void, std::string> {
  const fs::path src = dy::VFS::get().resolve(src_vfs);
  const fs::path dest_dir = dy::VFS::get().resolve(dest_vfs);

  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec)
    return std::unexpected(std::format("cannot create dest '{}': {}",
                                       dest_dir.string(), ec.message()));

  const auto [is_zip, is_tar_gz, is_tar_zst, is_raw_zstd] = detect_format(src);

  if (is_zip)
    return extract_zip_impl(src, dest_dir, nullptr);
  if (is_raw_zstd)
    return extract_raw_zstd_impl(src, nullptr, dest_dir);

  auto f = open_file(src);
  if (!f)
    return std::unexpected(f.error());

  if (is_tar_gz) {
    GzipStream gz(std::move(*f));
    if (auto r = gz.skip_header(); !r)
      return r;
    return extract_tar_impl(gz, dest_dir, nullptr);
  }
  if (is_tar_zst) {
    ZstdStream zs(std::move(*f));
    return extract_tar_impl(zs, dest_dir, nullptr);
  }

  return std::unexpected(
      std::format("unsupported archive format '{}' (supported: .zip .tar.gz "
                  ".tgz .tar.zst .tzst .zstd)",
                  src.extension().string()));
}

auto extract_to_memory(const dy::VFSPath &src_vfs, usize budget)
    -> std::expected<MemoryBundle, std::string> {
  const fs::path src = dy::VFS::get().resolve(src_vfs);

  const auto [is_zip, is_tar_gz, is_tar_zst, is_raw_zstd] = detect_format(src);

  if (!is_zip && !is_tar_gz && !is_tar_zst && !is_raw_zstd)
    return std::unexpected(
        std::format("unsupported archive format '{}' (supported: .zip .tar.gz "
                    ".tgz .tar.zst .tzst .zstd)",
                    src.extension().string()));

  // --- Cheap size estimate ---
  std::optional<u64> estimated;
  if (is_zip)
    estimated = estimate_zip(src);
  else if (is_tar_zst || is_raw_zstd)
    estimated = estimate_zstd(src);
  else if (is_tar_gz)
    estimated = estimate_gzip(src);

  if (estimated && *estimated > budget)
    return std::unexpected(std::format(
        "archive uncompressed size ({:.1f} GiB) exceeds budget ({:.1f} GiB) — "
        "increase budget or use archive::extract() to disk",
        static_cast<double>(*estimated) / (1ULL << 30),
        static_cast<double>(budget) / (1ULL << 30)));

  // Reserve virtual address space.
  // Use the estimate when available; otherwise fall back to the budget cap.
  const usize reserve_size = estimated ? *estimated : budget;
  void *base = vm_reserve(reserve_size);
  if (!base)
    return std::unexpected(
        std::format("failed to reserve {:.1f} GiB of virtual address space",
                    static_cast<double>(reserve_size) / (1ULL << 30)));

  MemoryBundle bundle;
  bundle.base = base;
  bundle.reserved = reserve_size;
  bundle.used = 0;

  BundleWriter bw{bundle};

  std::expected<void, std::string> result;

  if (is_zip) {
    result = extract_zip_impl(src, {}, &bw);
  } else if (is_raw_zstd) {
    result = extract_raw_zstd_impl(src, &bw, {});
  } else {
    auto f = open_file(src);
    if (!f) {
      vm_release(base, reserve_size);
      return std::unexpected(f.error());
    }

    if (is_tar_gz) {
      GzipStream gz(std::move(*f));
      if (auto r = gz.skip_header(); !r) {
        vm_release(base, reserve_size);
        return std::unexpected(r.error());
      }
      result = extract_tar_impl(gz, {}, &bw);
    } else {
      ZstdStream zs(std::move(*f));
      result = extract_tar_impl(zs, {}, &bw);
    }
  }

  if (!result) {
    vm_release(base, reserve_size);
    return std::unexpected(result.error());
  }

  // Decommit / advise-free the unused tail.
  vm_fit(base, reserve_size, bundle.used);
  return bundle;
}

} // namespace dy::archive
