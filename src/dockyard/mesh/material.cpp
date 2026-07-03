#include <dockyard/mesh/material.hpp>

#include <fastgltf/types.hpp>

namespace dy::mesh::detail {

namespace {

constexpr u32 fb_albedo = 0u;
constexpr u32 fb_normal = 1u;
constexpr u32 fb_mr = 2u;
constexpr u32 fb_occlusion = 3u;
constexpr u32 fb_emissive = 4u;

[[nodiscard]] auto resolve_tex(const fastgltf::Asset &asset,
                               const std::vector<TextureHandle> &handles,
                               const auto &info, u32 fallback) -> u32 {
  if (!info.has_value())
    return fallback;
  const auto &tex = asset.textures[info->textureIndex];
  if (!tex.imageIndex.has_value())
    return fallback;
  return handles[*tex.imageIndex].index();
}

} // namespace

auto classify_images(const fastgltf::Asset &asset)
    -> std::vector<ImageColorSpace> {
  std::vector<ImageColorSpace> cs(asset.images.size(), ImageColorSpace::linear);

  auto mark_srgb = [&](const fastgltf::Optional<fastgltf::TextureInfo> &info) {
    if (!info.has_value())
      return;
    const auto &tex = asset.textures[info->textureIndex];
    if (tex.imageIndex.has_value())
      cs[*tex.imageIndex] = ImageColorSpace::srgb;
  };

  for (const auto &mat : asset.materials) {
    mark_srgb(mat.pbrData.baseColorTexture);
    mark_srgb(mat.emissiveTexture);
    if (const auto *sg = mat.specularGlossiness.get(); sg != nullptr) {
      mark_srgb(sg->diffuseTexture);
      // specularGlossinessTexture RGB is linear per KHR_materials_pbrSpecularGlossiness spec
    }
    if (const auto *spec = mat.specular.get(); spec != nullptr) {
      mark_srgb(spec->specularColorTexture);
      // specularTexture alpha (specular strength) is linear — not marked
    }
  }
  return cs;
}

auto build_gpu_material(const fastgltf::Material &mat,
                        const fastgltf::Asset &asset,
                        const std::vector<TextureHandle> &handles)
    -> GPUMaterial {
  GPUMaterial gpu{};
  const auto &pbr = mat.pbrData;

  gpu.emissive_factor[0] = mat.emissiveFactor[0];
  gpu.emissive_factor[1] = mat.emissiveFactor[1];
  gpu.emissive_factor[2] = mat.emissiveFactor[2];
  gpu.emissive_factor[3] = mat.emissiveStrength;

  gpu.normal_scale = mat.normalTexture ? mat.normalTexture->scale : 1.0F;
  gpu.occlusion_strength =
      mat.occlusionTexture ? mat.occlusionTexture->strength : 1.0F;
  gpu.alpha_cutoff = mat.alphaCutoff;
  gpu.alpha_mode = static_cast<u32>(mat.alphaMode);

  gpu.normal_index = resolve_tex(asset, handles, mat.normalTexture, fb_normal);
  gpu.emissive_index =
      resolve_tex(asset, handles, mat.emissiveTexture, fb_emissive);
  gpu.occlusion_index =
      resolve_tex(asset, handles, mat.occlusionTexture, fb_occlusion);

  gpu.flags = MaterialFlags::depth_prepass;
  if (mat.alphaMode == fastgltf::AlphaMode::Mask)
    set_flag(gpu.flags, MaterialFlags::alpha_mask);
  if (mat.alphaMode == fastgltf::AlphaMode::Blend)
    set_flag(gpu.flags, MaterialFlags::alpha_blend);
  if (mat.doubleSided)
    set_flag(gpu.flags, MaterialFlags::two_sided);

  if (const auto *sg = mat.specularGlossiness.get(); sg != nullptr) {
    // KHR_materials_pbrSpecularGlossiness workflow.
    // diffuse → albedo slot; metallic_factor repurposed as glossiness_factor.
    gpu.albedo_factor[0] = sg->diffuseFactor[0];
    gpu.albedo_factor[1] = sg->diffuseFactor[1];
    gpu.albedo_factor[2] = sg->diffuseFactor[2];
    gpu.albedo_factor[3] = sg->diffuseFactor[3];
    gpu.specular_factor[0] = sg->specularFactor[0];
    gpu.specular_factor[1] = sg->specularFactor[1];
    gpu.specular_factor[2] = sg->specularFactor[2];
    gpu.metallic_factor = sg->glossinessFactor;
    gpu.albedo_index =
        resolve_tex(asset, handles, sg->diffuseTexture, fb_albedo);
    gpu.specular_glossiness_index =
        resolve_tex(asset, handles, sg->specularGlossinessTexture, fb_mr);
    if (!mat.occlusionTexture.has_value())
      set_flag(gpu.flags, MaterialFlags::no_occlusion);
    set_flag(gpu.flags, MaterialFlags::spec_gloss_workflow);
  } else {
    // Metallic-roughness (default glTF PBR workflow).
    gpu.albedo_factor[0] = pbr.baseColorFactor[0];
    gpu.albedo_factor[1] = pbr.baseColorFactor[1];
    gpu.albedo_factor[2] = pbr.baseColorFactor[2];
    gpu.albedo_factor[3] = pbr.baseColorFactor[3];
    gpu.metallic_factor = pbr.metallicFactor;
    gpu.roughness_factor = pbr.roughnessFactor;
    gpu.albedo_index =
        resolve_tex(asset, handles, pbr.baseColorTexture, fb_albedo);
    gpu.metallic_roughness_index =
        resolve_tex(asset, handles, pbr.metallicRoughnessTexture, fb_mr);

    const bool has_occlusion_ref = mat.occlusionTexture.has_value();
    const bool has_mr = pbr.metallicRoughnessTexture.has_value();
    if (has_occlusion_ref && has_mr &&
        mat.occlusionTexture->textureIndex ==
            pbr.metallicRoughnessTexture->textureIndex) {
      set_flag(gpu.flags, MaterialFlags::combined_orm);
    } else if (!has_occlusion_ref && has_mr) {
      set_flag(gpu.flags, MaterialFlags::combined_orm);
    } else if (!has_occlusion_ref && !has_mr) {
      set_flag(gpu.flags, MaterialFlags::no_occlusion);
    }

    if (const auto *spec = mat.specular.get(); spec != nullptr) {
      gpu.specular_color_factor[0] = spec->specularColorFactor[0];
      gpu.specular_color_factor[1] = spec->specularColorFactor[1];
      gpu.specular_color_factor[2] = spec->specularColorFactor[2];
      gpu.specular_color_index =
          resolve_tex(asset, handles, spec->specularColorTexture, fb_albedo);
    }
  }

  if (auto *ext = mat.transmission.get(); ext != nullptr) {
    gpu.transmission_factor = ext->transmissionFactor;
    if (gpu.transmission_factor > 0.0F)
      set_flag(gpu.flags, MaterialFlags::has_transmission);
  }

  if (auto *ext = mat.anisotropy.get(); ext != nullptr) {
    gpu.anisotropy_factor = ext->anisotropyStrength;
    gpu.anisotropy_rotation = ext->anisotropyRotation;
    if (gpu.anisotropy_factor > 0.0F)
      set_flag(gpu.flags, MaterialFlags::has_anisotropy);
  }

  gpu.uv_scale_x = 1.0F;
  gpu.uv_scale_y = 1.0F;
  gpu.uv_offset_x = 0.0F;
  gpu.uv_offset_y = 0.0F;
  gpu.cull_mode = mat.doubleSided ? static_cast<u32>(CullMode::None)
                                  : static_cast<u32>(CullMode::Back);

  // Build per-texture UV channel mask: bit N = 1 means that texture uses UV1.
  auto uses_uv1 = [](const auto &opt) -> u32 {
    return (opt.has_value() && opt->texCoordIndex == 1) ? 1u : 0u;
  };
  u32 uv_mask = 0;
  if (const auto *sg = mat.specularGlossiness.get(); sg != nullptr) {
    uv_mask |= uses_uv1(sg->diffuseTexture)            << 0u; // albedo
    uv_mask |= uses_uv1(sg->specularGlossinessTexture) << 5u; // specgloss
  } else {
    uv_mask |= uses_uv1(pbr.baseColorTexture)         << 0u; // albedo
    uv_mask |= uses_uv1(pbr.metallicRoughnessTexture) << 2u; // mr
    if (const auto *spec = mat.specular.get(); spec != nullptr)
      uv_mask |= uses_uv1(spec->specularColorTexture) << 6u; // speccolor
  }
  uv_mask |= uses_uv1(mat.normalTexture)   << 1u; // normal
  uv_mask |= uses_uv1(mat.emissiveTexture) << 3u; // emissive
  uv_mask |= uses_uv1(mat.occlusionTexture)<< 4u; // occlusion
  gpu.uv_channel_mask = uv_mask;

  return gpu;
}

auto build_patch_list(const fastgltf::Asset &asset, usize image_idx)
    -> std::vector<mesh_cache::MaterialTexturePatch> {
  std::vector<mesh_cache::MaterialTexturePatch> patches;

  for (usize mi = 0; mi < asset.materials.size(); ++mi) {
    const auto &mat = asset.materials[mi];
    const u32 local_index = static_cast<u32>(mi);

    auto try_add = [&](const auto &tex_opt, mesh_cache::TextureRole role) {
      if (!tex_opt.has_value())
        return;
      const auto &tex = asset.textures[tex_opt->textureIndex];
      if (tex.imageIndex.has_value() && *tex.imageIndex == image_idx)
        patches.push_back({local_index, role});
    };

    if (const auto *sg = mat.specularGlossiness.get(); sg != nullptr) {
      try_add(sg->diffuseTexture, mesh_cache::TextureRole::albedo);
      try_add(sg->specularGlossinessTexture,
              mesh_cache::TextureRole::specular_glossiness);
    } else {
      try_add(mat.pbrData.baseColorTexture, mesh_cache::TextureRole::albedo);
      try_add(mat.pbrData.metallicRoughnessTexture,
              mesh_cache::TextureRole::metallic_roughness);
      if (const auto *spec = mat.specular.get(); spec != nullptr) {
        try_add(spec->specularColorTexture,
                mesh_cache::TextureRole::specular_color);
      }
    }
    try_add(mat.normalTexture, mesh_cache::TextureRole::normal);
    try_add(mat.occlusionTexture, mesh_cache::TextureRole::occlusion);
    try_add(mat.emissiveTexture, mesh_cache::TextureRole::emissive);
  }
  return patches;
}

} // namespace dy::mesh::detail
