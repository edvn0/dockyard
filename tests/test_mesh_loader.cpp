#include <dockyard/mesh/animation_load.hpp>
#include <dockyard/mesh/load_types.hpp>
#include <dockyard/mesh/lod.hpp>
#include <dockyard/mesh/tangents.hpp>
#include <dockyard/mesh/texture_decode.hpp>
#include <doctest/doctest.h>

#include <fastgltf/types.hpp>

#include <glm/gtc/packing.hpp>

#include <array>
#include <expected>
#include <initializer_list>
#include <string>

using namespace dy::mesh::detail;

TEST_CASE("Given a normal-map hint, When choosing a transcode format, Then "
          "BC5 (two-channel) is selected") {
  CHECK(ktx_transcode_format(VK_FORMAT_R8G8B8A8_UNORM, true) == KTX_TTF_BC5_RG);
}

TEST_CASE("Given a non-normal-map hint, When choosing a transcode format, "
          "Then BC7 (RGBA) is selected") {
  CHECK(ktx_transcode_format(VK_FORMAT_R8G8B8A8_UNORM, false) ==
        KTX_TTF_BC7_RGBA);
}

TEST_CASE("Given BC7 transcode target, When mapping to a VkFormat, Then sRGB "
          "and linear variants are distinguished") {
  CHECK(vk_format_for_transcode(KTX_TTF_BC7_RGBA, true) ==
        VK_FORMAT_BC7_SRGB_BLOCK);
  CHECK(vk_format_for_transcode(KTX_TTF_BC7_RGBA, false) ==
        VK_FORMAT_BC7_UNORM_BLOCK);
}

TEST_CASE("Given BC5 transcode target, When mapping to a VkFormat, Then the "
          "result is always linear (no sRGB BC5 format exists)") {
  CHECK(vk_format_for_transcode(KTX_TTF_BC5_RG, true) == VK_FORMAT_BC5_UNORM_BLOCK);
  CHECK(vk_format_for_transcode(KTX_TTF_BC5_RG, false) == VK_FORMAT_BC5_UNORM_BLOCK);
}

TEST_CASE("Given BC1 transcode target, When mapping to a VkFormat, Then sRGB "
          "and linear variants are distinguished") {
  CHECK(vk_format_for_transcode(KTX_TTF_BC1_RGB, true) ==
        VK_FORMAT_BC1_RGB_SRGB_BLOCK);
  CHECK(vk_format_for_transcode(KTX_TTF_BC1_RGB, false) ==
        VK_FORMAT_BC1_RGB_UNORM_BLOCK);
}

TEST_CASE("Given an unsupported transcode target, When mapping to a "
          "VkFormat, Then VK_FORMAT_UNDEFINED is returned") {
  CHECK(vk_format_for_transcode(KTX_TTF_ETC2_RGBA, false) ==
        VK_FORMAT_UNDEFINED);
}

TEST_CASE("Given bytes starting with the KTX2 magic number, When checked, "
          "Then they are identified as KTX2") {
  const std::array<std::byte, 12> magic = {
      std::byte{0xAB}, std::byte{0x4B}, std::byte{0x54}, std::byte{0x58},
      std::byte{0x20}, std::byte{0x32}, std::byte{0x30}, std::byte{0xBB},
      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
  };
  CHECK(is_ktx2(magic));
}

TEST_CASE("Given bytes that do not match the KTX2 magic number, When "
          "checked, Then they are rejected") {
  const std::array<std::byte, 12> not_magic{};
  CHECK_FALSE(is_ktx2(not_magic));
}

TEST_CASE("Given fewer bytes than the KTX2 magic number, When checked, Then "
          "they are rejected without reading out of bounds") {
  const std::array<std::byte, 4> too_short{};
  CHECK_FALSE(is_ktx2(too_short));
}

namespace {

[[nodiscard]] auto make_quad_vertex(float px, float py, float u, float v)
    -> dy::Vertex {
  dy::Vertex vert{};
  vert.position[0] = px;
  vert.position[1] = py;
  vert.position[2] = 0.0F;
  vert.uvs = glm::packHalf2x16({u, v});
  vert.normal = glm::packSnorm4x8(glm::vec4{0.0F, 0.0F, 1.0F, 0.0F});
  return vert;
}

} // namespace

