#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace fs = std::filesystem;

template<typename T>
struct Deleter {
  auto operator()(T* ptr) const noexcept -> void = delete;
};

template<>
struct Deleter<cgltf_data> {
  auto operator()(cgltf_data* data) const noexcept -> void {
    cgltf_free(data);
  }
};



using GltfData = std::unique_ptr<cgltf_data, Deleter<cgltf_data>>;

struct Args
{
    fs::path input_path {};
    fs::path ktx2_dir {};
};

struct TextureRef
{
    std::size_t texture_index {};
    std::size_t image_index {};
    std::size_t texcoord {};
};

struct TextureBinding
{
    std::size_t material_index {};
    std::string_view material_name;
    std::string_view semantic;
    TextureRef ref {};
};

struct TextureSlot
{
    std::string_view semantic;
    const cgltf_texture_view* view {};
};

auto as_sv(const char* text) -> std::string_view
{
    return (text != nullptr) ? std::string_view {text} : std::string_view {};
}

auto parse_args(std::span<char*> argv) -> std::expected<Args, std::string>
{
    Args args {};

    for (std::size_t i = 1; i < argv.size(); ++i)
    {
        const std::string_view arg {argv[i]};

        if (arg == "--ktx2-dir")
        {
            if (i + 1 >= argv.size())
                return std::unexpected {"missing value for --ktx2-dir"};

            args.ktx2_dir = argv[++i];
            continue;
        }

        if (args.input_path.empty())
        {
            args.input_path = argv[i];
            continue;
        }

        return std::unexpected {std::format("unexpected argument: {}", arg)};
    }

    if (args.input_path.empty())
        return std::unexpected {"missing input path"};

    return args;
}

auto load_gltf(const fs::path& path) -> std::expected<GltfData, std::string>
{
    cgltf_options options {};
    cgltf_data* raw_data {};

    const auto path_string = path.string();

    if (cgltf_parse_file(&options, path_string.c_str(), &raw_data) != cgltf_result_success)
        return std::unexpected {std::format("failed to parse glTF/GLB: {}", path_string)};

    GltfData data {raw_data};

    if (cgltf_load_buffers(&options, data.get(), path_string.c_str()) != cgltf_result_success)
        return std::unexpected {std::format("failed to load buffers for: {}", path_string)};

    return data;
}

auto materials(const cgltf_data& data) -> std::span<const cgltf_material>
{
    return {data.materials, data.materials_count};
}

auto textures(const cgltf_data& data) -> std::span<const cgltf_texture>
{
    return {data.textures, data.textures_count};
}

auto images(const cgltf_data& data) -> std::span<const cgltf_image>
{
    return {data.images, data.images_count};
}

template <typename T>
auto index_of(std::span<const T> values, const T* ptr) -> std::optional<std::size_t>
{
    if (!ptr || values.empty())
        return std::nullopt;

    const auto* first = values.data();
    const auto* last = first + values.size();

    if (ptr < first || ptr >= last)
        return std::nullopt;

    return static_cast<std::size_t>(ptr - first);
}

auto image_for_texture(const cgltf_texture& texture) -> const cgltf_image*
{
    if (texture.basisu_image != nullptr)
        return texture.basisu_image;

    return texture.image;
}

auto texture_ref_of(
    const cgltf_data& data,
    const cgltf_texture_view& view) -> std::optional<TextureRef>
{
    if (view.texture == nullptr)
        return std::nullopt;

    const auto texture_index = index_of(textures(data), view.texture);
    if (!texture_index)
        return std::nullopt;

    const auto image_index = index_of(images(data), image_for_texture(*view.texture));
    if (!image_index)
        return std::nullopt;

    return TextureRef {
        .texture_index = *texture_index,
        .image_index = *image_index,
        .texcoord = static_cast<std::size_t>(view.texcoord),
    };
}

auto core_slots(const cgltf_material& material) -> std::vector<TextureSlot>
{
    std::vector<TextureSlot> slots {};

    if (material.has_pbr_metallic_roughness != 0)
    {
        const auto& pbr = material.pbr_metallic_roughness;

        slots.emplace_back("albedo", &pbr.base_color_texture);
        slots.emplace_back("metallic_roughness", &pbr.metallic_roughness_texture);
    }

    slots.emplace_back("normal", &material.normal_texture);
    slots.emplace_back("occlusion", &material.occlusion_texture);
    slots.emplace_back("emissive", &material.emissive_texture);

    return slots;
}

