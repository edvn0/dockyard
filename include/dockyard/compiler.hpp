#pragma once

#include <dockyard/types.hpp>
#include <dockyard/vfs_path.hpp>

#include <expected>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace dy {
class App;
}

namespace dy::shader {

enum class DescriptorType : u8 {
  SampledImage,
  Sampler,
  ComparisonSampler,
};

enum class Stage : u8 {
  Vertex,
  Fragment,
  Mesh,
  Task,
  Compute,
};
MAKE_BITFIELD(Stage);

struct DescriptorBinding {
  std::string name;
  u32 set{};
  u32 binding{};
  u32 array_count{1};
  DescriptorType type{};
};

struct PushConstantRange {
  u32 offset{};
  u32 size{};
};

struct EntryPointInfo {
  std::string name;
  Stage stage{};
  u32 workgroup_x{1}, workgroup_y{1}, workgroup_z{1};
};

struct CompiledEntryPoint {
  std::vector<u32> spirv{};
  EntryPointInfo entry_point{};
};

struct CompiledShader {
  std::vector<CompiledEntryPoint> entry_points{};
  std::vector<DescriptorBinding> bindings{};
  PushConstantRange push_constants{};
};

struct CompilationError {
  enum class Type : u8 { FileNotFound, Compilation };
  Type type;
  std::string message;
};

class Compiler {
public:
  static auto the() -> Compiler &;

  auto compile(const VFSPath &vfs_path)
      -> std::expected<CompiledShader, CompilationError>;
  auto compile(std::string_view)
      -> std::expected<CompiledShader, CompilationError>;

  [[nodiscard]] auto precache_shaders(Badge<App>) -> std::future<void>;
  auto invalidate(const VFSPath &) -> void;
  auto clear_cache() -> void;

  ~Compiler();

  Compiler(const Compiler &) = delete;
  auto operator=(const Compiler &) = delete;
  Compiler(Compiler &&) = delete;
  auto operator=(Compiler &&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  Compiler();
};

} // namespace dy::shader