TEST_CASE("Given a flat quad whose UVs are axis-aligned with its geometry, "
          "When mikktspace tangents are generated, Then the tangent points "
          "along +X and the bitangent along +Y") {
  PrimitiveData prim;
  prim.vertices = {
      make_quad_vertex(0.0F, 0.0F, 0.0F, 0.0F),
      make_quad_vertex(1.0F, 0.0F, 1.0F, 0.0F),
      make_quad_vertex(1.0F, 1.0F, 1.0F, 1.0F),
      make_quad_vertex(0.0F, 1.0F, 0.0F, 1.0F),
  };
  prim.indices = {0, 1, 2, 0, 2, 3};

  const auto result = generate_mikktspace_tangents(prim);
  REQUIRE(result.has_value());

  constexpr float tol = 1.0F / 64.0F;
  for (const auto &vert : prim.vertices) {
    const glm::vec4 tangent = glm::unpackSnorm4x8(vert.tangent);
    const glm::vec4 bitangent = glm::unpackSnorm4x8(vert.bitangent);

    CHECK(tangent.x == doctest::Approx(1.0F).epsilon(tol));
    CHECK(tangent.y == doctest::Approx(0.0F).epsilon(tol));
    CHECK(bitangent.y == doctest::Approx(1.0F).epsilon(tol));
    CHECK(bitangent.x == doctest::Approx(0.0F).epsilon(tol));
  }
}

TEST_CASE("Given a mix of successful and failed primitive extractions, When "
          "requirements are calculated, Then only the successful ones "
          "contribute vertex and index counts") {
  std::vector<std::expected<PrimitiveResult, std::string>> extracted;

  PrimitiveResult first;
  first.data.vertices.resize(4);
  first.data.indices.resize(6);
  extracted.push_back(first);

  extracted.push_back(std::unexpected("failed primitive"));

  PrimitiveResult second;
  second.data.vertices.resize(3);
  second.data.indices.resize(3);
  extracted.push_back(second);

  const auto reqs = calculate_requirements(extracted);
  CHECK(reqs.total_vertices == 7);
  CHECK(reqs.total_indices == 9);
}

TEST_CASE("Given no successful primitive extractions, When requirements are "
          "calculated, Then both totals are zero") {
  std::vector<std::expected<PrimitiveResult, std::string>> extracted;
  extracted.push_back(std::unexpected("failed"));
  extracted.push_back(std::unexpected("also failed"));

  const auto reqs = calculate_requirements(extracted);
  CHECK(reqs.total_vertices == 0);
  CHECK(reqs.total_indices == 0);
}

TEST_CASE("Given a mesh name with a trailing _lod{N} suffix, When parsed, "
          "Then the base name and LOD level are extracted") {
  const auto parsed = parse_lod_suffix("damaged_helmet_lod3");
  REQUIRE(parsed.has_value());
  CHECK(parsed->first == "damaged_helmet");
  CHECK(parsed->second == 3);
}

TEST_CASE("Given a mesh name with no _lod suffix, When parsed, Then nothing "
          "is returned") {
  CHECK_FALSE(parse_lod_suffix("damaged_helmet").has_value());
}

TEST_CASE("Given a mesh name whose suffix is not numeric, When parsed, Then "
          "nothing is returned") {
  CHECK_FALSE(parse_lod_suffix("foo_lodx").has_value());
}

TEST_CASE("Given a mesh name whose LOD level is at or beyond max_lods, When "
          "parsed, Then nothing is returned") {
  CHECK_FALSE(parse_lod_suffix("foo_lod6").has_value());
}

namespace {

[[nodiscard]] auto make_asset_with_mesh_names(std::vector<std::string> names)
    -> fastgltf::Asset {
  fastgltf::Asset asset;
  for (auto &name : names) {
    fastgltf::Mesh mesh;
    mesh.name = std::move(name);
    asset.meshes.push_back(std::move(mesh));
  }
  return asset;
}

} // namespace