auto extension_slots(const cgltf_material& material) -> std::vector<TextureSlot>
{
    std::vector<TextureSlot> slots {};

    if (material.has_clearcoat != 0)
    {
        slots.emplace_back("clearcoat", &material.clearcoat.clearcoat_texture);
        slots.emplace_back("clearcoat_roughness", &material.clearcoat.clearcoat_roughness_texture);
        slots.emplace_back("clearcoat_normal", &material.clearcoat.clearcoat_normal_texture);
    }

    if (material.has_sheen != 0)
    {
        slots.emplace_back("sheen_color", &material.sheen.sheen_color_texture);
        slots.emplace_back("sheen_roughness", &material.sheen.sheen_roughness_texture);
    }

    if (material.has_transmission != 0)
        slots.emplace_back("transmission", &material.transmission.transmission_texture);

    if (material.has_volume != 0)
        slots.emplace_back("thickness", &material.volume.thickness_texture);

    if (material.has_specular != 0)
    {
        slots.emplace_back("specular", &material.specular.specular_texture);
        slots.emplace_back("specular_color", &material.specular.specular_color_texture);
    }

    if (material.has_iridescence != 0)
    {
        slots.emplace_back("iridescence", &material.iridescence.iridescence_texture);
        slots.emplace_back("iridescence_thickness", &material.iridescence.iridescence_thickness_texture);
    }

    return slots;
}

auto texture_slots(const cgltf_material& material) -> std::vector<TextureSlot>
{
    auto slots = core_slots(material);
    auto extensions = extension_slots(material);

    slots.insert(slots.end(), extensions.begin(), extensions.end());

    return slots;
}

auto extract_bindings(const cgltf_data& data) -> std::vector<TextureBinding>
{
    std::vector<TextureBinding> bindings {};

    const auto material_span = materials(data);

    for (std::size_t material_index = 0; material_index < material_span.size(); ++material_index)
    {
        const auto& material = material_span[material_index];

        for (const auto& slot : texture_slots(material))
        {
            if (slot.view == nullptr)
                continue;

            const auto ref = texture_ref_of(data, *slot.view);
            if (!ref)
                continue;

            bindings.emplace_back(
                material_index,
                as_sv(material.name),
                slot.semantic,
                *ref);
        }
    }

    return bindings;
}

auto display_material_name(const TextureBinding& binding) -> std::string
{
    if (!binding.material_name.empty())
        return std::string {binding.material_name};

    return std::format("material_{}", binding.material_index);
}

auto sidecar_path(const fs::path& dir, std::size_t image_index) -> std::string
{
    auto filename = std::format("img{}.ktx2", image_index);

    if (dir.empty())
        return filename;

    return (dir / filename).generic_string();
}

auto print_material_table(
    const fs::path& asset,
    const fs::path& ktx2_dir,
    std::span<const TextureBinding> bindings) -> void
{
    std::cout << "Asset: " << asset.generic_string() << "\n\n";

    if (bindings.empty())
    {
        std::cout << "No material texture bindings found.\n";
        return;
    }

    std::optional<std::size_t> current_material_index {};

    for (const auto& binding : bindings)
    {
        if (current_material_index != binding.material_index)
        {
            if (current_material_index)
                std::cout << "\n";

            current_material_index = binding.material_index;

            std::cout
                << "Material[" << binding.material_index << "] "
                << display_material_name(binding)
                << "\n";
        }

        std::cout
            << "  " << std::format("{:<22}", binding.semantic)
            << " image=" << std::format("{:<4}", binding.ref.image_index)
            << " texture=" << std::format("{:<4}", binding.ref.texture_index)
            << " uv=" << std::format("{:<2}", binding.ref.texcoord)
            << " sidecar=" << sidecar_path(ktx2_dir, binding.ref.image_index)
            << "\n";
    }
}

auto print_usage(std::string_view exe) -> void
{
    std::cerr << std::format(
        "usage:\n"
        "  {} <asset.gltf|asset.glb> [--ktx2-dir <dir>]\n",
        exe);
}

auto main(int argc, char** raw_argv) -> int
{
    const auto argv = std::span {raw_argv, static_cast<std::size_t>(argc)};

    const auto args = parse_args(argv);
    if (!args)
    {
        std::cerr << args.error() << "\n";
        print_usage(argv.empty() ? "gltf-imgmap" : argv.front());
        return 1;
    }

    auto data = load_gltf(args->input_path);
    if (!data)
    {
        std::cerr << data.error() << "\n";
        return 1;
    }

    const auto bindings = extract_bindings(**data);

    print_material_table(args->input_path, args->ktx2_dir, bindings);

    return 0;
}