#pragma once

#include <dockyard/bindless_handle.hpp>
#include <dockyard/device_geometry.hpp>
#include <dockyard/mesh_cache.hpp>
#include <dockyard/types.hpp>

#include <fastgltf/core.hpp>

#include <vector>

namespace dy::mesh::detail {

enum class ImageColorSpace : u8 { linear, srgb };

[[nodiscard]] auto classify_images(const fastgltf::Asset &asset)
    -> std::vector<ImageColorSpace>;

[[nodiscard]] auto build_gpu_material(const fastgltf::Material &mat,
                                      const fastgltf::Asset &asset,
                                      const std::vector<TextureHandle> &handles)
    -> GPUMaterial;

// Which material texture slots reference image_idx, keyed by material index.
// Recomputed on demand (glTF asset or a mesh-cache reload) rather than cached,
// since material_base_slot is only known once materials are (re)allocated
// into pool slots at the point of use.
[[nodiscard]] auto build_patch_list(const fastgltf::Asset &asset, usize image_idx)
    -> std::vector<mesh_cache::MaterialTexturePatch>;

} // namespace dy::mesh::detail
