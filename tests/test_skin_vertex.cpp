#include <dockyard/device_geometry.hpp>
#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

using namespace dy;

namespace {

struct UnpackedSkin {
  std::array<u16, 4> joints;
  glm::vec4 weights;
};

[[nodiscard]] auto unpack(const SkinVertex &packed) -> UnpackedSkin {
  return UnpackedSkin{
      .joints =
          {
              static_cast<u16>(packed.joints_0_1 & 0xFFFFU),
              static_cast<u16>(packed.joints_0_1 >> 16U),
              static_cast<u16>(packed.joints_2_3 & 0xFFFFU),
              static_cast<u16>(packed.joints_2_3 >> 16U),
          },
      .weights = glm::unpackUnorm4x8(packed.weights),
  };
}

} // namespace

TEST_CASE("Given four joint indices, When packed, Then each index unpacks from "
          "its u16 lane") {
  const std::array<u16, 4> joints{3, 17, 250, 1024};
  const SkinVertex packed = pack_skin_vertex(joints, {1.0F, 0.0F, 0.0F, 0.0F});

  const auto out = unpack(packed);
  CHECK(out.joints[0] == 3);
  CHECK(out.joints[1] == 17);
  CHECK(out.joints[2] == 250);
  CHECK(out.joints[3] == 1024);
}

TEST_CASE("Given already-normalized weights, When packed, Then they round-trip "
          "within u8 precision") {
  const glm::vec4 weights{0.5F, 0.25F, 0.125F, 0.125F};
  const auto out = unpack(pack_skin_vertex({0, 1, 2, 3}, weights));

  constexpr f32 tol = 1.0F / 255.0F;
  CHECK(out.weights.x == doctest::Approx(weights.x).epsilon(tol));
  CHECK(out.weights.y == doctest::Approx(weights.y).epsilon(tol));
  CHECK(out.weights.z == doctest::Approx(weights.z).epsilon(tol));
  CHECK(out.weights.w == doctest::Approx(weights.w).epsilon(tol));
}

TEST_CASE("Given un-normalized weights, When packed, Then the weights are "
          "renormalized to sum to one") {
  const auto out = unpack(pack_skin_vertex({0, 1, 2, 3}, {2.0F, 1.0F, 1.0F, 0.0F}));

  const f32 sum = out.weights.x + out.weights.y + out.weights.z + out.weights.w;
  CHECK(sum == doctest::Approx(1.0F).epsilon(1.0F / 255.0F));
  CHECK(out.weights.x == doctest::Approx(0.5F).epsilon(1.0F / 255.0F));
}

TEST_CASE("Given all-zero weights, When packed, Then influence degenerates to "
          "joint zero") {
  const auto out = unpack(pack_skin_vertex({5, 6, 7, 8}, {0.0F, 0.0F, 0.0F, 0.0F}));

  CHECK(out.weights.x == doctest::Approx(1.0F).epsilon(1.0F / 255.0F));
  CHECK(out.weights.y == doctest::Approx(0.0F));
  CHECK(out.weights.z == doctest::Approx(0.0F));
  CHECK(out.weights.w == doctest::Approx(0.0F));
}
