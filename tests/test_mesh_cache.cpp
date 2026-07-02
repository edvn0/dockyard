#include <dockyard/mesh_cache.hpp>
#include <doctest/doctest.h>

using namespace dy;

namespace {

[[nodiscard]] auto make_sample_mesh() -> mesh_cache::BinaryMesh {
  mesh_cache::BinaryMesh mesh;

  mesh.vertices = {
      Vertex{.position = {0.0F, 0.0F, 0.0F}, .uvs = 0, .uvs1 = 0, .normal = 0, .tangent = 0, .bitangent = 0},
      Vertex{.position = {1.0F, 0.0F, 0.0F}, .uvs = 0, .uvs1 = 0, .normal = 0, .tangent = 0, .bitangent = 0},
      Vertex{.position = {0.0F, 1.0F, 0.0F}, .uvs = 0, .uvs1 = 0, .normal = 0, .tangent = 0, .bitangent = 0},
  };
  mesh.indices = {0, 1, 2};
  mesh.skin_vertices = {pack_skin_vertex({0, 1, 2, 3}, {1.0F, 0.0F, 0.0F, 0.0F})};

  GPUMaterial mat{};
  mat.metallic_factor = 0.5F;
  mesh.materials = {mat};
  mesh.material_slots = {0U};

  MeshLodGroup group{};
  group.vertex_offset = 0;
  group.vertex_count = 3;
  group.lod_count = 1;
  group.lods[0] = {.index_count = 3, .first_index = 0};
  mesh.meshes = {{group}};
  mesh.submesh_aabbs = {{AABB::create({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F})}};
  mesh.mesh_aabb = AABB::create({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F});

  MeshNodeDescription node;
  node.name = "triangle";
  node.primitives = {{.lod_group = group, .material_id = 0, .aabb = mesh.mesh_aabb, .flat_index = 0}};
  mesh.nodes = {std::move(node)};
  mesh.root_node_indices = {0U};

  mesh.images = {
      {
          .encoded_bytes = {std::byte{0xAB}, std::byte{0x4B}, std::byte{0xCD}},
          .format = VK_FORMAT_R8G8B8A8_SRGB,
          .srgb = true,
          .debug_name = "albedo",
          .cache_key = "albedo_key",
          .patches = {{.material_local_index = 0,
                       .role = mesh_cache::TextureRole::albedo}},
      },
  };

  return mesh;
}

} // namespace

TEST_CASE("Given a populated BinaryMesh, When serialized and deserialized, "
          "Then every section round-trips exactly") {
  const auto original = make_sample_mesh();
  const u64 source_hash = 0xDEADBEEFCAFEF00DULL;

  const auto bytes = mesh_cache::serialize(original, source_hash);
  auto result = mesh_cache::deserialize(bytes, source_hash);
  REQUIRE(result.has_value());

  const auto &loaded = *result;
  REQUIRE(loaded.vertices.size() == original.vertices.size());
  for (usize i = 0; i < original.vertices.size(); ++i) {
    CHECK(loaded.vertices[i].position[0] == original.vertices[i].position[0]);
    CHECK(loaded.vertices[i].position[1] == original.vertices[i].position[1]);
    CHECK(loaded.vertices[i].position[2] == original.vertices[i].position[2]);
  }
  CHECK(loaded.indices == original.indices);
  REQUIRE(loaded.skin_vertices.size() == 1);
  CHECK(loaded.skin_vertices[0].joints_0_1 == original.skin_vertices[0].joints_0_1);

  REQUIRE(loaded.materials.size() == 1);
  CHECK(loaded.materials[0].metallic_factor == original.materials[0].metallic_factor);
  CHECK(loaded.material_slots == original.material_slots);

  REQUIRE(loaded.meshes.size() == 1);
  REQUIRE(loaded.meshes[0].size() == 1);
  CHECK(loaded.meshes[0][0].vertex_count == 3);
  CHECK(loaded.meshes[0][0].lods[0].index_count == 3);

  REQUIRE(loaded.nodes.size() == 1);
  CHECK(loaded.nodes[0].name == "triangle");
  REQUIRE(loaded.nodes[0].primitives.size() == 1);
  CHECK(loaded.nodes[0].primitives[0].material_id == 0);

  REQUIRE(loaded.images.size() == 1);
  CHECK(loaded.images[0].encoded_bytes == original.images[0].encoded_bytes);
  CHECK(loaded.images[0].debug_name == "albedo");
  REQUIRE(loaded.images[0].patches.size() == 1);
  CHECK(loaded.images[0].patches[0].role == mesh_cache::TextureRole::albedo);
}

TEST_CASE("Given cache bytes with a corrupted magic, When deserialized, Then "
          "it is rejected") {
  const auto original = make_sample_mesh();
  auto bytes = mesh_cache::serialize(original, 1234ULL);
  bytes[0] ^= 0xFFU; // corrupt the magic field

  const auto result = mesh_cache::deserialize(bytes, 1234ULL);
  CHECK_FALSE(result.has_value());
}

TEST_CASE("Given cache bytes built for one version, When the reader expects "
          "another, Then it is rejected") {
  const auto original = make_sample_mesh();
  auto bytes = mesh_cache::serialize(original, 1234ULL);
  // version is the second u32 in the header, right after magic.
  bytes[4] ^= 0xFFU;

  const auto result = mesh_cache::deserialize(bytes, 1234ULL);
  CHECK_FALSE(result.has_value());
}

TEST_CASE("Given cache bytes hashed against one source, When the reader "
          "expects a different source hash, Then it is rejected as stale") {
  const auto original = make_sample_mesh();
  const auto bytes = mesh_cache::serialize(original, 1234ULL);

  const auto result = mesh_cache::deserialize(bytes, 5678ULL);
  CHECK_FALSE(result.has_value());
}

TEST_CASE("Given cache bytes with a corrupted compressed payload, When "
          "deserialized, Then the payload hash check rejects it") {
  const auto original = make_sample_mesh();
  auto bytes = mesh_cache::serialize(original, 1234ULL);
  REQUIRE(bytes.size() > sizeof(mesh_cache::MeshCacheHeader) + 1);
  bytes[sizeof(mesh_cache::MeshCacheHeader) + 1] ^= 0xFFU;

  const auto result = mesh_cache::deserialize(bytes, 1234ULL);
  CHECK_FALSE(result.has_value());
}

TEST_CASE("Given a truncated cache file, When deserialized, Then it is "
          "rejected rather than reading out of bounds") {
  const auto original = make_sample_mesh();
  auto bytes = mesh_cache::serialize(original, 1234ULL);
  bytes.resize(bytes.size() - 4);

  const auto result = mesh_cache::deserialize(bytes, 1234ULL);
  CHECK_FALSE(result.has_value());
}
