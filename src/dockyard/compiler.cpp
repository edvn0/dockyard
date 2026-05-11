#include <dockyard/compiler.hpp>

#include <dockyard/vfs_path.hpp>

#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

#include <atomic>
#include <format>
#include <string_view>

namespace dy::shader {

// ---------------------------------------------------------------------------
// OwnedBlob
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// SlangVFSAdapter
// ---------------------------------------------------------------------------

struct SlangVFSAdapter final : ISlangFileSystem {
  SlangVFSAdapter(std::string_view mount) : mount_(mount) {}

  SLANG_NO_THROW SlangResult SLANG_MCALL
  queryInterface(SlangUUID const &uuid, void **out) SLANG_OVERRIDE {
    if (uuid == ISlangFileSystem::getTypeGuid()) {
      *out = static_cast<ISlangFileSystem *>(this);
      return SLANG_OK;
    }
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE { return 1; }
  SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE { return 1; }

  SLANG_NO_THROW SlangResult SLANG_MCALL
  loadFile(char const *path, ISlangBlob **out_blob) SLANG_OVERRIDE {
    const auto vfs_path = VFSPath::create(std::format("{}://{}", mount_, path));
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
  std::string mount_;

public:
  void *castAs(const SlangUUID &guid) override { return nullptr; }
};

static auto blob_to_string(Slang::ComPtr<ISlangBlob> &blob) -> std::string {
  if (!blob)
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
  PushConstantRange push_constants{};

  slang::ProgramLayout *layout = linked->getLayout();
  const u32 param_count = layout->getParameterCount();

  for (u32 i = 0; i < param_count; ++i) {
    slang::VariableLayoutReflection *param = layout->getParameterByIndex(i);
    slang::TypeLayoutReflection *tl = param->getTypeLayout();

    if (param->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
      push_constants = {.offset = 0, .size = static_cast<u32>(tl->getSize())};
      continue;
    }

    if (param->getCategory() == slang::ParameterCategory::Uniform)
      continue;

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

  return {std::move(bindings), push_constants};
}

// Extract per-entry-point metadata from a layout. The SPIR-V blob itself is
// fetched separately via getEntryPointCode, so this only fills EntryPointInfo.
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

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Compiler::Impl {
  Slang::ComPtr<slang::IGlobalSession> global_session;

  Impl() {
    if (SLANG_FAILED(slang::createGlobalSession(global_session.writeRef())))
      std::abort();
  }
};

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

Compiler::Compiler() : impl(std::make_unique<Impl>()) {}
Compiler::~Compiler() = default;

auto Compiler::the() -> Compiler & {
  static Compiler instance;
  return instance;
}

#ifdef DOCKYARD_DEBUG
constexpr auto debug_level = SLANG_DEBUG_INFO_LEVEL_STANDARD;
constexpr auto optimisation_level = SLANG_OPTIMIZATION_LEVEL_NONE;
#else
constexpr auto debug_level = SLANG_DEBUG_INFO_LEVEL_NONE;
constexpr auto optimisation_level = SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
#endif

auto Compiler::compile(const std::string_view path)
    -> std::expected<CompiledShader, CompilationError> {
  auto vfs_path = VFSPath::create(path);
  return compile(vfs_path);
}

auto Compiler::compile(const VFSPath &vfs_path)
    -> std::expected<CompiledShader, CompilationError> {
  using enum CompilationError::Type;

  slang::CompilerOptionEntry target_options[] = {
      {slang::CompilerOptionName::VulkanUseEntryPointName,
       {slang::CompilerOptionValueKind::Int, 1}},
      {slang::CompilerOptionName::DebugInformation,
       {slang::CompilerOptionValueKind::Int, debug_level}},
      {slang::CompilerOptionName::Optimization,
       {slang::CompilerOptionValueKind::Int, optimisation_level}},
  };

  slang::CompilerOptionEntry session_options[] = {
      {slang::CompilerOptionName::MatrixLayoutColumn,
       {slang::CompilerOptionValueKind::Int, 1}},
  };
  slang::TargetDesc target{};
  target.format = SLANG_SPIRV;
  target.profile = impl->global_session->findProfile("spirv_1_6");
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
  if (SLANG_FAILED(impl->global_session->createSession(session_desc,
                                                       session.writeRef())))
    return std::unexpected{
        CompilationError{Compilation, "failed to create slang session"}};

  auto stem = std::filesystem::path{vfs_path.relative_path()}.stem().string();
  Slang::ComPtr<ISlangBlob> diag;
  slang::IModule *mod = session->loadModule(stem.c_str(), diag.writeRef());

  if (!mod)
    return std::unexpected{
        CompilationError{FileNotFound, blob_to_string(diag)}};

  const i32 ep_count = mod->getDefinedEntryPointCount();
  if (ep_count == 0)
    return std::unexpected{
        CompilationError{Compilation, "module defines no entry points"}};

  std::vector<Slang::ComPtr<slang::IEntryPoint>> entry_points(
      static_cast<usize>(ep_count));

  for (i32 i = 0; i < ep_count; ++i) {
    if (SLANG_FAILED(mod->getDefinedEntryPoint(i, entry_points[i].writeRef())))
      return std::unexpected{CompilationError{
          Compilation, std::format("failed to retrieve entry point {}", i)}};
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
    return std::unexpected{CompilationError{Compilation, blob_to_string(diag)}};

  Slang::ComPtr<slang::IComponentType> linked;
  if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef())))
    return std::unexpected{CompilationError{Compilation, blob_to_string(diag)}};

  auto [bindings, push_constants] = extract_program_reflection(linked.get());

  slang::ProgramLayout *layout = linked->getLayout();

  std::vector<CompiledEntryPoint> compiled_entry_points;
  compiled_entry_points.reserve(static_cast<usize>(ep_count));

  for (i32 i = 0; i < ep_count; ++i) {
    Slang::ComPtr<ISlangBlob> code;
    if (SLANG_FAILED(
            linked->getEntryPointCode(i, 0, code.writeRef(), diag.writeRef())))
      return std::unexpected{
          CompilationError{Compilation, std::format("entry point {}: {}", i,
                                                    blob_to_string(diag))}};

    const auto *words = static_cast<const u32 *>(code->getBufferPointer());
    const usize word_count = code->getBufferSize() / sizeof(u32);

    CompiledEntryPoint cep{};
    cep.spirv.assign(words, words + word_count);
    cep.entry_point = extract_entry_point_info(layout->getEntryPointByIndex(i));

    compiled_entry_points.push_back(std::move(cep));
  }

  return CompiledShader{
      .entry_points = std::move(compiled_entry_points),
      .bindings = std::move(bindings),
      .push_constants = push_constants,
  };
}

} // namespace dy::shader