TEST_CASE("Given meshes named with a shared base and _lod{N} suffixes, When "
          "LOD groups are detected, Then they are grouped with LOD0 first") {
  auto asset = make_asset_with_mesh_names(
      {"crate_lod1", "crate_lod0", "unrelated_mesh"});

  const auto groups = detect_lod_groups(asset);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].base_name == "crate");
  REQUIRE(groups[0].members.size() == 2);
  CHECK(groups[0].members[0].first == 0);
  CHECK(groups[0].members[1].first == 1);
  CHECK(groups[0].primary_mesh_idx() == groups[0].members[0].second);
}

TEST_CASE("Given a LOD chain missing LOD0, When LOD groups are detected, "
          "Then the incomplete chain is skipped") {
  auto asset = make_asset_with_mesh_names({"crate_lod1", "crate_lod2"});

  const auto groups = detect_lod_groups(asset);
  CHECK(groups.empty());
}

TEST_CASE("Given multiple LOD chains and a base-name filter, When LOD groups "
          "are detected, Then only the matching chain is returned") {
  auto asset = make_asset_with_mesh_names(
      {"crate_lod0", "crate_lod1", "barrel_lod0", "barrel_lod1"});

  const auto groups = detect_lod_groups(asset, std::string{"barrel"});
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].base_name == "barrel");
}

namespace {

[[nodiscard]] auto make_position_only_primitive(
    std::initializer_list<std::array<float, 3>> positions,
    std::vector<dy::u32> indices) -> PrimitiveData {
  PrimitiveData prim;
  for (const auto &pos : positions) {
    dy::Vertex vert{};
    vert.position[0] = pos[0];
    vert.position[1] = pos[1];
    vert.position[2] = pos[2];
    prim.vertices.push_back(vert);
  }
  prim.indices = std::move(indices);
  return prim;
}

} // namespace

TEST_CASE("Given a LODn sharing exact vertex positions with LOD0, When "
          "indices are remapped, Then each LODn index points at the "
          "matching LOD0 vertex and the remap is exact") {
  const auto lod0 = make_position_only_primitive(
      {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F},
       {0.0F, 1.0F, 0.0F}},
      {0, 1, 2, 0, 2, 3});

  // LODn only references two of LOD0's corners, in reverse order.
  const auto lod_n =
      make_position_only_primitive({{1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F}},
                                   {0, 1});

  const auto remap = remap_lod_indices_to_lod0(lod0, lod_n);
  REQUIRE(remap.exact);
  REQUIRE(remap.indices.size() == 2);
  CHECK(remap.indices[0] == 2); // lod_n vertex 0 == lod0 vertex 2
  CHECK(remap.indices[1] == 0); // lod_n vertex 1 == lod0 vertex 0
}

TEST_CASE("Given a LODn vertex whose position does not exactly match any "
          "LOD0 vertex, When indices are remapped, Then the nearest LOD0 "
          "vertex is used and the remap is reported as inexact") {
  const auto lod0 = make_position_only_primitive(
      {{0.0F, 0.0F, 0.0F}, {10.0F, 0.0F, 0.0F}}, {0, 1, 0});

  // Slightly off from lod0 vertex 0.
  const auto lod_n =
      make_position_only_primitive({{0.01F, 0.0F, 0.0F}}, {0});

  const auto remap = remap_lod_indices_to_lod0(lod0, lod_n);
  CHECK_FALSE(remap.exact);
  REQUIRE(remap.indices.size() == 1);
  CHECK(remap.indices[0] == 0);
}

TEST_CASE("Given each glTF animation interpolation mode, When mapped to the "
          "engine's enum, Then Step and CubicSpline map directly and "
          "anything else defaults to Linear") {
  CHECK(to_interpolation(fastgltf::AnimationInterpolation::Step) ==
        dy::AnimationInterpolation::Step);
  CHECK(to_interpolation(fastgltf::AnimationInterpolation::CubicSpline) ==
        dy::AnimationInterpolation::CubicSpline);
  CHECK(to_interpolation(fastgltf::AnimationInterpolation::Linear) ==
        dy::AnimationInterpolation::Linear);
}
