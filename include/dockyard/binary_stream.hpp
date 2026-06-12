#pragma once

#include <dockyard/types.hpp>
#include <dockyard/vfs.hpp>
#include <dockyard/vfs_path.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace dy {

struct BinaryWriter {
  virtual void write(const void *data, std::size_t size) = 0;
  virtual ~BinaryWriter() = default;

  template <typename T> void write_t(const T &val) { write(&val, sizeof(T)); }
};

struct BinaryReader {
  virtual void read(void *dest, std::size_t size) = 0;
  virtual ~BinaryReader() = default;

  template <typename T> auto read_t() -> T {
    T val;
    read(&val, sizeof(T));
    return val;
  }
};

class MemoryWriter : public BinaryWriter {
public:
  explicit MemoryWriter(std::vector<std::uint8_t> &buf) : buffer(buf) {}

  void write(const void *data, std::size_t size) override {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
  }

private:
  std::vector<std::uint8_t> &buffer;
};

class OwningMemoryWriter : public BinaryWriter {
public:
  OwningMemoryWriter() = default;

  // Non-copyable — copying a large buffer mid-serialization would be
  // accidental and expensive.
  OwningMemoryWriter(const OwningMemoryWriter &) = delete;
  OwningMemoryWriter &operator=(const OwningMemoryWriter &) = delete;

  // Movable — futures need to capture it by move.
  OwningMemoryWriter(OwningMemoryWriter &&other) noexcept
      : buffer(std::move(other.buffer)) {}

  OwningMemoryWriter &operator=(OwningMemoryWriter &&other) noexcept {
    if (this != &other) {
      buffer = std::move(other.buffer);
    }
    return *this;
  }

  ~OwningMemoryWriter() override = default;

  void write(const void *data, std::size_t size) override {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
  }

  [[nodiscard]] auto take() noexcept -> std::vector<std::uint8_t> &&{
    return std::move(buffer);
  }

  [[nodiscard]] auto data() const noexcept ->const auto& {
    return buffer;
  }

  [[nodiscard]] std::size_t size() const noexcept { return buffer.size(); }

private:
  std::vector<std::uint8_t> buffer;
};

class MemoryReader : public BinaryReader {
public:
  explicit MemoryReader(const std::vector<std::uint8_t> &buf)
      : buffer(buf), offset(0) {}

  void read(void *dest, std::size_t size) override {
    if (offset + size > buffer.size()) {
      throw std::runtime_error("MemoryReader out of bounds read attempt.");
    }
    std::memcpy(dest, buffer.data() + offset, size);
    offset += size;
  }

private:
  const std::vector<std::uint8_t> &buffer;
  std::size_t offset;
};

class FileWriter : public BinaryWriter {
public:
  explicit FileWriter(const VFSPath &path)
      : stream(VFS::get().resolve(path), std::ios::binary) {
    if (!stream.is_open())
      throw std::runtime_error("Failed to open file for writing.");
  }

  void write(const void *data, std::size_t size) override {
    stream.write(static_cast<const char *>(data), size);
  }

private:
  std::ofstream stream;
};

class FileReader : public BinaryReader {
public:
  explicit FileReader(const VFSPath &path)
      : stream(VFS::get().resolve(path), std::ios::binary) {
    if (!stream.is_open())
      throw std::runtime_error("Failed to open file for reading.");
  }

  void read(void *dest, std::size_t size) override {
    stream.read(static_cast<char *>(dest), size);
  }

private:
  std::ifstream stream;
};

} // namespace dy
