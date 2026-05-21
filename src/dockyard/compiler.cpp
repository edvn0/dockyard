#include <dockyard/compiler.hpp>

#include <dockyard/vfs.hpp>
#include <dockyard/vfs_path.hpp>

#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

#include <atomic>
#include <format>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dy::shader {

struct OwnedBlob final : ISlangBlob {
  explicit OwnedBlob(std::vector<u8> d) : blob_data(std::move(d)) {}

  SLANG_NO_THROW SlangResult SLANG_MCALL
  queryInterface(SlangUUID const &uuid, void **out) SLANG_OVERRIDE {
    if (uuid == ISlangBlob::getTypeGuid()) {
      *out = static_cast<ISlangBlob *>(this);
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE {
    return ++reference_count;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE {
    const auto n = --reference_count;
    if (n == 0)
      delete this;
    return n;
  }

  SLANG_NO_THROW void const *SLANG_MCALL getBufferPointer() SLANG_OVERRIDE {
    return blob_data.data();
  }

  SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() SLANG_OVERRIDE {
    return blob_data.size();
  }

private:
  std::atomic<uint32_t> reference_count{1};
  std::vector<u8> blob_data;
};

struct SlangVFSAdapter final : ISlangFileSystem {
  SlangVFSAdapter(std::string_view mount) : mount(mount) {}

  SLANG_NO_THROW SlangResult SLANG_MCALL
  queryInterface(SlangUUID const &uuid, void **out) SLANG_OVERRIDE {
    if (uuid == ISlangFileSystem::getTypeGuid() ||
        uuid == ISlangUnknown::getTypeGuid()) {
      *out = static_cast<ISlangFileSystem *>(this);
      addRef();
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE {
    return ++reference_count;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE {
    const auto n = --reference_count;
    if (n == 0) {
      // Stack-allocated; caller manages lifetime via ComPtr.
    }
    return n;
  }

  SLANG_NO_THROW SlangResult SLANG_MCALL
  loadFile(char const *path, ISlangBlob **out_blob) SLANG_OVERRIDE {
    const auto vfs_path = VFSPath::create(std::format("{}://{}", mount, path));
    auto result = VFS::get().read_bytes(vfs_path);
    if (!result) {
      const std::string_view sv{path};
      if (!sv.ends_with(".slang-module"))
        warn("could not find file: {}", vfs_path.view());
      return SLANG_E_NOT_FOUND;
    }
    *out_blob = new OwnedBlob{std::move(*result)};
    return SLANG_OK;
  }

private:
  std::atomic<uint32_t> reference_count{1};
  std::string mount;

public:
  void *castAs(const SlangUUID &guid) override { return nullptr; }
};

static auto blob_to_string(Slang::ComPtr<ISlangBlob> &blob) -> std::string {
  if (blob == nullptr)
    return "(no diagnostic)";
  return {static_cast<const char *>(blob->getBufferPointer()),
          blob->getBufferSize()};
}

static auto to_stage(SlangStage s) -> Stage {
  switch (s) {
  case SLANG_STAGE_VERTEX:
    return Stage::Vertex;
  case SLANG_STAGE_FRAGMENT:
    return Stage::Fragment;
  case SLANG_STAGE_COMPUTE:
    return Stage::Compute;
  case SLANG_STAGE_MESH:
    return Stage::Mesh;
  case SLANG_STAGE_AMPLIFICATION:
    return Stage::Task;
  default:
    return Stage::Compute;
  }
}

static auto to_descriptor_type(slang::TypeReflection *type) -> DescriptorType {
  using Kind = slang::TypeReflection::Kind;

  if (type->getKind() == Kind::SamplerState) {
    const std::string_view name{type->getName() ? type->getName() : ""};
    return name.contains("Comparison") ? DescriptorType::ComparisonSampler
                                       : DescriptorType::Sampler;
  }

  return DescriptorType::SampledImage;
}

static auto extract_program_reflection(slang::IComponentType *linked)
    -> std::pair<std::vector<DescriptorBinding>, PushConstantRange> {
  std::vector<DescriptorBinding> bindings;
  PushConstantRange push_constants{
      .offset = 0xFFFFFFFF, .size = 0}; // Initialize properly for min/max

  slang::ProgramLayout *layout = linked->getLayout();

  auto process_parameter = [&](slang::VariableLayoutReflection *param) {
    slang::TypeLayoutReflection *tl = param->getTypeLayout();
    auto category = param->getCategory();

    bool is_push_constant =
        (category == slang::ParameterCategory::PushConstantBuffer);
    if (!is_push_constant) {
      is_push_constant =
          (param->getOffset(slang::ParameterCategory::PushConstantBuffer) !=
           SLANG_UNKNOWN_SIZE);
    }

    if (is_push_constant) {
      size_t slang_offset =
          param->getOffset(slang::ParameterCategory::PushConstantBuffer);
      u32 offset = (slang_offset == SLANG_UNKNOWN_SIZE)
                       ? 0
                       : static_cast<u32>(slang_offset);
      u32 size = static_cast<u32>(tl->getSize());

      push_constants.offset = std::min(push_constants.offset, offset);
      push_constants.size = std::max(push_constants.size, offset + size);
      return true; // Handled as push constant
    }
    return false;
  };

  const u32 global_param_count = layout->getParameterCount();
  for (u32 i = 0; i < global_param_count; ++i) {
    slang::VariableLayoutReflection *param = layout->getParameterByIndex(i);

    if (process_parameter(param)) {
      continue;
    }

    auto category = param->getCategory();
    if (category == slang::ParameterCategory::Uniform) {
      continue;
    }

    slang::TypeLayoutReflection *tl = param->getTypeLayout();
    DescriptorBinding b{};
    b.name = param->getName();
    b.set = static_cast<u32>(param->getBindingSpace());
    b.binding = static_cast<u32>(param->getBindingIndex());
    b.array_count = (tl->getKind() == slang::TypeReflection::Kind::Array)
                        ? static_cast<u32>(tl->getElementCount())
                        : 1;
    b.type = to_descriptor_type(tl->getType());

    bindings.push_back(std::move(b));
  }

  const u32 entry_point_count = layout->getEntryPointCount();
  for (u32 ep_i = 0; ep_i < entry_point_count; ++ep_i) {
    slang::EntryPointLayout *ep_layout = layout->getEntryPointByIndex(ep_i);
    const u32 ep_param_count = ep_layout->getParameterCount();

    for (u32 p_i = 0; p_i < ep_param_count; ++p_i) {
      slang::VariableLayoutReflection *param =
          ep_layout->getParameterByIndex(p_i);

      process_parameter(param);
    }
  }

  if (push_constants.offset == 0xFFFFFFFF) {
    push_constants.offset = 0;
  }

  return {std::move(bindings), push_constants};
}

static auto extract_entry_point_info(slang::EntryPointLayout *ep)
    -> EntryPointInfo {
  EntryPointInfo info{};
  info.name = ep->getName();
  info.stage = to_stage(ep->getStage());

  if (ep->getStage() == SLANG_STAGE_COMPUTE) {
    SlangUInt wg[3] = {1, 1, 1};
    ep->getComputeThreadGroupSize(3, wg);
    info.workgroup_x = static_cast<u32>(wg[0]);
    info.workgroup_y = static_cast<u32>(wg[1]);
    info.workgroup_z = static_cast<u32>(wg[2]);
  }

  return info;
}

#ifdef DOCKYARD_DEBUG
constexpr auto debug_level = SLANG_DEBUG_INFO_LEVEL_STANDARD;
constexpr auto optimisation_level = SLANG_OPTIMIZATION_LEVEL_NONE;
#else
constexpr auto debug_level = SLANG_DEBUG_INFO_LEVEL_NONE;
constexpr auto optimisation_level = SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
#endif

constexpr u64 k_cache_config_seed = (static_cast<u64>(debug_level) << 32) |
                                    static_cast<u64>(optimisation_level);

static auto hash_source(std::span<const u8> bytes) -> u64 {
  u64 h = 14695981039346656037ULL; // FNV offset basis
  for (const u8 b : bytes) {
    h ^= b;
    h *= 1099511628211ULL; // FNV prime
  }
  return h ^ k_cache_config_seed;
}

struct Compiler::Impl {
  Slang::ComPtr<slang::IGlobalSession> global_session;
  std::mutex global_session_mutex;

  struct CacheEntry {
    u64 content_hash;
    CompiledShader shader;
  };

  mutable std::shared_mutex cache_mutex;
  std::unordered_map<std::string, CacheEntry> cache;

  Impl() {
    if (SLANG_FAILED(slang::createGlobalSession(global_session.writeRef())))
      std::abort();
  }
};

Compiler::Compiler() : impl(std::make_unique<Impl>()) {}
Compiler::~Compiler() = default;

auto Compiler::the() -> Compiler & {
  static Compiler instance;
  return instance;
}

auto Compiler::compile(const std::string_view path)
    -> std::expected<CompiledShader, CompilationError> {
  auto vfs_path = VFSPath::create(path);
  return compile(vfs_path);
}

auto Compiler::compile(const VFSPath &vfs_path)
    -> std::expected<CompiledShader, CompilationError> {
  using enum CompilationError::Type;

  // ------------------------------------------------------------------
  // 1. Read source bytes for cache key computation.
  //    This also lets us detect a missing file before spinning up a
  //    Slang session.
  // ------------------------------------------------------------------
  auto source_bytes = VFS::get().read_bytes(vfs_path);
  if (!source_bytes)
    return std::unexpected{CompilationError{
        .type = FileNotFound, .message = std::string(vfs_path.view())}};

  const u64 source_hash = hash_source(*source_bytes);
  const std::string cache_key{vfs_path.view()};

  // ------------------------------------------------------------------
  // 2. Cache lookup (shared lock — multiple readers are fine).
  // ------------------------------------------------------------------
  {
    std::shared_lock read_lock{impl->cache_mutex};
    if (auto it = impl->cache.find(cache_key); it != impl->cache.end()) {
      if (it->second.content_hash == source_hash) {
        return it->second.shader; // copy — CompiledShader owns its vectors
      }
    }
  }

  // ------------------------------------------------------------------
  // 3. Cache miss: run the full Slang pipeline.
  // ------------------------------------------------------------------
  slang::CompilerOptionEntry target_options[] = {
      {
          slang::CompilerOptionName::VulkanUseEntryPointName,
          {slang::CompilerOptionValueKind::Int, 1},
      },
      {
          slang::CompilerOptionName::DebugInformation,
          {slang::CompilerOptionValueKind::Int, debug_level},
      },
      {
          slang::CompilerOptionName::Optimization,
          {slang::CompilerOptionValueKind::Int, optimisation_level},
      },
      {
          slang::CompilerOptionName::GLSLForceScalarLayout,
          {slang::CompilerOptionValueKind::Int, 1},
      },
  };

  slang::CompilerOptionEntry session_options[] = {
      {slang::CompilerOptionName::MatrixLayoutColumn,
       {slang::CompilerOptionValueKind::Int, 1}},
      {
          slang::CompilerOptionName::GLSLForceScalarLayout,
          {slang::CompilerOptionValueKind::Int, 1},
      },
  };

  slang::TargetDesc target{};
  target.format = SLANG_SPIRV;
  target.compilerOptionEntries = target_options;
  target.compilerOptionEntryCount = static_cast<u32>(std::size(target_options));

  SlangVFSAdapter fs{vfs_path.scheme()};
  slang::SessionDesc session_desc{};
  session_desc.targets = &target;
  session_desc.targetCount = 1;
  session_desc.fileSystem = &fs;
  session_desc.compilerOptionEntries = session_options;
  session_desc.compilerOptionEntryCount =
      static_cast<u32>(std::size(session_options));

  Slang::ComPtr<slang::ISession> session;
  {
    std::scoped_lock lock(impl->global_session_mutex);
    target.profile = impl->global_session->findProfile("spirv_1_6");
    if (SLANG_FAILED(impl->global_session->createSession(session_desc,
                                                         session.writeRef()))) {
      return std::unexpected{CompilationError{
          .type = Compilation, .message = "failed to create slang session"}};
    }
  }

  auto stem = std::filesystem::path{vfs_path.relative_path()}.stem().string();
  Slang::ComPtr<ISlangBlob> diag;
  slang::IModule *mod = session->loadModule(stem.c_str(), diag.writeRef());

  if (mod == nullptr)
    return std::unexpected{CompilationError{.type = FileNotFound,
                                            .message = blob_to_string(diag)}};

  const i32 ep_count = mod->getDefinedEntryPointCount();
  if (ep_count == 0)
    return std::unexpected{CompilationError{
        .type = Compilation, .message = "module defines no entry points"}};

  std::vector<Slang::ComPtr<slang::IEntryPoint>> entry_points(
      static_cast<usize>(ep_count));

  for (i32 i = 0; i < ep_count; ++i) {
    if (SLANG_FAILED(mod->getDefinedEntryPoint(i, entry_points[i].writeRef())))
      return std::unexpected{CompilationError{
          .type = Compilation,
          .message = std::format("failed to retrieve entry point {}", i)}};
  }

  std::vector<slang::IComponentType *> components;
  components.reserve(1 + static_cast<usize>(ep_count));
  components.push_back(mod);
  for (auto &ep : entry_points)
    components.push_back(ep.get());

  Slang::ComPtr<slang::IComponentType> composite;
  if (SLANG_FAILED(session->createCompositeComponentType(
          components.data(), static_cast<SlangInt>(components.size()),
          composite.writeRef(), diag.writeRef())))
    return std::unexpected{
        CompilationError{.type = Compilation, .message = blob_to_string(diag)}};

  Slang::ComPtr<slang::IComponentType> linked;
  if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef())))
    return std::unexpected{
        CompilationError{.type = Compilation, .message = blob_to_string(diag)}};

  auto [bindings, push_constants] = extract_program_reflection(linked.get());

  slang::ProgramLayout *layout = linked->getLayout();

  std::vector<CompiledEntryPoint> compiled_entry_points;
  compiled_entry_points.reserve(static_cast<usize>(ep_count));

  for (i32 i = 0; i < ep_count; ++i) {
    Slang::ComPtr<ISlangBlob> code;
    if (SLANG_FAILED(
            linked->getEntryPointCode(i, 0, code.writeRef(), diag.writeRef())))
      return std::unexpected{
          CompilationError{.type = Compilation,
                           .message = std::format("entry point {}: {}", i,
                                                  blob_to_string(diag))}};

    const auto *words = static_cast<const u32 *>(code->getBufferPointer());
    const usize word_count = code->getBufferSize() / sizeof(u32);

    CompiledEntryPoint cep{};
    cep.spirv.assign(words, words + word_count);
    cep.entry_point = extract_entry_point_info(layout->getEntryPointByIndex(i));

    compiled_entry_points.push_back(std::move(cep));
  }

  CompiledShader result{
      .entry_points = std::move(compiled_entry_points),
      .bindings = std::move(bindings),
      .push_constants = push_constants,
  };

  {
    std::unique_lock write_lock{impl->cache_mutex};
    impl->cache.insert_or_assign(
        cache_key,
        Impl::CacheEntry{.content_hash = source_hash, .shader = result});
  }

  return result;
}

auto Compiler::invalidate(const VFSPath &vfs_path) -> void {
  std::unique_lock write_lock{impl->cache_mutex};
  impl->cache.erase(std::string{vfs_path.view()});
}

auto Compiler::clear_cache() -> void {
  std::unique_lock write_lock{impl->cache_mutex};
  impl->cache.clear();
}

auto Compiler::precache_shaders(Badge<App>) -> std::future<void> {
  using namespace std::literals;
  return std::async(std::launch::async, [this] {
    auto shader_paths = VFS::get().list(
        "shaders://", {
                          .ignored_dirs = {"include"sv, "includes"sv},
                          .included_extensions = {".slang"sv},
                      });
    for (const auto &path : shader_paths) {
      if (path.extension() == ".slang") {
        auto result = compile(VFSPath::create("shaders://{}", path.filename()));
        if (!result) {
          error("Failed to compile shader {}: {}", path.filename(),
                result.error().message);
        } else {
          info("Precompiled shader: {}", path.filename());
        }
      }
    }
  });
}

} // namespace dy::shader